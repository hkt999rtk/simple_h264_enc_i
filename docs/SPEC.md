# SPEC.md — Minimal H.264 I-Frame Encoder (SW, Non-Real-Time)

## 1. Overview

This specification defines a minimal software-based H.264 encoder that supports only **intra-frame (I-frame / IDR)** encoding.

The design goal is:

* Simplicity over compression efficiency
* JPEG-like processing model
* Guaranteed decodability by standard H.264 decoders
* Implemented as a reusable C library with test/application I/O kept separate

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
* `sh264e_encoder_destroy`
* `sh264e_encode_idr`
* `sh264e_get_max_output_size`

Required public API concepts:

* `sh264e_status_t` — success and error codes
* `sh264e_pixfmt_t` — supported input formats (`I420`, `NV12`)
* `sh264e_config_t` — encoder configuration
* `sh264e_frame_t` — caller-provided input frame buffers
* `sh264e_encoder_t` — opaque encoder handle

The output bitstream buffer is provided by the caller. The library reports bytes written or returns a buffer-too-small error.

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
* Output structure:

  ```
  SPS
  PPS
  IDR Slice
  ```

---

## 3. Encoder Pipeline

```
Input YUV
   ↓
Macroblock Partition (16x16)
   ↓
Intra Prediction (DC only)
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

* **DC Prediction ONLY**

#### Rules:

* Use average of:

  * Top pixels (if available)
  * Left pixels (if available)
* If unavailable (top/left boundary):

  * Use constant value (e.g. 128)

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

#### Constraints:

* Annex B format (start code prefix)
* Proper RBSP trailing bits

---

## 5. Simplifications (Intentional)

The following features are explicitly NOT supported:

* ❌ P-frame / B-frame
* ❌ Motion estimation
* ❌ Mode decision (no SAD)
* ❌ Multiple intra modes (only DC)
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

  * Must support full frame buffering (2560x1440 YUV)

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
* Verify:

  ```
  ffplay output.h264
  ```

---

### 9.2 Bitstream Check

* Validate:

  * SPS/PPS correctness
  * Slice header correctness

---

### 9.3 Visual Check

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
