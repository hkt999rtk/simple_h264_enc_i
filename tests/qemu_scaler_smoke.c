#include "sh264e.h"

#include <stddef.h>
#include <stdint.h>

#define SRC_W 1280u
#define SRC_H 720u
#define WORK_SIZE ((size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT + \
                   (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT)

extern unsigned long _estack;
extern unsigned long __bss_start__;
extern unsigned long __bss_end__;

static uint8_t src_y[(size_t)SRC_W * SRC_H];
static uint8_t src_u[(size_t)(SRC_W / 2u) * (SRC_H / 2u)];
static uint8_t src_v[(size_t)(SRC_W / 2u) * (SRC_H / 2u)];
static uint8_t work[WORK_SIZE];
static uint8_t bypass_y[(size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT];
static uint8_t bypass_u[(size_t)(SH264E_V1_WIDTH / 2u) * SH264E_V1_SLICE_CHROMA_HEIGHT];
static uint8_t bypass_v[(size_t)(SH264E_V1_WIDTH / 2u) * SH264E_V1_SLICE_CHROMA_HEIGHT];

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
            row[x] = (uint8_t)((x + y * 3u) & 0xffu);
        }
    }
    memset(src_u, 96, sizeof(src_u));
    memset(src_v, 176, sizeof(src_v));
}

static int smoke_scaled_slice(void)
{
    sh264e_frame_t frame;
    sh264e_slice_t slice;
    size_t work_size = 0;
    size_t i;
    uint32_t checksum = 0;

    memset(&frame, 0, sizeof(frame));
    frame.width = SRC_W;
    frame.height = SRC_H;
    frame.pixfmt = SH264E_PIXFMT_I420;
    frame.plane[0] = src_y;
    frame.plane[1] = src_u;
    frame.plane[2] = src_v;
    frame.stride[0] = SRC_W;
    frame.stride[1] = SRC_W / 2u;
    frame.stride[2] = SRC_W / 2u;

    if (sh264e_resize_get_slice_buffer_size(&frame, &work_size) != SH264E_OK) {
        return 1;
    }
    if (work_size != WORK_SIZE) {
        return 2;
    }
    if (sh264e_resize_make_slice(&frame, 0u, work, sizeof(work), &slice) != SH264E_OK) {
        return 3;
    }
    if (slice.plane[0] != work ||
        slice.plane[1] != work + (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT ||
        slice.stride[0] != (ptrdiff_t)SH264E_V1_WIDTH ||
        slice.stride[1] != (ptrdiff_t)(SH264E_V1_WIDTH / 2u)) {
        return 4;
    }

    for (i = 0; i < (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT; i += 257u) {
        checksum += slice.plane[0][i];
    }
    if (checksum == 0u) {
        return 5;
    }
    for (i = 0; i < (size_t)(SH264E_V1_WIDTH / 2u) * SH264E_V1_SLICE_CHROMA_HEIGHT; i++) {
        if (slice.plane[1][i] != 96u || slice.plane[2][i] != 176u) {
            return 6;
        }
    }
    return 0;
}

static int smoke_bypass_slice(void)
{
    sh264e_frame_t frame;
    sh264e_slice_t slice;
    size_t work_size = 1u;

    memset(&frame, 0, sizeof(frame));
    frame.width = SH264E_V1_WIDTH;
    frame.height = SH264E_V1_HEIGHT;
    frame.pixfmt = SH264E_PIXFMT_I420;
    frame.plane[0] = bypass_y;
    frame.plane[1] = bypass_u;
    frame.plane[2] = bypass_v;
    frame.stride[0] = SH264E_V1_WIDTH;
    frame.stride[1] = SH264E_V1_WIDTH / 2u;
    frame.stride[2] = SH264E_V1_WIDTH / 2u;

    if (sh264e_resize_get_slice_buffer_size(&frame, &work_size) != SH264E_OK) {
        return 10;
    }
    if (work_size != 0u) {
        return 11;
    }
    if (sh264e_resize_make_slice(&frame, 0u, NULL, 0u, &slice) != SH264E_OK) {
        return 12;
    }
    if (slice.plane[0] != bypass_y || slice.plane[1] != bypass_u || slice.plane[2] != bypass_v) {
        return 13;
    }
    return 0;
}

static int smoke_main(void)
{
    int rc;

    fill_source();
    rc = smoke_scaled_slice();
    if (rc != 0) {
        return rc;
    }
    return smoke_bypass_slice();
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

    rc = smoke_main();
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
