#include "sh264e.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define NJ_OK 0
#define NJ_NO_JPEG 1
#define NJ_UNSUPPORTED 2
#define NJ_OUT_OF_MEM 3
#define NJ_INTERNAL_ERR 4
#define NJ_SYNTAX_ERROR 5

#define SH264E_MB_SIZE 16u
#define SH264E_LUMA_4X4 4u
#define SH264E_MBS_X (SH264E_V1_WIDTH / SH264E_MB_SIZE)
#define SH264E_MBS_Y (SH264E_V1_HEIGHT / SH264E_MB_SIZE)
#define SH264E_LUMA4_X (SH264E_V1_WIDTH / SH264E_LUMA_4X4)
#define SH264E_SLICE_LUMA4_Y (SH264E_V1_SLICE_LUMA_HEIGHT / SH264E_LUMA_4X4)
#define SH264E_CHROMA_WIDTH (SH264E_V1_WIDTH / 2u)
#define SH264E_MAX_QP 51
#define SH264E_SCALE_FP_BITS 16u
#define SH264E_SCALE_FP_ONE (1u << SH264E_SCALE_FP_BITS)
#define SH264E_SCALE_FP_HALF (SH264E_SCALE_FP_ONE >> 1u)
#define SH264E_SCALE_FP_BLEND_ROUND ((uint64_t)1u << ((SH264E_SCALE_FP_BITS * 2u) - 1u))

#if !defined(SH264E_DISABLE_ARM_DSP) && defined(__ARM_FEATURE_DSP) && defined(__GNUC__)
#define SH264E_USE_ARM_DSP 1
#endif

typedef struct sh264e_bit_writer_t {
    uint8_t *data;
    size_t capacity;
    size_t byte_pos;
    unsigned bit_pos;
    int error;
} sh264e_bit_writer_t;

struct sh264e_encoder_t {
    sh264e_config_t config;
    uint8_t *rbsp;
    size_t rbsp_capacity;
    uint8_t *recon_y;
    uint8_t *recon_u;
    uint8_t *recon_v;
    uint8_t *nz_luma;
    unsigned idr_active;
    unsigned slices_encoded;
};

typedef struct sh264e_scale_coord_t {
    uint32_t index;
    uint32_t fraction;
} sh264e_scale_coord_t;

typedef struct sh264e_axis_mapper_t {
    int64_t pos;
    int64_t step;
    uint64_t rem;
    uint64_t rem_step;
    uint64_t denom;
} sh264e_axis_mapper_t;

typedef struct sh264e_jpeg_component_t {
    const uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    ptrdiff_t stride;
} sh264e_jpeg_component_t;

void njInit(void);
int njDecodeComponents(const void *jpeg, const int size);
int njGetWidth(void);
int njGetHeight(void);
int njGetComponentCount(void);
const unsigned char *njGetComponentPixels(int index);
int njGetComponentWidth(int index);
int njGetComponentHeight(int index);
int njGetComponentStride(int index);
void njDone(void);

static void reset_progressive_state(sh264e_encoder_t *encoder);

static const uint8_t k_luma4x4_x[16] = {
    0, 4, 0, 4, 8, 12, 8, 12, 0, 4, 0, 4, 8, 12, 8, 12
};

static const uint8_t k_luma4x4_y[16] = {
    0, 0, 4, 4, 0, 0, 4, 4, 8, 8, 12, 12, 8, 8, 12, 12
};

static const uint8_t k_cbp_intra_code_num[48] = {
    3, 29, 30, 17, 31, 18, 37, 8,
    32, 38, 19, 9, 20, 10, 11, 2,
    16, 33, 34, 21, 35, 22, 39, 4,
    36, 40, 23, 5, 24, 6, 7, 1,
    41, 42, 43, 25, 44, 26, 46, 12,
    45, 47, 27, 13, 28, 14, 15, 0
};

static const int k_dequant_dc_scale[6] = {10, 11, 13, 14, 16, 18};

static sh264e_status_t validate_config(const sh264e_config_t *config)
{
    if (config == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (config->width != SH264E_V1_WIDTH || config->height != SH264E_V1_HEIGHT) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    if (config->pixfmt != SH264E_PIXFMT_I420 && config->pixfmt != SH264E_PIXFMT_NV12) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    if (config->qp < 0 || config->qp > SH264E_MAX_QP) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    return SH264E_OK;
}

static sh264e_status_t validate_frame(const sh264e_config_t *config, const sh264e_frame_t *frame)
{
    if (config == NULL || frame == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (frame->width != config->width || frame->height != config->height ||
        frame->pixfmt != config->pixfmt) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    if (frame->plane[0] == NULL || frame->stride[0] < (ptrdiff_t)config->width) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (frame->pixfmt == SH264E_PIXFMT_I420) {
        if (frame->plane[1] == NULL || frame->plane[2] == NULL ||
            frame->stride[1] < (ptrdiff_t)(config->width / 2u) ||
            frame->stride[2] < (ptrdiff_t)(config->width / 2u)) {
            return SH264E_ERR_INVALID_ARGUMENT;
        }
    } else {
        if (frame->plane[1] == NULL ||
            frame->stride[1] < (ptrdiff_t)config->width) {
            return SH264E_ERR_INVALID_ARGUMENT;
        }
    }
    return SH264E_OK;
}

static sh264e_status_t validate_slice(const sh264e_config_t *config, const sh264e_slice_t *slice)
{
    if (config == NULL || slice == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (slice->pixfmt != config->pixfmt) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    if (slice->plane[0] == NULL || slice->stride[0] < (ptrdiff_t)config->width) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (slice->pixfmt == SH264E_PIXFMT_I420) {
        if (slice->plane[1] == NULL || slice->plane[2] == NULL ||
            slice->stride[1] < (ptrdiff_t)(config->width / 2u) ||
            slice->stride[2] < (ptrdiff_t)(config->width / 2u)) {
            return SH264E_ERR_INVALID_ARGUMENT;
        }
    } else {
        if (slice->plane[1] == NULL || slice->stride[1] < (ptrdiff_t)config->width) {
            return SH264E_ERR_INVALID_ARGUMENT;
        }
    }
    return SH264E_OK;
}

static sh264e_status_t validate_resize_source_frame(const sh264e_frame_t *frame)
{
    if (frame == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (frame->width < SH264E_RESIZE_MIN_SRC_WIDTH ||
        frame->width > SH264E_RESIZE_MAX_SRC_WIDTH ||
        frame->height < SH264E_RESIZE_MIN_SRC_HEIGHT ||
        frame->height > SH264E_RESIZE_MAX_SRC_HEIGHT ||
        (frame->width & 1u) != 0u ||
        (frame->height & 1u) != 0u) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    if (frame->pixfmt != SH264E_PIXFMT_I420 && frame->pixfmt != SH264E_PIXFMT_NV12) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    if (frame->plane[0] == NULL || frame->stride[0] < (ptrdiff_t)frame->width) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (frame->pixfmt == SH264E_PIXFMT_I420) {
        if (frame->plane[1] == NULL || frame->plane[2] == NULL ||
            frame->stride[1] < (ptrdiff_t)(frame->width / 2u) ||
            frame->stride[2] < (ptrdiff_t)(frame->width / 2u)) {
            return SH264E_ERR_INVALID_ARGUMENT;
        }
    } else {
        if (frame->plane[1] == NULL || frame->stride[1] < (ptrdiff_t)frame->width) {
            return SH264E_ERR_INVALID_ARGUMENT;
        }
    }
    return SH264E_OK;
}

static int resize_is_bypass(const sh264e_frame_t *frame)
{
    return frame->width == SH264E_V1_WIDTH && frame->height == SH264E_V1_HEIGHT;
}

static size_t resize_scaled_slice_buffer_size(void)
{
    return (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT +
           (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT;
}

static sh264e_axis_mapper_t scale_axis_mapper_init(uint32_t src_size,
                                                   uint32_t dst_size,
                                                   uint32_t dst_start)
{
    const uint64_t denom = (uint64_t)dst_size * 2u;
    const uint64_t base_num = (uint64_t)(2u * dst_start + 1u) *
                              (uint64_t)src_size *
                              (uint64_t)SH264E_SCALE_FP_ONE;
    const uint64_t step_num = (uint64_t)src_size *
                              (uint64_t)SH264E_SCALE_FP_ONE *
                              2u;
    sh264e_axis_mapper_t mapper;

    mapper.pos = (int64_t)(base_num / denom) - (int64_t)SH264E_SCALE_FP_HALF;
    mapper.step = (int64_t)(step_num / denom);
    mapper.rem = base_num % denom;
    mapper.rem_step = step_num % denom;
    mapper.denom = denom;
    return mapper;
}

static void scale_axis_mapper_advance(sh264e_axis_mapper_t *mapper)
{
    mapper->pos += mapper->step;
    mapper->rem += mapper->rem_step;
    if (mapper->rem >= mapper->denom) {
        mapper->rem -= mapper->denom;
        mapper->pos++;
    }
}

static sh264e_scale_coord_t scale_coord_from_raw(int64_t raw_pos, uint32_t src_size)
{
    const uint64_t max_pos = (uint64_t)(src_size - 1u) * (uint64_t)SH264E_SCALE_FP_ONE;
    sh264e_scale_coord_t coord;

    if (raw_pos <= 0) {
        coord.index = 0;
        coord.fraction = 0;
        return coord;
    }
    if ((uint64_t)raw_pos >= max_pos) {
        coord.index = src_size - 1u;
        coord.fraction = 0;
        return coord;
    }
    coord.index = (uint32_t)((uint64_t)raw_pos >> SH264E_SCALE_FP_BITS);
    coord.fraction = (uint32_t)((uint64_t)raw_pos & (uint64_t)(SH264E_SCALE_FP_ONE - 1u));
    return coord;
}

#if defined(SH264E_USE_ARM_DSP)
static int32_t arm_smlad(uint32_t a, uint32_t b, int32_t acc)
{
    int32_t result;
    __asm volatile("smlad %0, %1, %2, %3"
                   : "=r"(result)
                   : "r"(a), "r"(b), "r"(acc));
    return result;
}

static uint32_t arm_dsp_mul_pair_u8_q16(uint8_t p0, uint32_t w0, uint8_t p1, uint32_t w1)
{
    const uint32_t packed_pixels = (uint32_t)p0 | ((uint32_t)p1 << 16u);
    const uint32_t packed_weights = (w0 & 0xffffu) | ((w1 & 0xffffu) << 16u);
    int32_t sum = arm_smlad(packed_pixels, packed_weights, 0);

    if (w0 >= 0x8000u) {
        sum += (int32_t)((uint32_t)p0 << 16u);
    }
    if (w1 >= 0x8000u) {
        sum += (int32_t)((uint32_t)p1 << 16u);
    }
    return (uint32_t)sum;
}
#endif

static uint8_t bilinear_blend_u8(uint8_t p00,
                                 uint8_t p01,
                                 uint8_t p10,
                                 uint8_t p11,
                                 uint32_t wx,
                                 uint32_t wy)
{
    const uint32_t inv_wx = SH264E_SCALE_FP_ONE - wx;
    const uint32_t inv_wy = SH264E_SCALE_FP_ONE - wy;
#if defined(SH264E_USE_ARM_DSP)
    const uint64_t top = arm_dsp_mul_pair_u8_q16(p00, inv_wx, p01, wx);
    const uint64_t bottom = arm_dsp_mul_pair_u8_q16(p10, inv_wx, p11, wx);
#else
    const uint64_t top = (uint64_t)p00 * inv_wx + (uint64_t)p01 * wx;
    const uint64_t bottom = (uint64_t)p10 * inv_wx + (uint64_t)p11 * wx;
#endif
    const uint64_t blended = top * inv_wy + bottom * wy;

    return (uint8_t)((blended + SH264E_SCALE_FP_BLEND_ROUND) >> (SH264E_SCALE_FP_BITS * 2u));
}

static uint8_t bilinear_sample_plane_mapped(const uint8_t *src,
                                            uint32_t src_width,
                                            uint32_t src_height,
                                            ptrdiff_t src_stride,
                                            sh264e_scale_coord_t sx,
                                            sh264e_scale_coord_t sy)
{
    const uint32_t x0 = sx.index;
    const uint32_t y0 = sy.index;
    const uint32_t x1 = x0 + 1u < src_width ? x0 + 1u : x0;
    const uint32_t y1 = y0 + 1u < src_height ? y0 + 1u : y0;
    const uint8_t *row0 = src + (size_t)y0 * (size_t)src_stride;
    const uint8_t *row1 = src + (size_t)y1 * (size_t)src_stride;

    return bilinear_blend_u8(row0[x0], row0[x1], row1[x0], row1[x1], sx.fraction, sy.fraction);
}

static uint8_t bilinear_sample_nv12_chroma_mapped(const uint8_t *src,
                                                  uint32_t src_width,
                                                  uint32_t src_height,
                                                  ptrdiff_t src_stride,
                                                  unsigned component,
                                                  sh264e_scale_coord_t sx,
                                                  sh264e_scale_coord_t sy)
{
    const uint32_t x0 = sx.index;
    const uint32_t y0 = sy.index;
    const uint32_t x1 = x0 + 1u < src_width ? x0 + 1u : x0;
    const uint32_t y1 = y0 + 1u < src_height ? y0 + 1u : y0;
    const uint8_t *row0 = src + (size_t)y0 * (size_t)src_stride;
    const uint8_t *row1 = src + (size_t)y1 * (size_t)src_stride;
    const size_t c = component;

    return bilinear_blend_u8(row0[(size_t)x0 * 2u + c],
                             row0[(size_t)x1 * 2u + c],
                             row1[(size_t)x0 * 2u + c],
                             row1[(size_t)x1 * 2u + c],
                             sx.fraction,
                             sy.fraction);
}

static void jpeg_scale_component_slice(const sh264e_jpeg_component_t *src,
                                       uint8_t *dst,
                                       uint32_t dst_width,
                                       uint32_t dst_height,
                                       uint32_t dst_y_start,
                                       uint32_t dst_rows,
                                       ptrdiff_t dst_stride)
{
    sh264e_axis_mapper_t y_mapper = scale_axis_mapper_init(src->height, dst_height, dst_y_start);
    uint32_t y;

    for (y = 0; y < dst_rows; y++) {
        uint8_t *dst_row = dst + (size_t)y * (size_t)dst_stride;
        const sh264e_scale_coord_t sy = scale_coord_from_raw(y_mapper.pos, src->height);
        sh264e_axis_mapper_t x_mapper = scale_axis_mapper_init(src->width, dst_width, 0u);
        uint32_t x;

        for (x = 0; x < dst_width; x++) {
            const sh264e_scale_coord_t sx = scale_coord_from_raw(x_mapper.pos, src->width);
            dst_row[x] = bilinear_sample_plane_mapped(src->pixels, src->width, src->height,
                                                      src->stride, sx, sy);
            scale_axis_mapper_advance(&x_mapper);
        }
        scale_axis_mapper_advance(&y_mapper);
    }
}

static void jpeg_fill_i420_neutral_chroma(uint8_t *dst_u, uint8_t *dst_v)
{
    memset(dst_u, 128, (size_t)SH264E_CHROMA_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT);
    memset(dst_v, 128, (size_t)SH264E_CHROMA_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT);
}

static void jpeg_fill_nv12_neutral_chroma(uint8_t *dst_uv)
{
    uint32_t y;

    for (y = 0; y < SH264E_V1_SLICE_CHROMA_HEIGHT; y++) {
        uint32_t x;
        uint8_t *row = dst_uv + (size_t)y * SH264E_V1_WIDTH;
        for (x = 0; x < SH264E_CHROMA_WIDTH; x++) {
            row[(size_t)x * 2u] = 128u;
            row[(size_t)x * 2u + 1u] = 128u;
        }
    }
}

static void jpeg_scale_nv12_component_chroma_slice(const sh264e_jpeg_component_t *cb,
                                                   const sh264e_jpeg_component_t *cr,
                                                   uint8_t *dst_uv,
                                                   uint32_t dst_y_start)
{
    sh264e_axis_mapper_t y_mapper = scale_axis_mapper_init(cb->height,
                                                           SH264E_V1_HEIGHT / 2u,
                                                           dst_y_start);
    uint32_t y;

    for (y = 0; y < SH264E_V1_SLICE_CHROMA_HEIGHT; y++) {
        uint8_t *row = dst_uv + (size_t)y * SH264E_V1_WIDTH;
        const sh264e_scale_coord_t sy = scale_coord_from_raw(y_mapper.pos, cb->height);
        sh264e_axis_mapper_t x_mapper = scale_axis_mapper_init(cb->width, SH264E_CHROMA_WIDTH, 0u);
        uint32_t x;

        for (x = 0; x < SH264E_CHROMA_WIDTH; x++) {
            const sh264e_scale_coord_t sx = scale_coord_from_raw(x_mapper.pos, cb->width);
            row[(size_t)x * 2u] = bilinear_sample_plane_mapped(cb->pixels, cb->width, cb->height,
                                                               cb->stride, sx, sy);
            row[(size_t)x * 2u + 1u] = bilinear_sample_plane_mapped(cr->pixels, cr->width, cr->height,
                                                                    cr->stride, sx, sy);
            scale_axis_mapper_advance(&x_mapper);
        }
        scale_axis_mapper_advance(&y_mapper);
    }
}

static void jpeg_make_i420_slice(const sh264e_jpeg_component_t *components,
                                 int component_count,
                                 unsigned slice_index,
                                 uint8_t *work_buffer,
                                 sh264e_slice_t *slice)
{
    uint8_t *dst_y = work_buffer;
    uint8_t *dst_u = dst_y + (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT;
    uint8_t *dst_v = dst_u + (size_t)SH264E_CHROMA_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT;

    jpeg_scale_component_slice(&components[0], dst_y, SH264E_V1_WIDTH, SH264E_V1_HEIGHT,
                               slice_index * SH264E_V1_SLICE_LUMA_HEIGHT,
                               SH264E_V1_SLICE_LUMA_HEIGHT, SH264E_V1_WIDTH);
    if (component_count == 1) {
        jpeg_fill_i420_neutral_chroma(dst_u, dst_v);
    } else {
        jpeg_scale_component_slice(&components[1], dst_u, SH264E_CHROMA_WIDTH, SH264E_V1_HEIGHT / 2u,
                                   slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT,
                                   SH264E_V1_SLICE_CHROMA_HEIGHT, SH264E_CHROMA_WIDTH);
        jpeg_scale_component_slice(&components[2], dst_v, SH264E_CHROMA_WIDTH, SH264E_V1_HEIGHT / 2u,
                                   slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT,
                                   SH264E_V1_SLICE_CHROMA_HEIGHT, SH264E_CHROMA_WIDTH);
    }

    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = SH264E_PIXFMT_I420;
    slice->plane[0] = dst_y;
    slice->plane[1] = dst_u;
    slice->plane[2] = dst_v;
    slice->stride[0] = SH264E_V1_WIDTH;
    slice->stride[1] = SH264E_CHROMA_WIDTH;
    slice->stride[2] = SH264E_CHROMA_WIDTH;
}

static void jpeg_make_nv12_slice(const sh264e_jpeg_component_t *components,
                                 int component_count,
                                 unsigned slice_index,
                                 uint8_t *work_buffer,
                                 sh264e_slice_t *slice)
{
    uint8_t *dst_y = work_buffer;
    uint8_t *dst_uv = dst_y + (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT;

    jpeg_scale_component_slice(&components[0], dst_y, SH264E_V1_WIDTH, SH264E_V1_HEIGHT,
                               slice_index * SH264E_V1_SLICE_LUMA_HEIGHT,
                               SH264E_V1_SLICE_LUMA_HEIGHT, SH264E_V1_WIDTH);
    if (component_count == 1) {
        jpeg_fill_nv12_neutral_chroma(dst_uv);
    } else {
        jpeg_scale_nv12_component_chroma_slice(&components[1], &components[2], dst_uv,
                                               slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT);
    }

    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = SH264E_PIXFMT_NV12;
    slice->plane[0] = dst_y;
    slice->plane[1] = dst_uv;
    slice->stride[0] = SH264E_V1_WIDTH;
    slice->stride[1] = SH264E_V1_WIDTH;
}

static void resize_scale_plane_slice(const uint8_t *src,
                                     uint32_t src_width,
                                     uint32_t src_height,
                                     ptrdiff_t src_stride,
                                     uint8_t *dst,
                                     uint32_t dst_width,
                                     uint32_t dst_height,
                                     uint32_t dst_y_start,
                                     uint32_t dst_rows,
                                     ptrdiff_t dst_stride)
{
    sh264e_axis_mapper_t y_mapper = scale_axis_mapper_init(src_height, dst_height, dst_y_start);
    uint32_t y;

    for (y = 0; y < dst_rows; y++) {
        uint8_t *dst_row = dst + (size_t)y * (size_t)dst_stride;
        const sh264e_scale_coord_t sy = scale_coord_from_raw(y_mapper.pos, src_height);
        sh264e_axis_mapper_t x_mapper = scale_axis_mapper_init(src_width, dst_width, 0u);
        uint32_t x;

        for (x = 0; x < dst_width; x++) {
            const sh264e_scale_coord_t sx = scale_coord_from_raw(x_mapper.pos, src_width);
            dst_row[x] = bilinear_sample_plane_mapped(src, src_width, src_height, src_stride, sx, sy);
            scale_axis_mapper_advance(&x_mapper);
        }
        scale_axis_mapper_advance(&y_mapper);
    }
}

static void resize_scale_nv12_chroma_slice(const uint8_t *src_uv,
                                           uint32_t src_chroma_width,
                                           uint32_t src_chroma_height,
                                           ptrdiff_t src_stride,
                                           uint8_t *dst_uv,
                                           uint32_t dst_y_start)
{
    sh264e_axis_mapper_t y_mapper = scale_axis_mapper_init(src_chroma_height,
                                                           SH264E_V1_HEIGHT / 2u,
                                                           dst_y_start);
    uint32_t y;

    for (y = 0; y < SH264E_V1_SLICE_CHROMA_HEIGHT; y++) {
        uint8_t *dst_row = dst_uv + (size_t)y * SH264E_V1_WIDTH;
        const sh264e_scale_coord_t sy = scale_coord_from_raw(y_mapper.pos, src_chroma_height);
        sh264e_axis_mapper_t x_mapper = scale_axis_mapper_init(src_chroma_width, SH264E_CHROMA_WIDTH, 0u);
        uint32_t x;

        for (x = 0; x < SH264E_CHROMA_WIDTH; x++) {
            const sh264e_scale_coord_t sx = scale_coord_from_raw(x_mapper.pos, src_chroma_width);
            dst_row[(size_t)x * 2u] = bilinear_sample_nv12_chroma_mapped(src_uv,
                                                                         src_chroma_width,
                                                                         src_chroma_height,
                                                                         src_stride,
                                                                         0u,
                                                                         sx,
                                                                         sy);
            dst_row[(size_t)x * 2u + 1u] = bilinear_sample_nv12_chroma_mapped(src_uv,
                                                                              src_chroma_width,
                                                                              src_chroma_height,
                                                                              src_stride,
                                                                              1u,
                                                                              sx,
                                                                              sy);
            scale_axis_mapper_advance(&x_mapper);
        }
        scale_axis_mapper_advance(&y_mapper);
    }
}

static void resize_make_bypass_slice(const sh264e_frame_t *frame, unsigned slice_index, sh264e_slice_t *slice)
{
    const size_t y_offset = (size_t)slice_index * SH264E_V1_SLICE_LUMA_HEIGHT * (size_t)frame->stride[0];
    const size_t c_offset = (size_t)slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT;

    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = frame->pixfmt;
    slice->plane[0] = frame->plane[0] + y_offset;
    slice->stride[0] = frame->stride[0];
    if (frame->pixfmt == SH264E_PIXFMT_I420) {
        slice->plane[1] = frame->plane[1] + c_offset * (size_t)frame->stride[1];
        slice->plane[2] = frame->plane[2] + c_offset * (size_t)frame->stride[2];
        slice->stride[1] = frame->stride[1];
        slice->stride[2] = frame->stride[2];
    } else {
        slice->plane[1] = frame->plane[1] + c_offset * (size_t)frame->stride[1];
        slice->stride[1] = frame->stride[1];
    }
}

static void resize_make_i420_slice(const sh264e_frame_t *frame,
                                   unsigned slice_index,
                                   uint8_t *work_buffer,
                                   sh264e_slice_t *slice)
{
    uint8_t *dst_y = work_buffer;
    uint8_t *dst_u = dst_y + (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT;
    uint8_t *dst_v = dst_u + (size_t)SH264E_CHROMA_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT;

    resize_scale_plane_slice(frame->plane[0], frame->width, frame->height, frame->stride[0],
                             dst_y, SH264E_V1_WIDTH, SH264E_V1_HEIGHT,
                             slice_index * SH264E_V1_SLICE_LUMA_HEIGHT,
                             SH264E_V1_SLICE_LUMA_HEIGHT, SH264E_V1_WIDTH);
    resize_scale_plane_slice(frame->plane[1], frame->width / 2u, frame->height / 2u, frame->stride[1],
                             dst_u, SH264E_CHROMA_WIDTH, SH264E_V1_HEIGHT / 2u,
                             slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT,
                             SH264E_V1_SLICE_CHROMA_HEIGHT, SH264E_CHROMA_WIDTH);
    resize_scale_plane_slice(frame->plane[2], frame->width / 2u, frame->height / 2u, frame->stride[2],
                             dst_v, SH264E_CHROMA_WIDTH, SH264E_V1_HEIGHT / 2u,
                             slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT,
                             SH264E_V1_SLICE_CHROMA_HEIGHT, SH264E_CHROMA_WIDTH);

    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = SH264E_PIXFMT_I420;
    slice->plane[0] = dst_y;
    slice->plane[1] = dst_u;
    slice->plane[2] = dst_v;
    slice->stride[0] = SH264E_V1_WIDTH;
    slice->stride[1] = SH264E_CHROMA_WIDTH;
    slice->stride[2] = SH264E_CHROMA_WIDTH;
}

static void resize_make_nv12_slice(const sh264e_frame_t *frame,
                                   unsigned slice_index,
                                   uint8_t *work_buffer,
                                   sh264e_slice_t *slice)
{
    uint8_t *dst_y = work_buffer;
    uint8_t *dst_uv = dst_y + (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT;

    resize_scale_plane_slice(frame->plane[0], frame->width, frame->height, frame->stride[0],
                             dst_y, SH264E_V1_WIDTH, SH264E_V1_HEIGHT,
                             slice_index * SH264E_V1_SLICE_LUMA_HEIGHT,
                             SH264E_V1_SLICE_LUMA_HEIGHT, SH264E_V1_WIDTH);
    resize_scale_nv12_chroma_slice(frame->plane[1],
                                   frame->width / 2u,
                                   frame->height / 2u,
                                   frame->stride[1],
                                   dst_uv,
                                   slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT);

    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = SH264E_PIXFMT_NV12;
    slice->plane[0] = dst_y;
    slice->plane[1] = dst_uv;
    slice->stride[0] = SH264E_V1_WIDTH;
    slice->stride[1] = SH264E_V1_WIDTH;
}

static void bw_init(sh264e_bit_writer_t *bw, uint8_t *data, size_t capacity)
{
    bw->data = data;
    bw->capacity = capacity;
    bw->byte_pos = 0;
    bw->bit_pos = 0;
    bw->error = 0;
}

static size_t bw_size(const sh264e_bit_writer_t *bw)
{
    return bw->byte_pos + (bw->bit_pos != 0u ? 1u : 0u);
}

static void bw_write_bit(sh264e_bit_writer_t *bw, unsigned bit)
{
    if (bw->error != 0) {
        return;
    }
    if (bw->byte_pos >= bw->capacity) {
        bw->error = 1;
        return;
    }
    if (bw->bit_pos == 0u) {
        bw->data[bw->byte_pos] = 0;
    }
    if ((bit & 1u) != 0u) {
        bw->data[bw->byte_pos] |= (uint8_t)(1u << (7u - bw->bit_pos));
    }
    bw->bit_pos++;
    if (bw->bit_pos == 8u) {
        bw->bit_pos = 0;
        bw->byte_pos++;
    }
}

static void bw_write_bits(sh264e_bit_writer_t *bw, uint32_t bits, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; i++) {
        const unsigned shift = count - 1u - i;
        bw_write_bit(bw, (bits >> shift) & 1u);
    }
}

static void bw_write_ue(sh264e_bit_writer_t *bw, uint32_t value)
{
    uint32_t code_num = value + 1u;
    unsigned leading_zero_bits = 0;
    uint32_t tmp = code_num;
    while (tmp > 1u) {
        tmp >>= 1u;
        leading_zero_bits++;
    }
    while (leading_zero_bits > 0u) {
        bw_write_bit(bw, 0);
        leading_zero_bits--;
    }
    tmp = code_num;
    leading_zero_bits = 0;
    while (tmp > 1u) {
        tmp >>= 1u;
        leading_zero_bits++;
    }
    bw_write_bits(bw, code_num, leading_zero_bits + 1u);
}

static void bw_write_se(sh264e_bit_writer_t *bw, int32_t value)
{
    uint32_t code_num;
    if (value <= 0) {
        code_num = (uint32_t)(-value) * 2u;
    } else {
        code_num = ((uint32_t)value * 2u) - 1u;
    }
    bw_write_ue(bw, code_num);
}

static void bw_rbsp_trailing_bits(sh264e_bit_writer_t *bw)
{
    bw_write_bit(bw, 1);
    while (bw->bit_pos != 0u) {
        bw_write_bit(bw, 0);
    }
}

static int append_byte(uint8_t *out, size_t capacity, size_t *offset, uint8_t value)
{
    if (*offset >= capacity) {
        return 0;
    }
    out[*offset] = value;
    *offset += 1u;
    return 1;
}

static sh264e_status_t append_annexb_nalu(uint8_t *out,
                                          size_t capacity,
                                          size_t *offset,
                                          uint8_t nal_header,
                                          const uint8_t *rbsp,
                                          size_t rbsp_size)
{
    size_t i;
    unsigned zero_count = 0;

    if (!append_byte(out, capacity, offset, 0x00u) ||
        !append_byte(out, capacity, offset, 0x00u) ||
        !append_byte(out, capacity, offset, 0x01u) ||
        !append_byte(out, capacity, offset, nal_header)) {
        return SH264E_ERR_BUFFER_TOO_SMALL;
    }

    for (i = 0; i < rbsp_size; i++) {
        const uint8_t b = rbsp[i];
        if (zero_count >= 2u && b <= 0x03u) {
            if (!append_byte(out, capacity, offset, 0x03u)) {
                return SH264E_ERR_BUFFER_TOO_SMALL;
            }
            zero_count = 0;
        }
        if (!append_byte(out, capacity, offset, b)) {
            return SH264E_ERR_BUFFER_TOO_SMALL;
        }
        if (b == 0x00u) {
            zero_count++;
        } else {
            zero_count = 0;
        }
    }

    return SH264E_OK;
}

static sh264e_status_t make_sps(sh264e_encoder_t *encoder,
                                uint8_t *out,
                                size_t capacity,
                                size_t *offset)
{
    sh264e_bit_writer_t bw;
    bw_init(&bw, encoder->rbsp, encoder->rbsp_capacity);

    bw_write_bits(&bw, 66u, 8);      /* profile_idc: Baseline */
    bw_write_bits(&bw, 0x40u, 8);    /* constraint_set0_flag */
    bw_write_bits(&bw, 50u, 8);      /* level_idc: 5.0 */
    bw_write_ue(&bw, 0);             /* seq_parameter_set_id */
    bw_write_ue(&bw, 0);             /* log2_max_frame_num_minus4 */
    bw_write_ue(&bw, 0);             /* pic_order_cnt_type */
    bw_write_ue(&bw, 0);             /* log2_max_pic_order_cnt_lsb_minus4 */
    bw_write_ue(&bw, 1);             /* max_num_ref_frames */
    bw_write_bit(&bw, 0);            /* gaps_in_frame_num_value_allowed_flag */
    bw_write_ue(&bw, SH264E_MBS_X - 1u);
    bw_write_ue(&bw, SH264E_MBS_Y - 1u);
    bw_write_bit(&bw, 1);            /* frame_mbs_only_flag */
    bw_write_bit(&bw, 1);            /* direct_8x8_inference_flag */
    bw_write_bit(&bw, 0);            /* frame_cropping_flag */
    bw_write_bit(&bw, 0);            /* vui_parameters_present_flag */
    bw_rbsp_trailing_bits(&bw);

    if (bw.error != 0) {
        return SH264E_ERR_INTERNAL;
    }
    return append_annexb_nalu(out, capacity, offset, 0x67u, encoder->rbsp, bw_size(&bw));
}

static sh264e_status_t make_pps(sh264e_encoder_t *encoder,
                                uint8_t *out,
                                size_t capacity,
                                size_t *offset)
{
    sh264e_bit_writer_t bw;
    bw_init(&bw, encoder->rbsp, encoder->rbsp_capacity);

    bw_write_ue(&bw, 0);                         /* pic_parameter_set_id */
    bw_write_ue(&bw, 0);                         /* seq_parameter_set_id */
    bw_write_bit(&bw, 0);                        /* entropy_coding_mode_flag: CAVLC */
    bw_write_bit(&bw, 0);                        /* bottom_field_pic_order_in_frame_present_flag */
    bw_write_ue(&bw, 0);                         /* num_slice_groups_minus1 */
    bw_write_ue(&bw, 0);                         /* num_ref_idx_l0_default_active_minus1 */
    bw_write_ue(&bw, 0);                         /* num_ref_idx_l1_default_active_minus1 */
    bw_write_bit(&bw, 0);                        /* weighted_pred_flag */
    bw_write_bits(&bw, 0, 2);                    /* weighted_bipred_idc */
    bw_write_se(&bw, encoder->config.qp - 26);   /* pic_init_qp_minus26 */
    bw_write_se(&bw, 0);                         /* pic_init_qs_minus26 */
    bw_write_se(&bw, 0);                         /* chroma_qp_index_offset */
    bw_write_bit(&bw, 1);                        /* deblocking_filter_control_present_flag */
    bw_write_bit(&bw, 0);                        /* constrained_intra_pred_flag */
    bw_write_bit(&bw, 0);                        /* redundant_pic_cnt_present_flag */
    bw_rbsp_trailing_bits(&bw);

    if (bw.error != 0) {
        return SH264E_ERR_INTERNAL;
    }
    return append_annexb_nalu(out, capacity, offset, 0x68u, encoder->rbsp, bw_size(&bw));
}

static int clip_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return value;
}

static int dequant_effective_scale(int qp)
{
    const int rem = qp % 6;
    const int qbits = qp / 6;
    const int scale = k_dequant_dc_scale[rem];

    if (qbits >= 4) {
        return scale << (qbits - 4);
    }
    return (scale + (1 << (3 - qbits))) >> (4 - qbits);
}

static int inverse_dc_residual(int level, int qp)
{
    const int rem = qp % 6;
    const int qbits = qp / 6;
    const int scale = k_dequant_dc_scale[rem];
    int transformed;

    if (level == 0) {
        return 0;
    }
    if (qbits >= 4) {
        transformed = level * scale * (1 << (qbits - 4));
    } else {
        transformed = (level * scale + (1 << (3 - qbits))) >> (4 - qbits);
    }
    return (transformed + 32) >> 6;
}

static int quantize_dc_delta(int delta, int qp)
{
    const int scale = dequant_effective_scale(qp);
    int level;

    if (delta == 0) {
        return 0;
    }
    if (delta > 0) {
        level = (delta * 64 + (scale / 2)) / scale;
    } else {
        level = -(((-delta) * 64 + (scale / 2)) / scale);
    }
    if (level == 1) {
        level = 2;
    } else if (level == -1) {
        level = -2;
    }
    if (level > 2047) {
        level = 2047;
    } else if (level < -2047) {
        level = -2047;
    }
    return level;
}

static int predict_luma_dc(const sh264e_encoder_t *encoder, unsigned x, unsigned y)
{
    unsigned i;
    int sum = 0;
    const int have_top = y > 0u;
    const int have_left = x > 0u;

    if (!have_top && !have_left) {
        return 128;
    }
    if (have_top) {
        const uint8_t *top = encoder->recon_y + ((size_t)y - 1u) * SH264E_V1_WIDTH + x;
        for (i = 0; i < 4u; i++) {
            sum += top[i];
        }
    }
    if (have_left) {
        const uint8_t *left = encoder->recon_y + (size_t)y * SH264E_V1_WIDTH + x - 1u;
        for (i = 0; i < 4u; i++) {
            sum += left[(size_t)i * SH264E_V1_WIDTH];
        }
    }
    if (have_top && have_left) {
        return (sum + 4) >> 3;
    }
    return (sum + 2) >> 2;
}

static const uint8_t *slice_luma_row(const sh264e_slice_t *slice, unsigned y)
{
    return slice->plane[0] + (size_t)y * (size_t)slice->stride[0];
}

static uint8_t slice_chroma_sample(const sh264e_slice_t *slice, unsigned plane, unsigned x, unsigned y)
{
    if (slice->pixfmt == SH264E_PIXFMT_I420) {
        return *(slice->plane[plane] + (size_t)y * (size_t)slice->stride[plane] + x);
    }
    return *(slice->plane[1] + (size_t)y * (size_t)slice->stride[1] + x * 2u + (plane == 1u ? 0u : 1u));
}

static int encode_luma4x4(sh264e_encoder_t *encoder,
                          const sh264e_slice_t *slice,
                          unsigned x,
                          unsigned y)
{
    unsigned row;
    unsigned col;
    int sum_delta = 0;
    const int pred = predict_luma_dc(encoder, x, y);
    int level;
    int recon_delta;

    for (row = 0; row < 4u; row++) {
        const uint8_t *src = slice_luma_row(slice, y + row) + x;
        for (col = 0; col < 4u; col++) {
            sum_delta += (int)src[col] - pred;
        }
    }

    level = quantize_dc_delta((sum_delta + (sum_delta >= 0 ? 8 : -8)) / 16, encoder->config.qp);
    recon_delta = inverse_dc_residual(level, encoder->config.qp);

    for (row = 0; row < 4u; row++) {
        uint8_t *dst = encoder->recon_y + (size_t)(y + row) * SH264E_V1_WIDTH + x;
        for (col = 0; col < 4u; col++) {
            dst[col] = (uint8_t)clip_u8(pred + recon_delta);
        }
    }

    return level;
}

static int predict_chroma_dc(const uint8_t *recon, unsigned x, unsigned y)
{
    unsigned i;
    int sum = 0;
    const int have_top = y > 0u;
    const int have_left = x > 0u;

    if (!have_top && !have_left) {
        return 128;
    }
    if (have_top) {
        const uint8_t *top = recon + ((size_t)y - 1u) * SH264E_CHROMA_WIDTH + x;
        for (i = 0; i < 8u; i++) {
            sum += top[i];
        }
    }
    if (have_left) {
        const uint8_t *left = recon + (size_t)y * SH264E_CHROMA_WIDTH + x - 1u;
        for (i = 0; i < 8u; i++) {
            sum += left[(size_t)i * SH264E_CHROMA_WIDTH];
        }
    }
    if (have_top && have_left) {
        return (sum + 8) >> 4;
    }
    return (sum + 4) >> 3;
}

static int encode_chroma8x8_dc(sh264e_encoder_t *encoder,
                               const sh264e_slice_t *slice,
                               unsigned plane,
                               unsigned x,
                               unsigned y)
{
    unsigned row;
    unsigned col;
    int sum_delta = 0;
    uint8_t *recon = plane == 1u ? encoder->recon_u : encoder->recon_v;
    const int pred = predict_chroma_dc(recon, x, y);
    int level;
    int recon_delta;

    for (row = 0; row < 8u; row++) {
        for (col = 0; col < 8u; col++) {
            sum_delta += (int)slice_chroma_sample(slice, plane, x + col, y + row) - pred;
        }
    }

    level = quantize_dc_delta((sum_delta + (sum_delta >= 0 ? 32 : -32)) / 64, encoder->config.qp);
    if (level > 0) {
        level = 1;
    } else if (level < 0) {
        level = -1;
    }
    recon_delta = inverse_dc_residual(level, encoder->config.qp);

    for (row = 0; row < 8u; row++) {
        uint8_t *dst = recon + (size_t)(y + row) * SH264E_CHROMA_WIDTH + x;
        for (col = 0; col < 8u; col++) {
            dst[col] = (uint8_t)clip_u8(pred + recon_delta);
        }
    }

    return level;
}

static void write_coeff_token_one_or_zero(sh264e_bit_writer_t *bw, int has_coeff)
{
    if (has_coeff == 0) {
        bw_write_bit(bw, 1);             /* TotalCoeff=0 for nC 0..1 */
    } else {
        bw_write_bits(bw, 0x05u, 6);     /* TotalCoeff=1, TrailingOnes=0 for nC 0..1 */
    }
}

static uint32_t cavlc_parsed_level_code(unsigned prefix, unsigned suffix_length, uint32_t suffix)
{
    uint32_t level_code = ((prefix < 15u ? prefix : 15u) << suffix_length) + suffix;
    if (prefix >= 15u && suffix_length == 0u) {
        level_code += 15u;
    }
    if (prefix >= 16u) {
        level_code += (1u << (prefix - 3u)) - 4096u;
    }
    return level_code;
}

static int write_cavlc_level(sh264e_bit_writer_t *bw, int level)
{
    const uint32_t sign = level < 0 ? 1u : 0u;
    const uint32_t abs_level = (uint32_t)(level < 0 ? -level : level);
    const uint32_t final_level_code = (abs_level * 2u) - 2u + sign;
    const uint32_t target = final_level_code - 2u;
    unsigned prefix;

    for (prefix = 0; prefix < 32u; prefix++) {
        unsigned suffix_size = 0;
        uint32_t suffix_limit = 1u;
        uint32_t suffix;

        if (prefix == 14u) {
            suffix_size = 4u;
        } else if (prefix >= 15u) {
            suffix_size = prefix - 3u;
            if (suffix_size > 20u) {
                return 0;
            }
        }
        suffix_limit = 1u << suffix_size;
        for (suffix = 0; suffix < suffix_limit; suffix++) {
            if (cavlc_parsed_level_code(prefix, 0, suffix) == target) {
                unsigned i;
                for (i = 0; i < prefix; i++) {
                    bw_write_bit(bw, 0);
                }
                bw_write_bit(bw, 1);
                if (suffix_size > 0u) {
                    bw_write_bits(bw, suffix, suffix_size);
                }
                return 1;
            }
        }
    }
    return 0;
}

static void write_residual_dc_only(sh264e_bit_writer_t *bw, int level)
{
    if (level == 0) {
        write_coeff_token_one_or_zero(bw, 0);
        return;
    }
    if (level == 1) {
        level = 2;
    } else if (level == -1) {
        level = -2;
    }
    write_coeff_token_one_or_zero(bw, 1);
    if (!write_cavlc_level(bw, level)) {
        bw->error = 1;
        return;
    }
    bw_write_bit(bw, 1);                 /* total_zeros = 0 for TotalCoeff=1 */
}

static void write_chroma_dc_residual(sh264e_bit_writer_t *bw, int level)
{
    if (level == 0) {
        bw_write_bits(bw, 0x03u, 6);     /* TotalCoeff=0 for chroma DC */
        return;
    }
    bw_write_bit(bw, 1);                 /* TotalCoeff=1, TrailingOnes=1 for chroma DC */
    bw_write_bit(bw, level < 0 ? 1u : 0u);
    bw_write_bit(bw, 1);                 /* total_zeros = 0 for TotalCoeff=1 */
}

static void set_luma_nz(sh264e_encoder_t *encoder, unsigned block_x, unsigned block_y, uint8_t nz)
{
    encoder->nz_luma[(size_t)block_y * SH264E_LUMA4_X + block_x] = nz;
}

static sh264e_status_t make_idr_slice(sh264e_encoder_t *encoder,
                                      const sh264e_slice_t *slice,
                                      unsigned slice_index,
                                      uint8_t *out,
                                      size_t capacity,
                                      size_t *offset)
{
    sh264e_bit_writer_t bw;
    unsigned mb_x;

    memset(encoder->recon_y, 0, (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT);
    memset(encoder->recon_u, 128, (size_t)SH264E_CHROMA_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT);
    memset(encoder->recon_v, 128, (size_t)SH264E_CHROMA_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT);
    memset(encoder->nz_luma, 0, (size_t)SH264E_LUMA4_X * SH264E_SLICE_LUMA4_Y);

    bw_init(&bw, encoder->rbsp, encoder->rbsp_capacity);

    bw_write_ue(&bw, slice_index * SH264E_MBS_X);
    bw_write_ue(&bw, 7);                 /* slice_type: all I slices */
    bw_write_ue(&bw, 0);                 /* pic_parameter_set_id */
    bw_write_bits(&bw, 0, 4);            /* frame_num */
    bw_write_ue(&bw, 0);                 /* idr_pic_id */
    bw_write_bits(&bw, 0, 4);            /* pic_order_cnt_lsb */
    bw_write_bit(&bw, 0);                /* no_output_of_prior_pics_flag */
    bw_write_bit(&bw, 0);                /* long_term_reference_flag */
    bw_write_se(&bw, 0);                 /* slice_qp_delta */
    bw_write_ue(&bw, 1);                 /* disable_deblocking_filter_idc */

    for (mb_x = 0; mb_x < SH264E_MBS_X; mb_x++) {
        int levels[16];
        int chroma_dc[2];
        unsigned b;
        unsigned cbp_luma = 0;
        unsigned cbp_chroma;
        unsigned cbp;

        for (b = 0; b < 16u; b++) {
            const unsigned x = mb_x * 16u + k_luma4x4_x[b];
            const unsigned y = k_luma4x4_y[b];
            const int level = encode_luma4x4(encoder, slice, x, y);
            const unsigned block_x = x / 4u;
            const unsigned block_y = y / 4u;
            levels[b] = level;
            set_luma_nz(encoder, block_x, block_y, level != 0 ? 1u : 0u);
            if (level != 0) {
                cbp_luma |= 1u << (b / 4u);
            }
        }

        chroma_dc[0] = encode_chroma8x8_dc(encoder, slice, 1u, mb_x * 8u, 0u);
        chroma_dc[1] = encode_chroma8x8_dc(encoder, slice, 2u, mb_x * 8u, 0u);
        cbp_chroma = (chroma_dc[0] != 0 || chroma_dc[1] != 0) ? 1u : 0u;
        cbp = cbp_luma + cbp_chroma * 16u;

        bw_write_ue(&bw, 0);                         /* mb_type: I_NxN */
        for (b = 0; b < 16u; b++) {
            bw_write_bit(&bw, 1);                    /* prev_intra4x4_pred_mode_flag */
        }
        bw_write_ue(&bw, 0);                         /* intra_chroma_pred_mode: DC */
        bw_write_ue(&bw, k_cbp_intra_code_num[cbp]);

        if (cbp != 0u) {
            bw_write_se(&bw, 0);                     /* mb_qp_delta */
            for (b = 0; b < 16u; b++) {
                if ((cbp_luma & (1u << (b / 4u))) != 0u) {
                    write_residual_dc_only(&bw, levels[b]);
                }
            }
            if (cbp_chroma != 0u) {
                write_chroma_dc_residual(&bw, chroma_dc[0]);
                write_chroma_dc_residual(&bw, chroma_dc[1]);
            }
        }

        if (bw.error != 0) {
            return SH264E_ERR_INTERNAL;
        }
    }

    bw_rbsp_trailing_bits(&bw);
    if (bw.error != 0) {
        return SH264E_ERR_INTERNAL;
    }
    return append_annexb_nalu(out, capacity, offset, 0x65u, encoder->rbsp, bw_size(&bw));
}

static sh264e_status_t map_jpeg_result(int result)
{
    switch (result) {
    case NJ_OK:
        return SH264E_OK;
    case NJ_OUT_OF_MEM:
        return SH264E_ERR_ALLOCATION_FAILED;
    case NJ_INTERNAL_ERR:
        return SH264E_ERR_INTERNAL;
    case NJ_NO_JPEG:
    case NJ_UNSUPPORTED:
    case NJ_SYNTAX_ERROR:
    default:
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
}

static sh264e_status_t jpeg_get_component(int index, sh264e_jpeg_component_t *out_component)
{
    const unsigned char *pixels = njGetComponentPixels(index);
    const int width = njGetComponentWidth(index);
    const int height = njGetComponentHeight(index);
    const int stride = njGetComponentStride(index);

    if (out_component == NULL || pixels == NULL || width <= 0 || height <= 0 || stride < width) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }

    out_component->pixels = pixels;
    out_component->width = (uint32_t)width;
    out_component->height = (uint32_t)height;
    out_component->stride = (ptrdiff_t)stride;
    return SH264E_OK;
}

static sh264e_status_t jpeg_get_components(sh264e_jpeg_component_t components[3],
                                           int *out_component_count,
                                           uint32_t image_width,
                                           uint32_t image_height)
{
    const int component_count = njGetComponentCount();
    sh264e_status_t status;
    int i;

    if (components == NULL || out_component_count == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_component_count = 0;
    if (component_count != 1 && component_count != 3) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }

    for (i = 0; i < component_count; i++) {
        status = jpeg_get_component(i, &components[i]);
        if (status != SH264E_OK) {
            return status;
        }
    }

    if (components[0].width != image_width || components[0].height != image_height) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    if (component_count == 3 &&
        (components[1].width != components[2].width ||
         components[1].height != components[2].height)) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }

    *out_component_count = component_count;
    return SH264E_OK;
}

sh264e_status_t sh264e_resize_get_slice_buffer_size(const sh264e_frame_t *src_frame, size_t *out_size)
{
    sh264e_status_t status;

    if (out_size == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_size = 0;
    status = validate_resize_source_frame(src_frame);
    if (status != SH264E_OK) {
        return status;
    }
    if (resize_is_bypass(src_frame)) {
        return SH264E_OK;
    }
    *out_size = resize_scaled_slice_buffer_size();
    return SH264E_OK;
}

sh264e_status_t sh264e_resize_make_slice(const sh264e_frame_t *src_frame,
                                         unsigned slice_index,
                                         uint8_t *work_buffer,
                                         size_t work_buffer_capacity,
                                         sh264e_slice_t *out_slice)
{
    sh264e_status_t status;
    size_t required_size;

    if (out_slice == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    memset(out_slice, 0, sizeof(*out_slice));
    if (slice_index >= SH264E_V1_SLICE_COUNT) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    status = validate_resize_source_frame(src_frame);
    if (status != SH264E_OK) {
        return status;
    }
    if (resize_is_bypass(src_frame)) {
        resize_make_bypass_slice(src_frame, slice_index, out_slice);
        return SH264E_OK;
    }

    required_size = resize_scaled_slice_buffer_size();
    if (work_buffer == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (work_buffer_capacity < required_size) {
        return SH264E_ERR_BUFFER_TOO_SMALL;
    }
    if (src_frame->pixfmt == SH264E_PIXFMT_I420) {
        resize_make_i420_slice(src_frame, slice_index, work_buffer, out_slice);
    } else {
        resize_make_nv12_slice(src_frame, slice_index, work_buffer, out_slice);
    }
    return SH264E_OK;
}

sh264e_status_t sh264e_jpeg_get_slice_buffer_size(size_t *out_size)
{
    if (out_size == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_size = resize_scaled_slice_buffer_size();
    return SH264E_OK;
}

sh264e_status_t sh264e_encode_jpeg_idr(sh264e_encoder_t *encoder,
                                       const uint8_t *jpeg_data,
                                       size_t jpeg_size,
                                       uint8_t *work_buffer,
                                       size_t work_buffer_capacity,
                                       uint8_t *out,
                                       size_t out_capacity,
                                       size_t *out_size)
{
    sh264e_status_t status;
    size_t required_work = 0;
    size_t offset = 0;
    size_t bytes = 0;
    uint32_t width;
    uint32_t height;
    int component_count = 0;
    sh264e_jpeg_component_t components[3];
    unsigned slice_index;

    if (encoder == NULL || jpeg_data == NULL || work_buffer == NULL ||
        out == NULL || out_size == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_size = 0;
    if (encoder->idr_active != 0u) {
        return SH264E_ERR_BAD_STATE;
    }
    if (jpeg_size == 0u || jpeg_size > (size_t)INT_MAX) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    status = sh264e_jpeg_get_slice_buffer_size(&required_work);
    if (status != SH264E_OK) {
        return status;
    }
    if (work_buffer_capacity < required_work) {
        return SH264E_ERR_BUFFER_TOO_SMALL;
    }

    njInit();
    status = map_jpeg_result(njDecodeComponents(jpeg_data, (int)jpeg_size));
    if (status != SH264E_OK) {
        njDone();
        return status;
    }
    if (njGetWidth() <= 0 || njGetHeight() <= 0) {
        njDone();
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    width = (uint32_t)njGetWidth();
    height = (uint32_t)njGetHeight();
    if (width < SH264E_RESIZE_MIN_SRC_WIDTH ||
        width > SH264E_RESIZE_MAX_SRC_WIDTH ||
        height < SH264E_RESIZE_MIN_SRC_HEIGHT ||
        height > SH264E_RESIZE_MAX_SRC_HEIGHT ||
        (width & 1u) != 0u ||
        (height & 1u) != 0u) {
        njDone();
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }

    status = jpeg_get_components(components, &component_count, width, height);
    if (status != SH264E_OK) {
        njDone();
        return status;
    }

    status = sh264e_begin_idr(encoder, out, out_capacity, &bytes);
    if (status != SH264E_OK) {
        reset_progressive_state(encoder);
        njDone();
        return status;
    }
    offset += bytes;

    for (slice_index = 0; slice_index < SH264E_MBS_Y; slice_index++) {
        sh264e_slice_t slice;

        if (encoder->config.pixfmt == SH264E_PIXFMT_I420) {
            jpeg_make_i420_slice(components, component_count, slice_index, work_buffer, &slice);
        } else {
            jpeg_make_nv12_slice(components, component_count, slice_index, work_buffer, &slice);
        }

        status = sh264e_encode_idr_slice(encoder, &slice, out + offset, out_capacity - offset, &bytes);
        if (status != SH264E_OK) {
            reset_progressive_state(encoder);
            njDone();
            return status;
        }
        offset += bytes;
    }

    status = sh264e_end_idr(encoder);
    if (status != SH264E_OK) {
        reset_progressive_state(encoder);
        njDone();
        return status;
    }

    njDone();
    *out_size = offset;
    return SH264E_OK;
}

sh264e_status_t sh264e_encoder_create(const sh264e_config_t *config,
                                      sh264e_encoder_t **out_encoder)
{
    sh264e_status_t status;
    sh264e_encoder_t *encoder;
    size_t max_slice_output_size = 0;
    sh264e_config_t normalized;

    if (out_encoder == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_encoder = NULL;
    if (config == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }

    normalized = *config;
    if (normalized.qp == 0) {
        normalized.qp = SH264E_DEFAULT_QP;
    }

    status = validate_config(&normalized);
    if (status != SH264E_OK) {
        return status;
    }
    status = sh264e_get_max_slice_output_size(&normalized, &max_slice_output_size);
    if (status != SH264E_OK) {
        return status;
    }

    encoder = (sh264e_encoder_t *)calloc(1u, sizeof(*encoder));
    if (encoder == NULL) {
        return SH264E_ERR_ALLOCATION_FAILED;
    }
    encoder->config = normalized;
    encoder->rbsp_capacity = max_slice_output_size;
    encoder->rbsp = (uint8_t *)malloc(encoder->rbsp_capacity);
    encoder->recon_y = (uint8_t *)malloc((size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT);
    encoder->recon_u = (uint8_t *)malloc((size_t)SH264E_CHROMA_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT);
    encoder->recon_v = (uint8_t *)malloc((size_t)SH264E_CHROMA_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT);
    encoder->nz_luma = (uint8_t *)malloc((size_t)SH264E_LUMA4_X * SH264E_SLICE_LUMA4_Y);

    if (encoder->rbsp == NULL || encoder->recon_y == NULL || encoder->recon_u == NULL ||
        encoder->recon_v == NULL || encoder->nz_luma == NULL) {
        sh264e_encoder_destroy(encoder);
        return SH264E_ERR_ALLOCATION_FAILED;
    }

    *out_encoder = encoder;
    return SH264E_OK;
}

void sh264e_encoder_destroy(sh264e_encoder_t *encoder)
{
    if (encoder == NULL) {
        return;
    }
    free(encoder->rbsp);
    free(encoder->recon_y);
    free(encoder->recon_u);
    free(encoder->recon_v);
    free(encoder->nz_luma);
    free(encoder);
}

sh264e_status_t sh264e_get_max_header_output_size(const sh264e_config_t *config, size_t *out_size)
{
    sh264e_status_t status;
    sh264e_config_t normalized;

    if (config == NULL || out_size == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    normalized = *config;
    if (normalized.qp == 0) {
        normalized.qp = SH264E_DEFAULT_QP;
    }
    status = validate_config(&normalized);
    if (status != SH264E_OK) {
        return status;
    }
    *out_size = 1024u;
    return SH264E_OK;
}

sh264e_status_t sh264e_get_max_slice_output_size(const sh264e_config_t *config, size_t *out_size)
{
    sh264e_status_t status;
    sh264e_config_t normalized;
    size_t slice_input_size;

    if (config == NULL || out_size == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    normalized = *config;
    if (normalized.qp == 0) {
        normalized.qp = SH264E_DEFAULT_QP;
    }
    status = validate_config(&normalized);
    if (status != SH264E_OK) {
        return status;
    }
    slice_input_size = (size_t)normalized.width * SH264E_V1_SLICE_LUMA_HEIGHT +
                       (size_t)normalized.width * SH264E_V1_SLICE_CHROMA_HEIGHT;
    *out_size = slice_input_size * 2u + 4096u;
    return SH264E_OK;
}

sh264e_status_t sh264e_get_max_output_size(const sh264e_config_t *config, size_t *out_size)
{
    sh264e_status_t status;
    size_t header_size = 0;
    size_t slice_size = 0;

    if (config == NULL || out_size == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    status = sh264e_get_max_header_output_size(config, &header_size);
    if (status != SH264E_OK) {
        return status;
    }
    status = sh264e_get_max_slice_output_size(config, &slice_size);
    if (status != SH264E_OK) {
        return status;
    }
    *out_size = header_size + slice_size * SH264E_MBS_Y;
    return SH264E_OK;
}

sh264e_status_t sh264e_begin_idr(sh264e_encoder_t *encoder,
                                 uint8_t *out,
                                 size_t out_capacity,
                                 size_t *out_size)
{
    sh264e_status_t status;
    size_t offset = 0;

    if (encoder == NULL || out == NULL || out_size == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_size = 0;
    if (encoder->idr_active != 0u) {
        return SH264E_ERR_BAD_STATE;
    }
    if (out_capacity == 0u) {
        return SH264E_ERR_BUFFER_TOO_SMALL;
    }

    status = make_sps(encoder, out, out_capacity, &offset);
    if (status != SH264E_OK) {
        return status;
    }
    status = make_pps(encoder, out, out_capacity, &offset);
    if (status != SH264E_OK) {
        return status;
    }

    encoder->idr_active = 1u;
    encoder->slices_encoded = 0u;
    *out_size = offset;
    return SH264E_OK;
}

sh264e_status_t sh264e_encode_idr_slice(sh264e_encoder_t *encoder,
                                        const sh264e_slice_t *slice,
                                        uint8_t *out,
                                        size_t out_capacity,
                                        size_t *out_size)
{
    sh264e_status_t status;
    size_t offset = 0;

    if (encoder == NULL || slice == NULL || out == NULL || out_size == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_size = 0;
    if (encoder->idr_active == 0u) {
        return SH264E_ERR_BAD_STATE;
    }
    if (encoder->slices_encoded >= SH264E_MBS_Y) {
        return SH264E_ERR_FRAME_COMPLETE;
    }
    status = validate_slice(&encoder->config, slice);
    if (status != SH264E_OK) {
        return status;
    }
    if (out_capacity == 0u) {
        return SH264E_ERR_BUFFER_TOO_SMALL;
    }

    status = make_idr_slice(encoder, slice, encoder->slices_encoded, out, out_capacity, &offset);
    if (status != SH264E_OK) {
        return status;
    }

    encoder->slices_encoded++;
    *out_size = offset;
    return SH264E_OK;
}

sh264e_status_t sh264e_end_idr(sh264e_encoder_t *encoder)
{
    if (encoder == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (encoder->idr_active == 0u) {
        return SH264E_ERR_BAD_STATE;
    }
    if (encoder->slices_encoded != SH264E_MBS_Y) {
        return SH264E_ERR_INCOMPLETE_FRAME;
    }
    encoder->idr_active = 0u;
    encoder->slices_encoded = 0u;
    return SH264E_OK;
}

static void make_slice_from_frame(const sh264e_frame_t *frame, unsigned slice_index, sh264e_slice_t *slice)
{
    const size_t y_offset = (size_t)slice_index * SH264E_V1_SLICE_LUMA_HEIGHT * (size_t)frame->stride[0];
    const size_t c_offset = (size_t)slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT;

    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = frame->pixfmt;
    slice->plane[0] = frame->plane[0] + y_offset;
    slice->stride[0] = frame->stride[0];

    if (frame->pixfmt == SH264E_PIXFMT_I420) {
        slice->plane[1] = frame->plane[1] + c_offset * (size_t)frame->stride[1];
        slice->plane[2] = frame->plane[2] + c_offset * (size_t)frame->stride[2];
        slice->stride[1] = frame->stride[1];
        slice->stride[2] = frame->stride[2];
    } else {
        slice->plane[1] = frame->plane[1] + c_offset * (size_t)frame->stride[1];
        slice->stride[1] = frame->stride[1];
    }
}

static void reset_progressive_state(sh264e_encoder_t *encoder)
{
    if (encoder != NULL) {
        encoder->idr_active = 0u;
        encoder->slices_encoded = 0u;
    }
}

sh264e_status_t sh264e_encode_idr(sh264e_encoder_t *encoder,
                                  const sh264e_frame_t *frame,
                                  uint8_t *out,
                                  size_t out_capacity,
                                  size_t *out_size)
{
    sh264e_status_t status;
    size_t offset = 0;
    size_t bytes = 0;
    unsigned slice_index;

    if (encoder == NULL || frame == NULL || out == NULL || out_size == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_size = 0;
    if (encoder->idr_active != 0u) {
        return SH264E_ERR_BAD_STATE;
    }
    status = validate_frame(&encoder->config, frame);
    if (status != SH264E_OK) {
        return status;
    }

    status = sh264e_begin_idr(encoder, out, out_capacity, &bytes);
    if (status != SH264E_OK) {
        reset_progressive_state(encoder);
        return status;
    }
    offset += bytes;

    for (slice_index = 0; slice_index < SH264E_MBS_Y; slice_index++) {
        sh264e_slice_t slice;
        make_slice_from_frame(frame, slice_index, &slice);
        status = sh264e_encode_idr_slice(encoder, &slice, out + offset, out_capacity - offset, &bytes);
        if (status != SH264E_OK) {
            reset_progressive_state(encoder);
            return status;
        }
        offset += bytes;
    }

    status = sh264e_end_idr(encoder);
    if (status != SH264E_OK) {
        reset_progressive_state(encoder);
        return status;
    }

    *out_size = offset;
    return SH264E_OK;
}

const char *sh264e_status_string(sh264e_status_t status)
{
    switch (status) {
    case SH264E_OK:
        return "ok";
    case SH264E_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case SH264E_ERR_UNSUPPORTED_CONFIG:
        return "unsupported config";
    case SH264E_ERR_BUFFER_TOO_SMALL:
        return "buffer too small";
    case SH264E_ERR_ALLOCATION_FAILED:
        return "allocation failed";
    case SH264E_ERR_INTERNAL:
        return "internal error";
    case SH264E_ERR_BAD_STATE:
        return "bad state";
    case SH264E_ERR_INCOMPLETE_FRAME:
        return "incomplete frame";
    case SH264E_ERR_FRAME_COMPLETE:
        return "frame complete";
    default:
        return "unknown error";
    }
}
