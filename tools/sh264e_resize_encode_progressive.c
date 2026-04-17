#include "sh264e.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int read_exact(FILE *fp, uint8_t *buf, size_t size)
{
    return fread(buf, 1u, size, fp) == size;
}

static int write_exact(FILE *fp, const uint8_t *buf, size_t size)
{
    return fwrite(buf, 1u, size, fp) == size;
}

static size_t yuv420_frame_size(uint32_t width, uint32_t height)
{
    return (size_t)width * height * 3u / 2u;
}

static int valid_source_dimensions(uint32_t width, uint32_t height)
{
    if (width < SH264E_RESIZE_MIN_SRC_WIDTH || width > SH264E_RESIZE_MAX_SRC_WIDTH ||
        height < SH264E_RESIZE_MIN_SRC_HEIGHT || height > SH264E_RESIZE_MAX_SRC_HEIGHT) {
        return 0;
    }
    if ((width & 1u) != 0u || (height & 1u) != 0u) {
        return 0;
    }
    return 1;
}

static void make_source_frame(uint8_t *input_buf,
                              uint32_t src_width,
                              uint32_t src_height,
                              sh264e_pixfmt_t pixfmt,
                              sh264e_frame_t *frame)
{
    const size_t y_size = (size_t)src_width * src_height;
    const size_t c_size = (size_t)(src_width / 2u) * (src_height / 2u);

    memset(frame, 0, sizeof(*frame));
    frame->width = src_width;
    frame->height = src_height;
    frame->pixfmt = pixfmt;
    frame->plane[0] = input_buf;
    frame->stride[0] = src_width;
    if (pixfmt == SH264E_PIXFMT_I420) {
        frame->plane[1] = input_buf + y_size;
        frame->plane[2] = input_buf + y_size + c_size;
        frame->stride[1] = src_width / 2u;
        frame->stride[2] = src_width / 2u;
    } else {
        frame->plane[1] = input_buf + y_size;
        frame->stride[1] = src_width;
    }
}

static int encode_scaled_progressive(FILE *output,
                                     sh264e_encoder_t *encoder,
                                     const sh264e_frame_t *src_frame,
                                     uint8_t *scaled_slice,
                                     size_t scaled_slice_capacity,
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

        status = sh264e_resize_make_slice(src_frame, slice_index,
                                          scaled_slice, scaled_slice_capacity,
                                          &slice);
        if (status != SH264E_OK) {
            fprintf(stderr, "sh264e_resize_make_slice(%u) failed: %s\n",
                    slice_index, sh264e_status_string(status));
            return 0;
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
    sh264e_frame_t src_frame;
    sh264e_encoder_t *encoder = NULL;
    sh264e_status_t status;
    FILE *input = NULL;
    FILE *output = NULL;
    uint8_t *input_buf = NULL;
    uint8_t *scaled_slice = NULL;
    uint8_t *header_buf = NULL;
    uint8_t *slice_buf = NULL;
    size_t input_size;
    size_t scaled_slice_capacity = 0;
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

    if (!valid_source_dimensions(src_width, src_height)) {
        fprintf(stderr, "source size must be even and within 1280x720..5120x2880\n");
        return 2;
    }

    input_path = argv[7];
    output_path = argv[8];
    input_size = yuv420_frame_size(src_width, src_height);

    input_buf = (uint8_t *)malloc(input_size);
    if (input_buf == NULL) {
        fprintf(stderr, "failed to allocate input buffer\n");
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

    make_source_frame(input_buf, src_width, src_height, pixfmt, &src_frame);
    status = sh264e_resize_get_slice_buffer_size(&src_frame, &scaled_slice_capacity);
    if (status != SH264E_OK) {
        fprintf(stderr, "unsupported resize input: %s\n", sh264e_status_string(status));
        goto done;
    }
    if (scaled_slice_capacity != 0u) {
        scaled_slice = (uint8_t *)malloc(scaled_slice_capacity);
        if (scaled_slice == NULL) {
            fprintf(stderr, "failed to allocate scaled slice buffer\n");
            goto done;
        }
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

    if (!encode_scaled_progressive(output, encoder, &src_frame,
                                   scaled_slice, scaled_slice_capacity,
                                   header_buf, header_capacity,
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
