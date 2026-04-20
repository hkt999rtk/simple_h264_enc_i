#include "sh264e.h"

#include <stddef.h>
#include <stdint.h>

#if SH264E_QEMU_JPEG_STREAM_HALF
#define SRC_W 5120u
#define SRC_H 2880u
#else
#define SRC_W 1280u
#define SRC_H 720u
#endif

#define BENCH_SLICES 8u
#define SRC_LUMA_ROWS 272u
#define SRC_CHROMA_ROWS (SRC_LUMA_ROWS / 2u)
#define ENCODER_ARENA_CAPACITY 512u
#define STREAM_CHUNK_CAPACITY 1024u
#define WORK_SIZE ((size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT + \
                   (size_t)(SH264E_V1_WIDTH / 2u) * SH264E_V1_SLICE_CHROMA_HEIGHT * 2u)
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

sh264e_status_t sh264e_bench_encode_jpeg_stream_components(
    sh264e_encoder_t *encoder,
    const uint8_t *src_y,
    ptrdiff_t src_y_stride,
    const uint8_t *src_cb,
    const uint8_t *src_cr,
    ptrdiff_t src_c_stride,
    uint32_t src_width,
    uint32_t src_height,
    sh264e_pixfmt_t output_pixfmt,
    unsigned slice_count,
    uint8_t *work_buffer,
    size_t work_buffer_capacity,
    uint8_t *chunk_buffer,
    size_t chunk_capacity,
    sh264e_output_consumer_t consumer,
    void *consumer_user,
    size_t *out_size);

typedef struct bench_consumer_t {
    uint32_t checksum;
    size_t total_size;
    size_t chunks;
} bench_consumer_t;

static uint8_t encoder_arena[ENCODER_ARENA_CAPACITY];
static uint8_t src_y[(size_t)SRC_W * SRC_LUMA_ROWS];
static uint8_t src_cb[(size_t)(SRC_W / 2u) * SRC_CHROMA_ROWS];
static uint8_t src_cr[(size_t)(SRC_W / 2u) * SRC_CHROMA_ROWS];
static uint8_t work[WORK_SIZE];
static uint8_t stream_chunk[STREAM_CHUNK_CAPACITY];
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

static void fill_source(void)
{
    uint32_t y;
    uint32_t x;

    for (y = 0; y < SRC_LUMA_ROWS; y++) {
        uint8_t *row = src_y + (size_t)y * SRC_W;
        for (x = 0; x < SRC_W; x++) {
            row[x] = (uint8_t)((x * 7u + y * 3u) & 0xffu);
        }
    }
    for (y = 0; y < SRC_CHROMA_ROWS; y++) {
        uint8_t *row_cb = src_cb + (size_t)y * (SRC_W / 2u);
        uint8_t *row_cr = src_cr + (size_t)y * (SRC_W / 2u);
        for (x = 0; x < SRC_W / 2u; x++) {
            row_cb[x] = (uint8_t)(96u + ((x + y) & 15u));
            row_cr[x] = (uint8_t)(144u + ((x * 3u + y) & 15u));
        }
    }
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

static int run_benchmark(void)
{
    sh264e_config_t config;
    sh264e_encoder_t *encoder = NULL;
    bench_consumer_t consumer;
    size_t encoder_work_size = 0u;
    size_t bytes = 0u;
    uint32_t checksum;
    sh264e_status_t status;

    memset(&config, 0, sizeof(config));
    config.width = SH264E_V1_WIDTH;
    config.height = SH264E_V1_HEIGHT;
    config.pixfmt = SH264E_PIXFMT_I420;
    config.qp = SH264E_DEFAULT_QP;

    status = sh264e_encoder_get_work_size(&config, &encoder_work_size);
    if (status != SH264E_OK) {
        return 1;
    }
    if (encoder_work_size > sizeof(encoder_arena)) {
        return 2;
    }
    status = sh264e_encoder_create_with_arena(&config, encoder_arena,
                                              sizeof(encoder_arena), &encoder);
    if (status != SH264E_OK || encoder == NULL) {
        return 3;
    }

    memset(&consumer, 0, sizeof(consumer));
    consumer.checksum = FNV1A_OFFSET;

    dwt_start();
    status = sh264e_bench_encode_jpeg_stream_components(
        encoder,
        src_y,
        SRC_W,
        src_cb,
        src_cr,
        SRC_W / 2u,
        SRC_W,
        SRC_H,
        SH264E_PIXFMT_I420,
        BENCH_SLICES,
        work,
        sizeof(work),
        stream_chunk,
        sizeof(stream_chunk),
        checksum_consumer,
        &consumer,
        &bytes);
    sh264e_bench_cycles = dwt_stop();
    if (status != SH264E_OK || bytes == 0u) {
        return 4;
    }

    checksum = consumer.checksum;
    checksum ^= (uint32_t)consumer.total_size;
    checksum *= FNV1A_PRIME;
    checksum ^= (uint32_t)consumer.chunks;
    checksum *= FNV1A_PRIME;
    sh264e_bench_checksum = checksum;

    if (checksum == 0u) {
        return 5;
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

    fill_source();
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
