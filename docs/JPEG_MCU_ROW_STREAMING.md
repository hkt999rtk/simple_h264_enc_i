# JPEG MCU-Row Streaming Design

This document records the JPEG MCU-row streaming design and the current
production boundary. It supersedes the earlier prototype notes that compared
the streaming path against a full component-plane decode path.

## Current Status

MCU-row streaming is now the required decode model for every public JPEG API:

* `sh264e_encode_jpeg_idr`
* `sh264e_encode_jpeg_idr_with_arena`
* `sh264e_encode_jpeg_idr_with_arena_stream`

No public JPEG API may allocate, depend on, or route through a full decoded RGB,
grayscale, Y, Cb, or Cr image. The one-shot APIs may still require a
caller-provided complete H.264 output buffer, but that output model is separate
from JPEG decoded-image memory.

The legacy NanoJPEG full-image decode entry points are gated by
`NJ_ENABLE_FULL_IMAGE_DECODE`, which defaults to `0`. They are reserved for
explicit debug/example builds and must not be reachable from public JPEG APIs or
production tools.

## Production Pipeline

The production JPEG-to-H.264 pipeline is:

```text
compressed JPEG byte buffer
-> parse JPEG headers
-> decode one MCU row into NanoJPEG temporary component rows
-> copy the MCU row into a rolling component row cache
-> scale the available source row window into one 2560x16 encoder slice,
   or use the 2560x1440 4:2:0 1:1 direct path
-> encode one H.264 IDR slice
-> repeat until all 90 H.264 slices are emitted
```

The H.264 output delivery depends on the public entry point:

| API | JPEG allocation model | H.264 output model |
| --- | --- | --- |
| `sh264e_encode_jpeg_idr` | heap-backed streaming row cache | caller-provided complete output buffer |
| `sh264e_encode_jpeg_idr_with_arena` | caller-provided streaming row-cache arena | caller-provided complete output buffer |
| `sh264e_encode_jpeg_idr_with_arena_stream` | caller-provided streaming row-cache arena | reusable output chunk + consumer callback |

All three paths share the same MCU-row streaming implementation.

## Decode Contract

The internal `njDecodeMcuRows` contract is intentionally narrower than a general
JPEG decoder:

* Supported inputs are baseline sequential JPEGs with either one grayscale
  component or three interleaved YCbCr components.
* Component sampling factors must be nonzero powers of two.
* Source dimensions must stay within the public resize policy:
  `1280x720 <= source <= 5120x2880`, with even width and height.
* The decoder emits callbacks in raster MCU-row order after a complete MCU row
  has been decoded into temporary component row buffers.
* Restart markers preserve NanoJPEG behavior: bitstream state is byte-aligned,
  marker order is checked, and component DC predictors are reset at the
  configured interval.
* Unsupported SOF markers, arithmetic coding, progressive/lossless JPEG, CMYK
  or other component layouts, non-interleaved multi-scan layouts, and malformed
  marker lengths return deterministic error statuses before production callers
  write partial output.
* A nonzero callback return stops decoding with `NJ_CALLBACK_ABORT`; integration
  code maps callback-side errors back to the appropriate `sh264e_status_t`.

## Row Cache Lifetime

The row cache keeps only source rows needed by the scaler for the next H.264
output slice. Bilinear scaling needs at most two source rows for each output row
per component. Because JPEG emits whole MCU rows, retained rows are rounded up
to each component's MCU-row granularity with a margin for slice-window
boundaries.

For one output luma slice of 16 rows:

```text
needed_luma_source_rows =
    ceil(16 * source_height / 1440) + 1

cached_luma_rows =
    round_up_to_mcu_rows(needed_luma_source_rows + boundary_margin)
```

Chroma uses the same rule against the destination chroma slice height of 8 rows
and the decoded component's native height.

Decoded MCU rows are copied into the rolling cache. As soon as the source row
window for the next output slice is available, the integration layer scales one
YUV420 slice into the caller-provided 61,440-byte slice work buffer and calls
`sh264e_encode_idr_slice`.

For `2560x1440` 4:2:0 source JPEGs encoded to the fixed `2560x1440` target,
the pipeline uses a 1:1 fast path. It keeps exactly one MCU row per component,
feeds I420 slices directly from the retained cache, and only stages the 8-row
interleaved chroma window required for NV12 output. Non-1:1 sources keep the
general bilinear scaler cache and slice-work behavior.

## Memory Contract

Historical full component-plane decoded-image sizes were:

| Source | Full component-plane bytes |
| --- | ---: |
| 1280x720 4:2:0 | 1,382,400 |
| 1280x720 4:2:2 | 1,843,200 |
| 1280x720 4:4:4 | 2,764,800 |
| 2560x1440 4:2:0 | 5,529,600 |

The current row-streaming cache is bounded by retained MCU rows instead of full
image height:

| Source | Streaming row cache | Slice work buffer |
| --- | ---: | ---: |
| 1280x720 4:2:0 | 61,440 | 61,440 |
| 1280x720 4:2:2 | 81,920 | 61,440 |
| 1280x720 4:4:4 | 122,880 | 61,440 |
| 2560x1440 4:2:0, 1:1 fast path | 61,440 | 61,440 API capacity; effective I420 0, NV12 20,480 |

For the `2560x1440` 4:2:0 embedded path, excluding compressed JPEG input and
`.rodata`, the current planning budget is about 390 KiB:

| Block | Bytes |
| --- | ---: |
| JPEG work arena | 123,024 |
| JPEG/scaler slice work | 61,440 |
| Reusable H.264 output chunk buffer | 4,096 |
| Encoder arena | 191,055 |
| Static mutable RAM | about 4,529 |

`docs/ENCODER_MEMORY_REPORT.md` breaks the encoder arena row into context,
bitstream scratch, reconstructed-slice storage, neighbor/nonzero state, and
arena alignment padding.

The hidden one-shot comparison path still reports the complete H.264 output
capacity, `11,428,864` bytes, when testing one-shot APIs. That is caller-owned
H.264 output capacity, not JPEG decoded-image working memory.

## Diagnostics Boundary

The normal public diagnostic surface includes:

* `sh264e_jpeg_get_last_allocation_stats`
* `sh264e_jpeg_get_last_streaming_cache_bytes`
* `sh264e_jpeg_get_last_slice_work_bytes`
* `sh264e_encoder_get_memory_report`

Private regression hooks such as allocation-limit fault injection and the
streaming prototype entry points are declared in
`tests/sh264e_jpeg_test_hooks.h` and compiled only when
`SH264E_ENABLE_JPEG_TEST_HOOKS=1`. Production builds should leave those hooks
disabled.

## Validation

The expected validation command is:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The test matrix validates:

* public JPEG APIs use `njDecodeMcuRows`, not `njDecodeComponents`
* NanoJPEG full-image decode defaults to disabled
* diagnostic hooks are test-gated
* JPEG outputs decode with strict ffmpeg validation
* one-shot output and streaming-consumer output are byte-for-byte identical for
  representative fixtures
* `2560x1440` 4:2:0 public JPEG wrapper peak allocation stays at the streaming
  row-cache budget and does not regress to the old 5,529,600-byte full
  component-plane allocation
