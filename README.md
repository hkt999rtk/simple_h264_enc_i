# simple_h264_enc_i

Minimal C H.264 I-frame encoder library.

This project is intentionally small and narrow in scope:

* C static library built with CMake
* Single-frame IDR-only H.264 output
* Annex B bitstream output: SPS, PPS, 14,400 independent MB IDR slices
* Fixed v1 resolution: 2560x1440
* 8-bit YUV420 input: I420 or NV12
* Baseline / Constrained Baseline compatible CAVLC bitstream
* No file I/O inside the encoder library
* Progressive input-row API for lower SRAM footprint and input bandwidth
* Independent macroblock slice mode with no row reconstructed-neighbor reference
* Core fixed-point bilinear scaler for resizing source YUV420 into encoder slices
* Core memory-input and source-input baseline JPEG decode path via NanoJPEG

The encoder is designed for validation and offline experiments, not compression efficiency.
The current independent-MB bitstream intentionally trades compression ratio for
low SRAM use: it emits 14,400 IDR slice NALUs per frame instead of the former 90
row-slice NALUs, which increases slice-header overhead while eliminating
encoder-owned reconstructed-neighbor row state.

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

Embedded production builds should compile only the static library and leave
private JPEG fault-injection/prototype hooks disabled:

```sh
cmake -S . -B build-prod \
  -DSH264E_BUILD_TOOLS=OFF \
  -DSH264E_BUILD_TESTS=OFF \
  -DSH264E_ENABLE_JPEG_TEST_HOOKS=OFF
cmake --build build-prod
```

The intended production symbol surface can be audited with:

```sh
nm -g build-prod/libsimple_h264_enc_i.a | rg "sh264e_jpeg_set_test|streaming_prototype|njDecode([^A-Za-z0-9_]|$)|njDecodeComponents|njGetImage|njGetImageSize|njIsColor" && exit 1 || true
nm -g build-prod/libsimple_h264_enc_i.a | rg "sh264e_jpeg_get_last_(allocation_stats|streaming_cache_bytes|slice_work_bytes)|sh264e_encoder_get_memory_report"
```

The first command must find no private test hooks or NanoJPEG full-image decode
symbols. The second command documents the public diagnostic/stat APIs that stay
available in production for memory reporting.

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
* Cortex-M QEMU encoder benchmark firmware correctness for default DSP-capable and portable C variants when those tools are available and usable
* production-profile symbol audit when `nm` or `llvm-nm` is available

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
The common `1280x720 -> 2560x1440` and `5120x2880 -> 2560x1440`
ratios use exact fixed-ratio fast paths that preserve the same half-pixel
bilinear output as the general mapper for I420 and NV12.
On ARM builds with DSP extension support, the scaler uses guarded `smlad`
paths for horizontal pair sums and exact fixed-ratio quarter-step vertical
blends unless `SH264E_DISABLE_ARM_DSP` is defined.

Encode a baseline JPEG memory-input path through the library wrapper:

```sh
./build/sh264e_encode_jpeg --format i420 input.jpg output.h264
./build/sh264e_encode_jpeg --format nv12 input.jpg output.h264
```

The JPEG path uses NanoJPEG inside the core library. The tool only reads the JPEG file and writes the `.h264` output; the library API accepts either a caller-provided JPEG byte buffer or a sequential `sh264e_jpeg_source_t` reader and emits Annex B byte-stream chunks through caller-owned output memory.
The tool sizes a caller-provided JPEG decoder arena with `sh264e_jpeg_get_work_size`, passes that arena to `sh264e_encode_jpeg_idr_with_arena_stream`, and prints the arena requirement plus current and peak NanoJPEG allocation bytes reported by `sh264e_jpeg_get_last_allocation_stats`.
Embedded callers that cannot provide one contiguous compressed JPEG can use
`sh264e_jpeg_source_get_work_size`, `sh264e_jpeg_source_get_slice_work_size`,
and `sh264e_encode_jpeg_source_idr_with_arena_stream`. The source callback is
pull-only and sequential: it fills up to the requested byte count, reports the
actual bytes read, and returns `0` bytes at EOF. Work-size queries consume the
source, so callers must reset or recreate the source before the slice-work query
and encode call. File I/O, flash drivers, DMA-ring handling, and retry policy
remain outside the library.
The public diagnostic surface also exposes `sh264e_jpeg_get_last_streaming_cache_bytes` so tools and regression tests can report decoded-row cache usage without private declarations.
`sh264e_jpeg_get_slice_work_size` reports the path-specific caller work buffer
needed for a specific JPEG input and output pixel format; the older
`sh264e_jpeg_get_slice_buffer_size` remains a conservative worst-case query for
callers that have not parsed their JPEG yet.
`sh264e_jpeg_get_last_slice_work_bytes` reports the effective slice staging
used by the last JPEG encode. For `2560x1440` 4:2:0 source JPEGs, the 1:1 fast
path reduces retained row cache to 61,440 bytes and requires 0 bytes of
caller slice work for both I420 and NV12.
For `1280x720` and `5120x2880` 4:2:0 source JPEGs, the streaming JPEG scaler
uses the same exact 2x and 0.5x quarter-step paths as the raw resize API while
still retaining only the rolling MCU-row cache plus one encoder slice work
buffer.
`sh264e_encoder_get_memory_report` reports the fixed-v1 encoder heap breakdown
without creating an encoder; `docs/ENCODER_MEMORY_REPORT.md` records the current
296-byte total and sub-block composition.
Embedded integrations can call `sh264e_encoder_get_work_size` and
`sh264e_encoder_create_with_arena` to place the same encoder state in a
caller-provided arena. The fixed-v1 arena size is currently 303 bytes,
including worst-case control-structure alignment padding; `sh264e_encoder_destroy`
does not free caller-owned arena memory.
The arena-backed production path now uses MCU-row streaming by default. It keeps only NanoJPEG's current MCU-row buffers and the retained row cache, then feeds one scaled output slice at a time to the progressive encoder.
The printed peak allocation includes the streaming row cache and MCU-row temp buffers; it excludes caller-owned JPEG input, arena header overhead, slice-work, H.264 output buffers, NanoJPEG's compact in-context Huffman metadata, and the fixed 512-byte compressed-input parser window in the NanoJPEG context.
Diagnostic-only JPEG allocation-limit and streaming-prototype hooks are gated by `SH264E_ENABLE_JPEG_TEST_HOOKS` and declared in `tests/sh264e_jpeg_test_hooks.h`; they are not normal application APIs.
All public JPEG entry points must use the MCU-row streaming decode path. The
heap-backed convenience wrapper may remain non-deterministic in allocation
placement, but it must not decode the JPEG into a full RGB, grayscale, Y, Cb, or
Cr component frame. One-shot JPEG APIs may still require a caller-provided
complete H.264 output buffer; that output model is separate from JPEG decoded
image memory.
The JPEG tool uses the streaming H.264 output consumer API for its production arena path, so it flushes the Annex B byte stream as it is produced instead of allocating `sh264e_get_max_output_size()` bytes for the complete frame. For the current 2560x1440 encoder geometry this replaces the 5,588,224-byte one-shot output capacity with a reusable 4,096-byte output chunk buffer. Embedded callers can use the same API to forward chunks to flash, storage, DMA, or a ring buffer without a full-frame or full-slice output buffer.
IDR macroblock-slice NALUs on this path are written directly from the bit writer into the Annex B chunk writer as bytes become complete, including emulation-prevention insertion. The caller-buffer APIs still produce byte-identical Annex B output through their existing RBSP scratch path.

For Cortex-M validation, CMake builds QEMU smoke firmware for M3/M4/M7 when the ARM bare-metal toolchain and QEMU are available. CMake first probes whether `arm-none-eabi-gcc` can compile the library's standard-header usage; if the toolchain is only partially installed, QEMU firmware tests are skipped instead of breaking the host build.

The QEMU benchmark preflight targets are:

| Target | CPU / machine | Variant | Purpose |
| --- | --- | --- | --- |
| `sh264e_qemu_scaler_bench_m4` | `cortex-m4` / `mps2-an386` | default DSP-capable build | firmware boot, semihosting exit, checksum, cycle counter plumbing |
| `sh264e_qemu_scaler_bench_m4_portable` | `cortex-m4` / `mps2-an386` | `SH264E_DISABLE_ARM_DSP` | portable C comparison preflight |
| `sh264e_qemu_scaler_bench_m7` | `cortex-m7` / `mps2-an500` | default DSP-capable build | M7 benchmark preflight |
| `sh264e_qemu_scaler_bench_m7_portable` | `cortex-m7` / `mps2-an500` | `SH264E_DISABLE_ARM_DSP` | M7 portable C comparison preflight |
| `sh264e_qemu_encoder_bench_m4` | `cortex-m4` / `mps2-an386` | default DSP-capable build | progressive H.264 encoder benchmark preflight |
| `sh264e_qemu_encoder_bench_m4_portable` | `cortex-m4` / `mps2-an386` | `SH264E_DISABLE_ARM_DSP` | encoder portable C comparison preflight |
| `sh264e_qemu_encoder_bench_m7` | `cortex-m7` / `mps2-an500` | default DSP-capable build | M7 encoder benchmark preflight |
| `sh264e_qemu_encoder_bench_m7_portable` | `cortex-m7` / `mps2-an500` | `SH264E_DISABLE_ARM_DSP` | M7 encoder portable C comparison preflight |

QEMU validates firmware build/run behavior and stable nonzero `sh264e_bench_checksum`. Treat `sh264e_bench_cycles` from QEMU as a preflight signal only; final tuning-quality cycle counts still need real Cortex-M hardware with documented CPU, clock, compiler flags, cache state, and memory placement.
Optional QEMU proxy timing is limited to repeated host/QEMU wall-time trend
measurement. It must not be described as Cortex-M4/M7 cycle data or used as a
merge-blocking absolute threshold unless a later issue defines that policy.
Use `docs/CORTEX_M_SCALER_BENCHMARKS.md` as the capture checklist and result
schema for QEMU proxy timing plus real-board portable C versus DSP
measurements.
After building the QEMU benchmark firmware, run proxy timing with:

```sh
python3 tests/run_qemu_proxy_timing.py --build-dir build-qemu --repeat 7
```

The Cortex-M4/M7 optimization roadmap is tracked in
`docs/CORTEX_M_OPTIMIZATION_PLAN.md`. Implementation work must wait for its
documented baseline dependencies, preserve public APIs by default, keep
portable C fallbacks for guarded DSP paths, and avoid persistent SRAM growth
unless the issue explicitly allows it.

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
* `sh264e_encoder_get_work_size`
* `sh264e_encoder_create_with_arena`
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
* `sh264e_jpeg_get_slice_work_size`
* `sh264e_jpeg_source_get_slice_work_size`
* `sh264e_jpeg_get_work_size`
* `sh264e_jpeg_source_get_work_size`
* `sh264e_encode_jpeg_idr`
* `sh264e_encode_jpeg_idr_with_arena`
* `sh264e_encode_jpeg_idr_with_arena_stream`
* `sh264e_encode_jpeg_source_idr_with_arena_stream`

Diagnostic/stat entry points:

* `sh264e_jpeg_get_last_allocation_stats`
* `sh264e_jpeg_get_last_streaming_cache_bytes`
* `sh264e_jpeg_get_last_slice_work_bytes`
* `sh264e_encoder_get_memory_report`

Frame-mode convenience entry points:

* `sh264e_get_max_output_size`
* `sh264e_encode_idr`

The output bitstream buffer is owned by the caller.

## Progressive Mode

Progressive mode is the preferred v1 interface.

Each `sh264e_encode_idr_slice` call consumes one horizontal macroblock input row:

* Y: `2560 x 16`
* I420 U/V: `1280 x 8` each
* NV12 UV: `2560 x 8`

The required sequence for one frame is:

```text
sh264e_begin_idr
90x sh264e_encode_idr_slice
sh264e_end_idr
```

`sh264e_begin_idr` emits SPS/PPS. The encoder keeps the same 90 input-row
calls, but each row call emits 160 one-macroblock IDR slice NALUs.
`sh264e_end_idr` validates completion and emits no bytes.

The legacy frame API remains available and internally offsets full-frame planes into 90 progressive slice calls.

## Current Limits

The current minimal encoder deliberately does not support:

* arbitrary resolutions
* multiple frames
* P/B frames
* motion estimation
* CABAC
* rate control
* chroma AC or full multi-coefficient residual coding
* deblocking tuning

Independent-MB mode removes row reconstructed-neighbor intra prediction and the
associated reconstructed luma/chroma and neighbor state buffers.
