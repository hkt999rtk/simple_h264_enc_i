#include "sh264e.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s --format i420|nv12 input.jpg output.h264\n", argv0);
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

static int read_file(const char *path, uint8_t **out_data, size_t *out_size)
{
    FILE *fp;
    long end;
    uint8_t *data;
    size_t size;

    *out_data = NULL;
    *out_size = 0;
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    end = ftell(fp);
    if (end <= 0) {
        fclose(fp);
        return 0;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }
    size = (size_t)end;
    data = (uint8_t *)malloc(size);
    if (data == NULL) {
        fclose(fp);
        return 0;
    }
    if (fread(data, 1u, size, fp) != size) {
        free(data);
        fclose(fp);
        return 0;
    }
    fclose(fp);
    *out_data = data;
    *out_size = size;
    return 1;
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
    sh264e_encoder_t *encoder = NULL;
    sh264e_status_t status;
    uint8_t *jpeg_data = NULL;
    uint8_t *work = NULL;
    uint8_t *output_buf = NULL;
    FILE *output = NULL;
    size_t jpeg_size = 0;
    size_t work_size = 0;
    size_t output_capacity = 0;
    size_t output_size = 0;
    int rc = 1;

    if (argc != 5 || strcmp(argv[1], "--format") != 0 || !parse_format(argv[2], &pixfmt)) {
        usage(argv[0]);
        return 2;
    }
    input_path = argv[3];
    output_path = argv[4];

    if (!read_file(input_path, &jpeg_data, &jpeg_size)) {
        fprintf(stderr, "failed to read JPEG input: %s\n", input_path);
        goto done;
    }

    memset(&config, 0, sizeof(config));
    config.width = SH264E_V1_WIDTH;
    config.height = SH264E_V1_HEIGHT;
    config.pixfmt = pixfmt;
    config.qp = SH264E_DEFAULT_QP;

    status = sh264e_jpeg_get_slice_buffer_size(&work_size);
    if (status != SH264E_OK) {
        fprintf(stderr, "sh264e_jpeg_get_slice_buffer_size failed: %s\n", sh264e_status_string(status));
        goto done;
    }
    status = sh264e_get_max_output_size(&config, &output_capacity);
    if (status != SH264E_OK) {
        fprintf(stderr, "sh264e_get_max_output_size failed: %s\n", sh264e_status_string(status));
        goto done;
    }

    work = (uint8_t *)malloc(work_size);
    output_buf = (uint8_t *)malloc(output_capacity);
    if (work == NULL || output_buf == NULL) {
        fprintf(stderr, "failed to allocate work/output buffers\n");
        goto done;
    }

    status = sh264e_encoder_create(&config, &encoder);
    if (status != SH264E_OK) {
        fprintf(stderr, "sh264e_encoder_create failed: %s\n", sh264e_status_string(status));
        goto done;
    }

    status = sh264e_encode_jpeg_idr(encoder, jpeg_data, jpeg_size,
                                    work, work_size,
                                    output_buf, output_capacity, &output_size);
    if (status != SH264E_OK) {
        fprintf(stderr, "sh264e_encode_jpeg_idr failed: %s\n", sh264e_status_string(status));
        goto done;
    }

    output = fopen(output_path, "wb");
    if (output == NULL) {
        fprintf(stderr, "failed to open output: %s\n", output_path);
        goto done;
    }
    if (!write_exact(output, output_buf, output_size)) {
        fprintf(stderr, "failed to write output\n");
        goto done;
    }

    printf("encoded JPEG input to progressive IDR frame\n");
    rc = 0;

done:
    if (output != NULL) {
        fclose(output);
    }
    sh264e_encoder_destroy(encoder);
    free(output_buf);
    free(work);
    free(jpeg_data);
    return rc;
}
