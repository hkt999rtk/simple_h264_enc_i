# DRAM/SRAM Reduction Plan

## Goal

Reduce peak memory use for the JPEG decode -> bilinear resize -> H.264 I-frame encode pipeline while preserving the current library boundary:

* Core library accepts caller-provided memory buffers.
* File I/O remains in tools and tests only.
* Progressive encoder output stays slice based.
* v1 encoder target remains fixed at `2560x1440`, 8-bit YUV420, I420 or NV12.

The primary target is ARM Cortex-M class systems, where internal SRAM is limited and external DRAM/SDRAM bandwidth should be treated as expensive.

## Current Memory Pressure

The encoder and scaler are already slice-oriented. Their memory footprint is modest compared with JPEG decode:

| Area | Current behavior | Approximate memory |
| --- | --- | --- |
| Encoder reconstructed state | One luma slice plus chroma slice-local storage | about 60 KB plus small metadata |
| Scaler work buffer | One encoder output slice | about 60 KB |
| JPEG compressed input | Caller-owned byte buffer | image dependent |
| NanoJPEG component decode | Full decoded component planes | image/subsampling dependent |
| NanoJPEG RGB output | Full RGB image for color JPEG | `width * height * 3` bytes |

The biggest avoidable allocation is the full RGB image produced after NanoJPEG conversion.

Examples:

| Source JPEG | RGB output only | 4:2:0 component planes |
| --- | ---: | ---: |
| `1280x720` | about 2.64 MB | about 1.32 MB |
| `2560x1440` | about 10.55 MB | about 5.27 MB |

The current JPEG path works functionally, but the full RGB output is not an acceptable memory model for many Cortex-M targets.

## Phase 1: Remove Full RGB Output

### Plan

Modify the JPEG pipeline so the library does not call the NanoJPEG RGB conversion path for color JPEGs.

Instead, expose or wrap access to NanoJPEG's decoded component planes:

* Y component
* Cb component
* Cr component
* per-component width
* per-component height
* per-component stride

Then scale components directly into encoder slice format:

* Y component -> encoder Y slice `2560 x 16`
* Cb component -> I420 U slice `1280 x 8`, or NV12 U bytes
* Cr component -> I420 V slice `1280 x 8`, or NV12 V bytes

For grayscale JPEGs:

* Y comes from the grayscale component.
* U/V are filled with 128.

### Expected Benefit

This removes the full RGB allocation:

* `1280x720`: saves about 2.64 MB.
* `2560x1440`: saves about 10.55 MB.

The remaining large cost is the full decoded component planes. That is still large, but it is the correct intermediate step before MCU-row streaming.

### Implementation Notes

Preferred implementation:

* Add an internal NanoJPEG decode mode that stops after entropy decode and component-plane preparation.
* Do not allocate `nj.rgb`.
* Add internal component accessors in `src/nanojpeg.c`, for example:

```c
int njGetComponentCount(void);
const unsigned char *njGetComponentPixels(int index);
int njGetComponentWidth(int index);
int njGetComponentHeight(int index);
int njGetComponentStride(int index);
```

These can remain internal declarations used only by `src/sh264e.c`; they do not need to be added to `include/sh264e.h`.

### Validation

* Existing JPEG integration test must continue to decode with ffmpeg.
* Add a regression check that `sh264e_encode_jpeg_idr` does not allocate/use NanoJPEG RGB output for color JPEG.
* Verify I420 and NV12 JPEG pipeline outputs.

## Phase 2: Avoid Chroma Upsampling

### Plan

Preserve JPEG component-native chroma sizes and let the scaler handle each component's actual geometry.

For common 4:2:0 JPEG:

* Y may be full source resolution.
* Cb/Cr may be half width and half height.
* Scale Cb/Cr directly to encoder chroma resolution `1280x720`.

For 4:2:2 or 4:4:4 JPEG:

* Use each component's decoded width/height.
* Scale to the target YUV420 encoder plane size.

### Expected Benefit

Avoids intermediate full-resolution chroma planes and avoids RGB round-tripping:

```text
YCbCr component planes -> encoder YUV420 slices
```

This should reduce both memory and bandwidth versus:

```text
YCbCr component planes -> RGB full frame -> YUV420 slices
```

### Validation

* Generate JPEG fixtures with common subsampling modes if ffmpeg supports them in the test environment.
* Confirm output bitstreams decode without ffmpeg errors.
* Compare visual output with the current RGB path before removing it completely.

## Phase 3: Controlled Allocator and Peak Memory Report

### Plan

Add a library-internal allocation shim for NanoJPEG integration so peak allocation can be measured and bounded.

The goal is not to expose malloc-heavy behavior as a permanent interface. The goal is to make memory use visible:

* current allocated bytes
* peak allocated bytes
* allocation failure path coverage

Possible approach:

* Compile NanoJPEG with `NJ_USE_LIBC=0`.
* Provide `njAllocMem`, `njFreeMem`, `njFillMem`, and `njCopyMem` from the library.
* Initially back them with `malloc/free`.
* Later allow caller-provided arena allocation.

### Expected Benefit

This gives a concrete memory budget for each source size and subsampling pattern.

Measured with `sh264e_encode_jpeg` after routing NanoJPEG through the library allocation shim:

| Pipeline | Source | Peak bytes | Notes |
| --- | --- | ---: | --- |
| JPEG RGB path | 1280x720 4:2:0 | N/A | removed from library encode path |
| JPEG component path | 1280x720 4:2:0 | 1,382,400 | generated `yuvj420p` fixture |
| JPEG component path | 1280x720 4:2:2 | 1,843,200 | generated `yuvj422p` fixture |
| JPEG component path | 1280x720 4:4:4 | 2,764,800 | generated `yuvj444p` fixture |
| JPEG component path | 2560x1440 4:2:0 | 5,529,600 | generated `yuvj420p` fixture |

The public `sh264e_jpeg_get_last_allocation_stats` query reports the current
and peak NanoJPEG allocation bytes from the last JPEG encode. Current bytes
should return to zero after the encode path calls `njDone`.

### Validation

* Unit test peak allocation for representative JPEG fixtures.
* Confirm allocation-failure returns `SH264E_ERR_ALLOCATION_FAILED`.
* Keep the library file-I/O boundary test.

## Phase 4: Caller-Provided JPEG Arena

### Plan

Replace opaque internal dynamic allocation with an optional caller-provided JPEG work arena.

Possible public API direction:

```c
sh264e_status_t sh264e_jpeg_get_work_size(const uint8_t *jpeg_data,
                                          size_t jpeg_size,
                                          size_t *out_size);

sh264e_status_t sh264e_encode_jpeg_idr_with_arena(sh264e_encoder_t *encoder,
                                                  const uint8_t *jpeg_data,
                                                  size_t jpeg_size,
                                                  uint8_t *jpeg_arena,
                                                  size_t jpeg_arena_size,
                                                  uint8_t *slice_work,
                                                  size_t slice_work_size,
                                                  uint8_t *out,
                                                  size_t out_capacity,
                                                  size_t *out_size);
```

Implemented API:

* `sh264e_jpeg_get_work_size` decodes with the existing heap-backed path and returns the exact caller arena size needed by the deterministic arena path, including allocator headers/alignment.
* `sh264e_encode_jpeg_idr_with_arena` decodes through the caller-provided arena and returns `SH264E_ERR_BUFFER_TOO_SMALL` when the arena cannot satisfy NanoJPEG component-plane allocation.
* `sh264e_encode_jpeg_idr` remains the heap-backed convenience wrapper for callers that do not need deterministic JPEG decoder memory placement.

### Expected Benefit

* No hidden heap dependency for embedded integration.
* Application controls placement in SRAM, DRAM, SDRAM, or external memory.
* Easier failure handling and deterministic memory behavior.

### Validation

* Test exact-size arena succeeds.
* Test one-byte-too-small arena fails.
* QEMU smoke test covers arena path when the ARM/QEMU toolchain is available.

## Phase 5: MCU-Row Streaming Decode

### Plan

Longer term, avoid full decoded component planes. The detailed design note is
tracked in `docs/JPEG_MCU_ROW_STREAMING.md`.

Decode JPEG MCU rows into a rolling source-row cache:

```text
JPEG bitstream
-> MCU row decode
-> component row cache
-> scaler source row window
-> encoder output slice
-> H.264 slice encode
```

This is a larger decoder refactor and should not replace the component-plane
path until a narrow prototype is validated. The first slice should keep the
current baseline JPEG scope, decode one MCU row at a time into component row
rings, and feed the existing progressive H.264 slice encoder when enough source
rows are available for the next 16-row luma output slice.

### Expected Benefit

Potentially reduces decoded-image memory from megabytes to a small number of MCU rows plus scaler row windows.

For bilinear scaling, each output row needs at most two source rows per component. The practical buffer must account for:

* JPEG MCU granularity
* component subsampling
* vertical scaling ratio
* restart marker boundaries
* IDCT block output lifetime

Approximate component-cache targets from the design note:

| Source | Full component planes | Approx streaming cache |
| --- | ---: | ---: |
| 1280x720 4:2:0 | 1,382,400 | 61,440 |
| 1280x720 4:2:2 | 1,843,200 | 81,920 |
| 1280x720 4:4:4 | 2,764,800 | 122,880 |
| 5120x2880 4:2:0 | not yet measured | 368,640 |
| 5120x2880 4:4:4 | not yet measured | 614,400 |

These estimates exclude caller-owned JPEG input, the existing 61,440-byte
encoder slice work buffer, and the H.264 output buffer.

### First Prototype Boundary

* Keep the public API unchanged until the internal row-window contract is proven.
* Support baseline sequential grayscale or three-component YCbCr JPEGs.
* Reject progressive/lossless JPEG, arithmetic coding, CMYK/other color spaces,
  non-power-of-two sampling, and non-interleaved multi-scan JPEGs.
* Preserve restart marker handling by resetting DC predictors and bitstream
  state at the same MCU intervals as the current full-plane path.
* Validate 1280x720 4:2:0, 4:2:2, and 4:4:4 fixtures with
  `sh264e_encode_jpeg --streaming-prototype`, including a `cjpeg`-generated
  DRI/RST restart-marker fixture when `cjpeg` is available, then expand to
  grayscale before making it the default JPEG path.

### Risks

* NanoJPEG is currently full-image oriented.
* Streaming requires careful Huffman/bitstream state management.
* Restart interval handling must be preserved.
* More complex error recovery.

This should be treated as a v2 memory project after component-plane mode is stable.

## Recommended Next Commit

Implement Phase 1 and Phase 2 together:

```text
Decode JPEG to component planes only
Scale Y/Cb/Cr components directly into encoder slices
Remove RGB full-frame dependency from sh264e_encode_jpeg_idr
```

Expected outcome:

* Functional behavior unchanged from the user's perspective.
* `sh264e_encode_jpeg_idr` still accepts JPEG memory and emits one Annex B H.264 IDR frame.
* Peak memory drops substantially for color JPEGs.
* The codebase moves closer to MCU-row streaming without taking on that complexity yet.

## Open Questions

* Target SRAM and external DRAM/SDRAM sizes for the first hardware platform.
* Whether the input JPEGs are mostly 4:2:0, 4:2:2, or 4:4:4.
* Whether baseline JPEG source dimensions are always within the current `1280x720..5120x2880` range.
* Whether the embedded integration can provide a fixed JPEG arena.
* Whether thread safety is required for multiple simultaneous JPEG encodes. NanoJPEG's current global context implies "no" for v1.
