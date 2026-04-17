#include "sh264e.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s --format i420|nv12 input.yuv output.h264\n", argv0);
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

static int read_exact(FILE *fp, uint8_t *buf, size_t size)
{
    return fread(buf, 1u, size, fp) == size;
}

static int write_exact(FILE *fp, const uint8_t *buf, size_t size)
{
    return fwrite(buf, 1u, size, fp) == size;
}

static void make_slice(const uint8_t *input, sh264e_pixfmt_t pixfmt, unsigned slice_index, sh264e_slice_t *slice)
{
    const uint8_t *y = input;
    const uint8_t *uv_or_u = input + (size_t)SH264E_V1_WIDTH * SH264E_V1_HEIGHT;
    const size_t y_offset = (size_t)slice_index * SH264E_V1_SLICE_LUMA_HEIGHT * SH264E_V1_WIDTH;
    const size_t c_offset = (size_t)slice_index * SH264E_V1_SLICE_CHROMA_HEIGHT;

    memset(slice, 0, sizeof(*slice));
    slice->pixfmt = pixfmt;
    slice->plane[0] = y + y_offset;
    slice->stride[0] = SH264E_V1_WIDTH;

    if (pixfmt == SH264E_PIXFMT_I420) {
        const size_t c_stride = SH264E_V1_WIDTH / 2u;
        const size_t c_size = c_stride * (SH264E_V1_HEIGHT / 2u);
        const uint8_t *u = uv_or_u;
        const uint8_t *v = u + c_size;
        slice->plane[1] = u + c_offset * c_stride;
        slice->plane[2] = v + c_offset * c_stride;
        slice->stride[1] = (ptrdiff_t)c_stride;
        slice->stride[2] = (ptrdiff_t)c_stride;
    } else {
        slice->plane[1] = uv_or_u + c_offset * SH264E_V1_WIDTH;
        slice->stride[1] = SH264E_V1_WIDTH;
    }
}

static int write_progressive_idr(FILE *output,
                                 sh264e_encoder_t *encoder,
                                 sh264e_pixfmt_t pixfmt,
                                 const uint8_t *input,
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
        make_slice(input, pixfmt, slice_index, &slice);
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
    sh264e_config_t config;
    sh264e_encoder_t *encoder = NULL;
    sh264e_status_t status;
    FILE *input = NULL;
    FILE *output = NULL;
    uint8_t *input_buf = NULL;
    uint8_t *header_buf = NULL;
    uint8_t *slice_buf = NULL;
    size_t input_size;
    size_t header_capacity = 0;
    size_t slice_capacity = 0;
    int rc = 1;

    if (argc != 5 || strcmp(argv[1], "--format") != 0 || !parse_format(argv[2], &pixfmt)) {
        usage(argv[0]);
        return 2;
    }

    input_path = argv[3];
    output_path = argv[4];
    input_size = (size_t)SH264E_V1_WIDTH * SH264E_V1_HEIGHT * 3u / 2u;

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
        fprintf(stderr, "failed to read one %ux%u YUV420 frame\n", SH264E_V1_WIDTH, SH264E_V1_HEIGHT);
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
        fprintf(stderr, "failed to allocate output buffers\n");
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

    if (!write_progressive_idr(output, encoder, pixfmt, input_buf,
                               header_buf, header_capacity, slice_buf, slice_capacity)) {
        goto done;
    }

    printf("encoded progressive IDR frame\n");
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
    free(input_buf);
    return rc;
}
