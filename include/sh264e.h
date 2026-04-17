#ifndef SH264E_H
#define SH264E_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH264E_V1_WIDTH 2560u
#define SH264E_V1_HEIGHT 1440u
#define SH264E_DEFAULT_QP 28

typedef enum sh264e_status_t {
    SH264E_OK = 0,
    SH264E_ERR_INVALID_ARGUMENT = -1,
    SH264E_ERR_UNSUPPORTED_CONFIG = -2,
    SH264E_ERR_BUFFER_TOO_SMALL = -3,
    SH264E_ERR_ALLOCATION_FAILED = -4,
    SH264E_ERR_INTERNAL = -5
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

typedef struct sh264e_encoder_t sh264e_encoder_t;

sh264e_status_t sh264e_encoder_create(const sh264e_config_t *config,
                                      sh264e_encoder_t **out_encoder);

void sh264e_encoder_destroy(sh264e_encoder_t *encoder);

sh264e_status_t sh264e_get_max_output_size(const sh264e_config_t *config,
                                           size_t *out_size);

sh264e_status_t sh264e_encode_idr(sh264e_encoder_t *encoder,
                                  const sh264e_frame_t *frame,
                                  uint8_t *out,
                                  size_t out_capacity,
                                  size_t *out_size);

const char *sh264e_status_string(sh264e_status_t status);

#ifdef __cplusplus
}
#endif

#endif
