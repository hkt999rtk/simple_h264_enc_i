# FreeRTOS Cortex-M7 Public API

The public C API is declared in `include/sh264e.h`. This document summarizes
the application-facing surface included in the FreeRTOS Cortex-M7 package.

## Fixed Encoder Geometry

The current encoder supports a fixed v1 IDR frame geometry:

```text
SH264E_V1_WIDTH = 2560
SH264E_V1_HEIGHT = 1440
SH264E_V1_SLICE_COUNT = 90
SH264E_V1_SLICE_LUMA_HEIGHT = 16
SH264E_V1_SLICE_CHROMA_HEIGHT = 8
```

Input frames and slices use either `SH264E_PIXFMT_I420` or
`SH264E_PIXFMT_NV12`.

## Encoder Lifecycle

Use `sh264e_encoder_get_work_size` and
`sh264e_encoder_create_with_arena` for embedded integrations that provide a
caller-owned arena. Use `sh264e_encoder_destroy` to end the encoder lifetime;
it does not free caller-owned arena memory.

Heap-backed `sh264e_encoder_create` remains part of the public API, but
FreeRTOS integrations should prefer explicit arenas unless their runtime
allocator policy is already defined.

## Raw Progressive Encoding

For direct raw input, call:

```text
sh264e_begin_idr
sh264e_encode_idr_slice
sh264e_end_idr
```

The caller provides each 16-luma-row / 8-chroma-row output slice. The encoder
emits Annex B SPS, PPS, and independent macroblock-slice IDR NAL units.

`sh264e_encode_idr` remains available for callers that already have a complete
fixed-geometry input frame and a complete output buffer.

## Resize Helpers

Use `sh264e_resize_get_slice_buffer_size` to size the caller-owned resize work
buffer and `sh264e_resize_make_slice` to produce one fixed-geometry encoder
slice from an even-dimension I420 or NV12 source frame.

The supported source range is:

```text
1280 <= src_width  <= 5120
720  <= src_height <= 2880
```

The exact `1280x720 -> 2560x1440`, `5120x2880 -> 2560x1440`, and
`2560x1440 -> 2560x1440` paths use optimized fixed-ratio behavior.

## JPEG Input

Use `sh264e_jpeg_get_work_size` or `sh264e_jpeg_source_get_work_size` to size
the JPEG decoder arena. Use `sh264e_jpeg_get_slice_work_size` or
`sh264e_jpeg_source_get_slice_work_size` to size the path-specific slice work
buffer.

The production streaming entry points are:

```text
sh264e_encode_jpeg_idr_with_arena_stream
sh264e_encode_jpeg_source_idr_with_arena_stream
```

The memory-input variant accepts a contiguous compressed JPEG buffer. The
source-input variant accepts a pull-only sequential reader callback. Work-size
queries consume source readers, so reset or recreate the reader before the
slice-work query and the encode call.

## Output Consumer

The streaming JPEG APIs emit Annex B bytes through:

```text
typedef sh264e_status_t (*sh264e_output_consumer_t)(
    void *user,
    const uint8_t *data,
    size_t size);
```

The consumer is called synchronously. Return `SH264E_OK` after accepting the
chunk or another `sh264e_status_t` value to abort encoding.

## Diagnostics

The package keeps the public memory/stat APIs:

```text
sh264e_encoder_get_memory_report
sh264e_jpeg_get_last_allocation_stats
sh264e_jpeg_get_last_streaming_cache_bytes
sh264e_jpeg_get_last_slice_work_bytes
```

Use `sh264e_status_string` for diagnostic text for `sh264e_status_t` values.
