# SPEC.md — Minimal H.264 I-Frame Encoder (SW, Non-Real-Time)

## 1. Overview

This specification defines a minimal software-based H.264 encoder that supports only **intra-frame (I-frame / IDR)** encoding.

The design goal is:

* Simplicity over compression efficiency
* JPEG-like processing model
* Guaranteed decodability by standard H.264 decoders
* Implemented as a reusable C library with test/application I/O kept separate
* Prefer progressive slice input to reduce SRAM footprint and input bandwidth
* Process one horizontal macroblock row per progressive encode call
* Avoid full-frame reconstructed buffering in progressive mode

Target use cases:

* Bypass hardware encoder width limitations
* Offline encoding (non real-time acceptable)
* System validation / pipeline testing

---

## 1.1 Implementation Language and Library Boundary

* Implementation language: **C**
* Build system: **CMake**
* v1 library target: **`simple_h264_enc_i`** static library
* The encoder core must be implemented as a **library**, not as a standalone file-conversion program.
* The library must not directly perform file I/O such as:

  * `open`
  * `read`
  * `write`
  * `close`
  * `fopen`
  * `fread`
  * `fwrite`
  * `fclose`

### Library Responsibilities

The library should provide APIs that operate on caller-provided memory buffers:

* Accept input YUV frame buffers and frame metadata
* Encode one frame into H.264 Annex B bitstream data
* Write encoded output into a caller-provided output buffer, callback, or encoder-owned buffer exposed through the API
* Report required output size / consumed bytes / error status
* Keep encoder state, bitstream writing, SPS/PPS generation, slice generation, transform, quantization, and CAVLC inside the library

### Test / Application Responsibilities

Test programs, examples, or command-line tools may perform file I/O:

* Open and read input `.yuv` files
* Allocate input/output buffers
* Call the encoder library API
* Write `.h264` output files
* Close files and release test-side resources

File I/O code exists only for validation and examples, and must stay outside the encoder library.

---

## 1.2 Project Layout

The intended source layout is:

* `include/` — public C API headers
* `src/` — encoder library internals
* `tools/` — command-line examples / validation tools with file I/O
* `tests/` — unit and integration validation code

---

## 1.3 Public C API Shape

The public API uses the `sh264e_` prefix.

Required API entry points:

* `sh264e_encoder_create`
* `sh264e_encoder_get_work_size`
* `sh264e_encoder_create_with_arena`
* `sh264e_encoder_destroy`
* `sh264e_begin_idr`
* `sh264e_encode_idr_slice`
* `sh264e_end_idr`
* `sh264e_encode_idr`
* `sh264e_get_max_header_output_size`
* `sh264e_get_max_slice_output_size`
* `sh264e_get_max_output_size`
* `sh264e_resize_get_slice_buffer_size`
* `sh264e_resize_make_slice`
* `sh264e_jpeg_get_slice_buffer_size`
* `sh264e_jpeg_get_slice_work_size`
* `sh264e_jpeg_source_get_slice_work_size`
* `sh264e_jpeg_source_get_work_size`
* `sh264e_encode_jpeg_idr`
* `sh264e_encode_jpeg_idr_with_arena_stream`

Required public API concepts:

* `sh264e_status_t` — success and error codes
* `sh264e_pixfmt_t` — supported input formats (`I420`, `NV12`)
* `sh264e_config_t` — encoder configuration
* `sh264e_slice_t` — caller-provided progressive slice buffers
* `sh264e_frame_t` — caller-provided input frame buffers
* `sh264e_encoder_t` — opaque encoder handle
* `sh264e_output_consumer_t` — callback that consumes Annex B output byte chunks

The output bitstream buffer is provided by the caller. The library reports bytes written or returns a buffer-too-small error.

Progressive slice mode is the preferred v1 library interface. Frame mode (`sh264e_encode_idr`) remains available as a convenience wrapper around progressive mode. The resize API is part of the core library and produces encoder-sized progressive slices from supported YUV420 source frames. The JPEG API is also part of the core library and decodes baseline JPEG from either caller-owned memory or a caller-provided sequential source before resize and IDR encode.

---

## 2. Input / Output

### 2.1 Input Format

* Resolution: **2560 x 1440**
* Pixel format: **YUV420 (I420 or NV12)**
* Bit depth: **8-bit only**
* Frame count: **Single frame only**

---

### 2.2 Output Format

* Codec: **H.264 (AVC)**
* Profile: **Baseline Profile**
* Frame type: **IDR (I-frame only)**
* Bitstream format:

  * Annex B (start code: `0x000001`)
* Independent-MB output structure:

  ```
  SPS
  PPS
  IDR Slice[MB 0]
  ...
  IDR Slice[MB 14399]
  ```

---

## 2.3 Progressive Slice Mode

Progressive slice mode is intended to reduce SRAM footprint and input bandwidth pressure.

Instead of requiring a full-frame input buffer and full-frame reconstructed buffer, the caller provides one horizontal macroblock-row slice at a time.

### Slice Unit

* Fixed v1 slice height: **one macroblock row**
* Luma input per call: **2560 x 16**
* Chroma input per call:

  * I420: U = **1280 x 8**, V = **1280 x 8**
  * NV12: UV = **2560 x 8**

### Required Call Sequence

The caller must encode one IDR frame using this sequence:

```
sh264e_begin_idr
sh264e_encode_idr_slice  // slice 0
sh264e_encode_idr_slice  // slice 1
...
sh264e_encode_idr_slice  // slice 89
sh264e_end_idr
```

Exactly **90** slice calls are required for a 2560x1440 frame.

### Progressive Output Structure

```
SPS
PPS
IDR Slice[MB 0]
IDR Slice[MB 1]
...
IDR Slice[MB 14399]
```

`sh264e_begin_idr` emits SPS/PPS. `sh264e_end_idr` emits no bitstream in v1; it validates that exactly 90 input rows were submitted and resets the progressive frame state.

Independent-MB mode keeps the public progressive input call sequence but changes
H.264 slice granularity:

* `sh264e_encode_idr_slice` still consumes one horizontal macroblock input row.
* Each input-row call emits **160** complete IDR slice NALUs.
* Each emitted H.264 slice contains exactly one 16x16 macroblock.
* A full frame emits **14,400** IDR slice NALUs.

This distinction is important: an API input slice is one macroblock row, while
an H.264 bitstream slice is one independent macroblock NALU.

### Slice Indexing

The encoder owns the progressive slice index.

* Caller must submit slices in top-to-bottom raster order
* Caller does not pass a slice index
* Independent-MB mode: `first_mb_in_slice = row_index * 160 + mb_x`
* A 91st slice call must fail
* Calling slice encode before `sh264e_begin_idr` must fail
* Calling `sh264e_end_idr` before all 90 slices are encoded must fail

### Slice Buffer Layout

`sh264e_slice_t` points to the beginning of the current slice.

For I420:

* `plane[0]` = Y slice start
* `plane[1]` = U slice start
* `plane[2]` = V slice start
* `stride[0] >= 2560`
* `stride[1] >= 1280`
* `stride[2] >= 1280`

For NV12:

* `plane[0]` = Y slice start
* `plane[1]` = interleaved UV slice start
* `plane[2]` is unused
* `stride[0] >= 2560`
* `stride[1] >= 2560`

### Frame Mode Compatibility

`sh264e_frame_t` and `sh264e_encode_idr` remain available for convenience.

Frame mode may require a full-frame input buffer from the caller, but internally it should be implemented as a wrapper around progressive mode:

* Call `sh264e_begin_idr`
* Offset full-frame plane pointers by macroblock row
* Call `sh264e_encode_idr_slice` 90 times
* Call `sh264e_end_idr`

---

## 2.4 Core Bilinear Resize API

The project provides a core fixed-point bilinear scaler that resizes supported source YUV420 frames into the fixed 2560x1440 encoder target.

The scaler is part of the encoder library public API in v1. It is intended for low-SRAM progressive pipelines and file-based validation tools.

### Library API

```
sh264e_resize_get_slice_buffer_size
sh264e_resize_make_slice
```

`sh264e_resize_get_slice_buffer_size` reports the caller work-buffer size needed by `sh264e_resize_make_slice`.

If the source frame is already **2560x1440**, the required work-buffer size is **0** and `sh264e_resize_make_slice` bypasses scaling by pointing `sh264e_slice_t` directly into the source frame.

### Tool Command

```
sh264e_resize_encode_progressive --format i420|nv12 --src-width W --src-height H input.yuv output.h264
```

The tool is only a file I/O wrapper around the library scaler and progressive encoder.

### Resize Scope

* Source format: **YUV420 (I420 or NV12)**
* Source dimensions:

  * Width must be even
  * Height must be even
  * Valid width range: **1280..5120**
  * Valid height range: **720..2880**
* Destination dimensions:

  * Fixed output width: **2560**
  * Fixed output height: **1440**

The validated source range enforces the recommended bilinear scale range of approximately **0.5x..2.0x** per axis.

### Scaling Rule

Use half-pixel bilinear mapping:

```
src_pos = (dst_pos + 0.5) * src_size / dst_size - 0.5
```

Source sample indices are clamped at image boundaries.

The library scaler implementation must use fixed-point integer arithmetic for coordinate mapping and bilinear interpolation. The inner pixel loops should avoid division so the path remains suitable for ARM Cortex-M class CPUs.

Exact `2x` and `0.5x` source-to-target ratios may use specialized coordinate
paths that avoid the general mapper. These paths must remain byte-exact with
the half-pixel bilinear formula above for both I420 and NV12 output.

When compiled for ARM cores with the DSP extension, the bilinear horizontal
blend may use `smlad`/DSP intrinsics or inline assembly behind a compile-time
guard. Exact `2x` and `0.5x` paths may also use quarter-step `smlad` vertical
pair sums because those ratios only produce 0, 1/4, 1/2, and 3/4 blend
fractions. Defining `SH264E_DISABLE_ARM_DSP` must force the portable C path.

### Cortex-M Optimization Policy

`docs/CORTEX_M_OPTIMIZATION_PLAN.md` is the planning source for Cortex-M4/M7
optimization work. Cortex-M fast paths must preserve the public API, keep a
portable C fallback, and avoid persistent SRAM growth unless a specific issue
explicitly allows and documents the increase. Output is bit-exact by default:
DSP and portable paths must produce matching checksums unless a separate
documentation decision changes that policy. QEMU validates firmware build,
boot, semihosting, and checksum behavior only; real-board DWT cycle capture is
required for performance claims.

### Progressive Encoder Integration

Tools may read a full source frame for file-based validation, but the library resize output side is progressive.

For each encoder slice, the scaler produces:

* Y output: **2560 x 16**
* I420 chroma output:

  * U: **1280 x 8**
  * V: **1280 x 8**
* NV12 chroma output:

  * UV: **2560 x 8**

Each scaled output slice is passed directly to `sh264e_encode_idr_slice`.

---

## 2.5 Core JPEG Decode + Resize + Encode API

The project includes NanoJPEG in the core static library for a direct pipeline:

```
JPEG byte buffer or sequential source
   ↓
NanoJPEG MCU-row baseline decode
   ↓
rolling component row cache
   ↓
Fixed-point bilinear resize into one encoder slice
   ↓
H.264 IDR slice encode
```

### Library API

```
sh264e_jpeg_get_slice_buffer_size
sh264e_jpeg_get_slice_work_size
sh264e_jpeg_source_get_slice_work_size
sh264e_jpeg_get_work_size
sh264e_jpeg_source_get_work_size
sh264e_encode_jpeg_idr
sh264e_encode_jpeg_idr_with_arena
sh264e_encode_jpeg_idr_with_arena_stream
sh264e_encode_jpeg_source_idr_with_arena_stream
sh264e_jpeg_get_last_allocation_stats
sh264e_jpeg_get_last_streaming_cache_bytes
sh264e_jpeg_get_last_slice_work_bytes
sh264e_encoder_get_memory_report
```

The JPEG APIs accept either a caller-provided JPEG byte buffer or a sequential
`sh264e_jpeg_source_t`. File I/O remains outside the library.

The source form is intended for compressed JPEG data stored in flash, storage,
camera FIFOs, DMA rings, or application-managed chunks. The callback contract is
pull-only and sequential:

```c
typedef sh264e_status_t (*sh264e_jpeg_read_fn)(
    void *user,
    uint8_t *dst,
    size_t requested,
    size_t *out_read);
```

The callback must write at most `requested` bytes, set `*out_read` to the
number of bytes copied, and return `SH264E_OK` for successful reads. A successful
read of `0` bytes means EOF. Returning any other status aborts JPEG decode and
propagates that status to the caller when possible. Work-size and slice-work
queries consume the source, so callers must reset or recreate the source before
each query and before `sh264e_encode_jpeg_source_idr_with_arena_stream`.

NanoJPEG keeps a fixed 512-byte compressed-input window for source-backed
decode. That window is separate from caller-owned compressed JPEG storage,
decoded row cache, JPEG arena allocation, slice-work staging, and H.264 output
buffering.

All public JPEG APIs must decode through MCU-row streaming and must not allocate
or depend on a full decoded JPEG component frame. `sh264e_encode_jpeg_idr`,
`sh264e_encode_jpeg_idr_with_arena`, and
`sh264e_encode_jpeg_idr_with_arena_stream` differ only in allocation and output
delivery model; none of them may use a full-image RGB, grayscale, Y, Cb, or Cr
intermediate.

The one-shot JPEG API writes a complete H.264 IDR frame into a caller-provided
output buffer. This API is convenient for host tools, but it may require a
worst-case full H.264 output buffer. That one-shot H.264 output model does not
permit one-shot JPEG decode into full component planes.

The embedded JPEG API should support streaming H.264 output through a caller-provided consumer callback:

```c
typedef sh264e_status_t (*sh264e_output_consumer_t)(
    void *user,
    const uint8_t *data,
    size_t size);
```

The streaming-output JPEG API uses the caller-provided output buffer as a
reusable byte-stream flush buffer. It may split SPS/PPS or IDR slice NAL units
across multiple consumer calls while preserving Annex B ordering, RBSP trailing
bits, and emulation-prevention bytes across flush boundaries. The production
tool uses a 4 KiB buffer by default, and smaller buffers are valid for tests or
tighter embedded integrations. The library must not call file APIs; tools/tests
may implement a consumer that writes to a file.

`sh264e_jpeg_get_last_allocation_stats` reports the current and peak tracked
JPEG allocations from the last JPEG work-size query or encode call.
`sh264e_jpeg_get_last_streaming_cache_bytes` reports the decoded row-cache
capacity retained by the last MCU-row streaming query or encode call.
`sh264e_jpeg_get_slice_work_size` and
`sh264e_jpeg_source_get_slice_work_size` report the path-specific caller
work-buffer capacity for a specific JPEG input and output pixel format. This
lets embedded callers use zero slice-work bytes for the `2560x1440` 4:2:0 1:1
I420 and NV12 paths, while
`sh264e_jpeg_get_slice_buffer_size` remains a conservative worst-case query.
`sh264e_jpeg_get_last_slice_work_bytes` reports the effective slice staging used
by the last JPEG encode. These are diagnostic/stat APIs for regression reporting; allocation-limit and
streaming-prototype hooks remain private test hooks gated by
`SH264E_ENABLE_JPEG_TEST_HOOKS`.
`sh264e_encoder_get_memory_report` reports fixed-v1 encoder-owned memory
without constructing an encoder. The current report is 296 bytes total:
40 bytes of context/config and 256 bytes of one-macroblock RBSP scratch. The
row reconstructed luma/chroma slice storage and row neighbor/nonzero state are
not retained in the encoder arena.
`sh264e_encoder_get_work_size` reports the caller-provided arena requirement for
the same fixed-v1 encoder state, currently 303 bytes including worst-case
alignment padding. `sh264e_encoder_create_with_arena` constructs an encoder in
that caller-owned block; `sh264e_encoder_destroy` does not free arena memory.

## Embedded Production Profile

The recommended firmware-facing production profile builds only the static
library and disables tests, tools, and private JPEG hooks:

```sh
cmake -S . -B build-prod \
  -DSH264E_BUILD_TOOLS=OFF \
  -DSH264E_BUILD_TESTS=OFF \
  -DSH264E_ENABLE_JPEG_TEST_HOOKS=OFF
cmake --build build-prod
```

Production symbol inspection should show public encode and diagnostic/stat APIs
but no private JPEG test hooks or NanoJPEG full-image decode entry points:

```sh
nm -g build-prod/libsimple_h264_enc_i.a | rg "sh264e_jpeg_set_test|streaming_prototype|njDecode([^A-Za-z0-9_]|$)|njDecodeComponents|njGetImage|njGetImageSize|njIsColor" && exit 1 || true
nm -g build-prod/libsimple_h264_enc_i.a | rg "sh264e_(jpeg(_source)?_get|encode_jpeg(_source)?).*|sh264e_encoder_get_memory_report"
```

The normal CTest suite includes `sh264e_production_profile_symbols` when `nm` or
`llvm-nm` is available. That test configures this production profile, builds the
library, verifies tools/tests are absent, confirms the public diagnostic APIs,
and rejects private JPEG hooks plus NanoJPEG full-image decode symbols.

NanoJPEG decodes baseline JPEG through MCU-row streaming. The library scales decoded component rows directly into one YUV420 encoder slice at a time:

For `2560x1440` 4:2:0 source JPEGs encoded to the fixed target, the JPEG path
uses a 1:1 fast path. It skips bilinear scaling, keeps one MCU row per
component, and feeds slices directly from the retained rows without a
caller-provided slice-work buffer.

* I420 JPEG path: Y, U, and V retained rows
* NV12 JPEG path: Y, Cb, and Cr retained rows through the internal streaming
  bridge; the public NV12 slice API remains Y plus interleaved UV

The JPEG source dimensions must satisfy the same resize limits:

* Width: **1280..5120**, even
* Height: **720..2880**, even

The JPEG path is intended for validation and direct camera/snapshot encode use. NanoJPEG is not thread-safe, so the v1 JPEG API should be treated as single-call-at-a-time.

### Tool Command

```
sh264e_encode_jpeg --format i420|nv12 input.jpg output.h264
```

The tool is only a file I/O wrapper around the library JPEG pipeline.

---

## 3. Encoder Pipeline

```
Input YUV Slice
   ↓
Macroblock Partition (16x16)
   ↓
Independent MB Slice Partition (one H.264 slice per MB)
   ↓
Fixed Unavailable-Neighbor Prediction
   ↓
Residual Calculation
   ↓
Transform (4x4 integer transform)
   ↓
Quantization (fixed QP)
   ↓
Zigzag Scan
   ↓
CAVLC Entropy Coding
   ↓
NALU Packaging
```

---

## 4. Functional Requirements

### 4.1 Macroblock Structure

* Macroblock size: **16x16**
* Chroma format: **4:2:0**
* Sub-block transform: **4x4 only**

---

### 4.2 Intra Prediction

#### Supported Modes:

* **Fixed unavailable-neighbor DC-style prediction only**

#### Rules:

* Independent-MB mode emits one H.264 slice per macroblock.
* Left and top macroblocks are outside the current H.264 slice, so they are
  unavailable to the decoder.
* Encoder must not read or write row-level reconstructed-neighbor samples.
* Encoder must not retain reconstructed luma/chroma row slice buffers for
  prediction.
* Encoder must not retain temporary reconstructed-neighbor state inside a
  macroblock slice.
* CAVLC luma residual coding uses fixed `nC = 0` in independent-MB mode.

---

### 4.3 Transform

* Use H.264 **4x4 integer transform**
* Floating-point DCT NOT required

---

### 4.4 Quantization

* Fixed **QP (e.g. QP = 28)**
* No rate control
* No adaptive QP

---

### 4.5 Scan Order

* Standard **zigzag scan (4x4)**

---

### 4.6 Entropy Coding

* Use **CAVLC only**
* CABAC NOT supported

#### Required elements:

* coeff_token
* total_zeros
* run_before

---

### 4.7 NALU Packaging

#### Required NAL units:

* SPS (nal_unit_type = 7)
* PPS (nal_unit_type = 8)
* IDR Slice (nal_unit_type = 5)

Independent-MB mode emits one IDR slice NALU per macroblock.

#### Constraints:

* Annex B format (start code prefix)
* Proper RBSP trailing bits

---

## 5. Simplifications (Intentional)

The following features are explicitly NOT supported:

* ❌ P-frame / B-frame
* ❌ Motion estimation
* ❌ Mode decision (no SAD)
* ❌ Reconstructed-neighbor intra prediction
* ❌ Multiple intra modes (independent unavailable-neighbor DC-style only)
* ❌ CABAC
* ❌ Rate control
* ❌ Deblocking filter tuning (can be disabled or default)
* ❌ Weighted prediction
* ❌ Interlacing
* ❌ High profile tools

---

## 6. Bitstream Compliance

The generated bitstream must:

* Be decodable by:

  * ffmpeg / ffplay
  * VLC
  * Standard H.264 decoders
* Conform to:

  * H.264 Baseline Profile (subset)
* Not require:

  * External metadata

---

## 7. Performance Constraints

* Encoding time:

  * Up to **3 seconds per frame is acceptable**
* Memory:

  * Frame wrapper may require full-frame input buffering by the caller
  * Progressive mode must not require full-frame input buffering
  * Independent-MB mode must not require reconstructed luma/chroma
    slice storage.
  * Independent-MB mode must not require row neighbor/nonzero state.

---

## 8. Expected Trade-offs

| Aspect                 | Expectation                  |
| ---------------------- | ---------------------------- |
| Compression efficiency | Low                          |
| Bitrate                | High                         |
| Quality                | Acceptable (blocky possible) |
| Complexity             | Very low                     |
| Compatibility          | High                         |

---

## 9. Validation

### 9.1 Functional Test

* Encode 1 frame → output `.h264`
* Decode 1 baseline JPEG from memory → resize → output `.h264`
* Verify output structure:

  ```
  SPS
  PPS
  IDR Slice[MB 0]
  ...
  IDR Slice[MB 14399]
  ```
* Verify:

  ```
  ffplay output.h264
  ```

---

### 9.2 Bitstream Check

* Validate:

  * SPS/PPS correctness
  * Slice header correctness
  * Exactly 14,400 IDR slice NALUs for one 2560x1440 progressive frame
  * `first_mb_in_slice = 0..14399`

### 9.3 Progressive API Sequence Test

Validate:

* Slice before `sh264e_begin_idr` fails
* Calling `sh264e_begin_idr` twice fails
* Calling `sh264e_end_idr` before 90 slices fails
* 91st slice call fails
* Complete `begin -> 90 slices -> end` sequence succeeds

---

### 9.4 Resize API Test

Validate:

* 1:1 resize reports zero work-buffer bytes and bypasses copy
* Scaled resize reports non-zero work-buffer bytes and emits encoder-sized slices
* Exact 2x and 0.5x resize paths match the general half-pixel bilinear reference for I420 and NV12
* Invalid resize dimensions fail
* Cortex-M3/M4/M7 QEMU scaler smoke tests pass when the ARM bare-metal toolchain and QEMU are available
* Cortex-M4 QEMU scaler benchmark firmware passes correctness checks
* Cortex-M4/M7 QEMU encoder benchmark firmware passes correctness checks for default DSP-capable and portable C builds

---

### 9.5 Visual Check

* Confirm:

  * No crash in decoder
  * Image is recognizable

---

## 10. Future Extensions (Optional)

* Add I4x4 prediction modes
* Add SAD-based mode selection
* Add basic rate control
* Add multi-frame support
* Add SAR/VUI metadata support

---

## 11. Key Design Philosophy

> This encoder is designed to be:
>
> **"The simplest possible valid H.264 encoder that behaves like JPEG."**

---
