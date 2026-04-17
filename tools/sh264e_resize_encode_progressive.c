#include "sh264e.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SH264E_SCALE_MIN_SRC_WIDTH 1280u
#define SH264E_SCALE_MAX_SRC_WIDTH 5120u
#define SH264E_SCALE_MIN_SRC_HEIGHT 720u
#define SH264E_SCALE_MAX_SRC_HEIGHT 2880u
#define SH264E_DST_CHROMA_WIDTH (SH264E_V1_WIDTH / 2u)
#define SH264E_DST_CHROMA_HEIGHT (SH264E_V1_HEIGHT / 2u)

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --format i420|nv12 --src-width W --src-height H input.yuv output.h264\n",
            argv0);
}

static int parse_format(const char *text, sh264e_pixfmt_t *pixfmt)
{
    if (strcmp(text, "i420") == 0) {
        *pixfmt = SH264E_PIXFMT_I420;
        return 1;
    }
    if (strcmp(text, "nv12") == 0) {
        *pixfmt = SH264E_PIXFMT_NV12;
        return 1;
    }
    return 0;
}

static int parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT_MAX) {
        return 0;
    }
    *out = (uint32_t)value;
    return 1;
}

static int validate_source_size(uint32_t width, uint32_t height)
{
    if (width < SH264E_SCALE_MIN_SRC_WIDTH || width > SH264E_SCALE_MAX_SRC_WIDTH ||
        height < SH264E_SCALE_MIN_SRC_HEIGHT || height > SH264E_SCALE_MAX_SRC_HEIGHT) {
        return 0;
    }
    if ((width & 1u) != 0u || (height & 1u) != 0u) {
        return 0;
    }
    return 1;
}

static int read_exact(FILE *fp, uint8_t *buf, size_t size)
{
    return fread(buf, 1u, size, fp) == size;
}

static int write_exact(FILE *fp, const uint8_t *buf, size_t size)
{
    return fwrite(buf, 1u, size, fp) == size;
}

static int clip_u8_int(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return value;
}

static double map_half_pixel(uint32_t dst_pos, uint32_t src_size, uint32_t dst_size)
{
    double pos = ((((double)dst_pos + 0.5) * (double)src_size) / (double)dst_size) - 0.5;
    const double max_pos = (double)(src_size - 1u);

    if (pos < 0.0) {
        return 0.0;
    }
    if (pos > max_pos) {
        return max_pos;
    }
    return pos;
}

static uint8_t bilinear_sample_plane(const uint8_t *src,
                                     uint32_t src_width,
                                     uint32_t src_height,
                                     ptrdiff_t src_stride,
                                     uint32_t dst_x,
                                     uint32_t dst_y,
                                     uint32_t dst_width,
                                     uint32_t dst_height)
{
    const double sx = map_half_pixel(dst_x, src_width, dst_width);
    const double sy = map_half_pixel(dst_y, src_height, dst_height);
    const uint32_t x0 = (uint32_t)sx;
    const uint32_t y0 = (uint32_t)sy;
    const uint32_t x1 = x0 + 1u < src_width ? x0 + 1u : x0;
    const uint32_t y1 = y0 + 1u < src_height ? y0 + 1u : y0;
    const double wx = sx - (double)x0;
    const double wy = sy - (double)y0;
    const uint8_t *row0 = src + (size_t)y0 * (size_t)src_stride;
    const uint8_t *row1 = src + (size_t)y1 * (size_t)src_stride;
    const double p00 = (double)row0[x0];
    const double p01 = (double)row0[x1];
    const double p10 = (double)row1[x0];
    const double p11 = (double)row1[x1];
    const double top = p00 + (p01 - p00) * wx;
    const double bottom = p10 + (p11 - p10) * wx;
    const int rounded = (int)(top + (bottom - top) * wy + 0.5);

    return (uint8_t)clip_u8_int(rounded);
}

static uint8_t bilinear_sample_nv12_chroma(const uint8_t *src,
                                           uint32_t src_width,
                                           uint32_t src_height,
                                           ptrdiff_t src_stride,
                                           unsigned component,
                                           uint32_t dst_x,
                                           uint32_t dst_y)
{
    const double sx = map_half_pixel(dst_x, src_width, SH264E_DST_CHROMA_WIDTH);
    const double sy = map_half_pixel(dst_y, src_height, SH264E_DST_CHROMA_HEIGHT);
    const uint32_t x0 = (uint32_t)sx;
    const uint32_t y0 = (uint32_t)sy;
    const uint32_t x1 = x0 + 1u < src_width ? x0 + 1u : x0;
    const uint32_t y1 = y0 + 1u < src_height ? y0 + 1u : y0;
    const double wx = sx - (double)x0;
    const double wy = sy - (double)y0;
    const uint8_t *row0 = src + (size_t)y0 * (size_t)src_stride;
    const uint8_t *row1 = src + (size_t)y1 * (size_t)src_stride;
    const size_t c = component;
    const double p00 = (double)row0[(size_t)x0 * 2u + c];
    const double p01 = (double)row0[(size_t)x1 * 2u + c];
    const double p10 = (double)row1[(size_t)x0 * 2u + c];
    const double p11 = (double)row1[(size_t)x1 * 2u + c];
    const double top = p00 + (p01 - p00) * wx;
    const double bottom = p10 + (p11 - p10) * wx;
    const int rounded = (int)(top + (bottom - top) * wy + 0.5);

    return (uint8_t)clip_u8_int(rounded);
}

static void scale_plane_slice(const uint8_t *src,
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
    uint32_t x;

    for (y = 0; y < dst_rows; y++) {
        uint8_t *dst_row = dst + (size_t)y * (size_t)dst_stride;
        const uint32_t dst_y = dst_y_start + y;
        for (x = 0; x < dst_width; x++) {
            dst_row[x] = bilinear_sample_plane(src, src_width, src_height, src_stride,
                                               x, dst_y, dst_width, dst_height);
        }
    }
}

static void scale_nv12_chroma_slice(const uint8_t *src_uv,
                                    uint32_t src_chroma_width,
                                    uint32_t src_chroma_height,
                                    ptrdiff_t src_stride,
                                    uint8_t *dst_uv,
                                    uint32_t dst_y_start)
{
    uint32_t y;
    uint32_t x;

    for (y = 0; y < SH264E_V1_SLICE_CHROMA_HEIGHT; y++) {
        uint8_t *dst_row = dst_uv + (size_t)y * SH264E_V1_WIDTH;
        const uint32_t dst_y = dst_y_start + y;
        for (x = 0; x < SH264E_DST_CHROMA_WIDTH; x++) {
            dst_row[(size_t)x * 2u] = bilinear_sample_nv12_chroma(src_uv, src_chroma_width,
                                                                  src_chroma_height, src_stride,
                                                                  0u, x, dst_y);
            dst_row[(size_t)x * 2u + 1u] = bilinear_sample_nv12_chroma(src_uv, src_chroma_width,
                                                                      src_chroma_height, src_stride,
                                                                      1u, x, dst_y);
        }
    }
}

static void fill_scaled_slice_i420(const uint8_t *src,
                                   uint32_t src_width,
                                   uint32_t src_height,
                                   unsigned slice_index,
                                   uint8_t *slice_mem,
                                   sh264e_slice_t *slice)
{
    const uint8_t *src_y = src;
    const uint8_t *src_u = src_y + (size_t)src_width * src_height;
    const uint8_t *src_v = src_u + (size_t)(src_width / 2u) * (src_height / 2u);
    uint8_t *dst_y = slice_mem;
    uint8_t *dst_u = dst_y + (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT;
    uint8_t *dst_v = dst_u + (size_t)SH264E_DST_CHROMA_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT;

    scale_plane_slice(src_y, src_width, src_height, (ptrdiff_t)src_width,
                      dst_y, SH264E_V1_WIDTH, SH264E_V1_HEIGHT,
                      slice_index * SH264E_V1_SLICE_LUMA_HEIGHT,
                      SH264E_V1_SLICE_LUMA_HEIGHT, SH264E_V1_WIDTH);
    scale_plane_slice(src_u, src_width / 2u, src_height / 2u, (ptrdiff_t)(src_width / 2u),
                      dst_u, SH264E_DST_CHROMA_WIDTH, SH264E_DST_CHROMA_HEIGHT,
                      slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT,
                      SH264E_V1_SLICE_CHROMA_HEIGHT, SH264E_DST_CHROMA_WIDTH);
    scale_plane_slice(src_v, src_width / 2u, src_height / 2u, (ptrdiff_t)(src_width / 2u),
                      dst_v, SH264E_DST_CHROMA_WIDTH, SH264E_DST_CHROMA_HEIGHT,
                      slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT,
                      SH264E_V1_SLICE_CHROMA_HEIGHT, SH264E_DST_CHROMA_WIDTH);

    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = SH264E_PIXFMT_I420;
    slice->plane[0] = dst_y;
    slice->plane[1] = dst_u;
    slice->plane[2] = dst_v;
    slice->stride[0] = SH264E_V1_WIDTH;
    slice->stride[1] = SH264E_DST_CHROMA_WIDTH;
    slice->stride[2] = SH264E_DST_CHROMA_WIDTH;
}

static void fill_scaled_slice_nv12(const uint8_t *src,
                                   uint32_t src_width,
                                   uint32_t src_height,
                                   unsigned slice_index,
                                   uint8_t *slice_mem,
                                   sh264e_slice_t *slice)
{
    const uint8_t *src_y = src;
    const uint8_t *src_uv = src_y + (size_t)src_width * src_height;
    uint8_t *dst_y = slice_mem;
    uint8_t *dst_uv = dst_y + (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT;

    scale_plane_slice(src_y, src_width, src_height, (ptrdiff_t)src_width,
                      dst_y, SH264E_V1_WIDTH, SH264E_V1_HEIGHT,
                      slice_index * SH264E_V1_SLICE_LUMA_HEIGHT,
                      SH264E_V1_SLICE_LUMA_HEIGHT, SH264E_V1_WIDTH);
    scale_nv12_chroma_slice(src_uv, src_width / 2u, src_height / 2u, (ptrdiff_t)src_width,
                            dst_uv, slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT);

    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = SH264E_PIXFMT_NV12;
    slice->plane[0] = dst_y;
    slice->plane[1] = dst_uv;
    slice->stride[0] = SH264E_V1_WIDTH;
    slice->stride[1] = SH264E_V1_WIDTH;
}

static int encode_scaled_progressive(FILE *output,
                                     sh264e_encoder_t *encoder,
                                     sh264e_pixfmt_t pixfmt,
                                     const uint8_t *src,
                                     uint32_t src_width,
                                     uint32_t src_height,
                                     uint8_t *scaled_slice,
                                     uint8_t *header_buf,
                                     size_t header_capacity,
                                     uint8_t *slice_buf,
                                     size_t slice_capacity)
{
    sh264e_status_t status;
    size_t bytes = 0;
    unsigned slice_index;

    status = sh264e_begin_idr(encoder, header_buf, header_capacity, &bytes);
    if (status != SH264E_OK) {
        fprintf(stderr, "sh264e_begin_idr failed: %s\n", sh264e_status_string(status));
        return 0;
    }
    if (!write_exact(output, header_buf, bytes)) {
        fprintf(stderr, "failed to write SPS/PPS\n");
        return 0;
    }

    for (slice_index = 0; slice_index < SH264E_V1_SLICE_COUNT; slice_index++) {
        sh264e_slice_t slice;

        if (pixfmt == SH264E_PIXFMT_I420) {
            fill_scaled_slice_i420(src, src_width, src_height, slice_index, scaled_slice, &slice);
        } else {
            fill_scaled_slice_nv12(src, src_width, src_height, slice_index, scaled_slice, &slice);
        }

        status = sh264e_encode_idr_slice(encoder, &slice, slice_buf, slice_capacity, &bytes);
        if (status != SH264E_OK) {
            fprintf(stderr, "sh264e_encode_idr_slice(%u) failed: %s\n",
                    slice_index, sh264e_status_string(status));
            return 0;
        }
        if (!write_exact(output, slice_buf, bytes)) {
            fprintf(stderr, "failed to write IDR slice %u\n", slice_index);
            return 0;
        }
    }

    status = sh264e_end_idr(encoder);
    if (status != SH264E_OK) {
        fprintf(stderr, "sh264e_end_idr failed: %s\n", sh264e_status_string(status));
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *input_path;
    const char *output_path;
    sh264e_pixfmt_t pixfmt;
    uint32_t src_width = 0;
    uint32_t src_height = 0;
    sh264e_config_t config;
    sh264e_encoder_t *encoder = NULL;
    sh264e_status_t status;
    FILE *input = NULL;
    FILE *output = NULL;
    uint8_t *input_buf = NULL;
    uint8_t *scaled_slice = NULL;
    uint8_t *header_buf = NULL;
    uint8_t *slice_buf = NULL;
    size_t input_size;
    size_t scaled_slice_size;
    size_t header_capacity = 0;
    size_t slice_capacity = 0;
    int rc = 1;

    if (argc != 9 ||
        strcmp(argv[1], "--format") != 0 ||
        !parse_format(argv[2], &pixfmt) ||
        strcmp(argv[3], "--src-width") != 0 ||
        !parse_u32(argv[4], &src_width) ||
        strcmp(argv[5], "--src-height") != 0 ||
        !parse_u32(argv[6], &src_height)) {
        usage(argv[0]);
        return 2;
    }

    if (!validate_source_size(src_width, src_height)) {
        fprintf(stderr, "source size must be even and within 1280x720..5120x2880\n");
        return 2;
    }

    input_path = argv[7];
    output_path = argv[8];
    input_size = (size_t)src_width * src_height * 3u / 2u;
    scaled_slice_size = (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_LUMA_HEIGHT +
                        (size_t)SH264E_V1_WIDTH * SH264E_V1_SLICE_CHROMA_HEIGHT;

    input_buf = (uint8_t *)malloc(input_size);
    scaled_slice = (uint8_t *)malloc(scaled_slice_size);
    if (input_buf == NULL || scaled_slice == NULL) {
        fprintf(stderr, "failed to allocate input/scaled slice buffers\n");
        goto done;
    }

    input = fopen(input_path, "rb");
    if (input == NULL) {
        fprintf(stderr, "failed to open input: %s\n", input_path);
        goto done;
    }
    if (!read_exact(input, input_buf, input_size)) {
        fprintf(stderr, "failed to read one %ux%u YUV420 frame\n", src_width, src_height);
        goto done;
    }

    config.width = SH264E_V1_WIDTH;
    config.height = SH264E_V1_HEIGHT;
    config.pixfmt = pixfmt;
    config.qp = SH264E_DEFAULT_QP;

    status = sh264e_get_max_header_output_size(&config, &header_capacity);
    if (status != SH264E_OK) {
        fprintf(stderr, "sh264e_get_max_header_output_size failed: %s\n", sh264e_status_string(status));
        goto done;
    }
    status = sh264e_get_max_slice_output_size(&config, &slice_capacity);
    if (status != SH264E_OK) {
        fprintf(stderr, "sh264e_get_max_slice_output_size failed: %s\n", sh264e_status_string(status));
        goto done;
    }

    header_buf = (uint8_t *)malloc(header_capacity);
    slice_buf = (uint8_t *)malloc(slice_capacity);
    if (header_buf == NULL || slice_buf == NULL) {
        fprintf(stderr, "failed to allocate encoder output buffers\n");
        goto done;
    }

    status = sh264e_encoder_create(&config, &encoder);
    if (status != SH264E_OK) {
        fprintf(stderr, "sh264e_encoder_create failed: %s\n", sh264e_status_string(status));
        goto done;
    }

    output = fopen(output_path, "wb");
    if (output == NULL) {
        fprintf(stderr, "failed to open output: %s\n", output_path);
        goto done;
    }

    if (!encode_scaled_progressive(output, encoder, pixfmt, input_buf, src_width, src_height,
                                   scaled_slice, header_buf, header_capacity,
                                   slice_buf, slice_capacity)) {
        goto done;
    }

    printf("resized %ux%u to %ux%u and encoded progressive IDR frame\n",
           src_width, src_height, SH264E_V1_WIDTH, SH264E_V1_HEIGHT);
    rc = 0;

done:
    if (output != NULL) {
        fclose(output);
    }
    if (input != NULL) {
        fclose(input);
    }
    sh264e_encoder_destroy(encoder);
    free(slice_buf);
    free(header_buf);
    free(scaled_slice);
    free(input_buf);
    return rc;
}
