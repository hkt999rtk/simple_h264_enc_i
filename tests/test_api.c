#include "sh264e.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SCALE_FP_BITS 16u
#define TEST_SCALE_FP_ONE (1u << TEST_SCALE_FP_BITS)
#define TEST_SCALE_FP_HALF (TEST_SCALE_FP_ONE >> 1u)
#define TEST_SCALE_FP_BLEND_ROUND ((uint64_t)1u << ((TEST_SCALE_FP_BITS * 2u) - 1u))

typedef struct test_scale_coord_t {
    uint32_t index;
    uint32_t fraction;
} test_scale_coord_t;

static int expect_status(const char *name, sh264e_status_t got, sh264e_status_t expected)
{
    if (got != expected) {
        fprintf(stderr, "%s: got %s, expected %s\n",
                name, sh264e_status_string(got), sh264e_status_string(expected));
        return 0;
    }
    return 1;
}

static int next_nal_type(const uint8_t *data, size_t size, size_t *offset, unsigned *type)
{
    size_t i;
    for (i = *offset; i + 3u < size; i++) {
        if (data[i] == 0x00u && data[i + 1u] == 0x00u && data[i + 2u] == 0x01u) {
            *type = (unsigned)(data[i + 3u] & 0x1fu);
            *offset = i + 4u;
            return 1;
        }
    }
    return 0;
}

static int next_nal_payload(const uint8_t *data,
                            size_t size,
                            size_t *offset,
                            unsigned *type,
                            const uint8_t **payload,
                            size_t *payload_size)
{
    size_t start;
    size_t end;

    if (!next_nal_type(data, size, offset, type)) {
        return 0;
    }
    start = *offset;
    end = size;
    while (start + 3u < end && data[end - 3u] == 0x00u &&
           data[end - 2u] == 0x00u && data[end - 1u] == 0x01u) {
        end -= 3u;
    }
    {
        size_t i;
        for (i = start; i + 3u < size; i++) {
            if (data[i] == 0x00u && data[i + 1u] == 0x00u && data[i + 2u] == 0x01u) {
                end = i;
                break;
            }
        }
    }
    *payload = data + start;
    *payload_size = end - start;
    return 1;
}

static int expect_no_more_nals(const uint8_t *data, size_t size, size_t offset)
{
    unsigned type = 0;
    return !next_nal_type(data, size, &offset, &type);
}

static int read_first_ue(const uint8_t *payload, size_t payload_size, unsigned *out_value)
{
    uint8_t rbsp[4096];
    size_t rbsp_size = 0u;
    size_t i;
    unsigned zero_count = 0u;
    size_t bit = 0u;
    unsigned leading_zero_bits = 0u;
    unsigned info_bits = 0u;

    for (i = 0; i < payload_size; i++) {
        const uint8_t byte = payload[i];
        if (zero_count >= 2u && byte == 0x03u) {
            zero_count = 0u;
            continue;
        }
        if (rbsp_size >= sizeof(rbsp)) {
            return 0;
        }
        rbsp[rbsp_size++] = byte;
        if (byte == 0x00u) {
            zero_count++;
        } else {
            zero_count = 0u;
        }
    }

#define READ_RBSP_BIT(dst)                                      \
    do {                                                        \
        if (bit >= rbsp_size * 8u) {                            \
            return 0;                                           \
        }                                                       \
        (dst) = (rbsp[bit / 8u] >> (7u - (bit % 8u))) & 1u;     \
        bit++;                                                  \
    } while (0)

    for (;;) {
        unsigned bit_value = 0u;
        READ_RBSP_BIT(bit_value);
        if (bit_value != 0u) {
            break;
        }
        leading_zero_bits++;
        if (leading_zero_bits >= 31u) {
            return 0;
        }
    }
    for (i = 0; i < leading_zero_bits; i++) {
        unsigned bit_value = 0u;
        READ_RBSP_BIT(bit_value);
        info_bits = (info_bits << 1u) | bit_value;
    }

#undef READ_RBSP_BIT

    *out_value = ((1u << leading_zero_bits) - 1u) + info_bits;
    return 1;
}

static int expect_idr_mb_sequence(const uint8_t *data,
                                  size_t size,
                                  size_t offset,
                                  unsigned first_mb,
                                  unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        unsigned type = 0u;
        unsigned got_first_mb = 0u;
        const uint8_t *payload = NULL;
        size_t payload_size = 0u;
        if (!next_nal_payload(data, size, &offset, &type, &payload, &payload_size) ||
            type != 5u) {
            fprintf(stderr, "missing IDR slice NALU %u\n", i);
            return 0;
        }
        if (!read_first_ue(payload, payload_size, &got_first_mb)) {
            fprintf(stderr, "could not parse first_mb_in_slice for IDR NALU %u\n", i);
            return 0;
        }
        if (got_first_mb != first_mb + i) {
            fprintf(stderr, "first_mb_in_slice[%u] got %u, expected %u\n",
                    i, got_first_mb, first_mb + i);
            return 0;
        }
    }
    if (!expect_no_more_nals(data, size, offset)) {
        fprintf(stderr, "unexpected extra NALU after IDR macroblock slices\n");
        return 0;
    }
    return 1;
}

static int expect_wrapper_nal_sequence(const uint8_t *data, size_t size)
{
    size_t offset = 0;
    unsigned type = 0;
    const unsigned idr_count = SH264E_V1_MB_WIDTH * SH264E_V1_SLICE_COUNT;

    if (!next_nal_type(data, size, &offset, &type) || type != 7u) {
        fprintf(stderr, "missing SPS NALU\n");
        return 0;
    }
    if (!next_nal_type(data, size, &offset, &type) || type != 8u) {
        fprintf(stderr, "missing PPS NALU\n");
        return 0;
    }
    return expect_idr_mb_sequence(data, size, offset, 0u, idr_count);
}

static int expect_header_nal_sequence(const uint8_t *data, size_t size)
{
    size_t offset = 0;
    unsigned type = 0;

    if (!next_nal_type(data, size, &offset, &type) || type != 7u) {
        fprintf(stderr, "missing SPS NALU\n");
        return 0;
    }
    if (!next_nal_type(data, size, &offset, &type) || type != 8u) {
        fprintf(stderr, "missing PPS NALU\n");
        return 0;
    }
    if (!expect_no_more_nals(data, size, offset)) {
        fprintf(stderr, "unexpected extra NALU after SPS/PPS\n");
        return 0;
    }
    return 1;
}

static void fill_i420(uint8_t *buf)
{
    const size_t y_size = (size_t)SH264E_V1_WIDTH * SH264E_V1_HEIGHT;
    const size_t c_size = (size_t)(SH264E_V1_WIDTH / 2u) * (SH264E_V1_HEIGHT / 2u);
    uint32_t y;
    uint32_t x;

    for (y = 0; y < SH264E_V1_HEIGHT; y++) {
        uint8_t *row = buf + (size_t)y * SH264E_V1_WIDTH;
        for (x = 0; x < SH264E_V1_WIDTH; x++) {
            row[x] = (uint8_t)(((x / 16u) + (y / 16u)) & 0xffu);
        }
    }
    memset(buf + y_size, 128, c_size);
    memset(buf + y_size + c_size, 128, c_size);
}

static void fill_i420_sized(uint8_t *buf, uint32_t width, uint32_t height)
{
    const size_t y_size = (size_t)width * height;
    const size_t c_size = (size_t)(width / 2u) * (height / 2u);
    uint32_t y;
    uint32_t x;

    for (y = 0; y < height; y++) {
        uint8_t *row = buf + (size_t)y * width;
        for (x = 0; x < width; x++) {
            row[x] = (uint8_t)(((x / 8u) + (y / 6u)) & 0xffu);
        }
    }
    memset(buf + y_size, 96, c_size);
    memset(buf + y_size + c_size, 176, c_size);
}

static void fill_nv12_sized(uint8_t *buf, uint32_t width, uint32_t height)
{
    const size_t y_size = (size_t)width * height;
    uint32_t y;
    uint32_t x;

    for (y = 0; y < height; y++) {
        uint8_t *row = buf + (size_t)y * width;
        for (x = 0; x < width; x++) {
            row[x] = (uint8_t)((x * 7u + y * 11u) & 0xffu);
        }
    }
    for (y = 0; y < height / 2u; y++) {
        uint8_t *row = buf + y_size + (size_t)y * width;
        for (x = 0; x < width / 2u; x++) {
            row[(size_t)x * 2u] = (uint8_t)(64u + ((x * 5u + y * 3u) & 63u));
            row[(size_t)x * 2u + 1u] = (uint8_t)(144u + ((x * 9u + y * 7u) & 63u));
        }
    }
}

static test_scale_coord_t test_scale_coord(uint32_t src_size, uint32_t dst_size, uint32_t dst_pos)
{
    const uint64_t denom = (uint64_t)dst_size * 2u;
    const uint64_t pos_num = (uint64_t)(2u * dst_pos + 1u) *
                             (uint64_t)src_size *
                             (uint64_t)TEST_SCALE_FP_ONE;
    const int64_t raw_pos = (int64_t)(pos_num / denom) - (int64_t)TEST_SCALE_FP_HALF;
    const uint64_t max_pos = (uint64_t)(src_size - 1u) * (uint64_t)TEST_SCALE_FP_ONE;
    test_scale_coord_t coord;

    if (raw_pos <= 0) {
        coord.index = 0u;
        coord.fraction = 0u;
        return coord;
    }
    if ((uint64_t)raw_pos >= max_pos) {
        coord.index = src_size - 1u;
        coord.fraction = 0u;
        return coord;
    }
    coord.index = (uint32_t)((uint64_t)raw_pos >> TEST_SCALE_FP_BITS);
    coord.fraction = (uint32_t)((uint64_t)raw_pos & (uint64_t)(TEST_SCALE_FP_ONE - 1u));
    return coord;
}

static uint8_t test_bilinear_blend_u8(uint8_t p00,
                                      uint8_t p01,
                                      uint8_t p10,
                                      uint8_t p11,
                                      uint32_t wx,
                                      uint32_t wy)
{
    const uint32_t inv_wx = TEST_SCALE_FP_ONE - wx;
    const uint32_t inv_wy = TEST_SCALE_FP_ONE - wy;
    const uint64_t top = (uint64_t)p00 * inv_wx + (uint64_t)p01 * wx;
    const uint64_t bottom = (uint64_t)p10 * inv_wx + (uint64_t)p11 * wx;
    const uint64_t blended = top * inv_wy + bottom * wy;

    return (uint8_t)((blended + TEST_SCALE_FP_BLEND_ROUND) >> (TEST_SCALE_FP_BITS * 2u));
}

static void reference_scale_plane_slice(const uint8_t *src,
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
    uint32_t y;

    for (y = 0; y < dst_rows; y++) {
        const test_scale_coord_t sy = test_scale_coord(src_height, dst_height, dst_y_start + y);
        const uint32_t y0 = sy.index;
        const uint32_t y1 = y0 + 1u < src_height ? y0 + 1u : y0;
        const uint8_t *row0 = src + (size_t)y0 * (size_t)src_stride;
        const uint8_t *row1 = src + (size_t)y1 * (size_t)src_stride;
        uint8_t *dst_row = dst + (size_t)y * (size_t)dst_stride;
        uint32_t x;

        for (x = 0; x < dst_width; x++) {
            const test_scale_coord_t sx = test_scale_coord(src_width, dst_width, x);
            const uint32_t x0 = sx.index;
            const uint32_t x1 = x0 + 1u < src_width ? x0 + 1u : x0;

            dst_row[x] = test_bilinear_blend_u8(row0[x0], row0[x1],
                                                row1[x0], row1[x1],
                                                sx.fraction, sy.fraction);
        }
    }
}

static void reference_scale_nv12_chroma_slice(const uint8_t *src_uv,
                                              uint32_t src_chroma_width,
                                              uint32_t src_chroma_height,
                                              ptrdiff_t src_stride,
                                              uint8_t *dst_uv,
                                              uint32_t dst_y_start)
{
    uint32_t y;

    for (y = 0; y < SH264E_V1_SLICE_CHROMA_HEIGHT; y++) {
        const test_scale_coord_t sy = test_scale_coord(src_chroma_height,
                                                       SH264E_V1_HEIGHT / 2u,
                                                       dst_y_start + y);
        const uint32_t y0 = sy.index;
        const uint32_t y1 = y0 + 1u < src_chroma_height ? y0 + 1u : y0;
        const uint8_t *row0 = src_uv + (size_t)y0 * (size_t)src_stride;
        const uint8_t *row1 = src_uv + (size_t)y1 * (size_t)src_stride;
        uint8_t *dst_row = dst_uv + (size_t)y * SH264E_V1_WIDTH;
        uint32_t x;

        for (x = 0; x < SH264E_V1_WIDTH / 2u; x++) {
            const test_scale_coord_t sx = test_scale_coord(src_chroma_width,
                                                           SH264E_V1_WIDTH / 2u,
                                                           x);
            const uint32_t x0 = sx.index;
            const uint32_t x1 = x0 + 1u < src_chroma_width ? x0 + 1u : x0;

            dst_row[(size_t)x * 2u] =
                test_bilinear_blend_u8(row0[(size_t)x0 * 2u],
                                       row0[(size_t)x1 * 2u],
                                       row1[(size_t)x0 * 2u],
                                       row1[(size_t)x1 * 2u],
                                       sx.fraction,
                                       sy.fraction);
            dst_row[(size_t)x * 2u + 1u] =
                test_bilinear_blend_u8(row0[(size_t)x0 * 2u + 1u],
                                       row0[(size_t)x1 * 2u + 1u],
                                       row1[(size_t)x0 * 2u + 1u],
                                       row1[(size_t)x1 * 2u + 1u],
                                       sx.fraction,
                                       sy.fraction);
        }
    }
}

static int expect_resize_reference_slice(const char *name,
                                         uint32_t width,
                                         uint32_t height,
                                         sh264e_pixfmt_t pixfmt,
                                         unsigned slice_index)
{
    const size_t src_size = (size_t)width * height * 3u / 2u;
    const size_t dst_y_size = (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT;
    const size_t dst_c_size = (size_t)(SH264E_V1_WIDTH / 2u) * SH264E_V1_SLICE_CHROMA_HEIGHT;
    const size_t dst_uv_size = (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT;
    uint8_t *src = (uint8_t *)malloc(src_size);
    uint8_t *work = NULL;
    uint8_t *expected = NULL;
    sh264e_frame_t frame;
    sh264e_slice_t slice;
    size_t work_size = 0u;
    int ok = 1;

    if (src == NULL) {
        fprintf(stderr, "%s: source allocation failed\n", name);
        return 0;
    }
    if (pixfmt == SH264E_PIXFMT_I420) {
        fill_i420_sized(src, width, height);
    } else {
        fill_nv12_sized(src, width, height);
    }

    memset(&frame, 0, sizeof(frame));
    frame.width = width;
    frame.height = height;
    frame.pixfmt = pixfmt;
    frame.plane[0] = src;
    frame.stride[0] = width;
    if (pixfmt == SH264E_PIXFMT_I420) {
        const size_t y_size = (size_t)width * height;
        const size_t c_size = (size_t)(width / 2u) * (height / 2u);

        frame.plane[1] = src + y_size;
        frame.plane[2] = src + y_size + c_size;
        frame.stride[1] = width / 2u;
        frame.stride[2] = width / 2u;
    } else {
        frame.plane[1] = src + (size_t)width * height;
        frame.stride[1] = width;
    }

    if (!expect_status(name, sh264e_resize_get_slice_buffer_size(&frame, &work_size), SH264E_OK)) {
        free(src);
        return 0;
    }
    work = (uint8_t *)malloc(work_size);
    expected = (uint8_t *)malloc(work_size);
    if (work == NULL || expected == NULL) {
        fprintf(stderr, "%s: work allocation failed\n", name);
        free(src);
        free(work);
        free(expected);
        return 0;
    }
    memset(work, 0xa5, work_size);
    memset(expected, 0x5a, work_size);

    if (!expect_status(name,
                       sh264e_resize_make_slice(&frame, slice_index, work, work_size, &slice),
                       SH264E_OK)) {
        ok = 0;
    }
    if (ok && slice.plane[0] != work) {
        fprintf(stderr, "%s: scaled output did not use work buffer\n", name);
        ok = 0;
    }

    if (ok) {
        reference_scale_plane_slice(frame.plane[0], width, height, frame.stride[0],
                                    expected, SH264E_V1_WIDTH, SH264E_V1_HEIGHT,
                                    slice_index * SH264E_V1_SLICE_LUMA_HEIGHT,
                                    SH264E_V1_SLICE_LUMA_HEIGHT, SH264E_V1_WIDTH);
        if (memcmp(slice.plane[0], expected, dst_y_size) != 0) {
            fprintf(stderr, "%s: luma output differs from reference scaler\n", name);
            ok = 0;
        }
    }

    if (ok && pixfmt == SH264E_PIXFMT_I420) {
        uint8_t *expected_u = expected + dst_y_size;
        uint8_t *expected_v = expected_u + dst_c_size;

        reference_scale_plane_slice(frame.plane[1], width / 2u, height / 2u, frame.stride[1],
                                    expected_u, SH264E_V1_WIDTH / 2u, SH264E_V1_HEIGHT / 2u,
                                    slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT,
                                    SH264E_V1_SLICE_CHROMA_HEIGHT, SH264E_V1_WIDTH / 2u);
        reference_scale_plane_slice(frame.plane[2], width / 2u, height / 2u, frame.stride[2],
                                    expected_v, SH264E_V1_WIDTH / 2u, SH264E_V1_HEIGHT / 2u,
                                    slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT,
                                    SH264E_V1_SLICE_CHROMA_HEIGHT, SH264E_V1_WIDTH / 2u);
        if (memcmp(slice.plane[1], expected_u, dst_c_size) != 0 ||
            memcmp(slice.plane[2], expected_v, dst_c_size) != 0) {
            fprintf(stderr, "%s: I420 chroma output differs from reference scaler\n", name);
            ok = 0;
        }
    } else if (ok) {
        uint8_t *expected_uv = expected + dst_y_size;

        reference_scale_nv12_chroma_slice(frame.plane[1], width / 2u, height / 2u,
                                          frame.stride[1], expected_uv,
                                          slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT);
        if (memcmp(slice.plane[1], expected_uv, dst_uv_size) != 0) {
            fprintf(stderr, "%s: NV12 chroma output differs from reference scaler\n", name);
            ok = 0;
        }
    }

    free(src);
    free(work);
    free(expected);
    return ok;
}

static void make_i420_slice(const uint8_t *input, unsigned slice_index, sh264e_slice_t *slice)
{
    const size_t y_size = (size_t)SH264E_V1_WIDTH * SH264E_V1_HEIGHT;
    const size_t c_size = (size_t)(SH264E_V1_WIDTH / 2u) * (SH264E_V1_HEIGHT / 2u);
    const uint8_t *u = input + y_size;
    const uint8_t *v = u + c_size;

    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = SH264E_PIXFMT_I420;
    slice->plane[0] = input + (size_t)slice_index * SH264E_V1_SLICE_LUMA_HEIGHT * SH264E_V1_WIDTH;
    slice->plane[1] = u + (size_t)slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT * (SH264E_V1_WIDTH / 2u);
    slice->plane[2] = v + (size_t)slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT * (SH264E_V1_WIDTH / 2u);
    slice->stride[0] = SH264E_V1_WIDTH;
    slice->stride[1] = SH264E_V1_WIDTH / 2u;
    slice->stride[2] = SH264E_V1_WIDTH / 2u;
}

static void make_nv12_from_i420(const uint8_t *i420, uint8_t *nv12)
{
    const size_t y_size = (size_t)SH264E_V1_WIDTH * SH264E_V1_HEIGHT;
    const size_t c_size = (size_t)(SH264E_V1_WIDTH / 2u) * (SH264E_V1_HEIGHT / 2u);
    const uint8_t *u = i420 + y_size;
    const uint8_t *v = u + c_size;
    uint8_t *uv = nv12 + y_size;
    size_t i;

    memcpy(nv12, i420, y_size);
    for (i = 0; i < c_size; i++) {
        uv[i * 2u] = u[i];
        uv[i * 2u + 1u] = v[i];
    }
}

int main(void)
{
    sh264e_config_t config;
    sh264e_config_t bad_config;
    sh264e_frame_t frame;
    sh264e_encoder_t *encoder = NULL;
    sh264e_status_t status;
    uint8_t *input = NULL;
    uint8_t *output = NULL;
    uint8_t *nv12_input = NULL;
    uint8_t *nv12_output = NULL;
    size_t input_size;
    size_t output_capacity = 0;
    size_t header_capacity = 0;
    size_t slice_capacity = 0;
    size_t output_size = 0;
    size_t jpeg_work_size = 0;
    size_t encoder_work_size = 0;
    uint8_t *slice_output = NULL;
    uint8_t *small_input = NULL;
    uint8_t *resize_work = NULL;
    uint8_t *encoder_arena_alloc = NULL;
    sh264e_encoder_t *arena_encoder = NULL;
    sh264e_encoder_t *nv12_encoder = NULL;
    sh264e_jpeg_allocation_stats_t jpeg_alloc_stats;
    sh264e_encoder_memory_report_t encoder_memory_report;
    sh264e_jpeg_source_t bad_jpeg_source;
    int ok = 1;

    memset(&config, 0, sizeof(config));
    config.width = SH264E_V1_WIDTH;
    config.height = SH264E_V1_HEIGHT;
    config.pixfmt = SH264E_PIXFMT_I420;
    config.qp = SH264E_DEFAULT_QP;

    ok &= expect_status("null create config",
                        sh264e_encoder_create(NULL, &encoder),
                        SH264E_ERR_INVALID_ARGUMENT);

    bad_config = config;
    bad_config.width = 1920;
    ok &= expect_status("unsupported dimensions",
                        sh264e_encoder_create(&bad_config, &encoder),
                        SH264E_ERR_UNSUPPORTED_CONFIG);

    ok &= expect_status("max output size",
                        sh264e_get_max_output_size(&config, &output_capacity),
                        SH264E_OK);
    ok &= expect_status("max header output size",
                        sh264e_get_max_header_output_size(&config, &header_capacity),
                        SH264E_OK);
    ok &= expect_status("max slice output size",
                        sh264e_get_max_slice_output_size(&config, &slice_capacity),
                        SH264E_OK);
    ok &= expect_status("null encoder memory config",
                        sh264e_encoder_get_memory_report(NULL, &encoder_memory_report),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("null encoder memory report",
                        sh264e_encoder_get_memory_report(&config, NULL),
                        SH264E_ERR_INVALID_ARGUMENT);
    bad_config = config;
    bad_config.height = 720u;
    ok &= expect_status("unsupported encoder memory config",
                        sh264e_encoder_get_memory_report(&bad_config, &encoder_memory_report),
                        SH264E_ERR_UNSUPPORTED_CONFIG);
    ok &= expect_status("encoder memory report",
                        sh264e_encoder_get_memory_report(&config, &encoder_memory_report),
                        SH264E_OK);
    ok &= expect_status("null encoder work-size config",
                        sh264e_encoder_get_work_size(NULL, &encoder_work_size),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("null encoder work-size output",
                        sh264e_encoder_get_work_size(&config, NULL),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("encoder work size",
                        sh264e_encoder_get_work_size(&config, &encoder_work_size),
                        SH264E_OK);
    if (output_capacity == 0u) {
        fprintf(stderr, "max output size returned zero\n");
        ok = 0;
    }
    if (header_capacity == 0u || slice_capacity == 0u) {
        fprintf(stderr, "progressive max output size returned zero\n");
        ok = 0;
    }
    if (encoder_memory_report.context_bytes != 40u ||
        encoder_memory_report.bitstream_scratch_bytes != 256u ||
        encoder_memory_report.recon_luma_bytes != 0u ||
        encoder_memory_report.recon_chroma_bytes != 0u ||
        encoder_memory_report.neighbor_state_bytes != 0u ||
        encoder_memory_report.total_bytes != 296u) {
        fprintf(stderr,
                "unexpected encoder memory report: context=%zu bitstream=%zu "
                "luma=%zu chroma=%zu neighbor=%zu total=%zu\n",
                encoder_memory_report.context_bytes,
                encoder_memory_report.bitstream_scratch_bytes,
                encoder_memory_report.recon_luma_bytes,
                encoder_memory_report.recon_chroma_bytes,
                encoder_memory_report.neighbor_state_bytes,
                encoder_memory_report.total_bytes);
        ok = 0;
    }
    if (encoder_memory_report.total_bytes !=
        encoder_memory_report.context_bytes +
        encoder_memory_report.bitstream_scratch_bytes +
        encoder_memory_report.recon_luma_bytes +
        encoder_memory_report.recon_chroma_bytes +
        encoder_memory_report.neighbor_state_bytes) {
        fprintf(stderr, "encoder memory total does not match sub-block sum\n");
        ok = 0;
    }
    if (encoder_work_size != encoder_memory_report.total_bytes + sizeof(void *) - 1u) {
        fprintf(stderr, "unexpected encoder arena work size: %zu\n", encoder_work_size);
        ok = 0;
    }
    ok &= expect_status("null encoder arena",
                        sh264e_encoder_create_with_arena(&config, NULL,
                                                         encoder_work_size, &arena_encoder),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("null encoder arena output",
                        sh264e_encoder_create_with_arena(&config, input,
                                                         encoder_work_size, NULL),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("null JPEG allocation stats",
                        sh264e_jpeg_get_last_allocation_stats(NULL),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("null JPEG work-size input",
                        sh264e_jpeg_get_work_size(NULL, 1u, &jpeg_work_size),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("null JPEG work-size output",
                        sh264e_jpeg_get_work_size((const uint8_t *)"x", 1u, NULL),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("null JPEG slice-work input",
                        sh264e_jpeg_get_slice_work_size(NULL, 1u, SH264E_PIXFMT_I420, &jpeg_work_size),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("null JPEG slice-work output",
                        sh264e_jpeg_get_slice_work_size((const uint8_t *)"x", 1u,
                                                        SH264E_PIXFMT_I420, NULL),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("unsupported JPEG slice-work format",
                        sh264e_jpeg_get_slice_work_size((const uint8_t *)"x", 1u,
                                                        (sh264e_pixfmt_t)99, &jpeg_work_size),
                        SH264E_ERR_UNSUPPORTED_CONFIG);
    memset(&bad_jpeg_source, 0, sizeof(bad_jpeg_source));
    ok &= expect_status("null JPEG source work-size input",
                        sh264e_jpeg_source_get_work_size(NULL, &jpeg_work_size),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("null JPEG source work-size reader",
                        sh264e_jpeg_source_get_work_size(&bad_jpeg_source, &jpeg_work_size),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("null JPEG source work-size output",
                        sh264e_jpeg_source_get_work_size(&bad_jpeg_source, NULL),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("null JPEG source slice-work input",
                        sh264e_jpeg_source_get_slice_work_size(NULL,
                                                               SH264E_PIXFMT_I420,
                                                               &jpeg_work_size),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("unsupported JPEG source slice-work format",
                        sh264e_jpeg_source_get_slice_work_size(&bad_jpeg_source,
                                                               (sh264e_pixfmt_t)99,
                                                               &jpeg_work_size),
                        SH264E_ERR_UNSUPPORTED_CONFIG);
    ok &= expect_status("null JPEG arena encode",
                        sh264e_encode_jpeg_idr_with_arena(NULL, NULL, 0u, NULL, 0u,
                                                          NULL, 0u, NULL, 0u, NULL),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("null JPEG stream consumer",
                        sh264e_encode_jpeg_idr_with_arena_stream(NULL, NULL, 0u, NULL, 0u,
                                                                 NULL, 0u, NULL, 0u, NULL, NULL),
                        SH264E_ERR_INVALID_ARGUMENT);
    ok &= expect_status("null JPEG source stream consumer",
                        sh264e_encode_jpeg_source_idr_with_arena_stream(NULL, NULL,
                                                                        NULL, 0u,
                                                                        NULL, 0u,
                                                                        NULL, 0u,
                                                                        NULL, NULL),
                        SH264E_ERR_INVALID_ARGUMENT);
    jpeg_alloc_stats.current_bytes = 123u;
    jpeg_alloc_stats.peak_bytes = 456u;
    ok &= expect_status("initial JPEG allocation stats",
                        sh264e_jpeg_get_last_allocation_stats(&jpeg_alloc_stats),
                        SH264E_OK);
    if (jpeg_alloc_stats.current_bytes != 0u || jpeg_alloc_stats.peak_bytes != 0u) {
        fprintf(stderr, "initial JPEG allocation stats should be zero\n");
        ok = 0;
    }
    if (sh264e_jpeg_get_last_slice_work_bytes() != 0u) {
        fprintf(stderr, "initial JPEG effective slice work should be zero\n");
        ok = 0;
    }

    status = sh264e_encoder_create(&config, &encoder);
    ok &= expect_status("create encoder", status, SH264E_OK);
    if (!ok || encoder == NULL) {
        sh264e_encoder_destroy(encoder);
        return 1;
    }

    input_size = (size_t)SH264E_V1_WIDTH * SH264E_V1_HEIGHT * 3u / 2u;
    input = (uint8_t *)malloc(input_size);
    output = (uint8_t *)malloc(output_capacity);
    nv12_input = (uint8_t *)malloc(input_size);
    nv12_output = (uint8_t *)malloc(output_capacity);
    slice_output = (uint8_t *)malloc(slice_capacity);
    if (input == NULL || output == NULL || nv12_input == NULL ||
        nv12_output == NULL || slice_output == NULL) {
        fprintf(stderr, "allocation failed\n");
        sh264e_encoder_destroy(encoder);
        free(input);
        free(output);
        free(nv12_input);
        free(nv12_output);
        free(slice_output);
        return 1;
    }
    fill_i420(input);

    {
        sh264e_frame_t resize_frame;
        sh264e_slice_t resized_slice;
        size_t resize_work_size = 123u;
        const size_t small_input_size = (size_t)1280u * 720u * 3u / 2u;

        memset(&resize_frame, 0, sizeof(resize_frame));
        resize_frame.width = SH264E_V1_WIDTH;
        resize_frame.height = SH264E_V1_HEIGHT;
        resize_frame.pixfmt = SH264E_PIXFMT_I420;
        resize_frame.plane[0] = input;
        resize_frame.plane[1] = input + (size_t)SH264E_V1_WIDTH * SH264E_V1_HEIGHT;
        resize_frame.plane[2] = resize_frame.plane[1] +
                                (size_t)(SH264E_V1_WIDTH / 2u) * (SH264E_V1_HEIGHT / 2u);
        resize_frame.stride[0] = SH264E_V1_WIDTH;
        resize_frame.stride[1] = SH264E_V1_WIDTH / 2u;
        resize_frame.stride[2] = SH264E_V1_WIDTH / 2u;

        ok &= expect_status("resize bypass work size",
                            sh264e_resize_get_slice_buffer_size(&resize_frame, &resize_work_size),
                            SH264E_OK);
        if (resize_work_size != 0u) {
            fprintf(stderr, "1:1 resize should not require work buffer\n");
            ok = 0;
        }
        ok &= expect_status("resize bypass slice",
                            sh264e_resize_make_slice(&resize_frame, 3u, NULL, 0u, &resized_slice),
                            SH264E_OK);
        if (resized_slice.plane[0] != input + (size_t)3u * SH264E_V1_SLICE_LUMA_HEIGHT * SH264E_V1_WIDTH) {
            fprintf(stderr, "1:1 resize did not bypass luma copy\n");
            ok = 0;
        }

        small_input = (uint8_t *)malloc(small_input_size);
        if (small_input == NULL) {
            fprintf(stderr, "small resize allocation failed\n");
            ok = 0;
        } else {
            fill_i420_sized(small_input, 1280u, 720u);
            resize_frame.width = 1280u;
            resize_frame.height = 720u;
            resize_frame.plane[0] = small_input;
            resize_frame.plane[1] = small_input + (size_t)1280u * 720u;
            resize_frame.plane[2] = resize_frame.plane[1] + (size_t)640u * 360u;
            resize_frame.stride[0] = 1280u;
            resize_frame.stride[1] = 640u;
            resize_frame.stride[2] = 640u;

            ok &= expect_status("resize scaled work size",
                                sh264e_resize_get_slice_buffer_size(&resize_frame, &resize_work_size),
                                SH264E_OK);
            if (resize_work_size == 0u) {
                fprintf(stderr, "scaled resize should require work buffer\n");
                ok = 0;
            }
            resize_work = (uint8_t *)malloc(resize_work_size);
            if (resize_work == NULL) {
                fprintf(stderr, "resize work allocation failed\n");
                ok = 0;
            } else {
                ok &= expect_status("resize scaled slice",
                                    sh264e_resize_make_slice(&resize_frame, 0u,
                                                             resize_work, resize_work_size,
                                                             &resized_slice),
                                    SH264E_OK);
                if (resized_slice.plane[0] != resize_work ||
                    resized_slice.stride[0] != (ptrdiff_t)SH264E_V1_WIDTH ||
                    resized_slice.stride[1] != (ptrdiff_t)(SH264E_V1_WIDTH / 2u)) {
                    fprintf(stderr, "scaled resize returned unexpected slice layout\n");
                    ok = 0;
                }
            }
            resize_frame.width = 1281u;
            ok &= expect_status("resize odd width",
                                sh264e_resize_get_slice_buffer_size(&resize_frame, &resize_work_size),
                                SH264E_ERR_UNSUPPORTED_CONFIG);
        }
    }

    ok &= expect_resize_reference_slice("2x I420 resize reference",
                                        1280u, 720u, SH264E_PIXFMT_I420, 0u);
    ok &= expect_resize_reference_slice("2x NV12 resize reference",
                                        1280u, 720u, SH264E_PIXFMT_NV12,
                                        SH264E_V1_SLICE_COUNT - 1u);
    ok &= expect_resize_reference_slice("0.5x I420 resize reference",
                                        5120u, 2880u, SH264E_PIXFMT_I420, 37u);
    ok &= expect_resize_reference_slice("0.5x NV12 resize reference",
                                        5120u, 2880u, SH264E_PIXFMT_NV12,
                                        SH264E_V1_SLICE_COUNT - 1u);

    memset(&frame, 0, sizeof(frame));
    frame.width = SH264E_V1_WIDTH;
    frame.height = SH264E_V1_HEIGHT;
    frame.pixfmt = SH264E_PIXFMT_I420;
    frame.plane[0] = input;
    frame.plane[1] = input + (size_t)SH264E_V1_WIDTH * SH264E_V1_HEIGHT;
    frame.plane[2] = frame.plane[1] + (size_t)(SH264E_V1_WIDTH / 2u) * (SH264E_V1_HEIGHT / 2u);
    frame.stride[0] = SH264E_V1_WIDTH;
    frame.stride[1] = SH264E_V1_WIDTH / 2u;
    frame.stride[2] = SH264E_V1_WIDTH / 2u;

    encoder_arena_alloc = (uint8_t *)malloc(encoder_work_size + 1u);
    if (encoder_arena_alloc == NULL) {
        fprintf(stderr, "encoder arena allocation failed\n");
        ok = 0;
    } else {
        ok &= expect_status("encoder arena too small",
                            sh264e_encoder_create_with_arena(&config,
                                                             encoder_arena_alloc + 1u,
                                                             encoder_work_size - 1u,
                                                             &arena_encoder),
                            SH264E_ERR_BUFFER_TOO_SMALL);
        ok &= expect_status("create arena encoder",
                            sh264e_encoder_create_with_arena(&config,
                                                             encoder_arena_alloc + 1u,
                                                             encoder_work_size,
                                                             &arena_encoder),
                            SH264E_OK);
        if (arena_encoder != NULL) {
            output_size = 0u;
            status = sh264e_encode_idr(arena_encoder, &frame, output, output_capacity, &output_size);
            ok &= expect_status("arena encode idr", status, SH264E_OK);
            if (status == SH264E_OK && !expect_wrapper_nal_sequence(output, output_size)) {
                ok = 0;
            }
            sh264e_encoder_destroy(arena_encoder);
            arena_encoder = NULL;
            encoder_arena_alloc[0] = 0xa5u;
        }
    }

    ok &= expect_status("small output buffer",
                        sh264e_encode_idr(encoder, &frame, output, 8u, &output_size),
                        SH264E_ERR_BUFFER_TOO_SMALL);

    output_size = 0;
    status = sh264e_encode_idr(encoder, &frame, output, output_capacity, &output_size);
    ok &= expect_status("encode idr", status, SH264E_OK);
    if (output_size == 0u) {
        fprintf(stderr, "encode produced empty output\n");
        ok = 0;
    }

    if (!expect_wrapper_nal_sequence(output, output_size)) {
        ok = 0;
    }

    {
        sh264e_config_t nv12_config = config;
        sh264e_frame_t nv12_frame;
        size_t nv12_output_size = 0u;

        make_nv12_from_i420(input, nv12_input);
        nv12_config.pixfmt = SH264E_PIXFMT_NV12;
        ok &= expect_status("create NV12 encoder",
                            sh264e_encoder_create(&nv12_config, &nv12_encoder),
                            SH264E_OK);
        memset(&nv12_frame, 0, sizeof(nv12_frame));
        nv12_frame.width = SH264E_V1_WIDTH;
        nv12_frame.height = SH264E_V1_HEIGHT;
        nv12_frame.pixfmt = SH264E_PIXFMT_NV12;
        nv12_frame.plane[0] = nv12_input;
        nv12_frame.plane[1] = nv12_input + (size_t)SH264E_V1_WIDTH * SH264E_V1_HEIGHT;
        nv12_frame.stride[0] = SH264E_V1_WIDTH;
        nv12_frame.stride[1] = SH264E_V1_WIDTH;
        if (nv12_encoder != NULL) {
            status = sh264e_encode_idr(nv12_encoder, &nv12_frame,
                                       nv12_output, output_capacity, &nv12_output_size);
            ok &= expect_status("NV12 equivalent encode idr", status, SH264E_OK);
            if (status == SH264E_OK &&
                (nv12_output_size != output_size ||
                 memcmp(nv12_output, output, output_size) != 0)) {
                fprintf(stderr, "NV12 equivalent encode differs from I420 output\n");
                ok = 0;
            }
            sh264e_encoder_destroy(nv12_encoder);
            nv12_encoder = NULL;
        }
    }

    {
        sh264e_slice_t slice;
        unsigned i;

        make_i420_slice(input, 0u, &slice);
        ok &= expect_status("slice before begin",
                            sh264e_encode_idr_slice(encoder, &slice, slice_output, slice_capacity, &output_size),
                            SH264E_ERR_BAD_STATE);

        ok &= expect_status("begin idr",
                            sh264e_begin_idr(encoder, output, header_capacity, &output_size),
                            SH264E_OK);
        if (!expect_header_nal_sequence(output, output_size)) {
            fprintf(stderr, "begin should emit SPS/PPS only\n");
            ok = 0;
        }
        ok &= expect_status("begin twice",
                            sh264e_begin_idr(encoder, output, header_capacity, &output_size),
                            SH264E_ERR_BAD_STATE);
        ok &= expect_status("end before complete",
                            sh264e_end_idr(encoder),
                            SH264E_ERR_INCOMPLETE_FRAME);

        for (i = 0; i < SH264E_V1_SLICE_COUNT; i++) {
            make_i420_slice(input, i, &slice);
            ok &= expect_status("encode progressive slice",
                                sh264e_encode_idr_slice(encoder, &slice, slice_output, slice_capacity, &output_size),
                                SH264E_OK);
            if (!expect_idr_mb_sequence(slice_output, output_size, 0u,
                                        i * SH264E_V1_MB_WIDTH,
                                        SH264E_V1_MB_WIDTH)) {
                ok = 0;
            }
        }

        make_i420_slice(input, SH264E_V1_SLICE_COUNT - 1u, &slice);
        ok &= expect_status("91st slice",
                            sh264e_encode_idr_slice(encoder, &slice, slice_output, slice_capacity, &output_size),
                            SH264E_ERR_FRAME_COMPLETE);
        ok &= expect_status("end complete",
                            sh264e_end_idr(encoder),
                            SH264E_OK);
        ok &= expect_status("end idle",
                            sh264e_end_idr(encoder),
                            SH264E_ERR_BAD_STATE);
    }

    sh264e_encoder_destroy(arena_encoder);
    sh264e_encoder_destroy(nv12_encoder);
    sh264e_encoder_destroy(encoder);
    free(input);
    free(output);
    free(nv12_input);
    free(nv12_output);
    free(slice_output);
    free(resize_work);
    free(small_input);
    free(encoder_arena_alloc);
    return ok ? 0 : 1;
}
