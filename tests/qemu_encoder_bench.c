#include "sh264e.h"

#include <stddef.h>
#include <stdint.h>

#define BENCH_ROWS 8u
#define ENCODER_ARENA_CAPACITY 512u
#define HEADER_OUTPUT_CAPACITY 1024u
#define ROW_OUTPUT_CAPACITY 62080u
#define SLICE_Y_BYTES ((size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT)
#define SLICE_C_BYTES ((size_t)(SH264E_V1_WIDTH / 2u) * SH264E_V1_SLICE_CHROMA_HEIGHT)
#define EXPECTED_CHECKSUM 0x5926e2e5u

#define DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define DEMCR_TRCENA (1u << 24)
#define DWT_CTRL_CYCCNTENA 1u

extern unsigned long _estack;
extern unsigned long __bss_start__;
extern unsigned long __bss_end__;

static uint8_t encoder_arena[ENCODER_ARENA_CAPACITY];
static uint8_t input_y[(size_t)BENCH_ROWS * SLICE_Y_BYTES];
static uint8_t input_u[(size_t)BENCH_ROWS * SLICE_C_BYTES];
static uint8_t input_v[(size_t)BENCH_ROWS * SLICE_C_BYTES];
static uint8_t header_output[HEADER_OUTPUT_CAPACITY];
static uint8_t row_output[(size_t)BENCH_ROWS * ROW_OUTPUT_CAPACITY];
static size_t row_output_size[BENCH_ROWS];
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

static void fill_input_rows(void)
{
    uint32_t row;
    uint32_t y;
    uint32_t x;

    for (row = 0; row < BENCH_ROWS; row++) {
        uint8_t *slice_y = input_y + (size_t)row * SLICE_Y_BYTES;
        uint8_t *slice_u = input_u + (size_t)row * SLICE_C_BYTES;
        uint8_t *slice_v = input_v + (size_t)row * SLICE_C_BYTES;

        for (y = 0; y < SH264E_V1_SLICE_LUMA_HEIGHT; y++) {
            uint8_t *dst = slice_y + (size_t)y * SH264E_V1_WIDTH;
            for (x = 0; x < SH264E_V1_WIDTH; x++) {
                dst[x] = (uint8_t)((x * 3u + (row * 16u + y) * 5u) & 0xffu);
            }
        }

        for (y = 0; y < SH264E_V1_SLICE_CHROMA_HEIGHT; y++) {
            uint8_t *dst_u = slice_u + (size_t)y * (SH264E_V1_WIDTH / 2u);
            uint8_t *dst_v = slice_v + (size_t)y * (SH264E_V1_WIDTH / 2u);
            for (x = 0; x < SH264E_V1_WIDTH / 2u; x++) {
                dst_u[x] = (uint8_t)(80u + ((x + row + y) & 31u));
                dst_v[x] = (uint8_t)(160u + ((x * 2u + row + y) & 31u));
            }
        }
    }
}

static void make_input_slice(uint32_t row, sh264e_slice_t *slice)
{
    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = SH264E_PIXFMT_I420;
    slice->plane[0] = input_y + (size_t)row * SLICE_Y_BYTES;
    slice->plane[1] = input_u + (size_t)row * SLICE_C_BYTES;
    slice->plane[2] = input_v + (size_t)row * SLICE_C_BYTES;
    slice->stride[0] = SH264E_V1_WIDTH;
    slice->stride[1] = SH264E_V1_WIDTH / 2u;
    slice->stride[2] = SH264E_V1_WIDTH / 2u;
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

static uint32_t checksum_bytes(uint32_t checksum, const uint8_t *data, size_t size)
{
    size_t i;

    for (i = 0; i < size; i++) {
        checksum ^= data[i];
        checksum *= 16777619u;
    }
    return checksum;
}

static int run_benchmark(void)
{
    sh264e_config_t config;
    sh264e_encoder_t *encoder = NULL;
    sh264e_slice_t slice;
    size_t encoder_work_size = 0u;
    size_t max_header_size = 0u;
    size_t max_row_size = 0u;
    size_t header_size = 0u;
    uint32_t row;
    uint32_t checksum = 2166136261u;
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
    status = sh264e_get_max_header_output_size(&config, &max_header_size);
    if (status != SH264E_OK || max_header_size > sizeof(header_output)) {
        return 3;
    }
    status = sh264e_get_max_slice_output_size(&config, &max_row_size);
    if (status != SH264E_OK || max_row_size > ROW_OUTPUT_CAPACITY) {
        return 4;
    }
    status = sh264e_encoder_create_with_arena(&config, encoder_arena,
                                              sizeof(encoder_arena), &encoder);
    if (status != SH264E_OK || encoder == NULL) {
        return 5;
    }

    dwt_start();
    status = sh264e_begin_idr(encoder, header_output, sizeof(header_output), &header_size);
    if (status != SH264E_OK) {
        return 6;
    }

    for (row = 0; row < BENCH_ROWS; row++) {
        make_input_slice(row, &slice);
        status = sh264e_encode_idr_slice(encoder, &slice,
                                         row_output + (size_t)row * ROW_OUTPUT_CAPACITY,
                                         ROW_OUTPUT_CAPACITY,
                                         &row_output_size[row]);
        if (status != SH264E_OK) {
            return 7;
        }
        if (row_output_size[row] == 0u || row_output_size[row] > ROW_OUTPUT_CAPACITY) {
            return 8;
        }
    }
    sh264e_bench_cycles = dwt_stop();

    checksum = checksum_bytes(checksum, header_output, header_size);
    for (row = 0; row < BENCH_ROWS; row++) {
        checksum ^= (uint32_t)row_output_size[row];
        checksum *= 16777619u;
        checksum = checksum_bytes(checksum,
                                  row_output + (size_t)row * ROW_OUTPUT_CAPACITY,
                                  row_output_size[row]);
    }
    sh264e_bench_checksum = checksum;

    if (checksum != EXPECTED_CHECKSUM || sh264e_bench_cycles == 0u) {
        return 9;
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
