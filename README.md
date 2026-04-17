# simple_h264_enc_i

Minimal C H.264 I-frame encoder library.

This project is intentionally small and narrow in scope:

* C static library built with CMake
* Single-frame IDR-only H.264 output
* Annex B bitstream output: SPS, PPS, IDR slice
* Fixed v1 resolution: 2560x1440
* 8-bit YUV420 input: I420 or NV12
* Baseline / Constrained Baseline compatible CAVLC bitstream
* No file I/O inside the encoder library

The encoder is designed for validation and offline experiments, not compression efficiency.

## Layout

```text
include/  public C API
src/      encoder library implementation
tools/    command-line validation/example tools
tests/    CTest validation
docs/     design specification
```

## Build

```sh
cmake -S . -B build
cmake --build build
```

The main library target is:

```text
simple_h264_enc_i
```

The example encoder tool is:

```text
build/sh264e_encode_file
```

## Test

```sh
ctest --test-dir build --output-on-failure
```

The test suite covers:

* public API validation
* caller output buffer error handling
* SPS/PPS/IDR Annex B NALU sequence
* library boundary check for forbidden file I/O calls
* ffmpeg/ffprobe integration when those tools are available

## CLI Example

Encode one 2560x1440 raw YUV420 frame:

```sh
./build/sh264e_encode_file --format i420 input_i420.yuv output.h264
./build/sh264e_encode_file --format nv12 input_nv12.yuv output.h264
```

Validate with ffmpeg:

```sh
ffprobe output.h264
ffmpeg -v error -i output.h264 -f null -
ffplay output.h264
```

## Library Boundary

The encoder core operates only on caller-provided memory buffers. File I/O belongs in tools, tests, or applications using the library.

Library code must not call:

* `open`, `read`, `write`, `close`
* `fopen`, `fread`, `fwrite`, `fclose`

## Public API

The public API is declared in `include/sh264e.h`.

Main entry points:

* `sh264e_encoder_create`
* `sh264e_encoder_destroy`
* `sh264e_get_max_output_size`
* `sh264e_encode_idr`

The output bitstream buffer is owned by the caller.

## Current Limits

v1 deliberately does not support:

* arbitrary resolutions
* multiple frames
* P/B frames
* motion estimation
* CABAC
* rate control
* chroma AC or full multi-coefficient residual coding
* deblocking tuning
