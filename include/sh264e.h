#ifndef SH264E_H
#define SH264E_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH264E_V1_WIDTH 2560u
#define SH264E_V1_HEIGHT 1440u
#define SH264E_V1_MB_WIDTH 160u
#define SH264E_V1_SLICE_COUNT 90u
#define SH264E_V1_SLICE_LUMA_HEIGHT 16u
#define SH264E_V1_SLICE_CHROMA_HEIGHT 8u
#define SH264E_DEFAULT_QP 28
#define SH264E_RESIZE_MIN_SRC_WIDTH 1280u
#define SH264E_RESIZE_MAX_SRC_WIDTH 5120u
#define SH264E_RESIZE_MIN_SRC_HEIGHT 720u
#define SH264E_RESIZE_MAX_SRC_HEIGHT 2880u

typedef enum sh264e_status_t {
    SH264E_OK = 0,
    SH264E_ERR_INVALID_ARGUMENT = -1,
    SH264E_ERR_UNSUPPORTED_CONFIG = -2,
    SH264E_ERR_BUFFER_TOO_SMALL = -3,
    SH264E_ERR_ALLOCATION_FAILED = -4,
    SH264E_ERR_INTERNAL = -5,
    SH264E_ERR_BAD_STATE = -6,
    SH264E_ERR_INCOMPLETE_FRAME = -7,
    SH264E_ERR_FRAME_COMPLETE = -8
} sh264e_status_t;

typedef enum sh264e_pixfmt_t {
    SH264E_PIXFMT_I420 = 0,
    SH264E_PIXFMT_NV12 = 1
} sh264e_pixfmt_t;

typedef struct sh264e_config_t {
    uint32_t width;
    uint32_t height;
    sh264e_pixfmt_t pixfmt;
    int qp;
} sh264e_config_t;

typedef struct sh264e_frame_t {
    uint32_t width;
    uint32_t height;
    sh264e_pixfmt_t pixfmt;
    const uint8_t *plane[3];
    ptrdiff_t stride[3];
} sh264e_frame_t;

typedef struct sh264e_slice_t {
    sh264e_pixfmt_t pixfmt;
    const uint8_t *plane[3];
    ptrdiff_t stride[3];
} sh264e_slice_t;

typedef struct sh264e_jpeg_allocation_stats_t {
    size_t current_bytes;
    size_t peak_bytes;
} sh264e_jpeg_allocation_stats_t;

typedef struct sh264e_encoder_memory_report_t {
    size_t context_bytes;
    size_t bitstream_scratch_bytes;
    size_t recon_luma_bytes;
    size_t recon_chroma_bytes;
    size_t neighbor_state_bytes;
    size_t total_bytes;
} sh264e_encoder_memory_report_t;

typedef struct sh264e_encoder_t sh264e_encoder_t;

typedef sh264e_status_t (*sh264e_output_consumer_t)(void *user,
                                                    const uint8_t *data,
                                                    size_t size);

sh264e_status_t sh264e_encoder_create(const sh264e_config_t *config,
                                      sh264e_encoder_t **out_encoder);

void sh264e_encoder_destroy(sh264e_encoder_t *encoder);

sh264e_status_t sh264e_get_max_header_output_size(const sh264e_config_t *config,
                                                  size_t *out_size);

sh264e_status_t sh264e_get_max_slice_output_size(const sh264e_config_t *config,
                                                 size_t *out_size);

sh264e_status_t sh264e_get_max_output_size(const sh264e_config_t *config,
                                           size_t *out_size);

sh264e_status_t sh264e_encoder_get_memory_report(
    const sh264e_config_t *config,
    sh264e_encoder_memory_report_t *out_report);

sh264e_status_t sh264e_begin_idr(sh264e_encoder_t *encoder,
                                 uint8_t *out,
                                 size_t out_capacity,
                                 size_t *out_size);

sh264e_status_t sh264e_encode_idr_slice(sh264e_encoder_t *encoder,
                                        const sh264e_slice_t *slice,
                                        uint8_t *out,
                                        size_t out_capacity,
                                        size_t *out_size);

sh264e_status_t sh264e_end_idr(sh264e_encoder_t *encoder);

sh264e_status_t sh264e_encode_idr(sh264e_encoder_t *encoder,
                                  const sh264e_frame_t *frame,
                                  uint8_t *out,
                                  size_t out_capacity,
                                  size_t *out_size);

sh264e_status_t sh264e_resize_get_slice_buffer_size(const sh264e_frame_t *src_frame,
                                                    size_t *out_size);

sh264e_status_t sh264e_resize_make_slice(const sh264e_frame_t *src_frame,
                                         unsigned slice_index,
                                         uint8_t *work_buffer,
                                         size_t work_buffer_capacity,
                                         sh264e_slice_t *out_slice);

sh264e_status_t sh264e_jpeg_get_slice_buffer_size(size_t *out_size);

sh264e_status_t sh264e_jpeg_get_slice_work_size(const uint8_t *jpeg_data,
                                                size_t jpeg_size,
                                                sh264e_pixfmt_t pixfmt,
                                                size_t *out_size);

sh264e_status_t sh264e_jpeg_get_work_size(const uint8_t *jpeg_data,
                                          size_t jpeg_size,
                                          size_t *out_size);

sh264e_status_t sh264e_jpeg_get_last_allocation_stats(sh264e_jpeg_allocation_stats_t *out_stats);

size_t sh264e_jpeg_get_last_streaming_cache_bytes(void);

size_t sh264e_jpeg_get_last_slice_work_bytes(void);

sh264e_status_t sh264e_encode_jpeg_idr(sh264e_encoder_t *encoder,
                                       const uint8_t *jpeg_data,
                                       size_t jpeg_size,
                                       uint8_t *work_buffer,
                                       size_t work_buffer_capacity,
                                       uint8_t *out,
                                       size_t out_capacity,
                                       size_t *out_size);

sh264e_status_t sh264e_encode_jpeg_idr_with_arena(sh264e_encoder_t *encoder,
                                                  const uint8_t *jpeg_data,
                                                  size_t jpeg_size,
                                                  uint8_t *jpeg_arena,
                                                  size_t jpeg_arena_size,
                                                  uint8_t *work_buffer,
                                                  size_t work_buffer_capacity,
                                                  uint8_t *out,
                                                  size_t out_capacity,
                                                  size_t *out_size);

sh264e_status_t sh264e_encode_jpeg_idr_with_arena_stream(
    sh264e_encoder_t *encoder,
    const uint8_t *jpeg_data,
    size_t jpeg_size,
    uint8_t *jpeg_arena,
    size_t jpeg_arena_size,
    uint8_t *work_buffer,
    size_t work_buffer_capacity,
    uint8_t *out_buffer,
    size_t out_buffer_capacity,
    sh264e_output_consumer_t consumer,
    void *consumer_user);

const char *sh264e_status_string(sh264e_status_t status);

#ifdef __cplusplus
}
#endif

#endif
