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
#define SH264E_DC_PRED 128
#define SH264E_JPEG_EXACT_RESIZE_2X_MASK 1u
#define SH264E_JPEG_EXACT_RESIZE_HALF_MASK 2u

#if !defined(SH264E_DISABLE_ARM_DSP) && defined(__ARM_FEATURE_DSP) && defined(__GNUC__)
#define SH264E_USE_ARM_DSP 1
#endif

static unsigned sh264e_u32_floor_log2(uint32_t value)
{
    if (value == 0u) {
        return 0u;
    }
#if (defined(__GNUC__) || defined(__clang__)) && UINT_MAX == 0xffffffffu
    return 31u - (unsigned)__builtin_clz(value);
#else
    unsigned bits = 0u;
    while (value > 1u) {
        value >>= 1u;
        bits++;
    }
    return bits;
#endif
}

typedef struct sh264e_chunk_writer_t sh264e_chunk_writer_t;

typedef struct sh264e_bit_writer_t {
    uint8_t *data;
    size_t capacity;
    size_t byte_pos;
    uint8_t pending_byte;
    unsigned pending_bits;
    sh264e_chunk_writer_t *annexb_writer;
    unsigned annexb_zero_count;
    sh264e_status_t status;
    int error;
} sh264e_bit_writer_t;

struct sh264e_chunk_writer_t {
    uint8_t *buffer;
    size_t capacity;
    size_t size;
    size_t total;
    sh264e_output_consumer_t consumer;
    void *user;
};

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
#if SH264E_ENABLE_JPEG_TEST_HOOKS
static unsigned sh264e_jpeg_streaming_last_exact_resize_mask;
#endif

static void reset_progressive_state(sh264e_encoder_t *encoder);
static sh264e_status_t sh264e_begin_idr_to_consumer(sh264e_encoder_t *encoder,
                                                    uint8_t *chunk_buffer,
                                                    size_t chunk_capacity,
                                                    size_t *out_size,
                                                    sh264e_output_consumer_t consumer,
                                                    void *consumer_user);
static sh264e_status_t sh264e_encode_idr_slice_internal(sh264e_encoder_t *encoder,
                                                        const sh264e_slice_t *slice,
                                                        uint8_t *out,
                                                        size_t out_capacity,
                                                        size_t *out_size,
                                                        int require_config_pixfmt);
static sh264e_status_t sh264e_encode_idr_slice_to_consumer_internal(
    sh264e_encoder_t *encoder,
    const sh264e_slice_t *slice,
    uint8_t *chunk_buffer,
    size_t chunk_capacity,
    size_t *out_size,
    sh264e_output_consumer_t consumer,
    void *consumer_user,
    int require_config_pixfmt);
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
static sh264e_status_t chunk_writer_put_byte(sh264e_chunk_writer_t *writer, uint8_t value);

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

static sh264e_status_t validate_internal_slice_storage(const sh264e_config_t *config,
                                                       const sh264e_slice_t *slice)
{
    if (config == NULL || slice == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (slice->pixfmt != SH264E_PIXFMT_I420 && slice->pixfmt != SH264E_PIXFMT_NV12) {
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

static sh264e_status_t validate_slice(const sh264e_config_t *config, const sh264e_slice_t *slice)
{
    if (config == NULL || slice == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    if (slice->pixfmt != config->pixfmt) {
        return SH264E_ERR_UNSUPPORTED_CONFIG;
    }
    return validate_internal_slice_storage(config, slice);
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

static uint8_t bilinear_blend_quarter_u8(uint8_t p00,
                                         uint8_t p01,
                                         uint8_t p10,
                                         uint8_t p11,
                                         unsigned wx_quarters,
                                         unsigned wy_quarters)
{
    const unsigned inv_wx = 4u - wx_quarters;
    const unsigned inv_wy = 4u - wy_quarters;
#if defined(SH264E_USE_ARM_DSP)
    const uint32_t packed_x_weights = (uint32_t)inv_wx | ((uint32_t)wx_quarters << 16u);
    const uint32_t top = (uint32_t)arm_smlad((uint32_t)p00 | ((uint32_t)p01 << 16u),
                                             packed_x_weights,
                                             0);
    const uint32_t bottom = (uint32_t)arm_smlad((uint32_t)p10 | ((uint32_t)p11 << 16u),
                                                packed_x_weights,
                                                0);
    const uint32_t vertical = (uint32_t)arm_smlad(top | (bottom << 16u),
                                                  (uint32_t)inv_wy | ((uint32_t)wy_quarters << 16u),
                                                  0);
#else
    const uint32_t top = (uint32_t)p00 * inv_wx + (uint32_t)p01 * wx_quarters;
    const uint32_t bottom = (uint32_t)p10 * inv_wx + (uint32_t)p11 * wx_quarters;
    const uint32_t vertical = top * inv_wy + bottom * wy_quarters;
#endif

    return (uint8_t)((vertical + 8u) >> 4u);
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

static void exact_scale_axis_sample(int mode,
                                    uint32_t dst_pos,
                                    uint32_t dst_size,
                                    uint32_t src_size,
                                    uint32_t *index0,
                                    uint32_t *index1,
                                    unsigned *fraction_quarters)
{
    const sh264e_scale_coord_t coord = exact_scale_coord(mode, dst_pos, dst_size, src_size);

    *index0 = coord.index;
    *index1 = coord.index + 1u < src_size ? coord.index + 1u : coord.index;
    *fraction_quarters = coord.fraction >> (SH264E_SCALE_FP_BITS - 2u);
}

static void scale_exact_plane_row_quarter(const uint8_t *row0,
                                          const uint8_t *row1,
                                          uint32_t src_width,
                                          uint8_t *dst_row,
                                          uint32_t dst_width,
                                          int mode,
                                          unsigned wy_quarters)
{
    uint32_t x;

    if (mode == 1) {
        const uint32_t last_dst = dst_width - 1u;
        const uint32_t last_src = src_width - 1u;

        dst_row[0] = bilinear_blend_quarter_u8(row0[0], row0[0],
                                               row1[0], row1[0],
                                               0u, wy_quarters);
        for (x = 1u; x < last_dst; x++) {
            const uint32_t raw_quarters = x * 2u - 1u;
            const uint32_t x0 = raw_quarters >> 2u;
            const unsigned wx_quarters = (unsigned)(raw_quarters & 3u);

            dst_row[x] = bilinear_blend_quarter_u8(row0[x0], row0[x0 + 1u],
                                                   row1[x0], row1[x0 + 1u],
                                                   wx_quarters, wy_quarters);
        }
        dst_row[last_dst] = bilinear_blend_quarter_u8(row0[last_src], row0[last_src],
                                                      row1[last_src], row1[last_src],
                                                      0u, wy_quarters);
        return;
    }

    for (x = 0; x < dst_width; x++) {
        const uint32_t x0 = x * 2u;

        dst_row[x] = bilinear_blend_quarter_u8(row0[x0], row0[x0 + 1u],
                                               row1[x0], row1[x0 + 1u],
                                               2u, wy_quarters);
    }
}

static void scale_exact_nv12_chroma_row_quarter(const uint8_t *row0,
                                                const uint8_t *row1,
                                                uint32_t src_width,
                                                uint8_t *dst_row,
                                                uint32_t dst_width,
                                                int mode,
                                                unsigned wy_quarters)
{
    uint32_t x;
    unsigned c;

    if (mode == 1) {
        const uint32_t last_dst = dst_width - 1u;
        const uint32_t last_src = src_width - 1u;

        for (c = 0; c < 2u; c++) {
            dst_row[c] = bilinear_blend_quarter_u8(row0[c], row0[c],
                                                   row1[c], row1[c],
                                                   0u, wy_quarters);
        }
        for (x = 1u; x < last_dst; x++) {
            const uint32_t raw_quarters = x * 2u - 1u;
            const uint32_t x0 = raw_quarters >> 2u;
            const unsigned wx_quarters = (unsigned)(raw_quarters & 3u);
            const size_t dst_x = (size_t)x * 2u;
            const size_t src_x0 = (size_t)x0 * 2u;
            const size_t src_x1 = (size_t)(x0 + 1u) * 2u;

            dst_row[dst_x] = bilinear_blend_quarter_u8(row0[src_x0],
                                                       row0[src_x1],
                                                       row1[src_x0],
                                                       row1[src_x1],
                                                       wx_quarters,
                                                       wy_quarters);
            dst_row[dst_x + 1u] = bilinear_blend_quarter_u8(row0[src_x0 + 1u],
                                                            row0[src_x1 + 1u],
                                                            row1[src_x0 + 1u],
                                                            row1[src_x1 + 1u],
                                                            wx_quarters,
                                                            wy_quarters);
        }
        for (c = 0; c < 2u; c++) {
            const size_t dst_x = (size_t)last_dst * 2u + c;
            const size_t src_x = (size_t)last_src * 2u + c;

            dst_row[dst_x] = bilinear_blend_quarter_u8(row0[src_x], row0[src_x],
                                                       row1[src_x], row1[src_x],
                                                       0u, wy_quarters);
        }
        return;
    }

    for (x = 0; x < dst_width; x++) {
        const uint32_t x0 = x * 2u;
        const size_t dst_x = (size_t)x * 2u;
        const size_t src_x0 = (size_t)x0 * 2u;
        const size_t src_x1 = (size_t)(x0 + 1u) * 2u;

        dst_row[dst_x] = bilinear_blend_quarter_u8(row0[src_x0],
                                                   row0[src_x1],
                                                   row1[src_x0],
                                                   row1[src_x1],
                                                   2u,
                                                   wy_quarters);
        dst_row[dst_x + 1u] = bilinear_blend_quarter_u8(row0[src_x0 + 1u],
                                                        row0[src_x1 + 1u],
                                                        row1[src_x0 + 1u],
                                                        row1[src_x1 + 1u],
                                                        2u,
                                                        wy_quarters);
    }
}

static void scale_exact_component_pair_to_nv12_row_quarter(const uint8_t *cb_row0,
                                                           const uint8_t *cb_row1,
                                                           const uint8_t *cr_row0,
                                                           const uint8_t *cr_row1,
                                                           uint32_t src_width,
                                                           uint8_t *dst_row,
                                                           uint32_t dst_width,
                                                           int mode,
                                                           unsigned wy_quarters)
{
    uint32_t x;

    if (mode == 1) {
        const uint32_t last_dst = dst_width - 1u;
        const uint32_t last_src = src_width - 1u;

        dst_row[0] = bilinear_blend_quarter_u8(cb_row0[0], cb_row0[0],
                                               cb_row1[0], cb_row1[0],
                                               0u, wy_quarters);
        dst_row[1] = bilinear_blend_quarter_u8(cr_row0[0], cr_row0[0],
                                               cr_row1[0], cr_row1[0],
                                               0u, wy_quarters);
        for (x = 1u; x < last_dst; x++) {
            const uint32_t raw_quarters = x * 2u - 1u;
            const uint32_t x0 = raw_quarters >> 2u;
            const unsigned wx_quarters = (unsigned)(raw_quarters & 3u);
            const size_t dst_x = (size_t)x * 2u;

            dst_row[dst_x] = bilinear_blend_quarter_u8(cb_row0[x0], cb_row0[x0 + 1u],
                                                       cb_row1[x0], cb_row1[x0 + 1u],
                                                       wx_quarters, wy_quarters);
            dst_row[dst_x + 1u] = bilinear_blend_quarter_u8(cr_row0[x0], cr_row0[x0 + 1u],
                                                            cr_row1[x0], cr_row1[x0 + 1u],
                                                            wx_quarters, wy_quarters);
        }
        dst_row[(size_t)last_dst * 2u] = bilinear_blend_quarter_u8(cb_row0[last_src],
                                                                   cb_row0[last_src],
                                                                   cb_row1[last_src],
                                                                   cb_row1[last_src],
                                                                   0u,
                                                                   wy_quarters);
        dst_row[(size_t)last_dst * 2u + 1u] = bilinear_blend_quarter_u8(cr_row0[last_src],
                                                                        cr_row0[last_src],
                                                                        cr_row1[last_src],
                                                                        cr_row1[last_src],
                                                                        0u,
                                                                        wy_quarters);
        return;
    }

    for (x = 0; x < dst_width; x++) {
        const uint32_t x0 = x * 2u;
        const size_t dst_x = (size_t)x * 2u;

        dst_row[dst_x] = bilinear_blend_quarter_u8(cb_row0[x0], cb_row0[x0 + 1u],
                                                   cb_row1[x0], cb_row1[x0 + 1u],
                                                   2u, wy_quarters);
        dst_row[dst_x + 1u] = bilinear_blend_quarter_u8(cr_row0[x0], cr_row0[x0 + 1u],
                                                        cr_row1[x0], cr_row1[x0 + 1u],
                                                        2u, wy_quarters);
    }
}

static void streaming_note_exact_scale_mode(int mode)
{
#if SH264E_ENABLE_JPEG_TEST_HOOKS
    if (mode == 1) {
        sh264e_jpeg_streaming_last_exact_resize_mask |= SH264E_JPEG_EXACT_RESIZE_2X_MASK;
    } else if (mode == 2) {
        sh264e_jpeg_streaming_last_exact_resize_mask |= SH264E_JPEG_EXACT_RESIZE_HALF_MASK;
    }
#else
    (void)mode;
#endif
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

static const uint8_t *streaming_component_row_ptr(const sh264e_jpeg_stream_component_t *src,
                                                  uint32_t y)
{
    if (y < src->row0) {
        y = src->row0;
    }
    y -= src->row0;
    if (y >= src->rows) {
        y = src->rows - 1u;
    }
    return src->pixels + (size_t)y * (size_t)src->stride;
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

static void streaming_scale_component_exact_slice(const sh264e_jpeg_stream_component_t *src,
                                                  uint8_t *dst,
                                                  uint32_t dst_width,
                                                  uint32_t dst_height,
                                                  uint32_t dst_y_start,
                                                  uint32_t dst_rows,
                                                  ptrdiff_t dst_stride,
                                                  int exact_mode)
{
    uint32_t y;

    for (y = 0; y < dst_rows; y++) {
        uint32_t y0;
        uint32_t y1;
        unsigned wy_quarters;
        uint8_t *dst_row = dst + (size_t)y * (size_t)dst_stride;

        exact_scale_axis_sample(exact_mode,
                                dst_y_start + y,
                                dst_height,
                                src->height,
                                &y0,
                                &y1,
                                &wy_quarters);
        scale_exact_plane_row_quarter(streaming_component_row_ptr(src, y0),
                                      streaming_component_row_ptr(src, y1),
                                      src->width,
                                      dst_row,
                                      dst_width,
                                      exact_mode,
                                      wy_quarters);
    }
}

static void streaming_scale_component_slice(const sh264e_jpeg_stream_component_t *src,
                                            uint8_t *dst,
                                            uint32_t dst_width,
                                            uint32_t dst_height,
                                            uint32_t dst_y_start,
                                            uint32_t dst_rows,
                                            ptrdiff_t dst_stride)
{
    const int exact_mode = exact_scale_ratio_mode(src->width, src->height, dst_width, dst_height);
    sh264e_axis_mapper_t y_mapper = scale_axis_mapper_init(src->height, dst_height, dst_y_start);
    uint32_t y;

    streaming_note_exact_scale_mode(exact_mode);
    if (exact_mode != 0) {
        streaming_scale_component_exact_slice(src,
                                              dst,
                                              dst_width,
                                              dst_height,
                                              dst_y_start,
                                              dst_rows,
                                              dst_stride,
                                              exact_mode);
        return;
    }

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

static void streaming_scale_nv12_component_chroma_exact_slice(const sh264e_jpeg_stream_component_t *cb,
                                                             const sh264e_jpeg_stream_component_t *cr,
                                                             uint8_t *dst_uv,
                                                             uint32_t dst_y_start,
                                                             int exact_mode)
{
    uint32_t y;

    for (y = 0; y < SH264E_V1_SLICE_CHROMA_HEIGHT; y++) {
        uint32_t y0;
        uint32_t y1;
        unsigned wy_quarters;
        uint8_t *dst_row = dst_uv + (size_t)y * SH264E_V1_WIDTH;

        exact_scale_axis_sample(exact_mode,
                                dst_y_start + y,
                                SH264E_V1_HEIGHT / 2u,
                                cb->height,
                                &y0,
                                &y1,
                                &wy_quarters);
        scale_exact_component_pair_to_nv12_row_quarter(streaming_component_row_ptr(cb, y0),
                                                       streaming_component_row_ptr(cb, y1),
                                                       streaming_component_row_ptr(cr, y0),
                                                       streaming_component_row_ptr(cr, y1),
                                                       cb->width,
                                                       dst_row,
                                                       SH264E_CHROMA_WIDTH,
                                                       exact_mode,
                                                       wy_quarters);
    }
}

static void streaming_scale_nv12_component_chroma_slice(const sh264e_jpeg_stream_component_t *cb,
                                                        const sh264e_jpeg_stream_component_t *cr,
                                                        uint8_t *dst_uv,
                                                        uint32_t dst_y_start)
{
    const int exact_mode = exact_scale_ratio_mode(cb->width,
                                                 cb->height,
                                                 SH264E_CHROMA_WIDTH,
                                                 SH264E_V1_HEIGHT / 2u);
    sh264e_axis_mapper_t y_mapper = scale_axis_mapper_init(cb->height,
                                                           SH264E_V1_HEIGHT / 2u,
                                                           dst_y_start);
    uint32_t y;

    streaming_note_exact_scale_mode(exact_mode);
    if (exact_mode != 0) {
        streaming_scale_nv12_component_chroma_exact_slice(cb, cr, dst_uv, dst_y_start, exact_mode);
        return;
    }

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

static void streaming_make_nv12_slice(const sh264e_jpeg_stream_context_t *ctx,
                                      unsigned slice_index,
                                      sh264e_slice_t *slice)
{
    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = SH264E_PIXFMT_NV12;

    if (ctx->one_to_one_420) {
        const sh264e_jpeg_stream_component_t *y = &ctx->components[0];
        const sh264e_jpeg_stream_component_t *cb = &ctx->components[1];
        const sh264e_jpeg_stream_component_t *cr = &ctx->components[2];
        const uint32_t luma_row = (uint32_t)slice_index * SH264E_V1_SLICE_LUMA_HEIGHT;
        const uint32_t chroma_row = (uint32_t)slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT;

        slice->pixfmt = SH264E_PIXFMT_I420;
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
        ctx->effective_slice_work_bytes = 0u;
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
        return sh264e_encode_idr_slice_to_consumer_internal(ctx->encoder,
                                                            slice,
                                                            ctx->out,
                                                            ctx->out_capacity,
                                                            bytes,
                                                            ctx->consumer,
                                                            ctx->consumer_user,
                                                            0);
    }
    return sh264e_encode_idr_slice_internal(ctx->encoder, slice,
                                            streaming_output_ptr(ctx),
                                            streaming_output_capacity(ctx),
                                            bytes,
                                            0);
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

    if (exact_mode != 0) {
        for (y = 0; y < dst_rows; y++) {
            uint32_t y0;
            uint32_t y1;
            unsigned wy_quarters;
            uint8_t *dst_row = dst + (size_t)y * (size_t)dst_stride;

            exact_scale_axis_sample(exact_mode,
                                    dst_y_start + y,
                                    dst_height,
                                    src_height,
                                    &y0,
                                    &y1,
                                    &wy_quarters);
            scale_exact_plane_row_quarter(src + (size_t)y0 * (size_t)src_stride,
                                          src + (size_t)y1 * (size_t)src_stride,
                                          src_width,
                                          dst_row,
                                          dst_width,
                                          exact_mode,
                                          wy_quarters);
        }
        return;
    }

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
    const int exact_mode = exact_scale_ratio_mode(src_chroma_width,
                                                 src_chroma_height,
                                                 SH264E_CHROMA_WIDTH,
                                                 SH264E_V1_HEIGHT / 2u);
    sh264e_axis_mapper_t y_mapper = scale_axis_mapper_init(src_chroma_height,
                                                           SH264E_V1_HEIGHT / 2u,
                                                           dst_y_start);
    uint32_t y;

    if (exact_mode != 0) {
        for (y = 0; y < SH264E_V1_SLICE_CHROMA_HEIGHT; y++) {
            uint32_t y0;
            uint32_t y1;
            unsigned wy_quarters;
            uint8_t *dst_row = dst_uv + (size_t)y * SH264E_V1_WIDTH;

            exact_scale_axis_sample(exact_mode,
                                    dst_y_start + y,
                                    SH264E_V1_HEIGHT / 2u,
                                    src_chroma_height,
                                    &y0,
                                    &y1,
                                    &wy_quarters);
            scale_exact_nv12_chroma_row_quarter(src_uv + (size_t)y0 * (size_t)src_stride,
                                                src_uv + (size_t)y1 * (size_t)src_stride,
                                                src_chroma_width,
                                                dst_row,
                                                SH264E_CHROMA_WIDTH,
                                                exact_mode,
                                                wy_quarters);
        }
        return;
    }

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
    bw->pending_byte = 0;
    bw->pending_bits = 0;
    bw->annexb_writer = NULL;
    bw->annexb_zero_count = 0u;
    bw->status = SH264E_OK;
    bw->error = 0;
}

static size_t bw_size(const sh264e_bit_writer_t *bw)
{
    return bw->byte_pos + (bw->pending_bits != 0u ? 1u : 0u);
}

static void bw_store_pending(sh264e_bit_writer_t *bw)
{
    if (bw->annexb_writer != NULL) {
        return;
    }
    if (bw->byte_pos >= bw->capacity) {
        bw->error = 1;
        bw->status = SH264E_ERR_INTERNAL;
        return;
    }
    bw->data[bw->byte_pos] = bw->pending_byte;
}

static void bw_write_complete_byte(sh264e_bit_writer_t *bw, uint8_t value)
{
    sh264e_status_t status;

    if (bw->annexb_writer != NULL) {
        if (bw->annexb_zero_count >= 2u && value <= 0x03u) {
            status = chunk_writer_put_byte(bw->annexb_writer, 0x03u);
            if (status != SH264E_OK) {
                bw->status = status;
                bw->error = 1;
                return;
            }
            bw->annexb_zero_count = 0u;
        }

        status = chunk_writer_put_byte(bw->annexb_writer, value);
        if (status != SH264E_OK) {
            bw->status = status;
            bw->error = 1;
            return;
        }
        if (value == 0x00u) {
            bw->annexb_zero_count++;
        } else {
            bw->annexb_zero_count = 0u;
        }
        bw->byte_pos++;
        return;
    }

    if (bw->byte_pos >= bw->capacity) {
        bw->error = 1;
        bw->status = SH264E_ERR_INTERNAL;
        return;
    }
    bw->data[bw->byte_pos++] = value;
}

static void bw_commit_pending_byte(sh264e_bit_writer_t *bw)
{
    bw_write_complete_byte(bw, bw->pending_byte);
    bw->pending_byte = 0;
    bw->pending_bits = 0;
}

static void bw_write_aligned_byte(sh264e_bit_writer_t *bw, uint8_t value)
{
    bw_write_complete_byte(bw, value);
}

static void bw_write_bits(sh264e_bit_writer_t *bw, uint32_t bits, unsigned count);

static void bw_write_bit(sh264e_bit_writer_t *bw, unsigned bit)
{
    bw_write_bits(bw, bit & 1u, 1u);
}

static void bw_write_bits(sh264e_bit_writer_t *bw, uint32_t bits, unsigned count)
{
    while (count > 0u && bw->error == 0) {
        if (bw->pending_bits == 0u && count >= 8u) {
            const unsigned shift = count - 8u;
            bw_write_aligned_byte(bw, (uint8_t)((bits >> shift) & 0xffu));
            count -= 8u;
        } else {
            const unsigned free_bits = 8u - bw->pending_bits;
            const unsigned take = count < free_bits ? count : free_bits;
            const unsigned shift = count - take;
            const uint32_t mask = (1u << take) - 1u;
            const uint8_t chunk = (uint8_t)((bits >> shift) & mask);

            bw->pending_byte |= (uint8_t)(chunk << (free_bits - take));
            bw->pending_bits += take;
            bw_store_pending(bw);
            if (bw->error != 0) {
                return;
            }
            count -= take;
            if (bw->pending_bits == 8u) {
                bw_commit_pending_byte(bw);
            }
        }
    }
}

static void bw_write_zero_bits(sh264e_bit_writer_t *bw, unsigned count)
{
    while (count > 0u && bw->error == 0) {
        const unsigned take = count > 32u ? 32u : count;
        bw_write_bits(bw, 0u, take);
        count -= take;
    }
}

static void bw_write_ue(sh264e_bit_writer_t *bw, uint32_t value)
{
    const uint32_t code_num = value + 1u;
    const unsigned leading_zero_bits = sh264e_u32_floor_log2(code_num);
    bw_write_zero_bits(bw, leading_zero_bits);
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
    if (bw->pending_bits != 0u) {
        bw_write_zero_bits(bw, 8u - bw->pending_bits);
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

static sh264e_status_t chunk_writer_put_bytes(sh264e_chunk_writer_t *writer,
                                              const uint8_t *data,
                                              size_t size)
{
    while (size != 0u) {
        size_t available = writer->capacity - writer->size;
        size_t take;

        if (available == 0u) {
            sh264e_status_t status = chunk_writer_flush(writer);
            if (status != SH264E_OK) {
                return status;
            }
            available = writer->capacity;
        }

        take = size < available ? size : available;
        memcpy(writer->buffer + writer->size, data, take);
        writer->size += take;
        data += take;
        size -= take;
    }
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

    i = 0u;
    while (i < rbsp_size) {
        const size_t run_start = i;

        while (i < rbsp_size) {
            const uint8_t b = rbsp[i];

            if (zero_count >= 2u && b <= 0x03u) {
                break;
            }
            if (b == 0x00u) {
                zero_count++;
            } else {
                zero_count = 0;
            }
            i++;
        }

        if (i != run_start) {
            status = chunk_writer_put_bytes(&writer, rbsp + run_start, i - run_start);
            if (status != SH264E_OK) {
                return status;
            }
        }

        if (i < rbsp_size) {
            const uint8_t b = rbsp[i++];
            SH264E_TRY_PUT_BYTE(0x03u);
            zero_count = 0;
            SH264E_TRY_PUT_BYTE(b);
            if (b == 0x00u) {
                zero_count++;
            } else {
                zero_count = 0;
            }
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

static sh264e_status_t bw_begin_annexb_nalu(sh264e_bit_writer_t *bw,
                                            sh264e_chunk_writer_t *writer,
                                            uint8_t nal_header)
{
    sh264e_status_t status;

    if (bw == NULL || writer == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }

    bw_init(bw, NULL, 0u);
    bw->annexb_writer = writer;
    status = chunk_writer_put_byte(writer, 0x00u);
    if (status != SH264E_OK) {
        return status;
    }
    status = chunk_writer_put_byte(writer, 0x00u);
    if (status != SH264E_OK) {
        return status;
    }
    status = chunk_writer_put_byte(writer, 0x01u);
    if (status != SH264E_OK) {
        return status;
    }
    return chunk_writer_put_byte(writer, nal_header);
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

#if defined(SH264E_USE_ARM_DSP)
static uint32_t arm_usada8(uint32_t a, uint32_t b, uint32_t acc)
{
    uint32_t result;
    __asm volatile("usada8 %0, %1, %2, %3"
                   : "=r"(result)
                   : "r"(a), "r"(b), "r"(acc));
    return result;
}

static uint32_t pack_u8x4(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
    return (uint32_t)b0 |
           ((uint32_t)b1 << 8u) |
           ((uint32_t)b2 << 16u) |
           ((uint32_t)b3 << 24u);
}

#endif

static const uint8_t *slice_luma_row(const sh264e_slice_t *slice, unsigned y)
{
    return slice->plane[0] + (size_t)y * (size_t)slice->stride[0];
}

static unsigned luma4x4_block_index(unsigned row_group, unsigned col_group)
{
    return ((row_group & 2u) << 2u) |
           ((col_group & 2u) << 1u) |
           ((row_group & 1u) << 1u) |
           (col_group & 1u);
}

static void luma16x16_sum_deltas(const sh264e_slice_t *slice, unsigned mb_x, int out_sum_delta[16])
{
    const unsigned src_x = mb_x * SH264E_MB_SIZE;
    unsigned row;
    unsigned b;

    for (b = 0; b < 16u; b++) {
        out_sum_delta[b] = 0;
    }

    for (row = 0; row < SH264E_MB_SIZE; row++) {
        const uint8_t *src = slice_luma_row(slice, row) + src_x;
        const unsigned row_group = row >> 2u;
        unsigned col_group;

        for (col_group = 0; col_group < 4u; col_group++) {
            const unsigned block = luma4x4_block_index(row_group, col_group);
            const uint8_t *group = src + (size_t)col_group * 4u;
#if defined(SH264E_USE_ARM_DSP)
            out_sum_delta[block] = (int)arm_usada8(pack_u8x4(group[0], group[1], group[2], group[3]),
                                                   0u,
                                                   (uint32_t)out_sum_delta[block]);
#else
            out_sum_delta[block] += (int)group[0] + (int)group[1] + (int)group[2] + (int)group[3];
#endif
        }
    }

    for (b = 0; b < 16u; b++) {
        out_sum_delta[b] -= SH264E_DC_PRED * 16;
    }
}

static uint32_t chroma_i420_sum_u8x8(const sh264e_slice_t *slice,
                                     unsigned plane,
                                     unsigned src_x,
                                     unsigned y)
{
    unsigned row;
    uint32_t sum = 0u;

    for (row = 0; row < 8u; row++) {
        const uint8_t *src = slice->plane[plane] + (size_t)(y + row) * (size_t)slice->stride[plane] + src_x;
#if defined(SH264E_USE_ARM_DSP)
        sum = arm_usada8(pack_u8x4(src[0], src[1], src[2], src[3]), 0u, sum);
        sum = arm_usada8(pack_u8x4(src[4], src[5], src[6], src[7]), 0u, sum);
#else
        unsigned col;
        for (col = 0; col < 8u; col++) {
            sum += src[col];
        }
#endif
    }
    return sum;
}

static uint32_t chroma_nv12_sum_u8x8(const sh264e_slice_t *slice,
                                     unsigned plane,
                                     unsigned src_x,
                                     unsigned y)
{
    const unsigned component_offset = plane == 1u ? 0u : 1u;
    unsigned row;
    uint32_t sum = 0u;

    for (row = 0; row < 8u; row++) {
        const uint8_t *src = slice->plane[1] + (size_t)(y + row) * (size_t)slice->stride[1] +
                             (size_t)src_x * 2u + component_offset;
#if defined(SH264E_USE_ARM_DSP)
        sum = arm_usada8(pack_u8x4(src[0], src[2], src[4], src[6]), 0u, sum);
        sum = arm_usada8(pack_u8x4(src[8], src[10], src[12], src[14]), 0u, sum);
#else
        unsigned col;
        for (col = 0; col < 8u; col++) {
            sum += src[(size_t)col * 2u];
        }
#endif
    }
    return sum;
}

static void chroma_nv12_sum_u8x8_pair(const sh264e_slice_t *slice,
                                      unsigned src_x,
                                      unsigned y,
                                      uint32_t *out_u_sum,
                                      uint32_t *out_v_sum)
{
    unsigned row;
    uint32_t u_sum = 0u;
    uint32_t v_sum = 0u;

    for (row = 0; row < 8u; row++) {
        const uint8_t *src = slice->plane[1] + (size_t)(y + row) * (size_t)slice->stride[1] +
                             (size_t)src_x * 2u;
#if defined(SH264E_USE_ARM_DSP)
        u_sum = arm_usada8(pack_u8x4(src[0], src[2], src[4], src[6]), 0u, u_sum);
        v_sum = arm_usada8(pack_u8x4(src[1], src[3], src[5], src[7]), 0u, v_sum);
        u_sum = arm_usada8(pack_u8x4(src[8], src[10], src[12], src[14]), 0u, u_sum);
        v_sum = arm_usada8(pack_u8x4(src[9], src[11], src[13], src[15]), 0u, v_sum);
#else
        unsigned col;
        for (col = 0; col < 8u; col++) {
            u_sum += src[(size_t)col * 2u];
            v_sum += src[(size_t)col * 2u + 1u];
        }
#endif
    }

    *out_u_sum = u_sum;
    *out_v_sum = v_sum;
}

static int chroma8x8_sum_delta(const sh264e_slice_t *slice,
                               unsigned plane,
                               unsigned src_x,
                               unsigned y)
{
    uint32_t sum;

    if (slice->pixfmt == SH264E_PIXFMT_I420) {
        sum = chroma_i420_sum_u8x8(slice, plane, src_x, y);
    } else {
        sum = chroma_nv12_sum_u8x8(slice, plane, src_x, y);
    }
    return (int)sum - (SH264E_DC_PRED * 64);
}

static int quantize_chroma8x8_sum(sh264e_encoder_t *encoder, uint32_t sum)
{
    const int sum_delta = (int)sum - (SH264E_DC_PRED * 64);
    int level = quantize_dc_delta((sum_delta + (sum_delta >= 0 ? 32 : -32)) / 64,
                                  encoder->config.qp);

    if (level > 0) {
        level = 1;
    } else if (level < 0) {
        level = -1;
    }

    return level;
}

static unsigned encode_luma16x16_dc_levels(sh264e_encoder_t *encoder,
                                           const sh264e_slice_t *slice,
                                           unsigned mb_x,
                                           int levels[16])
{
    unsigned cbp_luma = 0u;
    unsigned b;

    luma16x16_sum_deltas(slice, mb_x, levels);
    for (b = 0; b < 16u; b++) {
        const int sum_delta = levels[b];
        const int level = quantize_dc_delta((sum_delta + (sum_delta >= 0 ? 8 : -8)) / 16,
                                            encoder->config.qp);

        levels[b] = level;
        if (level != 0) {
            cbp_luma |= 1u << (b / 4u);
        }
    }

    return cbp_luma;
}

static int encode_chroma8x8_dc(sh264e_encoder_t *encoder,
                               const sh264e_slice_t *slice,
                               unsigned plane,
                               unsigned mb_x,
                               unsigned x,
                               unsigned y)
{
    int level;
    const unsigned src_x = mb_x * (SH264E_MB_SIZE / 2u) + x;
    const int sum_delta = chroma8x8_sum_delta(slice, plane, src_x, y);

    level = quantize_dc_delta((sum_delta + (sum_delta >= 0 ? 32 : -32)) / 64, encoder->config.qp);
    if (level > 0) {
        level = 1;
    } else if (level < 0) {
        level = -1;
    }

    return level;
}

static void encode_chroma8x8_dc_pair(sh264e_encoder_t *encoder,
                                     const sh264e_slice_t *slice,
                                     unsigned mb_x,
                                     int out_chroma_dc[2])
{
    const unsigned src_x = mb_x * (SH264E_MB_SIZE / 2u);

    if (slice->pixfmt == SH264E_PIXFMT_NV12) {
        uint32_t u_sum;
        uint32_t v_sum;

        chroma_nv12_sum_u8x8_pair(slice, src_x, 0u, &u_sum, &v_sum);
        out_chroma_dc[0] = quantize_chroma8x8_sum(encoder, u_sum);
        out_chroma_dc[1] = quantize_chroma8x8_sum(encoder, v_sum);
        return;
    }

    out_chroma_dc[0] = encode_chroma8x8_dc(encoder, slice, 1u, mb_x, 0u, 0u);
    out_chroma_dc[1] = encode_chroma8x8_dc(encoder, slice, 2u, mb_x, 0u, 0u);
}

static int write_cavlc_level(sh264e_bit_writer_t *bw, int level);

static void write_luma_residual_dc_only_nc0(sh264e_bit_writer_t *bw, int level)
{
    if (level == 0) {
        bw_write_bit(bw, 1);             /* TotalCoeff=0 for fixed nC=0 luma */
        return;
    }
    if (level == 1) {
        level = 2;
    } else if (level == -1) {
        level = -2;
    }
    bw_write_bits(bw, 0x05u, 6);         /* TotalCoeff=1, TrailingOnes=0 for nC=0 */
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

    bw_write_zero_bits(bw, prefix);
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

enum {
    SH264E_IDR_MB_SLICE_FIXED_HEADER_BITS = 23,
    SH264E_I_NXN_FIXED_PREFIX_BITS = 18
};

static void write_idr_mb_slice_header(sh264e_bit_writer_t *bw, unsigned first_mb_in_slice)
{
    bw_write_ue(bw, first_mb_in_slice);
    /*
     * Fixed tail after first_mb_in_slice:
     * slice_type=I(7), pps=0, frame_num=0, idr_pic_id=0, poc_lsb=0,
     * ref flags=0, slice_qp_delta=0, disable_deblocking_filter_idc=1.
     */
    bw_write_bits(bw, 0x8840au, SH264E_IDR_MB_SLICE_FIXED_HEADER_BITS);
}

static void write_i_nxn_fixed_prefix(sh264e_bit_writer_t *bw)
{
    /*
     * mb_type=I_NxN (ue(0)), sixteen prev_intra4x4_pred_mode_flag bits set,
     * and intra_chroma_pred_mode=DC (ue(0)).
     */
    bw_write_bits(bw, 0x3ffffu, SH264E_I_NXN_FIXED_PREFIX_BITS);
}

static sh264e_status_t write_idr_mb_slice_payload(sh264e_encoder_t *encoder,
                                                  const sh264e_slice_t *slice,
                                                  unsigned row_index,
                                                  unsigned mb_x,
                                                  sh264e_bit_writer_t *bw)
{
    int levels[16];
    int chroma_dc[2];
    unsigned b;
    unsigned cbp_luma = 0;
    unsigned cbp_chroma;
    unsigned cbp;

    write_idr_mb_slice_header(bw, row_index * SH264E_MBS_X + mb_x);

    cbp_luma = encode_luma16x16_dc_levels(encoder, slice, mb_x, levels);
    encode_chroma8x8_dc_pair(encoder, slice, mb_x, chroma_dc);
    cbp_chroma = (chroma_dc[0] != 0 || chroma_dc[1] != 0) ? 1u : 0u;
    cbp = cbp_luma + cbp_chroma * 16u;

    write_i_nxn_fixed_prefix(bw);
    bw_write_ue(bw, k_cbp_intra_code_num[cbp]);

    if (cbp != 0u) {
        bw_write_bit(bw, 1);                     /* mb_qp_delta: se(0) */
        for (b = 0; b < 16u; b++) {
            if ((cbp_luma & (1u << (b / 4u))) != 0u) {
                write_luma_residual_dc_only_nc0(bw, levels[b]);
            }
        }
        if (cbp_chroma != 0u) {
            write_chroma_dc_residual(bw, chroma_dc[0]);
            write_chroma_dc_residual(bw, chroma_dc[1]);
        }
    }

    if (bw->error != 0) {
        return bw->status != SH264E_OK ? bw->status : SH264E_ERR_INTERNAL;
    }

    bw_rbsp_trailing_bits(bw);
    if (bw->error != 0) {
        return bw->status != SH264E_OK ? bw->status : SH264E_ERR_INTERNAL;
    }
    return SH264E_OK;
}

static sh264e_status_t write_idr_mb_slice_rbsp(sh264e_encoder_t *encoder,
                                               const sh264e_slice_t *slice,
                                               unsigned row_index,
                                               unsigned mb_x,
                                               size_t *out_rbsp_size)
{
    sh264e_bit_writer_t bw;
    sh264e_status_t status;

    bw_init(&bw, encoder->rbsp, encoder->rbsp_capacity);

    status = write_idr_mb_slice_payload(encoder, slice, row_index, mb_x, &bw);
    if (status != SH264E_OK) {
        return status;
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

static sh264e_status_t stream_idr_slice_nalu_direct(sh264e_encoder_t *encoder,
                                                    const sh264e_slice_t *slice,
                                                    unsigned row_index,
                                                    unsigned mb_x,
                                                    sh264e_chunk_writer_t *writer)
{
    sh264e_bit_writer_t bw;
    sh264e_status_t status;

    if (writer == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }

    status = bw_begin_annexb_nalu(&bw, writer, 0x65u);
    if (status != SH264E_OK) {
        return status;
    }
    status = write_idr_mb_slice_payload(encoder, slice, row_index, mb_x, &bw);
    if (status != SH264E_OK) {
        return status;
    }
    return SH264E_OK;
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
#if SH264E_ENABLE_JPEG_TEST_HOOKS
    sh264e_jpeg_streaming_last_exact_resize_mask = 0u;
#endif
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

unsigned sh264e_jpeg_get_test_last_exact_resize_mask(void)
{
    return sh264e_jpeg_streaming_last_exact_resize_mask;
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
#if SH264E_ENABLE_JPEG_TEST_HOOKS
    sh264e_jpeg_streaming_last_exact_resize_mask = 0u;
#endif
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

static sh264e_status_t sh264e_encode_idr_slice_internal(sh264e_encoder_t *encoder,
                                                        const sh264e_slice_t *slice,
                                                        uint8_t *out,
                                                        size_t out_capacity,
                                                        size_t *out_size,
                                                        int require_config_pixfmt)
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
    status = require_config_pixfmt != 0
                 ? validate_slice(&encoder->config, slice)
                 : validate_internal_slice_storage(&encoder->config, slice);
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

sh264e_status_t sh264e_encode_idr_slice(sh264e_encoder_t *encoder,
                                        const sh264e_slice_t *slice,
                                        uint8_t *out,
                                        size_t out_capacity,
                                        size_t *out_size)
{
    return sh264e_encode_idr_slice_internal(encoder, slice, out, out_capacity, out_size, 1);
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

static sh264e_status_t sh264e_encode_idr_slice_to_consumer_internal(
    sh264e_encoder_t *encoder,
    const sh264e_slice_t *slice,
    uint8_t *chunk_buffer,
    size_t chunk_capacity,
    size_t *out_size,
    sh264e_output_consumer_t consumer,
    void *consumer_user,
    int require_config_pixfmt)
{
    sh264e_chunk_writer_t writer;
    sh264e_status_t status;

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
    status = require_config_pixfmt != 0
                 ? validate_slice(&encoder->config, slice)
                 : validate_internal_slice_storage(&encoder->config, slice);
    if (status != SH264E_OK) {
        return status;
    }
    if (chunk_capacity == 0u) {
        return SH264E_ERR_BUFFER_TOO_SMALL;
    }

    memset(&writer, 0, sizeof(writer));
    writer.buffer = chunk_buffer;
    writer.capacity = chunk_capacity;
    writer.consumer = consumer;
    writer.user = consumer_user;

    {
        unsigned mb_x;
        for (mb_x = 0; mb_x < SH264E_MBS_X; mb_x++) {
            status = stream_idr_slice_nalu_direct(encoder, slice, encoder->slices_encoded,
                                                  mb_x, &writer);
            if (status != SH264E_OK) {
                return status;
            }
        }
    }

    status = chunk_writer_flush(&writer);
    if (status != SH264E_OK) {
        return status;
    }

    encoder->slices_encoded++;
    *out_size = writer.total;
    return SH264E_OK;
}

#if SH264E_ENABLE_BENCH_HOOKS

sh264e_status_t sh264e_bench_begin_idr_to_consumer(sh264e_encoder_t *encoder,
                                                   uint8_t *chunk_buffer,
                                                   size_t chunk_capacity,
                                                   size_t *out_size,
                                                   sh264e_output_consumer_t consumer,
                                                   void *consumer_user)
{
    return sh264e_begin_idr_to_consumer(encoder, chunk_buffer, chunk_capacity,
                                        out_size, consumer, consumer_user);
}

sh264e_status_t sh264e_bench_encode_idr_slice_to_consumer(
    sh264e_encoder_t *encoder,
    const sh264e_slice_t *slice,
    uint8_t *chunk_buffer,
    size_t chunk_capacity,
    size_t *out_size,
    sh264e_output_consumer_t consumer,
    void *consumer_user)
{
    return sh264e_encode_idr_slice_to_consumer_internal(
        encoder, slice, chunk_buffer, chunk_capacity, out_size,
        consumer, consumer_user, 1);
}

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
    size_t *out_size)
{
    sh264e_jpeg_stream_context_t ctx;
    size_t total = 0u;
    unsigned slice_index;
    sh264e_status_t status;

    if (encoder == NULL || src_y == NULL || src_cb == NULL || src_cr == NULL ||
        chunk_buffer == NULL || consumer == NULL || out_size == NULL) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }
    *out_size = 0u;
    if (!is_supported_pixfmt(output_pixfmt) ||
        src_width < SH264E_RESIZE_MIN_SRC_WIDTH ||
        src_width > SH264E_RESIZE_MAX_SRC_WIDTH ||
        src_height < SH264E_RESIZE_MIN_SRC_HEIGHT ||
        src_height > SH264E_RESIZE_MAX_SRC_HEIGHT ||
        (src_width & 1u) != 0u ||
        (src_height & 1u) != 0u ||
        src_y_stride < (ptrdiff_t)src_width ||
        src_c_stride < (ptrdiff_t)(src_width / 2u) ||
        slice_count == 0u ||
        slice_count > SH264E_V1_SLICE_COUNT ||
        chunk_capacity == 0u) {
        return SH264E_ERR_INVALID_ARGUMENT;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.encoder = encoder;
    ctx.work_buffer = work_buffer;
    ctx.work_buffer_capacity = work_buffer_capacity;
    ctx.out = chunk_buffer;
    ctx.out_capacity = chunk_capacity;
    ctx.consumer = consumer;
    ctx.consumer_user = consumer_user;
    ctx.output_pixfmt = output_pixfmt;
    ctx.component_count = 3;
    ctx.one_to_one_420 = src_width == SH264E_V1_WIDTH &&
                         src_height == SH264E_V1_HEIGHT;
    ctx.effective_slice_work_bytes =
        ctx.one_to_one_420 ? 0u : resize_scaled_slice_buffer_size();
    if (ctx.effective_slice_work_bytes != 0u) {
        if (work_buffer == NULL) {
            return SH264E_ERR_INVALID_ARGUMENT;
        }
        if (work_buffer_capacity < ctx.effective_slice_work_bytes) {
            return SH264E_ERR_BUFFER_TOO_SMALL;
        }
    }

    ctx.components[0].pixels = (uint8_t *)src_y;
    ctx.components[0].width = src_width;
    ctx.components[0].height = src_height;
    ctx.components[0].stride = src_y_stride;
    ctx.components[0].row0 = 0u;
    ctx.components[0].rows = slice_count * SH264E_V1_SLICE_LUMA_HEIGHT * 2u + 16u;
    if (ctx.components[0].rows > src_height) {
        ctx.components[0].rows = src_height;
    }

    ctx.components[1].pixels = (uint8_t *)src_cb;
    ctx.components[1].width = src_width / 2u;
    ctx.components[1].height = src_height / 2u;
    ctx.components[1].stride = src_c_stride;
    ctx.components[1].row0 = 0u;
    ctx.components[1].rows = slice_count * SH264E_V1_SLICE_CHROMA_HEIGHT * 2u + 8u;
    if (ctx.components[1].rows > src_height / 2u) {
        ctx.components[1].rows = src_height / 2u;
    }

    ctx.components[2] = ctx.components[1];
    ctx.components[2].pixels = (uint8_t *)src_cr;

    status = streaming_begin_idr(&ctx, &total);
    if (status != SH264E_OK) {
        return status;
    }

    for (slice_index = 0u; slice_index < slice_count; slice_index++) {
        sh264e_slice_t slice;
        size_t bytes = 0u;

        if (output_pixfmt == SH264E_PIXFMT_I420) {
            streaming_make_i420_slice(&ctx, slice_index, &slice);
        } else {
            streaming_make_nv12_slice(&ctx, slice_index, &slice);
        }
        status = streaming_encode_idr_slice(&ctx, &slice, &bytes);
        if (status != SH264E_OK) {
            return status;
        }
        total += bytes;
    }

    *out_size = total;
    return SH264E_OK;
}

#endif

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
