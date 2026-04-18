#include "sh264e.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sh264e_jpeg_set_test_allocation_limit(size_t max_bytes);
size_t sh264e_jpeg_get_last_streaming_cache_bytes(void);
sh264e_status_t sh264e_jpeg_get_streaming_work_size(const uint8_t *jpeg_data,
                                                    size_t jpeg_size,
                                                    size_t *out_size);
sh264e_status_t sh264e_encode_jpeg_idr_streaming_prototype(sh264e_encoder_t *encoder,
                                                           const uint8_t *jpeg_data,
                                                           size_t jpeg_size,
                                                           uint8_t *work_buffer,
                                                           size_t work_buffer_capacity,
                                                           uint8_t *out,
                                                           size_t out_capacity,
                                                           size_t *out_size);
sh264e_status_t sh264e_encode_jpeg_idr_streaming_prototype_with_arena(sh264e_encoder_t *encoder,
                                                                      const uint8_t *jpeg_data,
                                                                      size_t jpeg_size,
                                                                      uint8_t *jpeg_arena,
                                                                      size_t jpeg_arena_size,
                                                                      uint8_t *work_buffer,
                                                                      size_t work_buffer_capacity,
                                                                      uint8_t *out,
                                                                      size_t out_capacity,
                                                                      size_t *out_size);

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--streaming-prototype] [--test-allocation-limit BYTES] "
            "[--test-arena-shrink BYTES] [--test-arena-offset BYTES] "
            "[--test-one-shot-output] [--test-output-consumer] "
            "[--test-output-consumer-fail-after CHUNKS] "
            "--format i420|nv12 input.jpg output.h264\n",
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

static int parse_size(const char *text, size_t *out_size)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || *text == '\0' || out_size == NULL) {
        return 0;
    }
    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0') {
        return 0;
    }
    *out_size = (size_t)value;
    return 1;
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

typedef struct output_consumer_state_t {
    FILE *fp;
    uint8_t *data;
    size_t capacity;
    size_t size;
    unsigned chunks;
    int fail_after_chunks;
} output_consumer_state_t;

static sh264e_status_t collect_output_chunk(void *user, const uint8_t *data, size_t size)
{
    output_consumer_state_t *state = (output_consumer_state_t *)user;

    if (state == NULL || data == NULL || size == 0u) {
        return SH264E_ERR_INTERNAL;
    }
    if (state->fail_after_chunks >= 0 &&
        state->chunks >= (unsigned)state->fail_after_chunks) {
        return SH264E_ERR_INTERNAL;
    }
    if (state->fp != NULL) {
        if (!write_exact(state->fp, data, size)) {
            return SH264E_ERR_INTERNAL;
        }
    } else {
        if (size > state->capacity || state->size > state->capacity - size) {
            return SH264E_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(state->data + state->size, data, size);
    }
    state->size += size;
    state->chunks++;
    return SH264E_OK;
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
    uint8_t *jpeg_arena_alloc = NULL;
    uint8_t *jpeg_arena = NULL;
    uint8_t *work = NULL;
    uint8_t *output_buf = NULL;
    uint8_t *output_chunk_buf = NULL;
    FILE *output = NULL;
    size_t jpeg_size = 0;
    size_t jpeg_work_size = 0;
    size_t jpeg_arena_size = 0;
    size_t work_size = 0;
    size_t output_capacity = 0;
    size_t output_chunk_capacity = 0;
    size_t output_size = 0;
    size_t allocation_limit = (size_t)-1;
    size_t arena_shrink = 0;
    size_t arena_offset = 0;
    int streaming_prototype = 0;
    int one_shot_output_test = 0;
    int output_consumer_test = 0;
    int output_consumer_fail_after = -1;
    int argi = 1;
    int rc = 1;

    while (argi < argc) {
        if (strcmp(argv[argi], "--streaming-prototype") == 0) {
            streaming_prototype = 1;
            argi++;
        } else if (strcmp(argv[argi], "--test-one-shot-output") == 0) {
            one_shot_output_test = 1;
            argi++;
        } else if (strcmp(argv[argi], "--test-output-consumer") == 0) {
            output_consumer_test = 1;
            argi++;
        } else if (argi + 1 < argc && strcmp(argv[argi], "--test-output-consumer-fail-after") == 0) {
            size_t fail_after = 0u;
            if (!parse_size(argv[argi + 1], &fail_after) || fail_after > (size_t)INT_MAX) {
                usage(argv[0]);
                return 2;
            }
            output_consumer_test = 1;
            output_consumer_fail_after = (int)fail_after;
            argi += 2;
        } else if (argi + 1 < argc && strcmp(argv[argi], "--test-allocation-limit") == 0) {
            if (!parse_size(argv[argi + 1], &allocation_limit)) {
                usage(argv[0]);
                return 2;
            }
            argi += 2;
        } else if (argi + 1 < argc && strcmp(argv[argi], "--test-arena-shrink") == 0) {
            if (!parse_size(argv[argi + 1], &arena_shrink)) {
                usage(argv[0]);
                return 2;
            }
            argi += 2;
        } else if (argi + 1 < argc && strcmp(argv[argi], "--test-arena-offset") == 0) {
            if (!parse_size(argv[argi + 1], &arena_offset)) {
                usage(argv[0]);
                return 2;
            }
            argi += 2;
        } else {
            break;
        }
    }

    if (argc - argi != 4 || strcmp(argv[argi], "--format") != 0 ||
        !parse_format(argv[argi + 1], &pixfmt)) {
        usage(argv[0]);
        return 2;
    }
    input_path = argv[argi + 2];
    output_path = argv[argi + 3];

    if (!read_file(input_path, &jpeg_data, &jpeg_size)) {
        fprintf(stderr, "failed to read JPEG input: %s\n", input_path);
        goto done;
    }
    if (streaming_prototype && allocation_limit != (size_t)-1) {
        fprintf(stderr, "--streaming-prototype cannot be combined with --test-allocation-limit\n");
        goto done;
    }
    if (output_consumer_test && (streaming_prototype || allocation_limit != (size_t)-1)) {
        fprintf(stderr, "--test-output-consumer cannot be combined with streaming prototype or allocation-limit modes\n");
        goto done;
    }
    if (one_shot_output_test && (streaming_prototype || output_consumer_test ||
                                 allocation_limit != (size_t)-1)) {
        fprintf(stderr, "--test-one-shot-output cannot be combined with other output test modes\n");
        goto done;
    }
    if (allocation_limit == (size_t)-1) {
        if (streaming_prototype) {
            status = sh264e_jpeg_get_streaming_work_size(jpeg_data, jpeg_size, &jpeg_work_size);
        } else {
            status = sh264e_jpeg_get_work_size(jpeg_data, jpeg_size, &jpeg_work_size);
        }
        if (status != SH264E_OK) {
            fprintf(stderr, "JPEG work-size query failed: %s\n", sh264e_status_string(status));
            goto done;
        }
        jpeg_arena_size = jpeg_work_size;
        if (arena_shrink > jpeg_arena_size) {
            jpeg_arena_size = 0u;
        } else {
            jpeg_arena_size -= arena_shrink;
        }
    } else if (arena_shrink != 0u || arena_offset != 0u) {
        fprintf(stderr, "arena test options cannot be combined with --test-allocation-limit\n");
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
    if (allocation_limit != (size_t)-1 || streaming_prototype ||
        output_consumer_test || one_shot_output_test) {
        status = sh264e_get_max_output_size(&config, &output_capacity);
        if (status != SH264E_OK) {
            fprintf(stderr, "sh264e_get_max_output_size failed: %s\n", sh264e_status_string(status));
            goto done;
        }
    }
    if (allocation_limit == (size_t)-1 && !streaming_prototype && !one_shot_output_test) {
        size_t header_capacity = 0u;
        size_t slice_capacity = 0u;

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
        output_chunk_capacity = header_capacity > slice_capacity ? header_capacity : slice_capacity;
    }

    if (allocation_limit == (size_t)-1) {
        size_t jpeg_arena_alloc_size = jpeg_arena_size;

        if (arena_offset > ((size_t)-1) - jpeg_arena_alloc_size) {
            fprintf(stderr, "JPEG arena allocation size overflow\n");
            goto done;
        }
        jpeg_arena_alloc_size += arena_offset;
        jpeg_arena_alloc = (uint8_t *)malloc(jpeg_arena_alloc_size);
        if (jpeg_arena_alloc == NULL) {
            fprintf(stderr, "failed to allocate JPEG arena\n");
            goto done;
        }
        jpeg_arena = jpeg_arena_alloc + arena_offset;
    }
    work = (uint8_t *)malloc(work_size);
    if (output_capacity != 0u) {
        output_buf = (uint8_t *)malloc(output_capacity);
    }
    if (output_chunk_capacity != 0u) {
        output_chunk_buf = (uint8_t *)malloc(output_chunk_capacity);
    }
    if (work == NULL ||
        (output_capacity != 0u && output_buf == NULL) ||
        (output_chunk_capacity != 0u && output_chunk_buf == NULL)) {
        fprintf(stderr, "failed to allocate work/output buffers\n");
        goto done;
    }

    status = sh264e_encoder_create(&config, &encoder);
    if (status != SH264E_OK) {
        fprintf(stderr, "sh264e_encoder_create failed: %s\n", sh264e_status_string(status));
        goto done;
    }

    if (allocation_limit != (size_t)-1) {
        sh264e_jpeg_set_test_allocation_limit(allocation_limit);
        status = sh264e_encode_jpeg_idr(encoder, jpeg_data, jpeg_size,
                                        work, work_size,
                                        output_buf, output_capacity, &output_size);
    } else if (one_shot_output_test) {
        status = sh264e_encode_jpeg_idr_with_arena(encoder, jpeg_data, jpeg_size,
                                                   jpeg_arena, jpeg_arena_size,
                                                   work, work_size,
                                                   output_buf, output_capacity, &output_size);
    } else if (output_consumer_test) {
        output_consumer_state_t consumer_state;

        memset(&consumer_state, 0, sizeof(consumer_state));
        consumer_state.data = output_buf;
        consumer_state.capacity = output_capacity;
        consumer_state.fail_after_chunks = output_consumer_fail_after;
        status = sh264e_encode_jpeg_idr_with_arena_stream(
            encoder, jpeg_data, jpeg_size,
            jpeg_arena, jpeg_arena_size,
            work, work_size,
            output_chunk_buf, output_chunk_capacity,
            collect_output_chunk, &consumer_state);
        output_size = consumer_state.size;
        if (status != SH264E_OK && output_consumer_fail_after >= 0) {
            size_t probe_size = 0u;
            sh264e_status_t probe_status =
                sh264e_begin_idr(encoder, output_chunk_buf, output_chunk_capacity, &probe_size);
            if (probe_status != SH264E_OK) {
                fprintf(stderr, "encoder did not reset after consumer failure: %s\n",
                        sh264e_status_string(probe_status));
                status = probe_status;
            }
        }
        if (status == SH264E_OK && consumer_state.chunks != 1u + SH264E_V1_SLICE_COUNT) {
            fprintf(stderr, "output consumer saw %u chunks, expected %u\n",
                    consumer_state.chunks, 1u + SH264E_V1_SLICE_COUNT);
            status = SH264E_ERR_INTERNAL;
        }
        if (status == SH264E_OK) {
            printf("jpeg output consumer chunks: %u\n", consumer_state.chunks);
            printf("jpeg output chunk buffer bytes: %zu\n", output_chunk_capacity);
        }
    } else if (streaming_prototype) {
        status = sh264e_encode_jpeg_idr_streaming_prototype_with_arena(
            encoder, jpeg_data, jpeg_size,
            jpeg_arena, jpeg_arena_size,
            work, work_size,
            output_buf, output_capacity, &output_size);
    } else {
        output_consumer_state_t consumer_state;

        output = fopen(output_path, "wb");
        if (output == NULL) {
            fprintf(stderr, "failed to open output: %s\n", output_path);
            goto done;
        }
        memset(&consumer_state, 0, sizeof(consumer_state));
        consumer_state.fp = output;
        consumer_state.fail_after_chunks = output_consumer_fail_after;
        status = sh264e_encode_jpeg_idr_with_arena_stream(
            encoder, jpeg_data, jpeg_size,
            jpeg_arena, jpeg_arena_size,
            work, work_size,
            output_chunk_buf, output_chunk_capacity,
            collect_output_chunk, &consumer_state);
        output_size = consumer_state.size;
        if (status == SH264E_OK && consumer_state.chunks != 1u + SH264E_V1_SLICE_COUNT) {
            fprintf(stderr, "output consumer saw %u chunks, expected %u\n",
                    consumer_state.chunks, 1u + SH264E_V1_SLICE_COUNT);
            status = SH264E_ERR_INTERNAL;
        }
        if (status == SH264E_OK) {
            printf("jpeg output consumer chunks: %u\n", consumer_state.chunks);
            printf("jpeg output chunk buffer bytes: %zu\n", output_chunk_capacity);
        }
    }
    if (status != SH264E_OK) {
        fprintf(stderr, "JPEG encode failed: %s\n", sh264e_status_string(status));
        goto done;
    }

    if (output == NULL) {
        output = fopen(output_path, "wb");
        if (output == NULL) {
            fprintf(stderr, "failed to open output: %s\n", output_path);
            goto done;
        }
        if (!write_exact(output, output_buf, output_size)) {
            fprintf(stderr, "failed to write output\n");
            goto done;
        }
    }

    {
        sh264e_jpeg_allocation_stats_t stats;
        status = sh264e_jpeg_get_last_allocation_stats(&stats);
        if (status != SH264E_OK) {
            fprintf(stderr, "sh264e_jpeg_get_last_allocation_stats failed: %s\n",
                    sh264e_status_string(status));
            goto done;
        }
        printf("jpeg work arena bytes: %zu\n", jpeg_work_size);
        printf("jpeg slice work bytes: %zu\n", work_size);
        printf("jpeg current allocation bytes: %zu\n", stats.current_bytes);
        printf("jpeg peak allocation bytes: %zu\n", stats.peak_bytes);
        if (streaming_prototype || sh264e_jpeg_get_last_streaming_cache_bytes() != 0u) {
            printf("jpeg streaming cache bytes: %zu\n", sh264e_jpeg_get_last_streaming_cache_bytes());
        }
    }
    printf("encoded JPEG input to progressive IDR frame\n");
    rc = 0;

done:
    if (output != NULL) {
        fclose(output);
    }
    sh264e_encoder_destroy(encoder);
    free(output_chunk_buf);
    free(output_buf);
    free(work);
    free(jpeg_arena_alloc);
    free(jpeg_data);
    return rc;
}
