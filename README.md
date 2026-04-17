# simple_h264_enc_i

Minimal C H.264 I-frame encoder library.

This project is intentionally small and narrow in scope:

* C static library built with CMake
* Single-frame IDR-only H.264 output
* Annex B bitstream output: SPS, PPS, 90 IDR slices
* Fixed v1 resolution: 2560x1440
* 8-bit YUV420 input: I420 or NV12
* Baseline / Constrained Baseline compatible CAVLC bitstream
* No file I/O inside the encoder library
* Progressive slice API for lower SRAM footprint and input bandwidth
* Core fixed-point bilinear scaler for resizing source YUV420 into encoder slices
* Core memory-input baseline JPEG decode path via NanoJPEG

The encoder is designed for validation and offline experiments, not compression efficiency.

## Layout

```text
include/  public C API
src/      encoder library implementation
tools/    command-line validation/example tools
tests/    CTest validation
docs/     design specifications and follow-on design notes
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

The direct progressive API example tool is:

```text
build/sh264e_encode_file_progressive
```

The bilinear resize + progressive encode tool is:

```text
build/sh264e_resize_encode_progressive
```

The JPEG decode + resize + progressive encode tool is:

```text
build/sh264e_encode_jpeg
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
* Cortex-M QEMU scaler smoke tests when `arm-none-eabi-gcc` and `qemu-system-arm` are available and usable
* Cortex-M QEMU scaler benchmark firmware correctness for default DSP-capable and portable C variants when those tools are available and usable

## CLI Example

Encode one 2560x1440 raw YUV420 frame:

```sh
./build/sh264e_encode_file --format i420 input_i420.yuv output.h264
./build/sh264e_encode_file --format nv12 input_nv12.yuv output.h264
```

Encode the same inputs through the direct progressive API example:

```sh
./build/sh264e_encode_file_progressive --format i420 input_i420.yuv output.h264
./build/sh264e_encode_file_progressive --format nv12 input_nv12.yuv output.h264
```

Resize a YUV420 source into the fixed 2560x1440 encoder target and encode progressively:

```sh
./build/sh264e_resize_encode_progressive --format i420 --src-width 1280 --src-height 720 input_i420_720p.yuv output.h264
./build/sh264e_resize_encode_progressive --format nv12 --src-width 1280 --src-height 720 input_nv12_720p.yuv output.h264
```

The resize tool accepts even source dimensions in the bilinear-friendly range:

```text
1280 <= src_width  <= 5120
720  <= src_height <= 2880
```

It reads a full source frame for file-based validation, then calls the library scaler to generate one 2560x16 luma / 8-row chroma output slice at a time and immediately feeds that slice to the progressive encoder.
The library scaler uses fixed-point bilinear interpolation. When the source is already 2560x1440, `sh264e_resize_make_slice` bypasses scaling and points the output slice directly into the source frame.
On ARM builds with DSP extension support, the scaler uses an `smlad` guarded path unless `SH264E_DISABLE_ARM_DSP` is defined.

Encode a baseline JPEG memory-input path through the library wrapper:

```sh
./build/sh264e_encode_jpeg --format i420 input.jpg output.h264
./build/sh264e_encode_jpeg --format nv12 input.jpg output.h264
```

The JPEG path uses NanoJPEG inside the core library. The tool only reads the JPEG file; the library API accepts a caller-provided JPEG byte buffer and emits H.264 into a caller-provided output buffer.
The tool sizes a caller-provided JPEG decoder arena with `sh264e_jpeg_get_work_size`, passes that arena to `sh264e_encode_jpeg_idr_with_arena`, and prints the arena requirement plus current and peak NanoJPEG allocation bytes reported by `sh264e_jpeg_get_last_allocation_stats`. The allocation stats cover decoded JPEG component planes and exclude caller-owned input, arena overhead, slice-work, and H.264 output buffers.
The hidden `--streaming-prototype` flag exercises the experimental MCU-row streaming path for 1280x720 4:2:0, 4:2:2, and 4:4:4 JPEG inputs to I420 output without making it the default encoder path.

For Cortex-M validation, CMake builds QEMU smoke firmware for M3/M4/M7 when the ARM bare-metal toolchain and QEMU are available. CMake first probes whether `arm-none-eabi-gcc` can compile the library's standard-header usage; if the toolchain is only partially installed, QEMU firmware tests are skipped instead of breaking the host build.

The QEMU benchmark preflight targets are:

| Target | CPU / machine | Variant | Purpose |
| --- | --- | --- | --- |
| `sh264e_qemu_scaler_bench_m4` | `cortex-m4` / `mps2-an386` | default DSP-capable build | firmware boot, semihosting exit, checksum, cycle counter plumbing |
| `sh264e_qemu_scaler_bench_m4_portable` | `cortex-m4` / `mps2-an386` | `SH264E_DISABLE_ARM_DSP` | portable C comparison preflight |
| `sh264e_qemu_scaler_bench_m7` | `cortex-m7` / `mps2-an500` | default DSP-capable build | M7 benchmark preflight |
| `sh264e_qemu_scaler_bench_m7_portable` | `cortex-m7` / `mps2-an500` | `SH264E_DISABLE_ARM_DSP` | M7 portable C comparison preflight |

QEMU validates firmware build/run behavior and stable nonzero `sh264e_bench_checksum`. Treat `sh264e_bench_cycles` from QEMU as a preflight signal only; final tuning-quality cycle counts still need real Cortex-M hardware with documented CPU, clock, compiler flags, cache state, and memory placement.

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

Progressive entry points:

* `sh264e_encoder_create`
* `sh264e_encoder_destroy`
* `sh264e_begin_idr`
* `sh264e_encode_idr_slice`
* `sh264e_end_idr`
* `sh264e_get_max_header_output_size`
* `sh264e_get_max_slice_output_size`

Resize entry points:

* `sh264e_resize_get_slice_buffer_size`
* `sh264e_resize_make_slice`

JPEG pipeline entry points:

* `sh264e_jpeg_get_slice_buffer_size`
* `sh264e_jpeg_get_work_size`
* `sh264e_jpeg_get_last_allocation_stats`
* `sh264e_encode_jpeg_idr`
* `sh264e_encode_jpeg_idr_with_arena`

Frame-mode convenience entry points:

* `sh264e_get_max_output_size`
* `sh264e_encode_idr`

The output bitstream buffer is owned by the caller.

## Progressive Mode

Progressive mode is the preferred v1 interface.

Each `sh264e_encode_idr_slice` call consumes one horizontal macroblock row:

* Y: `2560 x 16`
* I420 U/V: `1280 x 8` each
* NV12 UV: `2560 x 8`

The required sequence for one frame is:

```text
sh264e_begin_idr
90x sh264e_encode_idr_slice
sh264e_end_idr
```

`sh264e_begin_idr` emits SPS/PPS. Each slice call emits one IDR slice NALU. `sh264e_end_idr` validates completion and emits no bytes in v1.

The legacy frame API remains available and internally offsets full-frame planes into 90 progressive slice calls.

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
