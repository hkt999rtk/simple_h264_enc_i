#include "sh264e.h"

#include <stddef.h>
#include <stdint.h>

#define SRC_W 1280u
#define SRC_H 720u
#define WORK_SIZE ((size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT + \
                   (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT)
#define BENCH_ITERS 8u

#define DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define DEMCR_TRCENA (1u << 24)
#define DWT_CTRL_CYCCNTENA 1u

extern unsigned long _estack;
extern unsigned long __bss_start__;
extern unsigned long __bss_end__;

static uint8_t src_y[(size_t)SRC_W * SRC_H];
static uint8_t src_uv[(size_t)SRC_W * (SRC_H / 2u)];
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

    for (y = 0; y < SRC_H; y++) {
        uint8_t *row = src_y + (size_t)y * SRC_W;
        for (x = 0; x < SRC_W; x++) {
            row[x] = (uint8_t)((x * 3u + y * 5u) & 0xffu);
        }
    }
    for (y = 0; y < SRC_H / 2u; y++) {
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

static int run_benchmark(void)
{
    sh264e_frame_t frame;
    sh264e_slice_t slice;
    size_t work_size = 0;
    uint32_t iter;
    uint32_t checksum = 0;

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
    if (work_size != WORK_SIZE) {
        return 2;
    }

    dwt_start();
    for (iter = 0; iter < BENCH_ITERS; iter++) {
        if (sh264e_resize_make_slice(&frame, iter % SH264E_V1_SLICE_COUNT,
                                     work, sizeof(work), &slice) != SH264E_OK) {
            return 3;
        }
        checksum += slice.plane[0][iter * 97u];
        checksum += slice.plane[1][iter * 53u];
    }
    sh264e_bench_cycles = dwt_stop();
    sh264e_bench_checksum = checksum;

    if (checksum == 0u) {
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
