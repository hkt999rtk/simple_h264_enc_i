#include "sh264e.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef SH264E_ENABLE_JPEG_TEST_HOOKS
#define SH264E_ENABLE_JPEG_TEST_HOOKS 0
#endif

#define NJ_OK 0
#define NJ_NO_JPEG 1
#define NJ_UNSUPPORTED 2
#define NJ_OUT_OF_MEM 3
#define NJ_INTERNAL_ERR 4
#define NJ_SYNTAX_ERROR 5
#define NJ_CALLBACK_ABORT 6

#define SH264E_MB_SIZE 16u
#define SH264E_LUMA_4X4 4u
#define SH264E_MBS_X (SH264E_V1_WIDTH / SH264E_MB_SIZE)
#define SH264E_MBS_Y (SH264E_V1_HEIGHT / SH264E_MB_SIZE)
#define SH264E_CHROMA_WIDTH (SH264E_V1_WIDTH / 2u)
#define SH264E_MAX_IDR_MB_RBSP_SIZE 256u
#define SH264E_MAX_IDR_MB_NALU_OUTPUT_SIZE \
    (SH264E_MAX_IDR_MB_RBSP_SIZE + (SH264E_MAX_IDR_MB_RBSP_SIZE / 2u) + 4u)
#define SH264E_MAX_QP 51
#define SH264E_SCALE_FP_BITS 16u
#define SH264E_SCALE_FP_ONE (1u << SH264E_SCALE_FP_BITS)
#define SH264E_SCALE_FP_HALF (SH264E_SCALE_FP_ONE >> 1u)
#define SH264E_SCALE_FP_BLEND_ROUND ((uint64_t)1u << ((SH264E_SCALE_FP_BITS * 2u) - 1u))
#define SH264E_ENCODER_FLAG_IDR_ACTIVE 1u
#define SH264E_ENCODER_FLAG_OWNS_MEMORY 2u
#define SH264E_ENCODER_ARENA_ALIGNMENT ((size_t)sizeof(void *))

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

typedef struct sh264e_chunk_writer_t {
    uint8_t *buffer;
    size_t capacity;
    size_t size;
    size_t total;
    sh264e_output_consumer_t consumer;
    void *user;
} sh264e_chunk_writer_t;

struct sh264e_encoder_t {
    sh264e_config_t config;
    uint8_t *rbsp;
    size_t rbsp_capacity;
    unsigned flags;
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

typedef enum sh264e_jpeg_alloc_mode_t {
    SH264E_JPEG_ALLOC_HEAP = 0,
    SH264E_JPEG_ALLOC_ARENA = 1
} sh264e_jpeg_alloc_mode_t;

typedef struct sh264e_jpeg_stream_component_t {
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t row0;
    uint32_t rows;
    uint32_t cache_rows;
    uint32_t rows_per_mcu;
    ptrdiff_t stride;
} sh264e_jpeg_stream_component_t;

typedef struct sh264e_jpeg_stream_context_t {
    sh264e_encoder_t *encoder;
    uint8_t *work_buffer;
    size_t work_buffer_capacity;
    uint8_t *out;
    size_t out_capacity;
    size_t offset;
    sh264e_output_consumer_t consumer;
    void *consumer_user;
    size_t cache_bytes;
    size_t effective_slice_work_bytes;
    unsigned next_slice;
    unsigned idr_started;
    int component_count;
    int one_to_one_420;
    sh264e_pixfmt_t output_pixfmt;
    int measure_only;
    int initialized;
    sh264e_status_t status;
    sh264e_jpeg_stream_component_t components[3];
} sh264e_jpeg_stream_context_t;

typedef struct sh264e_jpeg_alloc_header_t {
    size_t size;
    size_t arena_span;
    unsigned from_arena;
} sh264e_jpeg_alloc_header_t;

typedef int (*nj_mcu_row_callback_t)(int mcu_y, void *user);
typedef int (*nj_input_read_callback_t)(void *user,
                                        unsigned char *dst,
                                        int requested,
                                        int *out_read);
typedef struct nj_input_source_t {
    nj_input_read_callback_t read;
    void *user;
} nj_input_source_t;

void njInit(void);
int njDecodeMcuRows(const void *jpeg, const int size, nj_mcu_row_callback_t callback, void *user);
int njDecodeMcuRowsFromSource(const nj_input_source_t *source,
                              nj_mcu_row_callback_t callback,
                              void *user);
int njGetWidth(void);
int njGetHeight(void);
int njGetComponentCount(void);
const unsigned char *njGetComponentPixels(int index);
int njGetComponentWidth(int index);
int njGetComponentHeight(int index);
int njGetComponentStride(int index);
int njGetComponentSsx(int index);
int njGetComponentSsy(int index);
void njDone(void);

static size_t sh264e_jpeg_alloc_current_bytes;
static size_t sh264e_jpeg_alloc_peak_bytes;
static size_t sh264e_jpeg_alloc_limit = (size_t)-1;
static size_t sh264e_jpeg_alloc_arena_offset;
static size_t sh264e_jpeg_alloc_peak_arena_bytes;
static unsigned sh264e_jpeg_alloc_arena_failed;
static sh264e_jpeg_alloc_mode_t sh264e_jpeg_alloc_mode = SH264E_JPEG_ALLOC_HEAP;
static uint8_t *sh264e_jpeg_alloc_arena;
static size_t sh264e_jpeg_alloc_arena_capacity;
static size_t sh264e_jpeg_streaming_last_cache_bytes;
static size_t sh264e_jpeg_streaming_last_slice_work_bytes;

static void reset_progressive_state(sh264e_encoder_t *encoder);
static sh264e_status_t sh264e_begin_idr_to_consumer(sh264e_encoder_t *encoder,
                                                    uint8_t *chunk_buffer,
                                                    size_t chunk_capacity,
                                                    size_t *out_size,
                                                    sh264e_output_consumer_t consumer,
                                                    void *consumer_user);
static sh264e_status_t sh264e_encode_idr_slice_to_consumer(sh264e_encoder_t *encoder,
                                                           const sh264e_slice_t *slice,
                                                           uint8_t *chunk_buffer,
                                                           size_t chunk_capacity,
                                                           size_t *out_size,
                                                           sh264e_output_consumer_t consumer,
                                                           void *consumer_user);
static sh264e_status_t sh264e_encode_jpeg_idr_streaming_impl(sh264e_encoder_t *encoder,
                                                             const uint8_t *jpeg_data,
                                                             size_t jpeg_size,
                                                             const sh264e_jpeg_source_t *jpeg_source,
                                                             uint8_t *jpeg_arena,
                                                             size_t jpeg_arena_size,
                                                             int use_arena,
                                                             uint8_t *work_buffer,
                                                             size_t work_buffer_capacity,
                                                             uint8_t *out,
                                                             size_t out_capacity,
                                                             size_t *out_size,
                                                             sh264e_output_consumer_t consumer,
                                                             void *consumer_user);

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

static void jpeg_allocation_stats_reset(void)
{
    sh264e_jpeg_alloc_current_bytes = 0u;
    sh264e_jpeg_alloc_peak_bytes = 0u;
    sh264e_jpeg_alloc_arena_offset = 0u;
    sh264e_jpeg_alloc_peak_arena_bytes = 0u;
    sh264e_jpeg_alloc_arena_failed = 0u;
}

static size_t jpeg_alloc_alignment(void)
{
    return sizeof(void *);
}

static size_t jpeg_alloc_header_size(void)
{
    const size_t align = jpeg_alloc_alignment();
    const size_t rem = sizeof(sh264e_jpeg_alloc_header_t) % align;
    return rem == 0u ? sizeof(sh264e_jpeg_alloc_header_t) :
                       sizeof(sh264e_jpeg_alloc_header_t) + align - rem;
}

static int jpeg_align_up(size_t value, size_t *out_value)
{
    const size_t align = jpeg_alloc_alignment();
    const size_t rem = value % align;

    if (out_value == NULL) {
        return 0;
    }
    if (rem == 0u) {
        *out_value = value;
        return 1;
    }
    if (value > ((size_t)-1) - (align - rem)) {
        return 0;
    }
    *out_value = value + align - rem;
    return 1;
}

static int jpeg_allocation_span(size_t requested, size_t *out_span)
{
    const size_t header_size = jpeg_alloc_header_size();

    if (out_span == NULL || requested == 0u || requested > ((size_t)-1) - header_size) {
        return 0;
    }
    *out_span = header_size + requested;
    return 1;
}

static void jpeg_allocation_begin_heap(void)
{
    sh264e_jpeg_alloc_mode = SH264E_JPEG_ALLOC_HEAP;
    sh264e_jpeg_alloc_arena = NULL;
    sh264e_jpeg_alloc_arena_capacity = 0u;
    jpeg_allocation_stats_reset();
}

static void jpeg_allocation_begin_arena(uint8_t *arena, size_t arena_capacity)
{
    sh264e_jpeg_alloc_mode = SH264E_JPEG_ALLOC_ARENA;
    sh264e_jpeg_alloc_arena = arena;
    sh264e_jpeg_alloc_arena_capacity = arena_capacity;
    jpeg_allocation_stats_reset();
}

static void jpeg_allocation_end(void)
{
    sh264e_jpeg_alloc_mode = SH264E_JPEG_ALLOC_HEAP;
    sh264e_jpeg_alloc_arena = NULL;
    sh264e_jpeg_alloc_arena_capacity = 0u;
}

void *njAllocMem(int size)
{
    const size_t requested = (size > 0) ? (size_t)size : 0u;
    const size_t header_size = jpeg_alloc_header_size();
    size_t span = 0u;
    size_t aligned_offset = 0u;
    size_t end_offset = 0u;
    sh264e_jpeg_alloc_header_t header;
    uint8_t *header_bytes;
    void *raw;

    if (!jpeg_allocation_span(requested, &span)) {
        return NULL;
    }
    if (sh264e_jpeg_alloc_current_bytes > ((size_t)-1) - requested) {
        return NULL;
    }
    if (sh264e_jpeg_alloc_limit != (size_t)-1) {
        if (sh264e_jpeg_alloc_current_bytes >= sh264e_jpeg_alloc_limit ||
            requested > sh264e_jpeg_alloc_limit - sh264e_jpeg_alloc_current_bytes) {
            return NULL;
        }
    }
    if (!jpeg_align_up(sh264e_jpeg_alloc_arena_offset, &aligned_offset)) {
        sh264e_jpeg_alloc_arena_failed = 1u;
        return NULL;
    }
    if (aligned_offset > ((size_t)-1) - span) {
        sh264e_jpeg_alloc_arena_failed = 1u;
        return NULL;
    }
    end_offset = aligned_offset + span;

    if (sh264e_jpeg_alloc_mode == SH264E_JPEG_ALLOC_ARENA) {
        if (sh264e_jpeg_alloc_arena == NULL || end_offset > sh264e_jpeg_alloc_arena_capacity) {
            sh264e_jpeg_alloc_arena_failed = 1u;
            return NULL;
        }

        header.size = requested;
        header.arena_span = span;
        header.from_arena = 1u;
        header_bytes = sh264e_jpeg_alloc_arena + aligned_offset;
        /* Caller arenas are byte buffers; do not require the arena base itself to be aligned. */
        memcpy(header_bytes, &header, sizeof(header));
        raw = sh264e_jpeg_alloc_arena + aligned_offset + header_size;
        sh264e_jpeg_alloc_arena_offset = end_offset;
    } else {
        raw = malloc(span);
        if (raw == NULL) {
            return NULL;
        }

        header.size = requested;
        header.arena_span = span;
        header.from_arena = 0u;
        header_bytes = (uint8_t *)raw;
        memcpy(header_bytes, &header, sizeof(header));
        raw = (uint8_t *)raw + header_size;
        sh264e_jpeg_alloc_arena_offset = end_offset;
    }
    if (sh264e_jpeg_alloc_arena_offset > sh264e_jpeg_alloc_peak_arena_bytes) {
        sh264e_jpeg_alloc_peak_arena_bytes = sh264e_jpeg_alloc_arena_offset;
    }

    sh264e_jpeg_alloc_current_bytes += requested;
    if (sh264e_jpeg_alloc_current_bytes > sh264e_jpeg_alloc_peak_bytes) {
        sh264e_jpeg_alloc_peak_bytes = sh264e_jpeg_alloc_current_bytes;
    }

    return raw;
}

void njFreeMem(void *block)
{
    uint8_t *header_bytes;
    sh264e_jpeg_alloc_header_t header;

    if (block == NULL) {
        return;
    }

    header_bytes = (uint8_t *)block - jpeg_alloc_header_size();
    memcpy(&header, header_bytes, sizeof(header));
    if (sh264e_jpeg_alloc_current_bytes >= header.size) {
        sh264e_jpeg_alloc_current_bytes -= header.size;
    } else {
        sh264e_jpeg_alloc_current_bytes = 0u;
    }
    if (header.from_arena == 0u) {
        free(header_bytes);
    }
}

void njFillMem(void *block, unsigned char byte, int size)
{
    if (block != NULL && size > 0) {
        memset(block, byte, (size_t)size);
    }
}

void njCopyMem(void *dest, const void *src, int size)
{
    if (dest != NULL && src != NULL && size > 0) {
        memcpy(dest, src, (size_t)size);
    }
}

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

static int encoder_idr_active(const sh264e_encoder_t *encoder)
{
    return (encoder->flags & SH264E_ENCODER_FLAG_IDR_ACTIVE) != 0u;
}

static void encoder_set_idr_active(sh264e_encoder_t *encoder, int active)
{
    if (active) {
        encoder->flags |= SH264E_ENCODER_FLAG_IDR_ACTIVE;
    } else {
        encoder->flags &= ~SH264E_ENCODER_FLAG_IDR_ACTIVE;
    }
}

static int is_supported_pixfmt(sh264e_pixfmt_t pixfmt)
{
    return pixfmt == SH264E_PIXFMT_I420 || pixfmt == SH264E_PIXFMT_NV12;
}

static size_t encoder_recon_luma_bytes(void)
{
    return 0u;
}

static size_t encoder_recon_chroma_plane_bytes(void)
{
    return 0u;
}

static size_t encoder_recon_chroma_bytes(void)
{
    return encoder_recon_chroma_plane_bytes() * 2u;
}

static size_t encoder_neighbor_state_bytes(void)
{
    return 0u;
}

static size_t encoder_rbsp_scratch_bytes(void)
{
    return SH264E_MAX_IDR_MB_RBSP_SIZE;
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

static int exact_scale_ratio_mode(uint32_t src_width,
                                  uint32_t src_height,
                                  uint32_t dst_width,
                                  uint32_t dst_height)
{
    if (src_width * 2u == dst_width && src_height * 2u == dst_height) {
        return 1;
    }
    if (src_width == dst_width * 2u && src_height == dst_height * 2u) {
        return 2;
    }
    return 0;
}

static sh264e_scale_coord_t exact_scale_coord(int mode,
                                              uint32_t dst_pos,
                                              uint32_t dst_size,
                                              uint32_t src_size)
{
    sh264e_scale_coord_t coord;

    if (mode == 1) {
        uint32_t raw_quarters;

        if (dst_pos == 0u) {
            coord.index = 0u;
            coord.fraction = 0u;
            return coord;
        }
        if (dst_pos + 1u >= dst_size) {
            coord.index = src_size - 1u;
            coord.fraction = 0u;
            return coord;
        }
        raw_quarters = dst_pos * 2u - 1u;
        coord.index = raw_quarters >> 2u;
        coord.fraction = (raw_quarters & 3u) * (SH264E_SCALE_FP_ONE / 4u);
        return coord;
    }

    coord.index = dst_pos * 2u;
    coord.fraction = SH264E_SCALE_FP_HALF;
    return coord;
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

static uint8_t streaming_bilinear_sample_plane(const sh264e_jpeg_stream_component_t *src,
                                               sh264e_scale_coord_t sx,
                                               sh264e_scale_coord_t sy)
{
    uint32_t y0 = sy.index;
    uint32_t y1 = y0 + 1u < src->height ? y0 + 1u : y0;
    const uint32_t x0 = sx.index;
    const uint32_t x1 = x0 + 1u < src->width ? x0 + 1u : x0;
    const uint8_t *row0;
    const uint8_t *row1;

    if (y0 < src->row0) {
        y0 = src->row0;
    }
    if (y1 < src->row0) {
        y1 = src->row0;
    }
    y0 -= src->row0;
    y1 -= src->row0;
    if (y0 >= src->rows) {
        y0 = src->rows - 1u;
    }
    if (y1 >= src->rows) {
        y1 = src->rows - 1u;
    }

    row0 = src->pixels + (size_t)y0 * (size_t)src->stride;
    row1 = src->pixels + (size_t)y1 * (size_t)src->stride;
    return bilinear_blend_u8(row0[x0], row0[x1], row1[x0], row1[x1], sx.fraction, sy.fraction);
}

static void streaming_scale_component_slice(const sh264e_jpeg_stream_component_t *src,
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
            dst_row[x] = streaming_bilinear_sample_plane(src, sx, sy);
            scale_axis_mapper_advance(&x_mapper);
        }
        scale_axis_mapper_advance(&y_mapper);
    }
}

static void streaming_scale_nv12_component_chroma_slice(const sh264e_jpeg_stream_component_t *cb,
                                                        const sh264e_jpeg_stream_component_t *cr,
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
            row[(size_t)x * 2u] = streaming_bilinear_sample_plane(cb, sx, sy);
            row[(size_t)x * 2u + 1u] = streaming_bilinear_sample_plane(cr, sx, sy);
            scale_axis_mapper_advance(&x_mapper);
        }
        scale_axis_mapper_advance(&y_mapper);
    }
}

static void streaming_source_range(uint32_t src_height,
                                   uint32_t dst_height,
                                   uint32_t dst_y_start,
                                   uint32_t dst_rows,
                                   uint32_t *out_min_y,
                                   uint32_t *out_max_y)
{
    sh264e_axis_mapper_t y_mapper = scale_axis_mapper_init(src_height, dst_height, dst_y_start);
    uint32_t min_y = UINT_MAX;
    uint32_t max_y = 0u;
    uint32_t y;

    for (y = 0; y < dst_rows; y++) {
        const sh264e_scale_coord_t sy = scale_coord_from_raw(y_mapper.pos, src_height);
        const uint32_t y1 = sy.index + 1u < src_height ? sy.index + 1u : sy.index;
        if (sy.index < min_y) {
            min_y = sy.index;
        }
        if (y1 > max_y) {
            max_y = y1;
        }
        scale_axis_mapper_advance(&y_mapper);
    }
    *out_min_y = min_y;
    *out_max_y = max_y;
}

static uint32_t streaming_cache_rows_for_slice_window(uint32_t src_height,
                                                      uint32_t dst_height,
                                                      uint32_t dst_rows,
                                                      uint32_t rows_per_mcu)
{
    uint32_t max_rows = rows_per_mcu;
    uint32_t slice_y;

    for (slice_y = 0u; slice_y < dst_height; slice_y += dst_rows) {
        uint32_t min_y;
        uint32_t max_y;
        uint32_t rows;

        streaming_source_range(src_height, dst_height, slice_y, dst_rows, &min_y, &max_y);
        rows = max_y - min_y + 1u;
        if (rows > max_rows) {
            max_rows = rows;
        }
    }

    if ((max_rows % rows_per_mcu) != 0u) {
        max_rows += rows_per_mcu - (max_rows % rows_per_mcu);
    }
    return max_rows + rows_per_mcu;
}

static int streaming_is_one_to_one_420(void)
{
    if (njGetComponentCount() != 3 ||
        njGetWidth() != (int)SH264E_V1_WIDTH ||
        njGetHeight() != (int)SH264E_V1_HEIGHT) {
        return 0;
    }
    return njGetComponentWidth(0) == (int)SH264E_V1_WIDTH &&
           njGetComponentHeight(0) == (int)SH264E_V1_HEIGHT &&
           njGetComponentWidth(1) == (int)SH264E_CHROMA_WIDTH &&
           njGetComponentHeight(1) == (int)(SH264E_V1_HEIGHT / 2u) &&
           njGetComponentWidth(2) == (int)SH264E_CHROMA_WIDTH &&
           njGetComponentHeight(2) == (int)(SH264E_V1_HEIGHT / 2u) &&
           njGetComponentSsx(0) == 2 &&
           njGetComponentSsy(0) == 2 &&
           njGetComponentSsx(1) == 1 &&
           njGetComponentSsy(1) == 1 &&
           njGetComponentSsx(2) == 1 &&
           njGetComponentSsy(2) == 1;
}

static void streaming_direct_source_range(unsigned component_index,
                                          unsigned slice_index,
                                          uint32_t *out_min_y,
                                          uint32_t *out_max_y)
{
    const uint32_t rows = component_index == 0u ? SH264E_V1_SLICE_LUMA_HEIGHT :
                                                 SH264E_V1_SLICE_CHROMA_HEIGHT;
    const uint32_t min_y = (uint32_t)slice_index * rows;

    *out_min_y = min_y;
    *out_max_y = min_y + rows - 1u;
}

static int streaming_slice_available(const sh264e_jpeg_stream_context_t *ctx, unsigned slice_index)
{
    uint32_t min_y;
    uint32_t max_y;
    unsigned i;

    if (ctx->one_to_one_420) {
        streaming_direct_source_range(0u, slice_index, &min_y, &max_y);
    } else {
        streaming_source_range(ctx->components[0].height,
                               SH264E_V1_HEIGHT,
                               slice_index * SH264E_V1_SLICE_LUMA_HEIGHT,
                               SH264E_V1_SLICE_LUMA_HEIGHT,
                               &min_y,
                               &max_y);
    }
    if (min_y < ctx->components[0].row0 ||
        max_y >= ctx->components[0].row0 + ctx->components[0].rows) {
        return 0;
    }

    for (i = 1u; i < (unsigned)ctx->component_count; i++) {
        if (ctx->one_to_one_420) {
            streaming_direct_source_range(i, slice_index, &min_y, &max_y);
        } else {
            streaming_source_range(ctx->components[i].height,
                                   SH264E_V1_HEIGHT / 2u,
                                   slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT,
                                   SH264E_V1_SLICE_CHROMA_HEIGHT,
                                   &min_y,
                                   &max_y);
        }
        if (min_y < ctx->components[i].row0 ||
            max_y >= ctx->components[i].row0 + ctx->components[i].rows) {
            return 0;
        }
    }
    return 1;
}

static void streaming_make_i420_slice(const sh264e_jpeg_stream_context_t *ctx,
                                      unsigned slice_index,
                                      sh264e_slice_t *slice)
{
    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = SH264E_PIXFMT_I420;

    if (ctx->one_to_one_420) {
        const sh264e_jpeg_stream_component_t *y = &ctx->components[0];
        const sh264e_jpeg_stream_component_t *cb = &ctx->components[1];
        const sh264e_jpeg_stream_component_t *cr = &ctx->components[2];
        const uint32_t luma_row = (uint32_t)slice_index * SH264E_V1_SLICE_LUMA_HEIGHT;
        const uint32_t chroma_row = (uint32_t)slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT;

        slice->plane[0] = y->pixels + (size_t)(luma_row - y->row0) * (size_t)y->stride;
        slice->plane[1] = cb->pixels + (size_t)(chroma_row - cb->row0) * (size_t)cb->stride;
        slice->plane[2] = cr->pixels + (size_t)(chroma_row - cr->row0) * (size_t)cr->stride;
        slice->stride[0] = y->stride;
        slice->stride[1] = cb->stride;
        slice->stride[2] = cr->stride;
        return;
    }

    {
        uint8_t *dst_y = ctx->work_buffer;
        uint8_t *dst_u = dst_y + (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT;
        uint8_t *dst_v = dst_u + (size_t)SH264E_CHROMA_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT;

        streaming_scale_component_slice(&ctx->components[0], dst_y,
                                        SH264E_V1_WIDTH, SH264E_V1_HEIGHT,
                                        slice_index * SH264E_V1_SLICE_LUMA_HEIGHT,
                                        SH264E_V1_SLICE_LUMA_HEIGHT,
                                        SH264E_V1_WIDTH);
        if (ctx->component_count == 1) {
            jpeg_fill_i420_neutral_chroma(dst_u, dst_v);
        } else {
            streaming_scale_component_slice(&ctx->components[1], dst_u,
                                            SH264E_CHROMA_WIDTH, SH264E_V1_HEIGHT / 2u,
                                            slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT,
                                            SH264E_V1_SLICE_CHROMA_HEIGHT,
                                            SH264E_CHROMA_WIDTH);
            streaming_scale_component_slice(&ctx->components[2], dst_v,
                                            SH264E_CHROMA_WIDTH, SH264E_V1_HEIGHT / 2u,
                                            slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT,
                                            SH264E_V1_SLICE_CHROMA_HEIGHT,
                                            SH264E_CHROMA_WIDTH);
        }

        slice->plane[0] = dst_y;
        slice->plane[1] = dst_u;
        slice->plane[2] = dst_v;
        slice->stride[0] = SH264E_V1_WIDTH;
        slice->stride[1] = SH264E_CHROMA_WIDTH;
        slice->stride[2] = SH264E_CHROMA_WIDTH;
    }
}

static void streaming_interleave_direct_nv12_chroma_slice(const sh264e_jpeg_stream_context_t *ctx,
                                                          unsigned slice_index,
                                                          uint8_t *dst_uv)
{
    const sh264e_jpeg_stream_component_t *cb = &ctx->components[1];
    const sh264e_jpeg_stream_component_t *cr = &ctx->components[2];
    const uint32_t chroma_row = (uint32_t)slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT;
    const uint8_t *src_cb = cb->pixels + (size_t)(chroma_row - cb->row0) * (size_t)cb->stride;
    const uint8_t *src_cr = cr->pixels + (size_t)(chroma_row - cr->row0) * (size_t)cr->stride;
    uint32_t y;

    for (y = 0; y < SH264E_V1_SLICE_CHROMA_HEIGHT; y++) {
        const uint8_t *row_cb = src_cb + (size_t)y * (size_t)cb->stride;
        const uint8_t *row_cr = src_cr + (size_t)y * (size_t)cr->stride;
        uint8_t *row = dst_uv + (size_t)y * SH264E_V1_WIDTH;
        uint32_t x;

        for (x = 0; x < SH264E_CHROMA_WIDTH; x++) {
            row[(size_t)x * 2u] = row_cb[x];
            row[(size_t)x * 2u + 1u] = row_cr[x];
        }
    }
}

static void streaming_make_nv12_slice(const sh264e_jpeg_stream_context_t *ctx,
                                      unsigned slice_index,
                                      sh264e_slice_t *slice)
{
    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = SH264E_PIXFMT_NV12;

    if (ctx->one_to_one_420) {
        const sh264e_jpeg_stream_component_t *y = &ctx->components[0];
        const uint32_t luma_row = (uint32_t)slice_index * SH264E_V1_SLICE_LUMA_HEIGHT;
        uint8_t *dst_uv = ctx->work_buffer;

        streaming_interleave_direct_nv12_chroma_slice(ctx, slice_index, dst_uv);

        slice->plane[0] = y->pixels + (size_t)(luma_row - y->row0) * (size_t)y->stride;
        slice->plane[1] = dst_uv;
        slice->stride[0] = y->stride;
        slice->stride[1] = SH264E_V1_WIDTH;
        return;
    }

    {
        uint8_t *dst_y = ctx->work_buffer;
        uint8_t *dst_uv = dst_y + (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT;

        streaming_scale_component_slice(&ctx->components[0], dst_y,
                                        SH264E_V1_WIDTH, SH264E_V1_HEIGHT,
                                        slice_index * SH264E_V1_SLICE_LUMA_HEIGHT,
                                        SH264E_V1_SLICE_LUMA_HEIGHT,
                                        SH264E_V1_WIDTH);
        if (ctx->component_count == 1) {
            jpeg_fill_nv12_neutral_chroma(dst_uv);
        } else {
            streaming_scale_nv12_component_chroma_slice(&ctx->components[1], &ctx->components[2],
                                                        dst_uv,
                                                        slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT);
        }

        slice->plane[0] = dst_y;
        slice->plane[1] = dst_uv;
        slice->stride[0] = SH264E_V1_WIDTH;
        slice->stride[1] = SH264E_V1_WIDTH;
    }
}

static void streaming_free_cache(sh264e_jpeg_stream_context_t *ctx)
{
    unsigned i;

    for (i = 0u; i < 3u; i++) {
        njFreeMem(ctx->components[i].pixels);
        ctx->components[i].pixels = NULL;
    }
    ctx->initialized = 0;
}

static sh264e_status_t streaming_init_cache(sh264e_jpeg_stream_context_t *ctx)
{
    const int component_count = njGetComponentCount();
    const int image_width = njGetWidth();
    const int image_height = njGetHeight();
    const int y_width = njGetComponentWidth(0);
    const int y_height = njGetComponentHeight(0);
    const sh264e_pixfmt_t pixfmt = ctx->encoder != NULL ? ctx->encoder->config.pixfmt :
                                                          ctx->output_pixfmt;
    unsigned i;

    if (component_count != 1 && component_count != 3) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    if (image_width < (int)SH264E_RESIZE_MIN_SRC_WIDTH ||
        image_width > (int)SH264E_RESIZE_MAX_SRC_WIDTH ||
        image_height < (int)SH264E_RESIZE_MIN_SRC_HEIGHT ||
        image_height > (int)SH264E_RESIZE_MAX_SRC_HEIGHT ||
        (image_width & 1) != 0 ||
        (image_height & 1) != 0) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    if (y_width != image_width || y_height != image_height) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    if (component_count == 3) {
        const int cb_width = njGetComponentWidth(1);
        const int cb_height = njGetComponentHeight(1);
        const int cr_width = njGetComponentWidth(2);
        const int cr_height = njGetComponentHeight(2);
        if (cb_width != cr_width || cb_height != cr_height) {
            return SH264E_ERR_UNSUPPORTED_CONFIG;
        }
    }
    ctx->component_count = component_count;
    ctx->one_to_one_420 = streaming_is_one_to_one_420();
    ctx->effective_slice_work_bytes = resize_scaled_slice_buffer_size();
    if (ctx->one_to_one_420) {
        ctx->effective_slice_work_bytes = pixfmt == SH264E_PIXFMT_NV12
                                              ? (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT
                                              : 0u;
    }
    if (!ctx->measure_only && ctx->effective_slice_work_bytes != 0u) {
        if (ctx->work_buffer == NULL) {
            return SH264E_ERR_INVALID_ARGUMENT;
        }
        if (ctx->work_buffer_capacity < ctx->effective_slice_work_bytes) {
            return SH264E_ERR_BUFFER_TOO_SMALL;
        }
    }

    for (i = 0u; i < (unsigned)component_count; i++) {
        sh264e_jpeg_stream_component_t *component = &ctx->components[i];
        const int width = njGetComponentWidth((int)i);
        const int height = njGetComponentHeight((int)i);
        const int stride = njGetComponentStride((int)i);
        const int rows_per_mcu = njGetComponentSsy((int)i) * 8;
        size_t bytes;

        if (width <= 0 || height <= 0 || stride < width || rows_per_mcu <= 0) {
            return SH264E_ERR_UNSUPPORTED_CONFIG;
        }
        component->width = (uint32_t)width;
        component->height = (uint32_t)height;
        component->stride = (ptrdiff_t)stride;
        component->rows_per_mcu = (uint32_t)rows_per_mcu;
        component->row0 = 0u;
        component->rows = 0u;
        component->cache_rows = ctx->one_to_one_420
                                    ? component->rows_per_mcu
                                    : streaming_cache_rows_for_slice_window(
                                          component->height,
                                          i == 0u ? SH264E_V1_HEIGHT : SH264E_V1_HEIGHT / 2u,
                                          i == 0u ? SH264E_V1_SLICE_LUMA_HEIGHT :
                                                    SH264E_V1_SLICE_CHROMA_HEIGHT,
                                          component->rows_per_mcu);
        bytes = (size_t)stride * (size_t)component->cache_rows;
        if (bytes > (size_t)INT_MAX) {
            streaming_free_cache(ctx);
            return SH264E_ERR_UNSUPPORTED_CONFIG;
        }
        component->pixels = (uint8_t *)njAllocMem((int)bytes);
        if (component->pixels == NULL) {
            sh264e_status_t status = sh264e_jpeg_alloc_arena_failed != 0u ?
                                     SH264E_ERR_BUFFER_TOO_SMALL :
                                     SH264E_ERR_ALLOCATION_FAILED;
            streaming_free_cache(ctx);
            return status;
        }
        ctx->cache_bytes += bytes;
    }
    ctx->initialized = 1;
    return SH264E_OK;
}

static sh264e_status_t streaming_copy_current_mcu_row(sh264e_jpeg_stream_context_t *ctx, int mcu_y)
{
    unsigned i;

    for (i = 0u; i < (unsigned)ctx->component_count; i++) {
        sh264e_jpeg_stream_component_t *component = &ctx->components[i];
        const uint8_t *src = njGetComponentPixels((int)i);
        const uint32_t new_row0 = (uint32_t)mcu_y * component->rows_per_mcu;
        uint32_t rows_to_copy = component->rows_per_mcu;

        if (src == NULL || component->pixels == NULL || component->cache_rows < component->rows_per_mcu) {
            return SH264E_ERR_INTERNAL;
        }
        if (new_row0 >= component->height) {
            continue;
        }
        if (new_row0 + rows_to_copy > component->height) {
            rows_to_copy = component->height - new_row0;
        }
        if (component->rows != 0u && component->row0 + component->rows != new_row0) {
            return SH264E_ERR_INTERNAL;
        }
        while (component->rows + rows_to_copy > component->cache_rows) {
            const uint32_t drop_rows = component->rows < component->rows_per_mcu
                                           ? component->rows
                                           : component->rows_per_mcu;
            if (drop_rows == 0u || drop_rows > component->rows) {
                return SH264E_ERR_INTERNAL;
            }
            memmove(component->pixels,
                    component->pixels + (size_t)drop_rows * (size_t)component->stride,
                    (size_t)(component->rows - drop_rows) * (size_t)component->stride);
            component->row0 += drop_rows;
            component->rows -= drop_rows;
        }
        memcpy(component->pixels + (size_t)component->rows * (size_t)component->stride,
               src,
               (size_t)rows_to_copy * (size_t)component->stride);
        component->rows += rows_to_copy;
    }
    return SH264E_OK;
}

static uint8_t *streaming_output_ptr(sh264e_jpeg_stream_context_t *ctx)
{
    if (ctx->consumer != NULL) {
        return ctx->out;
    }
    return ctx->out + ctx->offset;
}

static size_t streaming_output_capacity(const sh264e_jpeg_stream_context_t *ctx)
{
    if (ctx->consumer != NULL) {
        return ctx->out_capacity;
    }
    if (ctx->offset > ctx->out_capacity) {
        return 0u;
    }
    return ctx->out_capacity - ctx->offset;
}

static sh264e_status_t streaming_emit_output(sh264e_jpeg_stream_context_t *ctx, size_t bytes)
{
    sh264e_status_t status;

    if (ctx->consumer != NULL) {
        status = ctx->consumer(ctx->consumer_user, ctx->out, bytes);
        if (status != SH264E_OK) {
            return status;
        }
    }
    ctx->offset += bytes;
    return SH264E_OK;
}

static sh264e_status_t streaming_begin_idr(sh264e_jpeg_stream_context_t *ctx, size_t *bytes)
{
    if (ctx->consumer != NULL) {
        return sh264e_begin_idr_to_consumer(ctx->encoder,
                                            ctx->out,
                                            ctx->out_capacity,
                                            bytes,
                                            ctx->consumer,
                                            ctx->consumer_user);
    }
    return sh264e_begin_idr(ctx->encoder,
                            streaming_output_ptr(ctx),
                            streaming_output_capacity(ctx),
                            bytes);
}

static sh264e_status_t streaming_encode_idr_slice(sh264e_jpeg_stream_context_t *ctx,
                                                  const sh264e_slice_t *slice,
                                                  size_t *bytes)
{
    if (ctx->consumer != NULL) {
        return sh264e_encode_idr_slice_to_consumer(ctx->encoder,
                                                   slice,
                                                   ctx->out,
                                                   ctx->out_capacity,
                                                   bytes,
                                                   ctx->consumer,
                                                   ctx->consumer_user);
    }
    return sh264e_encode_idr_slice(ctx->encoder, slice,
                                   streaming_output_ptr(ctx),
                                   streaming_output_capacity(ctx),
                                   bytes);
}

static int streaming_mcu_row_ready(int mcu_y, void *user)
{
    sh264e_jpeg_stream_context_t *ctx = (sh264e_jpeg_stream_context_t *)user;

    if (ctx == NULL || ctx->status != SH264E_OK) {
        return 1;
    }
    if (!ctx->initialized) {
        ctx->status = streaming_init_cache(ctx);
        if (ctx->status != SH264E_OK) {
            return 1;
        }
    }

    ctx->status = streaming_copy_current_mcu_row(ctx, mcu_y);
    if (ctx->status != SH264E_OK) {
        return 1;
    }
    if (ctx->measure_only) {
        return 0;
    }
    if (!ctx->idr_started) {
        size_t bytes = 0u;
        ctx->status = streaming_begin_idr(ctx, &bytes);
        if (ctx->status == SH264E_OK && ctx->consumer == NULL) {
            ctx->status = streaming_emit_output(ctx, bytes);
        } else if (ctx->status == SH264E_OK) {
            ctx->offset += bytes;
        }
        if (ctx->status != SH264E_OK) {
            return 1;
        }
        ctx->idr_started = 1u;
    }

    while (ctx->next_slice < SH264E_V1_SLICE_COUNT &&
           streaming_slice_available(ctx, ctx->next_slice)) {
        sh264e_slice_t slice;
        size_t bytes = 0u;

        if (ctx->encoder->config.pixfmt == SH264E_PIXFMT_I420) {
            streaming_make_i420_slice(ctx, ctx->next_slice, &slice);
        } else {
            streaming_make_nv12_slice(ctx, ctx->next_slice, &slice);
        }
        ctx->status = streaming_encode_idr_slice(ctx, &slice, &bytes);
        if (ctx->status == SH264E_OK && ctx->consumer == NULL) {
            ctx->status = streaming_emit_output(ctx, bytes);
        } else if (ctx->status == SH264E_OK) {
            ctx->offset += bytes;
        }
        if (ctx->status != SH264E_OK) {
            return 1;
        }
        ctx->next_slice++;
    }
    return 0;
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
    const int exact_mode = exact_scale_ratio_mode(src_width, src_height, dst_width, dst_height);
    sh264e_axis_mapper_t y_mapper = scale_axis_mapper_init(src_height, dst_height, dst_y_start);
    uint32_t y;

    for (y = 0; y < dst_rows; y++) {
        uint8_t *dst_row = dst + (size_t)y * (size_t)dst_stride;
        const uint32_t dst_y = dst_y_start + y;
        const sh264e_scale_coord_t sy = exact_mode != 0 ?
                                        exact_scale_coord(exact_mode, dst_y, dst_height, src_height) :
                                        scale_coord_from_raw(y_mapper.pos, src_height);
        sh264e_axis_mapper_t x_mapper = scale_axis_mapper_init(src_width, dst_width, 0u);
        uint32_t x;

        for (x = 0; x < dst_width; x++) {
            const sh264e_scale_coord_t sx = exact_mode != 0 ?
                                            exact_scale_coord(exact_mode, x, dst_width, src_width) :
                                            scale_coord_from_raw(x_mapper.pos, src_width);
            dst_row[x] = bilinear_sample_plane_mapped(src, src_width, src_height, src_stride, sx, sy);
            if (exact_mode == 0) {
                scale_axis_mapper_advance(&x_mapper);
            }
        }
        if (exact_mode == 0) {
            scale_axis_mapper_advance(&y_mapper);
        }
    }
}

static void resize_scale_nv12_chroma_slice(const uint8_t *src_uv,
                                           uint32_t src_chroma_width,
                                           uint32_t src_chroma_height,
                                           ptrdiff_t src_stride,
                                           uint8_t *dst_uv,
                                           uint32_t dst_y_start)
{
    const int exact_mode = exact_scale_ratio_mode(src_chroma_width,
                                                 src_chroma_height,
                                                 SH264E_CHROMA_WIDTH,
                                                 SH264E_V1_HEIGHT / 2u);
    sh264e_axis_mapper_t y_mapper = scale_axis_mapper_init(src_chroma_height,
                                                           SH264E_V1_HEIGHT / 2u,
                                                           dst_y_start);
    uint32_t y;

    for (y = 0; y < SH264E_V1_SLICE_CHROMA_HEIGHT; y++) {
        uint8_t *dst_row = dst_uv + (size_t)y * SH264E_V1_WIDTH;
        const uint32_t dst_y = dst_y_start + y;
        const sh264e_scale_coord_t sy = exact_mode != 0 ?
                                        exact_scale_coord(exact_mode,
                                                          dst_y,
                                                          SH264E_V1_HEIGHT / 2u,
                                                          src_chroma_height) :
                                        scale_coord_from_raw(y_mapper.pos, src_chroma_height);
        sh264e_axis_mapper_t x_mapper = scale_axis_mapper_init(src_chroma_width, SH264E_CHROMA_WIDTH, 0u);
        uint32_t x;

        for (x = 0; x < SH264E_CHROMA_WIDTH; x++) {
            const sh264e_scale_coord_t sx = exact_mode != 0 ?
                                            exact_scale_coord(exact_mode,
                                                              x,
                                                              SH264E_CHROMA_WIDTH,
                                                              src_chroma_width) :
                                            scale_coord_from_raw(x_mapper.pos, src_chroma_width);
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
            if (exact_mode == 0) {
                scale_axis_mapper_advance(&x_mapper);
            }
        }
        if (exact_mode == 0) {
            scale_axis_mapper_advance(&y_mapper);
        }
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

static sh264e_status_t chunk_writer_flush(sh264e_chunk_writer_t *writer)
{
    sh264e_status_t status;

    if (writer->size == 0u) {
        return SH264E_OK;
    }
    status = writer->consumer(writer->user, writer->buffer, writer->size);
    if (status != SH264E_OK) {
        return status;
    }
    writer->total += writer->size;
    writer->size = 0u;
    return SH264E_OK;
}

static sh264e_status_t chunk_writer_put_byte(sh264e_chunk_writer_t *writer, uint8_t value)
{
    sh264e_status_t status;

    if (writer->size == writer->capacity) {
        status = chunk_writer_flush(writer);
        if (status != SH264E_OK) {
            return status;
        }
    }
    writer->buffer[writer->size++] = value;
    return SH264E_OK;
}

static sh264e_status_t stream_annexb_nalu(uint8_t *chunk_buffer,
                                          size_t chunk_capacity,
                                          size_t *out_size,
                                          sh264e_output_consumer_t consumer,
                                          void *consumer_user,
                                          uint8_t nal_header,
                                          const uint8_t *rbsp,
                                          size_t rbsp_size)
{
    sh264e_chunk_writer_t writer;
    size_t i;
    unsigned zero_count = 0;
    sh264e_status_t status;

    if (chunk_buffer == NULL || out_size == NULL || consumer == NULL ||
        rbsp == NULL || chunk_capacity == 0u) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }

    memset(&writer, 0, sizeof(writer));
    writer.buffer = chunk_buffer;
    writer.capacity = chunk_capacity;
    writer.consumer = consumer;
    writer.user = consumer_user;

#define SH264E_TRY_PUT_BYTE(v)            \
    do {                                  \
        status = chunk_writer_put_byte(&writer, (uint8_t)(v)); \
        if (status != SH264E_OK) {        \
            return status;                \
        }                                 \
    } while (0)

    SH264E_TRY_PUT_BYTE(0x00u);
    SH264E_TRY_PUT_BYTE(0x00u);
    SH264E_TRY_PUT_BYTE(0x01u);
    SH264E_TRY_PUT_BYTE(nal_header);

    for (i = 0; i < rbsp_size; i++) {
        const uint8_t b = rbsp[i];
        if (zero_count >= 2u && b <= 0x03u) {
            SH264E_TRY_PUT_BYTE(0x03u);
            zero_count = 0;
        }
        SH264E_TRY_PUT_BYTE(b);
        if (b == 0x00u) {
            zero_count++;
        } else {
            zero_count = 0;
        }
    }

#undef SH264E_TRY_PUT_BYTE

    status = chunk_writer_flush(&writer);
    if (status != SH264E_OK) {
        return status;
    }
    *out_size = writer.total;
    return SH264E_OK;
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

static sh264e_status_t write_sps_rbsp(sh264e_encoder_t *encoder, size_t *out_rbsp_size)
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
    *out_rbsp_size = bw_size(&bw);
    return SH264E_OK;
}

static sh264e_status_t make_sps(sh264e_encoder_t *encoder,
                                uint8_t *out,
                                size_t capacity,
                                size_t *offset)
{
    sh264e_status_t status;
    size_t rbsp_size = 0u;

    status = write_sps_rbsp(encoder, &rbsp_size);
    if (status != SH264E_OK) {
        return status;
    }
    return append_annexb_nalu(out, capacity, offset, 0x67u, encoder->rbsp, rbsp_size);
}

static sh264e_status_t write_pps_rbsp(sh264e_encoder_t *encoder, size_t *out_rbsp_size)
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
    *out_rbsp_size = bw_size(&bw);
    return SH264E_OK;
}

static sh264e_status_t make_pps(sh264e_encoder_t *encoder,
                                uint8_t *out,
                                size_t capacity,
                                size_t *offset)
{
    sh264e_status_t status;
    size_t rbsp_size = 0u;

    status = write_pps_rbsp(encoder, &rbsp_size);
    if (status != SH264E_OK) {
        return status;
    }
    return append_annexb_nalu(out, capacity, offset, 0x68u, encoder->rbsp, rbsp_size);
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
                          unsigned mb_x,
                          unsigned x,
                          unsigned y)
{
    unsigned row;
    unsigned col;
    int sum_delta = 0;
    const unsigned src_x = mb_x * SH264E_MB_SIZE + x;
    const int pred = 128;
    int level;

    for (row = 0; row < 4u; row++) {
        const uint8_t *src = slice_luma_row(slice, y + row) + src_x;
        for (col = 0; col < 4u; col++) {
            sum_delta += (int)src[col] - pred;
        }
    }

    level = quantize_dc_delta((sum_delta + (sum_delta >= 0 ? 8 : -8)) / 16, encoder->config.qp);

    return level;
}

static int encode_chroma8x8_dc(sh264e_encoder_t *encoder,
                               const sh264e_slice_t *slice,
                               unsigned plane,
                               unsigned mb_x,
                               unsigned x,
                               unsigned y)
{
    unsigned row;
    unsigned col;
    int sum_delta = 0;
    const int pred = 128;
    int level;
    const unsigned src_x = mb_x * (SH264E_MB_SIZE / 2u) + x;

    for (row = 0; row < 8u; row++) {
        for (col = 0; col < 8u; col++) {
            sum_delta += (int)slice_chroma_sample(slice, plane, src_x + col, y + row) - pred;
        }
    }

    level = quantize_dc_delta((sum_delta + (sum_delta >= 0 ? 32 : -32)) / 64, encoder->config.qp);
    if (level > 0) {
        level = 1;
    } else if (level < 0) {
        level = -1;
    }

    return level;
}

static unsigned cavlc_coeff_token_table_for_nc(unsigned nC)
{
    if (nC < 2u) {
        return 0u;
    }
    if (nC < 4u) {
        return 1u;
    }
    if (nC < 8u) {
        return 2u;
    }
    return 3u;
}

static void write_coeff_token_one_or_zero(sh264e_bit_writer_t *bw, int has_coeff, unsigned nC)
{
    static const uint8_t len[4][2] = {
        {1, 6},
        {2, 6},
        {4, 6},
        {6, 6}
    };
    static const uint8_t bits[4][2] = {
        {1, 5},
        {3, 11},
        {15, 15},
        {3, 0}
    };
    const unsigned table = cavlc_coeff_token_table_for_nc(nC);
    const unsigned index = has_coeff != 0 ? 1u : 0u;

    bw_write_bits(bw, bits[table][index], len[table][index]);
}

static int write_cavlc_level(sh264e_bit_writer_t *bw, int level);

static void write_luma_residual_dc_only(sh264e_bit_writer_t *bw, int level, unsigned nC)
{
    if (level == 0) {
        write_coeff_token_one_or_zero(bw, 0, nC);
        return;
    }
    if (level == 1) {
        level = 2;
    } else if (level == -1) {
        level = -2;
    }
    write_coeff_token_one_or_zero(bw, 1, nC);
    if (!write_cavlc_level(bw, level)) {
        bw->error = 1;
        return;
    }
    bw_write_bit(bw, 1);                 /* total_zeros = 0 for TotalCoeff=1 */
}

static int write_cavlc_level(sh264e_bit_writer_t *bw, int level)
{
    const uint32_t sign = level < 0 ? 1u : 0u;
    const uint32_t abs_level = (uint32_t)(level < 0 ? -level : level);
    const uint32_t final_level_code = (abs_level * 2u) - 2u + sign;
    const uint32_t target = final_level_code - 2u;
    unsigned prefix;
    unsigned suffix_size = 0u;
    uint32_t suffix = 0u;
    unsigned i;

    if (target < 14u) {
        prefix = (unsigned)target;
    } else if (target < 30u) {
        prefix = 14u;
        suffix_size = 4u;
        suffix = target - 14u;
    } else if (target <= 4125u) {
        prefix = 15u;
        suffix_size = 12u;
        suffix = target - 30u;
    } else {
        return 0;
    }

    for (i = 0; i < prefix; i++) {
        bw_write_bit(bw, 0);
    }
    bw_write_bit(bw, 1);
    if (suffix_size > 0u) {
        bw_write_bits(bw, suffix, suffix_size);
    }
    return 1;
}

static void write_chroma_dc_residual(sh264e_bit_writer_t *bw, int level)
{
    if (level == 0) {
        bw_write_bits(bw, 0x01u, 2);     /* TotalCoeff=0 for chroma DC */
        return;
    }
    bw_write_bit(bw, 1);                 /* TotalCoeff=1, TrailingOnes=1 for chroma DC */
    bw_write_bit(bw, level < 0 ? 1u : 0u);
    bw_write_bit(bw, 1);                 /* total_zeros = 0 for TotalCoeff=1 */
}

static sh264e_status_t write_idr_mb_slice_rbsp(sh264e_encoder_t *encoder,
                                               const sh264e_slice_t *slice,
                                               unsigned row_index,
                                               unsigned mb_x,
                                               size_t *out_rbsp_size)
{
    sh264e_bit_writer_t bw;
    int levels[16];
    int chroma_dc[2];
    unsigned b;
    unsigned cbp_luma = 0;
    unsigned cbp_chroma;
    unsigned cbp;

    bw_init(&bw, encoder->rbsp, encoder->rbsp_capacity);

    bw_write_ue(&bw, row_index * SH264E_MBS_X + mb_x);
    bw_write_ue(&bw, 7);                 /* slice_type: all I slices */
    bw_write_ue(&bw, 0);                 /* pic_parameter_set_id */
    bw_write_bits(&bw, 0, 4);            /* frame_num */
    bw_write_ue(&bw, 0);                 /* idr_pic_id */
    bw_write_bits(&bw, 0, 4);            /* pic_order_cnt_lsb */
    bw_write_bit(&bw, 0);                /* no_output_of_prior_pics_flag */
    bw_write_bit(&bw, 0);                /* long_term_reference_flag */
    bw_write_se(&bw, 0);                 /* slice_qp_delta */
    bw_write_ue(&bw, 1);                 /* disable_deblocking_filter_idc */

    for (b = 0; b < 16u; b++) {
        const unsigned x = k_luma4x4_x[b];
        const unsigned y = k_luma4x4_y[b];
        const int level = encode_luma4x4(encoder, slice, mb_x, x, y);
        levels[b] = level;
        if (level != 0) {
            cbp_luma |= 1u << (b / 4u);
        }
    }

    chroma_dc[0] = encode_chroma8x8_dc(encoder, slice, 1u, mb_x, 0u, 0u);
    chroma_dc[1] = encode_chroma8x8_dc(encoder, slice, 2u, mb_x, 0u, 0u);
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
                write_luma_residual_dc_only(&bw, levels[b], 0u);
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

    bw_rbsp_trailing_bits(&bw);
    if (bw.error != 0) {
        return SH264E_ERR_INTERNAL;
    }
    *out_rbsp_size = bw_size(&bw);
    return SH264E_OK;
}

static sh264e_status_t make_idr_slice(sh264e_encoder_t *encoder,
                                      const sh264e_slice_t *slice,
                                      unsigned row_index,
                                      unsigned mb_x,
                                      uint8_t *out,
                                      size_t capacity,
                                      size_t *offset)
{
    sh264e_status_t status;
    size_t rbsp_size = 0u;

    status = write_idr_mb_slice_rbsp(encoder, slice, row_index, mb_x, &rbsp_size);
    if (status != SH264E_OK) {
        return status;
    }
    return append_annexb_nalu(out, capacity, offset, 0x65u, encoder->rbsp, rbsp_size);
}

static sh264e_status_t map_jpeg_result(int result)
{
    switch (result) {
    case NJ_OK:
        return SH264E_OK;
    case NJ_OUT_OF_MEM:
        if (sh264e_jpeg_alloc_arena_failed != 0u) {
            return SH264E_ERR_BUFFER_TOO_SMALL;
        }
        return SH264E_ERR_ALLOCATION_FAILED;
    case NJ_INTERNAL_ERR:
    case NJ_CALLBACK_ABORT:
        return SH264E_ERR_INTERNAL;
    case NJ_NO_JPEG:
    case NJ_UNSUPPORTED:
    case NJ_SYNTAX_ERROR:
    default:
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
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

sh264e_status_t sh264e_jpeg_get_last_allocation_stats(sh264e_jpeg_allocation_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    out_stats->current_bytes = sh264e_jpeg_alloc_current_bytes;
    out_stats->peak_bytes = sh264e_jpeg_alloc_peak_bytes;
    return SH264E_OK;
}

typedef struct sh264e_nj_source_adapter_t {
    const sh264e_jpeg_source_t *source;
    sh264e_status_t status;
} sh264e_nj_source_adapter_t;

static int sh264e_nj_source_read(void *user,
                                 unsigned char *dst,
                                 int requested,
                                 int *out_read)
{
    sh264e_nj_source_adapter_t *adapter = (sh264e_nj_source_adapter_t *)user;
    sh264e_jpeg_read_fn source_reader;
    size_t read_bytes = 0u;
    sh264e_status_t status;

    if (adapter == NULL || adapter->source == NULL || adapter->source->read == NULL ||
        dst == NULL || requested <= 0 || out_read == NULL) {
        if (adapter != NULL) {
            adapter->status = SH264E_ERR_INVALID_ARGUMENT;
        }
        return -1;
    }

    source_reader = adapter->source->read;
    status = source_reader(adapter->source->user,
                           dst,
                           (size_t)requested,
                           &read_bytes);
    if (status != SH264E_OK) {
        adapter->status = status;
        return -1;
    }
    if (read_bytes > (size_t)requested) {
        adapter->status = SH264E_ERR_INVALID_ARGUMENT;
        return -1;
    }
    *out_read = (int)read_bytes;
    return 0;
}

static sh264e_status_t decode_mcu_rows_from_input(const uint8_t *jpeg_data,
                                                  size_t jpeg_size,
                                                  const sh264e_jpeg_source_t *jpeg_source,
                                                  nj_mcu_row_callback_t callback,
                                                  void *user,
                                                  sh264e_status_t *source_status)
{
    if (source_status != NULL) {
        *source_status = SH264E_OK;
    }
    if (jpeg_source != NULL) {
        sh264e_nj_source_adapter_t adapter;
        nj_input_source_t nj_source;

        if (jpeg_source->read == NULL) {
            return SH264E_ERR_INVALID_ARGUMENT;
        }
        adapter.source = jpeg_source;
        adapter.status = SH264E_OK;
        nj_source.read = sh264e_nj_source_read;
        nj_source.user = &adapter;
        {
            sh264e_status_t status = map_jpeg_result(
                njDecodeMcuRowsFromSource(&nj_source, callback, user));
            if (adapter.status != SH264E_OK) {
                status = adapter.status;
            }
            if (source_status != NULL) {
                *source_status = adapter.status;
            }
            return status;
        }
    }

    if (jpeg_data == NULL || jpeg_size == 0u || jpeg_size > (size_t)INT_MAX) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    return map_jpeg_result(njDecodeMcuRows(jpeg_data, (int)jpeg_size,
                                           callback, user));
}

static sh264e_status_t jpeg_measure_streaming_requirements(const uint8_t *jpeg_data,
                                                           size_t jpeg_size,
                                                           const sh264e_jpeg_source_t *jpeg_source,
                                                           sh264e_pixfmt_t pixfmt,
                                                           size_t *out_arena_size,
                                                           size_t *out_slice_work_size)
{
    sh264e_jpeg_stream_context_t ctx;
    sh264e_status_t status;

    if ((jpeg_data == NULL && jpeg_source == NULL) ||
        (out_arena_size == NULL && out_slice_work_size == NULL)) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (!is_supported_pixfmt(pixfmt)) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    if (out_arena_size != NULL) {
        *out_arena_size = 0u;
    }
    if (out_slice_work_size != NULL) {
        *out_slice_work_size = 0u;
    }
    sh264e_jpeg_streaming_last_cache_bytes = 0u;
    sh264e_jpeg_streaming_last_slice_work_bytes = 0u;
    if (jpeg_source == NULL && (jpeg_size == 0u || jpeg_size > (size_t)INT_MAX)) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.measure_only = 1;
    ctx.output_pixfmt = pixfmt;
    ctx.status = SH264E_OK;

    jpeg_allocation_begin_heap();
    njInit();
    status = decode_mcu_rows_from_input(jpeg_data, jpeg_size, jpeg_source,
                                        streaming_mcu_row_ready, &ctx, NULL);
    if (ctx.status != SH264E_OK) {
        status = ctx.status;
    }
    if (status == SH264E_OK) {
        if (out_arena_size != NULL) {
            *out_arena_size = sh264e_jpeg_alloc_peak_arena_bytes;
        }
        if (out_slice_work_size != NULL) {
            *out_slice_work_size = ctx.effective_slice_work_bytes;
            sh264e_jpeg_streaming_last_slice_work_bytes = ctx.effective_slice_work_bytes;
        }
    }
    sh264e_jpeg_streaming_last_cache_bytes = ctx.cache_bytes;
    streaming_free_cache(&ctx);
    njDone();
    jpeg_allocation_end();
    return status;
}

static sh264e_status_t jpeg_get_streaming_work_size(const uint8_t *jpeg_data,
                                                    size_t jpeg_size,
                                                    size_t *out_size)
{
    return jpeg_measure_streaming_requirements(jpeg_data, jpeg_size,
                                               NULL,
                                               SH264E_PIXFMT_I420,
                                               out_size, NULL);
}

sh264e_status_t sh264e_jpeg_get_work_size(const uint8_t *jpeg_data,
                                          size_t jpeg_size,
                                          size_t *out_size)
{
    return jpeg_get_streaming_work_size(jpeg_data, jpeg_size, out_size);
}

sh264e_status_t sh264e_jpeg_get_slice_work_size(const uint8_t *jpeg_data,
                                                size_t jpeg_size,
                                                sh264e_pixfmt_t pixfmt,
                                                size_t *out_size)
{
    return jpeg_measure_streaming_requirements(jpeg_data, jpeg_size,
                                               NULL,
                                               pixfmt,
                                               NULL, out_size);
}

sh264e_status_t sh264e_jpeg_source_get_work_size(const sh264e_jpeg_source_t *source,
                                                 size_t *out_size)
{
    return jpeg_measure_streaming_requirements(NULL, 0u,
                                               source,
                                               SH264E_PIXFMT_I420,
                                               out_size, NULL);
}

sh264e_status_t sh264e_jpeg_source_get_slice_work_size(const sh264e_jpeg_source_t *source,
                                                       sh264e_pixfmt_t pixfmt,
                                                       size_t *out_size)
{
    return jpeg_measure_streaming_requirements(NULL, 0u,
                                               source,
                                               pixfmt,
                                               NULL, out_size);
}

#if SH264E_ENABLE_JPEG_TEST_HOOKS

void sh264e_jpeg_set_test_allocation_limit(size_t max_bytes)
{
    sh264e_jpeg_alloc_limit = max_bytes;
}

#endif

sh264e_status_t sh264e_encode_jpeg_idr(sh264e_encoder_t *encoder,
                                       const uint8_t *jpeg_data,
                                       size_t jpeg_size,
                                       uint8_t *work_buffer,
                                       size_t work_buffer_capacity,
                                       uint8_t *out,
                                       size_t out_capacity,
                                       size_t *out_size)
{
    return sh264e_encode_jpeg_idr_streaming_impl(encoder, jpeg_data, jpeg_size,
                                                 NULL,
                                                 NULL, 0u, 0,
                                                 work_buffer, work_buffer_capacity,
                                                 out, out_capacity, out_size,
                                                 NULL, NULL);
}

sh264e_status_t sh264e_encode_jpeg_idr_with_arena(sh264e_encoder_t *encoder,
                                                  const uint8_t *jpeg_data,
                                                  size_t jpeg_size,
                                                  uint8_t *jpeg_arena,
                                                  size_t jpeg_arena_size,
                                                  uint8_t *work_buffer,
                                                  size_t work_buffer_capacity,
                                                  uint8_t *out,
                                                  size_t out_capacity,
                                                  size_t *out_size)
{
    return sh264e_encode_jpeg_idr_streaming_impl(encoder, jpeg_data, jpeg_size,
                                                 NULL,
                                                 jpeg_arena, jpeg_arena_size, 1,
                                                 work_buffer, work_buffer_capacity,
                                                 out, out_capacity, out_size,
                                                 NULL, NULL);
}

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
    void *consumer_user)
{
    size_t ignored_size = 0u;

    if (consumer == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    return sh264e_encode_jpeg_idr_streaming_impl(encoder, jpeg_data, jpeg_size,
                                                 NULL,
                                                 jpeg_arena, jpeg_arena_size, 1,
                                                 work_buffer, work_buffer_capacity,
                                                 out_buffer, out_buffer_capacity,
                                                 &ignored_size,
                                                 consumer, consumer_user);
}

sh264e_status_t sh264e_encode_jpeg_source_idr_with_arena_stream(
    sh264e_encoder_t *encoder,
    const sh264e_jpeg_source_t *source,
    uint8_t *jpeg_arena,
    size_t jpeg_arena_size,
    uint8_t *work_buffer,
    size_t work_buffer_capacity,
    uint8_t *out_buffer,
    size_t out_buffer_capacity,
    sh264e_output_consumer_t consumer,
    void *consumer_user)
{
    size_t ignored_size = 0u;

    if (consumer == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    return sh264e_encode_jpeg_idr_streaming_impl(encoder, NULL, 0u,
                                                 source,
                                                 jpeg_arena, jpeg_arena_size, 1,
                                                 work_buffer, work_buffer_capacity,
                                                 out_buffer, out_buffer_capacity,
                                                 &ignored_size,
                                                 consumer, consumer_user);
}

size_t sh264e_jpeg_get_last_streaming_cache_bytes(void)
{
    return sh264e_jpeg_streaming_last_cache_bytes;
}

size_t sh264e_jpeg_get_last_slice_work_bytes(void)
{
    return sh264e_jpeg_streaming_last_slice_work_bytes;
}

static sh264e_status_t sh264e_encode_jpeg_idr_streaming_impl(sh264e_encoder_t *encoder,
                                                             const uint8_t *jpeg_data,
                                                             size_t jpeg_size,
                                                             const sh264e_jpeg_source_t *jpeg_source,
                                                             uint8_t *jpeg_arena,
                                                             size_t jpeg_arena_size,
                                                             int use_arena,
                                                             uint8_t *work_buffer,
                                                             size_t work_buffer_capacity,
                                                             uint8_t *out,
                                                             size_t out_capacity,
                                                             size_t *out_size,
                                                             sh264e_output_consumer_t consumer,
                                                             void *consumer_user)
{
    sh264e_jpeg_stream_context_t ctx;
    sh264e_status_t status;

    if (encoder == NULL || (jpeg_data == NULL && jpeg_source == NULL) ||
        out == NULL || out_size == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_size = 0u;
    sh264e_jpeg_streaming_last_cache_bytes = 0u;
    sh264e_jpeg_streaming_last_slice_work_bytes = 0u;
    if (encoder_idr_active(encoder)) {
        return SH264E_ERR_BAD_STATE;
    }
    if (jpeg_source == NULL && (jpeg_size == 0u || jpeg_size > (size_t)INT_MAX)) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (use_arena != 0 && (jpeg_arena == NULL || jpeg_arena_size == 0u)) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.encoder = encoder;
    ctx.work_buffer = work_buffer;
    ctx.work_buffer_capacity = work_buffer_capacity;
    ctx.out = out;
    ctx.out_capacity = out_capacity;
    ctx.consumer = consumer;
    ctx.consumer_user = consumer_user;
    ctx.status = SH264E_OK;

    if (use_arena != 0) {
        jpeg_allocation_begin_arena(jpeg_arena, jpeg_arena_size);
    } else {
        jpeg_allocation_begin_heap();
    }
    njInit();
    status = decode_mcu_rows_from_input(jpeg_data, jpeg_size, jpeg_source,
                                        streaming_mcu_row_ready, &ctx, NULL);
    if (ctx.status != SH264E_OK) {
        status = ctx.status;
    }
    if (status == SH264E_OK && ctx.next_slice != SH264E_V1_SLICE_COUNT) {
        status = SH264E_ERR_INTERNAL;
    }
    if (status == SH264E_OK) {
        size_t bytes = 0u;
        status = sh264e_end_idr(encoder);
        if (status == SH264E_OK) {
            ctx.offset += bytes;
            *out_size = ctx.offset;
        }
    }
    if (status != SH264E_OK) {
        reset_progressive_state(encoder);
    }
    sh264e_jpeg_streaming_last_cache_bytes = ctx.cache_bytes;
    sh264e_jpeg_streaming_last_slice_work_bytes = ctx.effective_slice_work_bytes;
    streaming_free_cache(&ctx);
    njDone();
    jpeg_allocation_end();
    return status;
}

#if SH264E_ENABLE_JPEG_TEST_HOOKS

sh264e_status_t sh264e_encode_jpeg_idr_streaming_prototype(sh264e_encoder_t *encoder,
                                                           const uint8_t *jpeg_data,
                                                           size_t jpeg_size,
                                                           uint8_t *work_buffer,
                                                           size_t work_buffer_capacity,
                                                           uint8_t *out,
                                                           size_t out_capacity,
                                                           size_t *out_size)
{
    return sh264e_encode_jpeg_idr_streaming_impl(encoder, jpeg_data, jpeg_size,
                                                 NULL,
                                                 NULL, 0u, 0,
                                                 work_buffer, work_buffer_capacity,
                                                 out, out_capacity, out_size,
                                                 NULL, NULL);
}

sh264e_status_t sh264e_encode_jpeg_idr_streaming_prototype_with_arena(sh264e_encoder_t *encoder,
                                                                      const uint8_t *jpeg_data,
                                                                      size_t jpeg_size,
                                                                      uint8_t *jpeg_arena,
                                                                      size_t jpeg_arena_size,
                                                                      uint8_t *work_buffer,
                                                                      size_t work_buffer_capacity,
                                                                      uint8_t *out,
                                                                      size_t out_capacity,
                                                                      size_t *out_size)
{
    return sh264e_encode_jpeg_idr_streaming_impl(encoder, jpeg_data, jpeg_size,
                                                 NULL,
                                                 jpeg_arena, jpeg_arena_size, 1,
                                                 work_buffer, work_buffer_capacity,
                                                 out, out_capacity, out_size,
                                                 NULL, NULL);
}

#endif

static sh264e_status_t normalize_encoder_config(const sh264e_config_t *config,
                                                sh264e_config_t *out_normalized,
                                                size_t *out_rbsp_scratch_size)
{
    sh264e_status_t status;
    sh264e_config_t normalized;

    if (config == NULL || out_normalized == NULL || out_rbsp_scratch_size == NULL) {
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

    *out_normalized = normalized;
    *out_rbsp_scratch_size = encoder_rbsp_scratch_bytes();
    return SH264E_OK;
}

static int checked_add_size(size_t a, size_t b, size_t *out)
{
    if (a > (size_t)-1 - b) {
        return 0;
    }
    *out = a + b;
    return 1;
}

static sh264e_status_t encoder_arena_work_size(size_t max_slice_output_size, size_t *out_size)
{
    size_t total = 0u;

    if (!checked_add_size(total, sizeof(sh264e_encoder_t), &total) ||
        !checked_add_size(total, max_slice_output_size, &total) ||
        !checked_add_size(total, encoder_recon_luma_bytes(), &total) ||
        !checked_add_size(total, encoder_recon_chroma_bytes(), &total) ||
        !checked_add_size(total, encoder_neighbor_state_bytes(), &total) ||
        !checked_add_size(total, SH264E_ENCODER_ARENA_ALIGNMENT - 1u, &total)) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    *out_size = total;
    return SH264E_OK;
}

sh264e_status_t sh264e_encoder_create(const sh264e_config_t *config,
                                      sh264e_encoder_t **out_encoder)
{
    sh264e_status_t status;
    sh264e_encoder_t *encoder;
    size_t rbsp_scratch_size = 0u;
    sh264e_config_t normalized;

    if (out_encoder == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_encoder = NULL;
    if (config == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }

    status = normalize_encoder_config(config, &normalized, &rbsp_scratch_size);
    if (status != SH264E_OK) {
        return status;
    }

    encoder = (sh264e_encoder_t *)calloc(1u, sizeof(*encoder));
    if (encoder == NULL) {
        return SH264E_ERR_ALLOCATION_FAILED;
    }
    encoder->config = normalized;
    encoder->flags = SH264E_ENCODER_FLAG_OWNS_MEMORY;
    encoder->rbsp_capacity = rbsp_scratch_size;
    encoder->rbsp = (uint8_t *)malloc(encoder->rbsp_capacity);

    if (encoder->rbsp == NULL) {
        sh264e_encoder_destroy(encoder);
        return SH264E_ERR_ALLOCATION_FAILED;
    }

    *out_encoder = encoder;
    return SH264E_OK;
}

sh264e_status_t sh264e_encoder_get_work_size(const sh264e_config_t *config,
                                             size_t *out_size)
{
    sh264e_status_t status;
    size_t rbsp_scratch_size = 0u;
    sh264e_config_t normalized;

    if (out_size == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_size = 0u;
    status = normalize_encoder_config(config, &normalized, &rbsp_scratch_size);
    if (status != SH264E_OK) {
        return status;
    }
    return encoder_arena_work_size(rbsp_scratch_size, out_size);
}

static sh264e_status_t encoder_arena_take(uint8_t **cursor,
                                          uintptr_t end,
                                          size_t size,
                                          uint8_t **out)
{
    const uintptr_t current = (uintptr_t)*cursor;

    if (current > end || size > end - current) {
        return SH264E_ERR_BUFFER_TOO_SMALL;
    }
    *out = *cursor;
    memset(*out, 0, size);
    *cursor += size;
    return SH264E_OK;
}

sh264e_status_t sh264e_encoder_create_with_arena(const sh264e_config_t *config,
                                                 void *arena,
                                                 size_t arena_size,
                                                 sh264e_encoder_t **out_encoder)
{
    sh264e_status_t status;
    sh264e_config_t normalized;
    sh264e_encoder_t *encoder;
    size_t rbsp_scratch_size = 0u;
    size_t required_size = 0u;
    uintptr_t start;
    uintptr_t aligned;
    uintptr_t end;
    size_t remainder;
    uint8_t *cursor;
    uint8_t *block;

    if (out_encoder == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_encoder = NULL;
    if (config == NULL || arena == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }

    status = normalize_encoder_config(config, &normalized, &rbsp_scratch_size);
    if (status != SH264E_OK) {
        return status;
    }
    status = encoder_arena_work_size(rbsp_scratch_size, &required_size);
    if (status != SH264E_OK) {
        return status;
    }
    if (arena_size < required_size) {
        return SH264E_ERR_BUFFER_TOO_SMALL;
    }

    start = (uintptr_t)arena;
    if (arena_size > UINTPTR_MAX - start) {
        return SH264E_ERR_BUFFER_TOO_SMALL;
    }
    end = start + arena_size;
    remainder = start % SH264E_ENCODER_ARENA_ALIGNMENT;
    aligned = remainder == 0u ? start : start + (SH264E_ENCODER_ARENA_ALIGNMENT - remainder);
    if (aligned < start || sizeof(*encoder) > end - aligned) {
        return SH264E_ERR_BUFFER_TOO_SMALL;
    }

    encoder = (sh264e_encoder_t *)aligned;
    memset(encoder, 0, sizeof(*encoder));
    cursor = (uint8_t *)encoder + sizeof(*encoder);

    status = encoder_arena_take(&cursor, end, rbsp_scratch_size, &block);
    if (status != SH264E_OK) {
        return status;
    }
    encoder->rbsp = block;

    encoder->config = normalized;
    encoder->rbsp_capacity = rbsp_scratch_size;
    *out_encoder = encoder;
    return SH264E_OK;
}

void sh264e_encoder_destroy(sh264e_encoder_t *encoder)
{
    if (encoder == NULL) {
        return;
    }
    if ((encoder->flags & SH264E_ENCODER_FLAG_OWNS_MEMORY) != 0u) {
        free(encoder->rbsp);
        free(encoder);
    }
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
    *out_size = (size_t)SH264E_MBS_X * SH264E_MAX_IDR_MB_NALU_OUTPUT_SIZE;
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

sh264e_status_t sh264e_encoder_get_memory_report(
    const sh264e_config_t *config,
    sh264e_encoder_memory_report_t *out_report)
{
    sh264e_status_t status;
    sh264e_config_t normalized;

    if (config == NULL || out_report == NULL) {
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

    memset(out_report, 0, sizeof(*out_report));
    out_report->context_bytes = sizeof(sh264e_encoder_t);
    out_report->bitstream_scratch_bytes = encoder_rbsp_scratch_bytes();
    out_report->recon_luma_bytes = encoder_recon_luma_bytes();
    out_report->recon_chroma_bytes = encoder_recon_chroma_bytes();
    out_report->neighbor_state_bytes = encoder_neighbor_state_bytes();
    out_report->total_bytes = out_report->context_bytes +
                              out_report->bitstream_scratch_bytes +
                              out_report->recon_luma_bytes +
                              out_report->recon_chroma_bytes +
                              out_report->neighbor_state_bytes;
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
    if (encoder_idr_active(encoder)) {
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

    encoder_set_idr_active(encoder, 1);
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
    if (!encoder_idr_active(encoder)) {
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

    {
        unsigned mb_x;
        for (mb_x = 0; mb_x < SH264E_MBS_X; mb_x++) {
            status = make_idr_slice(encoder, slice, encoder->slices_encoded, mb_x,
                                    out, out_capacity, &offset);
            if (status != SH264E_OK) {
                return status;
            }
        }
    }

    encoder->slices_encoded++;
    *out_size = offset;
    return SH264E_OK;
}

static sh264e_status_t sh264e_begin_idr_to_consumer(sh264e_encoder_t *encoder,
                                                    uint8_t *chunk_buffer,
                                                    size_t chunk_capacity,
                                                    size_t *out_size,
                                                    sh264e_output_consumer_t consumer,
                                                    void *consumer_user)
{
    sh264e_status_t status;
    size_t bytes = 0u;
    size_t total = 0u;

    if (encoder == NULL || chunk_buffer == NULL || out_size == NULL || consumer == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_size = 0u;
    if (encoder_idr_active(encoder)) {
        return SH264E_ERR_BAD_STATE;
    }
    if (chunk_capacity == 0u) {
        return SH264E_ERR_BUFFER_TOO_SMALL;
    }

    status = write_sps_rbsp(encoder, &bytes);
    if (status != SH264E_OK) {
        return status;
    }
    status = stream_annexb_nalu(chunk_buffer, chunk_capacity, &bytes,
                                consumer, consumer_user, 0x67u,
                                encoder->rbsp, bytes);
    if (status != SH264E_OK) {
        return status;
    }
    total += bytes;

    status = write_pps_rbsp(encoder, &bytes);
    if (status != SH264E_OK) {
        return status;
    }
    status = stream_annexb_nalu(chunk_buffer, chunk_capacity, &bytes,
                                consumer, consumer_user, 0x68u,
                                encoder->rbsp, bytes);
    if (status != SH264E_OK) {
        return status;
    }
    total += bytes;

    encoder_set_idr_active(encoder, 1);
    encoder->slices_encoded = 0u;
    *out_size = total;
    return SH264E_OK;
}

static sh264e_status_t sh264e_encode_idr_slice_to_consumer(sh264e_encoder_t *encoder,
                                                           const sh264e_slice_t *slice,
                                                           uint8_t *chunk_buffer,
                                                           size_t chunk_capacity,
                                                           size_t *out_size,
                                                           sh264e_output_consumer_t consumer,
                                                           void *consumer_user)
{
    sh264e_status_t status;
    size_t total = 0u;

    if (encoder == NULL || slice == NULL || chunk_buffer == NULL ||
        out_size == NULL || consumer == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_size = 0u;
    if (!encoder_idr_active(encoder)) {
        return SH264E_ERR_BAD_STATE;
    }
    if (encoder->slices_encoded >= SH264E_MBS_Y) {
        return SH264E_ERR_FRAME_COMPLETE;
    }
    status = validate_slice(&encoder->config, slice);
    if (status != SH264E_OK) {
        return status;
    }
    if (chunk_capacity == 0u) {
        return SH264E_ERR_BUFFER_TOO_SMALL;
    }

    {
        unsigned mb_x;
        for (mb_x = 0; mb_x < SH264E_MBS_X; mb_x++) {
            size_t rbsp_size = 0u;
            size_t nalu_size = 0u;
            status = write_idr_mb_slice_rbsp(encoder, slice, encoder->slices_encoded,
                                             mb_x, &rbsp_size);
            if (status != SH264E_OK) {
                return status;
            }
            status = stream_annexb_nalu(chunk_buffer, chunk_capacity, &nalu_size,
                                        consumer, consumer_user, 0x65u,
                                        encoder->rbsp, rbsp_size);
            if (status != SH264E_OK) {
                return status;
            }
            total += nalu_size;
        }
    }

    encoder->slices_encoded++;
    *out_size = total;
    return SH264E_OK;
}

sh264e_status_t sh264e_end_idr(sh264e_encoder_t *encoder)
{
    if (encoder == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (!encoder_idr_active(encoder)) {
        return SH264E_ERR_BAD_STATE;
    }
    if (encoder->slices_encoded != SH264E_MBS_Y) {
        return SH264E_ERR_INCOMPLETE_FRAME;
    }
    encoder_set_idr_active(encoder, 0);
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
        encoder_set_idr_active(encoder, 0);
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
    if (encoder_idr_active(encoder)) {
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
