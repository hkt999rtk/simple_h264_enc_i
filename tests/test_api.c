#include "sh264e.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void)
{
    sh264e_config_t config;
    sh264e_config_t bad_config;
    sh264e_frame_t frame;
    sh264e_encoder_t *encoder = NULL;
    sh264e_status_t status;
    uint8_t *input = NULL;
    uint8_t *output = NULL;
    size_t input_size;
    size_t output_capacity = 0;
    size_t output_size = 0;
    size_t offset = 0;
    unsigned type = 0;
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
    if (output_capacity == 0u) {
        fprintf(stderr, "max output size returned zero\n");
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
    if (input == NULL || output == NULL) {
        fprintf(stderr, "allocation failed\n");
        sh264e_encoder_destroy(encoder);
        free(input);
        free(output);
        return 1;
    }
    fill_i420(input);

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

    ok &= next_nal_type(output, output_size, &offset, &type) && type == 7u;
    ok &= next_nal_type(output, output_size, &offset, &type) && type == 8u;
    ok &= next_nal_type(output, output_size, &offset, &type) && type == 5u;
    if (!ok) {
        fprintf(stderr, "unexpected Annex B NALU sequence\n");
    }

    sh264e_encoder_destroy(encoder);
    free(input);
    free(output);
    return ok ? 0 : 1;
}
