#ifndef SH264E_JPEG_TEST_HOOKS_H
#define SH264E_JPEG_TEST_HOOKS_H

#include "sh264e.h"

#ifndef SH264E_ENABLE_JPEG_TEST_HOOKS
#define SH264E_ENABLE_JPEG_TEST_HOOKS 0
#endif

#if !SH264E_ENABLE_JPEG_TEST_HOOKS
#error "sh264e_jpeg_test_hooks.h requires SH264E_ENABLE_JPEG_TEST_HOOKS"
#endif

void sh264e_jpeg_set_test_allocation_limit(size_t max_bytes);

unsigned sh264e_jpeg_get_test_last_exact_resize_mask(void);

sh264e_status_t sh264e_encode_jpeg_idr_streaming_prototype(
    sh264e_encoder_t *encoder,
    const uint8_t *jpeg_data,
    size_t jpeg_size,
    uint8_t *work_buffer,
    size_t work_buffer_capacity,
    uint8_t *out,
    size_t out_capacity,
    size_t *out_size);

sh264e_status_t sh264e_encode_jpeg_idr_streaming_prototype_with_arena(
    sh264e_encoder_t *encoder,
    const uint8_t *jpeg_data,
    size_t jpeg_size,
    uint8_t *jpeg_arena,
    size_t jpeg_arena_size,
    uint8_t *work_buffer,
    size_t work_buffer_capacity,
    uint8_t *out,
    size_t out_capacity,
    size_t *out_size);

#endif
