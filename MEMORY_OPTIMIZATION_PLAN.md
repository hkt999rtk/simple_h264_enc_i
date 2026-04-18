# Memory Optimization Plan

## Goal

Reduce the current embedded-memory budget for the fixed
`2560x1440 JPEG 4:2:0 -> H.264 IDR` path after the completed MCU-row streaming
and public JPEG API cleanup work.

This document is the planning source for issues #48 through #54. Developers
working on those issues must fetch the latest `main` and read this file before
implementation.

## Current Budget

Current `2560x1440` 4:2:0 embedded budget, excluding compressed JPEG input and
`.rodata`:

| Block | Bytes | Notes |
| --- | ---: | --- |
| JPEG work arena | 123,024 | 1:1 4:2:0 path: one MCU-row cache plus NanoJPEG MCU-row temp buffers |
| JPEG/scaler slice work | 61,440 | API capacity; 1:1 effective use is I420 0 bytes, NV12 20,480 bytes |
| Reusable H.264 output chunk buffer | 4,096 | Byte-stream flush buffer |
| Encoder heap | about 191,024 | Needs detailed breakdown |
| Static mutable RAM | about 4,529 | Excludes `.rodata` |
| Total | about 390 KiB class | Excludes compressed JPEG input |

The old full decoded JPEG component-frame allocation was 5,529,600 bytes for
`2560x1440` 4:2:0 and is no longer allowed for public JPEG APIs.

## Issue Dependency Graph

```text
#50 encoder heap breakdown
  -> #51 caller-provided encoder arena

#49 JPEG 1:1 direct MCU-row fast path
  -> #53 reduce/bypass JPEG scaler slice work

#48 streaming H.264 output below one-slice chunk buffering
  independent of #49/#50/#51/#53

#54 embedded production build profile
  independent, low risk

#52 compressed JPEG input streaming
  v2, higher risk, schedule after the lower-risk memory work
```

## Issues

### #48 Streaming H.264 Output Below One-Slice Chunk Buffering

Target the reusable H.264 output buffer:

| Previous | Implemented target |
| ---: | ---: |
| 126,976 bytes | 4,096 bytes |

The JPEG streaming-output API now flushes Annex B byte chunks through the
caller-provided consumer instead of waiting for one complete SPS/PPS or IDR
slice NALU. The implementation keeps file I/O outside the library and preserves
RBSP trailing bits and emulation-prevention behavior across flush boundaries.

Acceptance focus:

* No complete-slice output buffer required for the embedded streaming path.
* Consumer failure remains deterministic.
* Bitstreams remain byte-for-byte identical to the one-shot path for covered
  fixtures, including tiny chunk-boundary regression coverage.

### #49 JPEG 1:1 2560x1440 4:2:0 Direct MCU-Row Fast Path

Target the `2560x1440` 4:2:0 row cache when source and destination geometry are
identical:

| Current | Target direction |
| ---: | ---: |
| 184,320 bytes | implemented at 61,440 bytes for 1:1 4:2:0 |

When the source JPEG is already `2560x1440` 4:2:0, bilinear scaling should be
unnecessary for I420 output and partly avoidable for NV12 output. The pipeline
should detect this case, skip fixed-point scaling where safe, and feed encoder
slices from the smallest valid row-cache/staging window.

The implemented 1:1 path keeps one MCU row per component. I420 output feeds the
encoder directly from the row cache, while NV12 still stages only the 20,480-byte
interleaved chroma window. The public slice-work capacity remains 61,440 bytes
until issue #53 narrows the caller-facing work-buffer contract.

Acceptance focus:

* Non-1:1 sources keep the general MCU-row scaler path.
* 1:1 I420 and NV12 behavior is measured and documented separately.
* Strict ffmpeg validation remains green.

### #50 Encoder Heap Breakdown and Regression Report

Target the opaque encoder heap line item:

| Current | Target |
| ---: | --- |
| about 191,024 bytes | measured sub-block report |

This is a measurement issue before optimization. The report should identify the
major encoder state contributors such as reconstructed storage, neighbor/nonzero
state, bitstream scratch, and context/config overhead.

Acceptance focus:

* CTest-visible or tool-visible report for the fixed v1 config.
* Documentation explains how the encoder heap total is composed.
* Regression guard catches accidental large growth.

### #51 Caller-Provided Encoder Arena API

Depends on #50.

This may not reduce total bytes immediately, but it is important for Cortex-M
placement and deterministic production memory management.

Expected API direction:

```c
sh264e_status_t sh264e_encoder_get_work_size(
    const sh264e_config_t *config,
    size_t *out_size);

sh264e_status_t sh264e_encoder_create_with_arena(
    const sh264e_config_t *config,
    void *arena,
    size_t arena_size,
    sh264e_encoder_t **out_encoder);
```

Acceptance focus:

* Existing heap-backed `sh264e_encoder_create` remains compatible.
* Arena-backed creation has deterministic alignment and too-small behavior.
* `sh264e_encoder_destroy` must not free caller-owned arena memory.

### #52 Streaming Compressed JPEG Input Source

This is a v2 item. It targets memory currently excluded from the budget: the
caller-owned compressed JPEG input buffer.

Current public JPEG APIs accept:

```c
const uint8_t *jpeg_data, size_t jpeg_size
```

A future source abstraction may read from flash, storage, camera FIFO, DMA ring,
or application chunks. This touches NanoJPEG's parser and entropy reader, so it
should be scheduled after the lower-risk memory reductions unless compressed
input storage becomes the dominant product constraint.

Acceptance focus:

* Existing memory-buffer APIs remain compatible.
* Chunk-boundary tests cover headers, entropy data, byte-stuffed sequences, and
  restart markers.
* New compressed-input window size is documented.

### #53 Reduce or Bypass JPEG/Scaler Slice Work Buffer

Depends on or should follow #49.

Target the current fixed slice work buffer:

| Current | Target direction |
| ---: | --- |
| 61,440 bytes | path-dependent reduction or documented reason it cannot shrink |

The easiest first case is likely 1:1 I420 after #49. NV12 may still need chroma
interleave staging. General scaled sources may require deeper encoder/scaler
input refactoring.

Acceptance focus:

* At least one production JPEG path reports slice-work memory below 61,440
  bytes, or a measured reason is documented.
* I420 and NV12 are documented separately.
* General resize path remains correct.

### #54 Embedded Production Build Profile and Symbol Surface

This is a low-risk hygiene issue that should be completed before firmware
integration.

Expected production profile:

```sh
cmake -S . -B build-prod \
  -DSH264E_BUILD_TOOLS=OFF \
  -DSH264E_BUILD_TESTS=OFF \
  -DSH264E_ENABLE_JPEG_TEST_HOOKS=OFF
cmake --build build-prod
```

Acceptance focus:

* Private JPEG test hooks are absent from production symbols.
* NanoJPEG full-image decode remains disabled.
* Public diagnostics are intentionally documented.
* Production build command and symbol-inspection command are documented.

## Validation Baseline

All memory optimization issues should keep the normal validation green:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
git diff --check
```

When an issue changes memory behavior, update the relevant report:

* `docs/JPEG_STREAMING_MEMORY_REPORT.md`
* `docs/JPEG_MCU_ROW_STREAMING.md`
* `README.md` if public commands or APIs change
* `docs/SPEC.md` if public API or library contracts change

## Scheduling Recommendation

1. Do #50 first to make encoder memory visible.
2. Do #48 and #49 in parallel; both can save large RAM blocks.
3. Do #51 after #50.
4. Do #53 after #49 unless scoped narrowly.
5. Do #54 any time before firmware integration.
6. Keep #52 as v2 unless compressed JPEG input storage becomes the top product
   blocker.
