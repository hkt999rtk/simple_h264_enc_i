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

static int expect_no_more_nals(const uint8_t *data, size_t size, size_t offset)
{
    unsigned type = 0;
    return !next_nal_type(data, size, &offset, &type);
}

static int expect_wrapper_nal_sequence(const uint8_t *data, size_t size)
{
    size_t offset = 0;
    unsigned type = 0;
    unsigned i;

    if (!next_nal_type(data, size, &offset, &type) || type != 7u) {
        fprintf(stderr, "missing SPS NALU\n");
        return 0;
    }
    if (!next_nal_type(data, size, &offset, &type) || type != 8u) {
        fprintf(stderr, "missing PPS NALU\n");
        return 0;
    }
    for (i = 0; i < SH264E_V1_SLICE_COUNT; i++) {
        if (!next_nal_type(data, size, &offset, &type) || type != 5u) {
            fprintf(stderr, "missing IDR slice NALU %u\n", i);
            return 0;
        }
    }
    if (!expect_no_more_nals(data, size, offset)) {
        fprintf(stderr, "unexpected extra NALU after progressive IDR slices\n");
        return 0;
    }
    return 1;
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

static int expect_single_nal_type(const uint8_t *data, size_t size, unsigned expected_type)
{
    size_t offset = 0;
    unsigned type = 0;
    if (!next_nal_type(data, size, &offset, &type) || type != expected_type) {
        fprintf(stderr, "expected NALU type %u\n", expected_type);
        return 0;
    }
    if (!expect_no_more_nals(data, size, offset)) {
        fprintf(stderr, "unexpected extra NALU\n");
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
    if (encoder_memory_report.context_bytes != 72u ||
        encoder_memory_report.bitstream_scratch_bytes != slice_capacity ||
        encoder_memory_report.recon_luma_bytes != 40960u ||
        encoder_memory_report.recon_chroma_bytes != 20480u ||
        encoder_memory_report.neighbor_state_bytes != 2560u ||
        encoder_memory_report.total_bytes != 191048u) {
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
    slice_output = (uint8_t *)malloc(slice_capacity);
    if (input == NULL || output == NULL || slice_output == NULL) {
        fprintf(stderr, "allocation failed\n");
        sh264e_encoder_destroy(encoder);
        free(input);
        free(output);
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
            if (!expect_single_nal_type(slice_output, output_size, 5u)) {
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
    sh264e_encoder_destroy(encoder);
    free(input);
    free(output);
    free(slice_output);
    free(resize_work);
    free(small_input);
    free(encoder_arena_alloc);
    return ok ? 0 : 1;
}
