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
| 1280x720 4:2:0 | 1,382,400 | 616,616 | 616,448 | 61,440 | 61,440 |
| 2560x1440 4:2:0 | 5,529,600 | 770,216 | 770,048 | 184,320 | 61,440 |

`Production peak allocation` includes the dynamic NanoJPEG VLC table block
introduced by the static-BSS reduction work. That block is tracked through the
same JPEG allocation shim as the row cache so embedded integrations can place it
in the caller-provided arena. The streaming row cache is the decoded-image
replacement for the previous full component planes.

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
