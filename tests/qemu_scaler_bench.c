#include "sh264e.h"

#include <stddef.h>
#include <stdint.h>

#if SH264E_QEMU_SCALER_BYPASS
#define SRC_W SH264E_V1_WIDTH
#define SRC_H SH264E_V1_HEIGHT
#elif SH264E_QEMU_SCALER_HALF
#define SRC_W 5120u
#define SRC_H 2880u
#elif SH264E_QEMU_SCALER_GENERAL
#define SRC_W 1920u
#define SRC_H 1080u
#else
#define SRC_W 1280u
#define SRC_H 720u
#endif

#define BENCH_ITERS 8u
#define SRC_LUMA_ROWS 272u
#define SRC_CHROMA_ROWS (SRC_LUMA_ROWS / 2u)
#define WORK_SIZE ((size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT + \
                   (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT)
#define FNV1A_OFFSET 2166136261u
#define FNV1A_PRIME 16777619u

#if SH264E_QEMU_SCALER_BYPASS
#define EXPECTED_CHECKSUM 0x0c2f7d05u
#elif SH264E_QEMU_SCALER_HALF
#define EXPECTED_CHECKSUM 0x85f66d05u
#elif SH264E_QEMU_SCALER_GENERAL
#define EXPECTED_CHECKSUM 0x8b3bf894u
#else
#define EXPECTED_CHECKSUM 0x6e421d33u
#endif

#define DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define DEMCR_TRCENA (1u << 24)
#define DWT_CTRL_CYCCNTENA 1u

extern unsigned long _estack;
extern unsigned long __bss_start__;
extern unsigned long __bss_end__;

static uint8_t src_y[(size_t)SRC_W * SRC_LUMA_ROWS];
static uint8_t src_uv[(size_t)SRC_W * SRC_CHROMA_ROWS];
static uint8_t work[WORK_SIZE];
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

static void fill_source(void)
{
    uint32_t y;
    uint32_t x;

    for (y = 0; y < SRC_LUMA_ROWS; y++) {
        uint8_t *row = src_y + (size_t)y * SRC_W;
        for (x = 0; x < SRC_W; x++) {
            row[x] = (uint8_t)((x * 3u + y * 5u) & 0xffu);
        }
    }
    for (y = 0; y < SRC_CHROMA_ROWS; y++) {
        uint8_t *row = src_uv + (size_t)y * SRC_W;
        for (x = 0; x < SRC_W / 2u; x++) {
            row[(size_t)x * 2u] = (uint8_t)(80u + ((x + y) & 31u));
            row[(size_t)x * 2u + 1u] = (uint8_t)(160u + ((x * 2u + y) & 31u));
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

static uint32_t checksum_bytes(uint32_t checksum, const uint8_t *data, size_t size)
{
    size_t i;

    for (i = 0; i < size; i++) {
        checksum ^= data[i];
        checksum *= FNV1A_PRIME;
    }
    return checksum;
}

static uint32_t checksum_slice(uint32_t checksum, const sh264e_slice_t *slice)
{
    checksum = checksum_bytes(checksum,
                              slice->plane[0],
                              (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT);
    checksum = checksum_bytes(checksum,
                              slice->plane[1],
                              (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT);
    checksum ^= (uint32_t)slice->stride[0];
    checksum *= FNV1A_PRIME;
    checksum ^= (uint32_t)slice->stride[1];
    checksum *= FNV1A_PRIME;
    return checksum;
}

static int run_benchmark(void)
{
    sh264e_frame_t frame;
    sh264e_slice_t slice;
    size_t work_size = 0u;
    const size_t expected_work_size =
#if SH264E_QEMU_SCALER_BYPASS
        0u;
#else
        WORK_SIZE;
#endif
    uint32_t iter;
    uint32_t checksum = FNV1A_OFFSET;

    memset(&frame, 0, sizeof(frame));
    frame.width = SRC_W;
    frame.height = SRC_H;
    frame.pixfmt = SH264E_PIXFMT_NV12;
    frame.plane[0] = src_y;
    frame.plane[1] = src_uv;
    frame.stride[0] = SRC_W;
    frame.stride[1] = SRC_W;

    if (sh264e_resize_get_slice_buffer_size(&frame, &work_size) != SH264E_OK) {
        return 1;
    }
    if (work_size != expected_work_size) {
        return 2;
    }

    dwt_start();
    for (iter = 0; iter < BENCH_ITERS; iter++) {
        uint8_t *work_ptr = expected_work_size == 0u ? NULL : work;
        size_t work_capacity = expected_work_size == 0u ? 0u : sizeof(work);

        if (sh264e_resize_make_slice(&frame, iter % SH264E_V1_SLICE_COUNT,
                                     work_ptr, work_capacity, &slice) != SH264E_OK) {
            return 3;
        }
    }
    sh264e_bench_cycles = dwt_stop();

    for (iter = 0; iter < BENCH_ITERS; iter++) {
        uint8_t *work_ptr = expected_work_size == 0u ? NULL : work;
        size_t work_capacity = expected_work_size == 0u ? 0u : sizeof(work);

        if (sh264e_resize_make_slice(&frame, iter % SH264E_V1_SLICE_COUNT,
                                     work_ptr, work_capacity, &slice) != SH264E_OK) {
            return 3;
        }
        checksum = checksum_slice(checksum, &slice);
    }
    sh264e_bench_checksum = checksum;

    if (checksum != EXPECTED_CHECKSUM) {
        return 4;
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
