#include "sh264e.h"

#include <stddef.h>
#include <stdint.h>

#define BENCH_ROWS 8u
#define ENCODER_ARENA_CAPACITY 512u
#define HEADER_OUTPUT_CAPACITY 1024u
#define ROW_OUTPUT_CAPACITY 62080u
#define STREAM_CHUNK_CAPACITY 1024u
#define SLICE_Y_BYTES ((size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT)
#define SLICE_C_BYTES ((size_t)(SH264E_V1_WIDTH / 2u) * SH264E_V1_SLICE_CHROMA_HEIGHT)
#define SLICE_UV_BYTES ((size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT)
#define EXPECTED_I420_BUFFER_CHECKSUM 0x5926e2e5u
#define FNV1A_OFFSET 2166136261u
#define FNV1A_PRIME 16777619u

#define DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define DEMCR_TRCENA (1u << 24)
#define DWT_CTRL_CYCCNTENA 1u

extern unsigned long _estack;
extern unsigned long __bss_start__;
extern unsigned long __bss_end__;

#if SH264E_QEMU_ENCODER_STREAM
sh264e_status_t sh264e_bench_begin_idr_to_consumer(sh264e_encoder_t *encoder,
                                                   uint8_t *chunk_buffer,
                                                   size_t chunk_capacity,
                                                   size_t *out_size,
                                                   sh264e_output_consumer_t consumer,
                                                   void *consumer_user);
sh264e_status_t sh264e_bench_encode_idr_slice_to_consumer(
    sh264e_encoder_t *encoder,
    const sh264e_slice_t *slice,
    uint8_t *chunk_buffer,
    size_t chunk_capacity,
    size_t *out_size,
    sh264e_output_consumer_t consumer,
    void *consumer_user);
#endif

typedef struct bench_consumer_t {
    uint32_t checksum;
    size_t total_size;
    size_t chunks;
} bench_consumer_t;

static uint8_t encoder_arena[ENCODER_ARENA_CAPACITY];
static uint8_t input_y[(size_t)BENCH_ROWS * SLICE_Y_BYTES];
#if SH264E_QEMU_ENCODER_NV12
static uint8_t input_uv[(size_t)BENCH_ROWS * SLICE_UV_BYTES];
#else
static uint8_t input_u[(size_t)BENCH_ROWS * SLICE_C_BYTES];
static uint8_t input_v[(size_t)BENCH_ROWS * SLICE_C_BYTES];
#endif
#if SH264E_QEMU_ENCODER_STREAM
static uint8_t stream_chunk[STREAM_CHUNK_CAPACITY];
#else
static uint8_t header_output[HEADER_OUTPUT_CAPACITY];
static uint8_t row_output[(size_t)BENCH_ROWS * ROW_OUTPUT_CAPACITY];
static size_t row_output_size[BENCH_ROWS];
#endif
volatile uint32_t sh264e_bench_cycles;
volatile uint32_t sh264e_bench_checksum;

void *memset(void *dst, int value, size_t size)
{
    uint8_t *p = (uint8_t *)dst;
    while (size > 0u) {
        *p++ = (uint8_t)value;
        size--;
    }
    return dst;
}

static uint32_t checksum_bytes(uint32_t checksum, const uint8_t *data, size_t size)
{
    size_t i;

    for (i = 0; i < size; i++) {
        checksum ^= data[i];
        checksum *= FNV1A_PRIME;
    }
    return checksum;
}

static void fill_input_rows(void)
{
    uint32_t row;
    uint32_t y;
    uint32_t x;

    for (row = 0; row < BENCH_ROWS; row++) {
        uint8_t *slice_y = input_y + (size_t)row * SLICE_Y_BYTES;

        for (y = 0; y < SH264E_V1_SLICE_LUMA_HEIGHT; y++) {
            uint8_t *dst = slice_y + (size_t)y * SH264E_V1_WIDTH;
            for (x = 0; x < SH264E_V1_WIDTH; x++) {
                dst[x] = (uint8_t)((x * 3u + (row * 16u + y) * 5u) & 0xffu);
            }
        }

#if SH264E_QEMU_ENCODER_NV12
        {
            uint8_t *slice_uv = input_uv + (size_t)row * SLICE_UV_BYTES;
            for (y = 0; y < SH264E_V1_SLICE_CHROMA_HEIGHT; y++) {
                uint8_t *dst = slice_uv + (size_t)y * SH264E_V1_WIDTH;
                for (x = 0; x < SH264E_V1_WIDTH / 2u; x++) {
                    dst[(size_t)x * 2u] = (uint8_t)(80u + ((x + row + y) & 31u));
                    dst[(size_t)x * 2u + 1u] = (uint8_t)(160u + ((x * 2u + row + y) & 31u));
                }
            }
        }
#else
        {
            uint8_t *slice_u = input_u + (size_t)row * SLICE_C_BYTES;
            uint8_t *slice_v = input_v + (size_t)row * SLICE_C_BYTES;
            for (y = 0; y < SH264E_V1_SLICE_CHROMA_HEIGHT; y++) {
                uint8_t *dst_u = slice_u + (size_t)y * (SH264E_V1_WIDTH / 2u);
                uint8_t *dst_v = slice_v + (size_t)y * (SH264E_V1_WIDTH / 2u);
                for (x = 0; x < SH264E_V1_WIDTH / 2u; x++) {
                    dst_u[x] = (uint8_t)(80u + ((x + row + y) & 31u));
                    dst_v[x] = (uint8_t)(160u + ((x * 2u + row + y) & 31u));
                }
            }
        }
#endif
    }
}

static sh264e_pixfmt_t bench_pixfmt(void)
{
#if SH264E_QEMU_ENCODER_NV12
    return SH264E_PIXFMT_NV12;
#else
    return SH264E_PIXFMT_I420;
#endif
}

static void make_input_slice(uint32_t row, sh264e_slice_t *slice)
{
    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = bench_pixfmt();
    slice->plane[0] = input_y + (size_t)row * SLICE_Y_BYTES;
    slice->stride[0] = SH264E_V1_WIDTH;
#if SH264E_QEMU_ENCODER_NV12
    slice->plane[1] = input_uv + (size_t)row * SLICE_UV_BYTES;
    slice->stride[1] = SH264E_V1_WIDTH;
#else
    slice->plane[1] = input_u + (size_t)row * SLICE_C_BYTES;
    slice->plane[2] = input_v + (size_t)row * SLICE_C_BYTES;
    slice->stride[1] = SH264E_V1_WIDTH / 2u;
    slice->stride[2] = SH264E_V1_WIDTH / 2u;
#endif
}

static void dwt_start(void)
{
    DEMCR |= DEMCR_TRCENA;
    DWT_CYCCNT = 0u;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
}

static uint32_t dwt_stop(void)
{
    return DWT_CYCCNT;
}

#if SH264E_QEMU_ENCODER_STREAM
static sh264e_status_t checksum_consumer(void *user, const uint8_t *data, size_t size)
{
    bench_consumer_t *state = (bench_consumer_t *)user;

    if (state == NULL || data == NULL || size == 0u) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    state->checksum = checksum_bytes(state->checksum, data, size);
    state->total_size += size;
    state->chunks++;
    return SH264E_OK;
}
#endif

static int checksum_is_expected(uint32_t checksum)
{
#if !SH264E_QEMU_ENCODER_STREAM && !SH264E_QEMU_ENCODER_NV12
    return checksum == EXPECTED_I420_BUFFER_CHECKSUM;
#else
    return checksum != 0u;
#endif
}

static int run_benchmark(void)
{
    sh264e_config_t config;
    sh264e_encoder_t *encoder = NULL;
    sh264e_slice_t slice;
    size_t encoder_work_size = 0u;
    size_t max_row_size = 0u;
    uint32_t row;
    uint32_t checksum = FNV1A_OFFSET;
    sh264e_status_t status;

    memset(&config, 0, sizeof(config));
    config.width = SH264E_V1_WIDTH;
    config.height = SH264E_V1_HEIGHT;
    config.pixfmt = bench_pixfmt();
    config.qp = SH264E_DEFAULT_QP;

    status = sh264e_encoder_get_work_size(&config, &encoder_work_size);
    if (status != SH264E_OK) {
        return 1;
    }
    if (encoder_work_size > sizeof(encoder_arena)) {
        return 2;
    }
    status = sh264e_get_max_slice_output_size(&config, &max_row_size);
    if (status != SH264E_OK || max_row_size > ROW_OUTPUT_CAPACITY) {
        return 3;
    }
    status = sh264e_encoder_create_with_arena(&config, encoder_arena,
                                              sizeof(encoder_arena), &encoder);
    if (status != SH264E_OK || encoder == NULL) {
        return 4;
    }

    dwt_start();
#if SH264E_QEMU_ENCODER_STREAM
    {
        bench_consumer_t consumer;
        size_t bytes = 0u;

        memset(&consumer, 0, sizeof(consumer));
        consumer.checksum = FNV1A_OFFSET;

        status = sh264e_bench_begin_idr_to_consumer(encoder,
                                                    stream_chunk,
                                                    sizeof(stream_chunk),
                                                    &bytes,
                                                    checksum_consumer,
                                                    &consumer);
        if (status != SH264E_OK || bytes == 0u) {
            return 5;
        }

        for (row = 0; row < BENCH_ROWS; row++) {
            make_input_slice(row, &slice);
            status = sh264e_bench_encode_idr_slice_to_consumer(
                encoder, &slice, stream_chunk, sizeof(stream_chunk), &bytes,
                checksum_consumer, &consumer);
            if (status != SH264E_OK) {
                return 6;
            }
            if (bytes == 0u) {
                return 7;
            }
        }
        checksum = consumer.checksum;
        checksum ^= (uint32_t)consumer.total_size;
        checksum *= FNV1A_PRIME;
        checksum ^= (uint32_t)consumer.chunks;
        checksum *= FNV1A_PRIME;
    }
#else
    {
        size_t header_size = 0u;

        status = sh264e_begin_idr(encoder, header_output, sizeof(header_output), &header_size);
        if (status != SH264E_OK) {
            return 5;
        }

        for (row = 0; row < BENCH_ROWS; row++) {
            make_input_slice(row, &slice);
            status = sh264e_encode_idr_slice(encoder, &slice,
                                             row_output + (size_t)row * ROW_OUTPUT_CAPACITY,
                                             ROW_OUTPUT_CAPACITY,
                                             &row_output_size[row]);
            if (status != SH264E_OK) {
                return 6;
            }
            if (row_output_size[row] == 0u || row_output_size[row] > ROW_OUTPUT_CAPACITY) {
                return 7;
            }
        }

        checksum = checksum_bytes(checksum, header_output, header_size);
        for (row = 0; row < BENCH_ROWS; row++) {
            checksum ^= (uint32_t)row_output_size[row];
            checksum *= FNV1A_PRIME;
            checksum = checksum_bytes(checksum,
                                      row_output + (size_t)row * ROW_OUTPUT_CAPACITY,
                                      row_output_size[row]);
        }
    }
#endif
    sh264e_bench_cycles = dwt_stop();
    sh264e_bench_checksum = checksum;

    if (!checksum_is_expected(checksum)) {
        return 8;
    }
    return 0;
}

static void semihost_exit(int code)
{
    uint32_t args[2];
    register uint32_t r0 __asm("r0") = 0x20u;
    register uint32_t r1 __asm("r1");

    args[0] = 0x20026u;
    args[1] = (uint32_t)code;
    r1 = (uint32_t)args;
    __asm volatile("bkpt 0xab" : : "r"(r0), "r"(r1) : "memory");
    for (;;) {
    }
}

void Reset_Handler(void)
{
    unsigned long *p;
    int rc;

    for (p = &__bss_start__; p < &__bss_end__; p++) {
        *p = 0u;
    }

    fill_input_rows();
    rc = run_benchmark();
    semihost_exit(rc);
}

void Default_Handler(void)
{
    semihost_exit(99);
}

__attribute__((section(".isr_vector"), used))
void (*const g_vector_table[])(void) = {
    (void (*)(void))(&_estack),
    Reset_Handler,
    Default_Handler
};
