# JPEG Streaming Memory Report

Issue #21 records the production memory regression target after the
arena-backed JPEG encoder switched to the MCU-row streaming path.

The baseline component-plane values are the decoded image component allocations
from the earlier allocation report. The production measurements below are from
`sh264e_encode_jpeg`, which calls `sh264e_jpeg_get_work_size` and
`sh264e_encode_jpeg_idr_with_arena` on the default arena path.

## Measured 4:2:0 Production Path

| Source JPEG | Previous full component planes | Production arena work | Production peak allocation | Streaming row cache | Slice work buffer |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1280x720 4:2:0 | 1,382,400 | 92,304 | 92,160 | 61,440 | 61,440 |
| 2560x1440 4:2:0 | 5,529,600 | 245,904 | 245,760 | 184,320 | 61,440 |

`Production peak allocation` now excludes the previous dynamic NanoJPEG VLC
lookup block. NanoJPEG decodes DHT tables into compact canonical Huffman
metadata in the decoder context, uses a small `NJ_VLC_FAST_BITS` fast table for
short codes, and falls back to canonical range lookup for longer codes. The
streaming row cache is the decoded-image replacement for the previous full
component planes.

For the `2560x1440` 4:2:0 row, the measured peak breaks down as:

| Block | Bytes |
| --- | ---: |
| Streaming retained row cache | 184,320 |
| NanoJPEG MCU-row temp buffers | 61,440 |
| Total tracked peak allocation | 245,760 |

Issue #31 reduced production peak allocation below the 270 KiB target without
increasing the 184,320-byte row cache or regressing to full component-plane
decode. Custom DHT baseline JPEGs remain supported.

## Regression Coverage

`tests/run_ffmpeg_integration.py` asserts the exact production arena work,
peak-allocation, and row-cache values for the representative `1280x720` and
`2560x1440` 4:2:0 JPEG fixtures. It also keeps broader color-subsampling
coverage that fails if the production arena path regresses to full
component-plane allocation.

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
jpeg work arena bytes: 616616
jpeg peak allocation bytes: 616448
jpeg streaming cache bytes: 61440

2560x1440:
jpeg work arena bytes: 770216
jpeg peak allocation bytes: 770048
jpeg streaming cache bytes: 184320
```

After issue #31 lands, these expected peak/work values should be updated while
keeping the row-cache value stable unless the row-cache algorithm is
intentionally changed and remeasured.
