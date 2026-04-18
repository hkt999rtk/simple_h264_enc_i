# JPEG Streaming Memory Report

Issue #36 records the production output-buffer memory regression target after
the JPEG tool moved to the public streaming H.264 output consumer API.

The baseline component-plane values are the decoded image component allocations
from the earlier allocation report. The production measurements below are from
`sh264e_encode_jpeg`, which calls `sh264e_jpeg_get_work_size` and
`sh264e_encode_jpeg_idr_with_arena_stream` on the default arena path.

## Measured 4:2:0 Production Path

| Source JPEG | Previous full component planes | Production arena work | Production peak allocation | Streaming row cache | Slice work buffer |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1280x720 4:2:0 | 1,382,400 | 92,304 | 92,160 | 61,440 | 61,440 |
| 2560x1440 4:2:0, 1:1 fast path, I420 | 5,529,600 | 123,024 | 122,880 | 61,440 | 0 |
| 2560x1440 4:2:0, 1:1 fast path, NV12 | 5,529,600 | 123,024 | 122,880 | 61,440 | 20,480 |

`Production peak allocation` now excludes the previous dynamic NanoJPEG VLC
lookup block. NanoJPEG decodes DHT tables into compact canonical Huffman
metadata in the decoder context, uses a small `NJ_VLC_FAST_BITS` fast table for
short codes, and falls back to canonical range lookup for longer codes. The
streaming row cache is the decoded-image replacement for the previous full
component planes.

Regression tools read tracked allocation stats through
`sh264e_jpeg_get_last_allocation_stats` and row-cache capacity through
`sh264e_jpeg_get_last_streaming_cache_bytes`. Allocation-limit fault injection
and streaming-prototype comparison entry points are private test hooks gated by
`SH264E_ENABLE_JPEG_TEST_HOOKS`, not production application APIs.

For the `2560x1440` 4:2:0 1:1 row, the measured peak breaks down as:

| Block | Bytes |
| --- | ---: |
| Streaming retained row cache | 61,440 |
| NanoJPEG MCU-row temp buffers | 61,440 |
| Total tracked peak allocation | 122,880 |

Issue #31 reduced production peak allocation below the 270 KiB target without
regressing to full component-plane decode. Issue #49 then added the 1:1
`2560x1440` 4:2:0 fast path, reducing that row cache to one MCU/slice row.
Custom DHT baseline JPEGs remain supported.

For callers that do not yet know JPEG geometry, `sh264e_jpeg_get_slice_buffer_size`
still returns the conservative 61,440-byte worst-case slice work capacity.
Callers that have the JPEG bytes should use `sh264e_jpeg_get_slice_work_size`
to get the path-specific requirement before allocating the slice work buffer:

| Output format | Effective slice work bytes | Notes |
| --- | ---: | --- |
| I420 | 0 | Encoder slice planes point directly into the retained MCU-row cache; `work_buffer` may be `NULL` with zero capacity |
| NV12 | 20,480 | Luma points into the cache; chroma is interleaved into an 8-row UV staging window |

## Output Buffer Boundary

The measurements above separate JPEG decoder/scaler memory from H.264 output
buffering. A one-shot JPEG API may still require the maximum complete-frame
H.264 output capacity:

```text
header max = 1,024 bytes
slice max  = 126,976 bytes
slice count = 90
max output = 1,024 + 90 * 126,976 = 11,428,864 bytes
```

This is a caller-owned H.264 output capacity, not JPEG decoder working memory,
and it does not permit full decoded JPEG component-frame allocation. It is much
larger than typical encoded output; the measured `2560x1440` JPEG 4:2:0 fixture
currently emits about 123 KiB of H.264 data.

The production JPEG tool path now uses the streaming H.264 output consumer API
and reuses one caller-owned byte-stream flush buffer, currently 4,096 bytes,
instead of the 11,428,864-byte one-shot maximum output buffer or the old
126,976-byte one-slice output chunk. The tool writes Annex B chunks through a
file consumer outside the library; tests also cover tiny chunk boundaries to
lock emulation-prevention behavior across flushes.

For the `2560x1440` 4:2:0 embedded path, excluding compressed JPEG input and
`.rodata`, the current budget is:

| Block | Bytes |
| --- | ---: |
| JPEG work arena | 123,024 |
| JPEG/scaler slice work | 0 for I420, 20,480 for NV12 |
| Reusable H.264 output chunk buffer | 4,096 |
| Encoder heap | 191,048 |
| Static mutable RAM | about 4,529 |
| Rounded planning budget | about 330 KiB I420 / 350 KiB NV12 |

The I420 arithmetic subtotal of the rows above is about 322,697 bytes, or about
315 KiB in binary units. The equivalent NV12 subtotal is about 343,177 bytes,
or about 335 KiB. The rounded planning budget is now about 330 KiB for I420 and
about 350 KiB for NV12 on this embedded path.

The encoder heap row is measured by `sh264e_encoder_get_memory_report`; see
`docs/ENCODER_MEMORY_REPORT.md` for the context, bitstream scratch,
reconstructed-slice, and neighbor-state breakdown.

## Regression Coverage

`tests/run_ffmpeg_integration.py` asserts the exact production arena work,
peak-allocation, row-cache, effective slice work, reusable output chunk, encoder
memory report, path-specific slice work, and one-shot output capacity values for representative JPEG
fixtures. It also keeps broader
color-subsampling coverage that fails if the production arena path regresses to
full component-plane allocation or if the default JPEG tool path regresses to
allocating `sh264e_get_max_output_size()` for H.264 output.

The expected closeout is stricter: every public JPEG API, including the
heap-backed one-shot wrapper, must use MCU-row streaming and must not touch the
old full decoded component-frame path. Regression coverage should fail if
`sh264e_encode_jpeg_idr` reports the old 5,529,600-byte `2560x1440` 4:2:0
component-plane peak.

The expected validation command is:

```sh
ctest --test-dir build --output-on-failure
```

The `sh264e_ffmpeg_integration` case generates the JPEG fixtures with ffmpeg,
encodes I420 and NV12 H.264 outputs where applicable, validates the bitstreams
with ffprobe/ffmpeg, and compares decoded output frames against the internal
streaming comparison path.

## Manual Measurement Commands

After configuring and building the repository:

```sh
build/sh264e_encode_jpeg --format i420 \
  build/integration/input_720p_yuvj420p.jpg \
  build/issue21_default_720_i420.h264

build/sh264e_encode_jpeg --format i420 \
  build/integration/input_1440p_yuvj420p.jpg \
  build/issue21_default_1440_i420.h264
```

Expected metric lines:

```text
1280x720:
jpeg output consumer chunks: 92
jpeg output chunk buffer bytes: 4096
jpeg work arena bytes: 92304
jpeg slice work bytes: 61440
jpeg effective slice work bytes: 61440
jpeg peak allocation bytes: 92160
jpeg streaming cache bytes: 61440
jpeg output buffer bytes: 4096

2560x1440:
jpeg output consumer chunks: 92
jpeg output chunk buffer bytes: 4096
jpeg work arena bytes: 123024
jpeg slice work bytes: 0
jpeg effective slice work bytes: 0
jpeg peak allocation bytes: 122880
jpeg streaming cache bytes: 61440
jpeg output buffer bytes: 4096
```

The hidden one-shot comparison path remains available for regression tests and
reports the old full-frame output capacity:

```sh
build/sh264e_encode_jpeg --test-one-shot-output --format i420 \
  build/integration/input_1440p_yuvj420p.jpg \
  build/issue36_one_shot_1440_i420.h264
```

Expected one-shot output metric:

```text
jpeg output buffer bytes: 11428864
```
