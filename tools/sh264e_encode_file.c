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

int main(int argc, char **argv)
{
    const char *input_path;
    const char *output_path;
    sh264e_pixfmt_t pixfmt;
    sh264e_config_t config;
    sh264e_frame_t frame;
    sh264e_encoder_t *encoder = NULL;
    sh264e_status_t status;
    FILE *input = NULL;
    FILE *output = NULL;
    uint8_t *input_buf = NULL;
    uint8_t *output_buf = NULL;
    size_t input_size;
    size_t output_capacity = 0;
    size_t output_size = 0;
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

    status = sh264e_get_max_output_size(&config, &output_capacity);
    if (status != SH264E_OK) {
        fprintf(stderr, "sh264e_get_max_output_size failed: %s\n", sh264e_status_string(status));
        goto done;
    }

    output_buf = (uint8_t *)malloc(output_capacity);
    if (output_buf == NULL) {
        fprintf(stderr, "failed to allocate output buffer\n");
        goto done;
    }

    status = sh264e_encoder_create(&config, &encoder);
    if (status != SH264E_OK) {
        fprintf(stderr, "sh264e_encoder_create failed: %s\n", sh264e_status_string(status));
        goto done;
    }

    memset(&frame, 0, sizeof(frame));
    frame.width = SH264E_V1_WIDTH;
    frame.height = SH264E_V1_HEIGHT;
    frame.pixfmt = pixfmt;
    frame.plane[0] = input_buf;
    frame.stride[0] = SH264E_V1_WIDTH;
    if (pixfmt == SH264E_PIXFMT_I420) {
        frame.plane[1] = input_buf + (size_t)SH264E_V1_WIDTH * SH264E_V1_HEIGHT;
        frame.plane[2] = frame.plane[1] + (size_t)(SH264E_V1_WIDTH / 2u) * (SH264E_V1_HEIGHT / 2u);
        frame.stride[1] = SH264E_V1_WIDTH / 2u;
        frame.stride[2] = SH264E_V1_WIDTH / 2u;
    } else {
        frame.plane[1] = input_buf + (size_t)SH264E_V1_WIDTH * SH264E_V1_HEIGHT;
        frame.stride[1] = SH264E_V1_WIDTH;
    }

    status = sh264e_encode_idr(encoder, &frame, output_buf, output_capacity, &output_size);
    if (status != SH264E_OK) {
        fprintf(stderr, "sh264e_encode_idr failed: %s\n", sh264e_status_string(status));
        goto done;
    }

    output = fopen(output_path, "wb");
    if (output == NULL) {
        fprintf(stderr, "failed to open output: %s\n", output_path);
        goto done;
    }
    if (!write_exact(output, output_buf, output_size)) {
        fprintf(stderr, "failed to write output bitstream\n");
        goto done;
    }

    printf("encoded %zu bytes\n", output_size);
    rc = 0;

done:
    if (output != NULL) {
        fclose(output);
    }
    if (input != NULL) {
        fclose(input);
    }
    sh264e_encoder_destroy(encoder);
    free(output_buf);
    free(input_buf);
    return rc;
}
