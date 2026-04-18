# JPEG Memory Reduction Plan

## Goal

Reduce the current `2560x1440` JPEG 4:2:0 -> H.264 production working memory by
removing the full decoded component-frame dependency.

This document is the focused implementation handoff for making MCU-row
streaming JPEG decode the production path. `SPACE_REDUCTION.md` remains the
broader DRAM/SRAM history and roadmap.

Public JPEG API shape should remain stable for this work. All public JPEG entry
points must use the streaming path internally, while preserving caller-provided
JPEG input, arena, slice-work, and output ownership where applicable.

## Historical Full-Component Memory

The original JPEG path decoded the complete JPEG image into full Y/Cb/Cr
component planes, then scaled and encoded one H.264 slice at a time:

```text
compressed JPEG
-> full decoded Y/Cb/Cr component planes
-> one scaled 2560x16 output slice
-> one H.264 IDR slice
```

For `2560x1440` 4:2:0, the dominant block is the decoded component arena:

| Component | Formula | Bytes |
| --- | ---: | ---: |
| Y | `2560 * 1440` | 3,686,400 |
| Cb | `1280 * 720` | 921,600 |
| Cr | `1280 * 720` | 921,600 |
| Total | full YUV420 component frame | 5,529,600 |

Approximate total production working memory for `2560x1440` 4:2:0, excluding
the compressed JPEG input buffer:

| Area | Bytes | Notes |
| --- | ---: | --- |
| JPEG decoded component arena | 5,529,600 | Main target for this plan |
| NanoJPEG static context BSS | about 525,032 | Mainly VLC tables |
| Encoder internal heap | 191,048 | One encoder instance; see `docs/ENCODER_MEMORY_REPORT.md` |
| H.264 header + reusable slice output buffers | 128,000 | Caller-owned |
| JPEG/scaler output slice work buffer | 61,440 | Caller-owned |
| Total | about 6.14 MiB | Excludes compressed JPEG input |

The encoder and scaler are already slice-oriented. The memory problem was the
full decoded JPEG component frame. That memory model is now disallowed for every
public JPEG API, not only for the arena-backed embedded path.

## Target Production Path

Use the JPEG decoder's MCU-row boundary as the production streaming unit:

```text
compressed JPEG
-> decode one MCU row
-> rolling component row cache
-> scaler source row window
-> one scaled 2560x16 output slice
-> one H.264 IDR slice
```

For bilinear scaling, each output row needs at most two source rows per
component. The row cache still needs extra margin for JPEG MCU granularity,
component subsampling, restart marker boundaries, and row-window overlap.

Target decoded-image working memory for `2560x1440` 4:2:0:

| Path | Decoded-image working memory |
| --- | ---: |
| Current full component planes | 5,529,600 |
| MCU-row streaming cache after 1:1 fast path | 61,440 |
| Expected reduction | about 5.21 MiB |

The target excludes compressed JPEG input, encoder state, H.264 output buffers,
and the existing 61,440-byte output slice work buffer.

## Production Behavior

The production API should keep the existing shape:

* `sh264e_encode_jpeg_idr_with_arena` remains the deterministic embedded entry
  point.
* The caller still owns the JPEG input buffer, JPEG arena, slice-work buffer,
  and H.264 output buffer.
* `sh264e_encode_jpeg_idr` may remain a heap-backed convenience wrapper for
  callers that do not need deterministic JPEG arena placement.
* `sh264e_encode_jpeg_idr_with_arena_stream` remains the embedded
  streaming-output entry point for avoiding the full H.264 output buffer.
* All public JPEG entry points must decode JPEG MCU rows into the rolling row
  cache. No public JPEG API may call a full component-frame decode path or
  allocate a full-image RGB, grayscale, Y, Cb, or Cr intermediate.
* Any retained component-plane implementation must be isolated to explicit
  non-public debug or test code and must not be reachable from public JPEG APIs
  or production tools.

Supported streaming production scope:

* Baseline sequential JPEG only.
* Grayscale and three-component YCbCr.
* Source dimensions inside the existing public JPEG size policy.
* I420 and NV12 H.264 output.
* Restart markers must preserve the same predictor reset behavior as the
  current decoder path.

Rejected or out-of-scope JPEG structures should return stable error statuses:

* progressive or lossless JPEG
* arithmetic coding
* CMYK or other color spaces
* non-power-of-two sampling factors
* unsupported multi-scan layouts

## Implementation Milestones

### 1. MCU-Row Decoder Contract

Make the `njDecodeMcuRows` contract production-safe.

Acceptance criteria:

* Baseline grayscale and YCbCr fixtures pass.
* Unsupported JPEG structures fail with stable statuses.
* Callback failure propagates deterministically.
* Restart marker behavior remains covered.
* Existing hidden streaming prototype tests still pass.

### 2. Rolling Row Cache and Scaler Bridge

Replace the full component-plane dependency with a rolling component row cache
that feeds the existing fixed-point bilinear scaler.

Acceptance criteria:

* Cache sizing is derived from source size, component geometry, MCU row height,
  and destination slice row window.
* One scaled output slice still uses the existing 61,440-byte work buffer.
* `2560x1440` 4:2:0 uses row-cache scaling instead of full component planes.
* The measured cache target for `2560x1440` 4:2:0 is documented.

Implemented bridge contract:

* The hidden streaming bridge sizes row-cache storage from decoded component
  geometry and the scaler's per-slice source row window.
* The integration matrix asserts exact row-cache bytes for color JPEG inputs:
  61,440 bytes for 1280x720 4:2:0, 81,920 bytes for 1280x720 4:2:2,
  122,880 bytes for 1280x720 4:4:4, and 61,440 bytes for the 2560x1440 4:2:0
  1:1 fast path.
* The JPEG tool and integration matrix assert path-specific slice work. The
  general scaler still needs 61,440 bytes, while the 1:1 fast path needs 0 bytes
  for I420 and 20,480 bytes for NV12.

### 3. Production API Switch

Make the arena-backed JPEG encoder use the streaming path by default.

Acceptance criteria:

* `sh264e_encode_jpeg_idr_with_arena` keeps the same public signature.
* Production encode no longer requires full component-frame allocation.
* Arena-too-small behavior remains deterministic.
* Existing JPEG encode tests pass for I420 and NV12 output.

### 4. Memory Regression and Report

Add tests and documentation that prevent accidental regression to full-frame
JPEG component allocation.

Acceptance criteria:

* Production JPEG path fails tests if it allocates a full decoded component
  frame for `2560x1440` 4:2:0.
* Memory report includes before/after numbers for `1280x720` and `2560x1440`
  4:2:0 fixtures.
* ffmpeg integration still validates produced H.264 bitstreams.

### 5. NanoJPEG Static BSS Follow-Up

The next largest fixed memory block is NanoJPEG's static context, about 525 KiB
of BSS, mainly from VLC tables.

This can run after or in parallel with the streaming production work. Candidate
directions:

* caller-provided NanoJPEG context
* caller-provided or arena-backed VLC tables
* smaller generated-on-demand decode tables
* build-time option to trade speed for SRAM

Interim implementation direction:

* Defaulted to arena-backed/dynamic VLC tables with `NJ_DYNAMIC_VLC=1`.
* Kept `NJ_DYNAMIC_VLC=0` as a build-time escape hatch for projects that prefer
  the original static-table model.
* Routed the dynamic tables through the existing NanoJPEG allocation shim so
  heap and caller-provided arena paths both report the 524,288-byte table block
  in their work-size/peak-allocation accounting.

Issue #31 supersedes this interim direction by replacing the dynamic 16-bit
lookup block with compact canonical Huffman decode and `NJ_VLC_FAST_BITS`.

Acceptance criteria:

* Cortex-M object-size report shows reduced fixed SRAM usage.
* JPEG correctness tests still pass.

### 6. Compact Huffman Decode

The static BSS reduction moved the 524,288-byte VLC table block out of fixed
SRAM, but it still appeared in decode-time arena peak allocation. Issue #31
replaced the 16-bit SRAM VLC lookup block with canonical Huffman metadata plus
a configurable small fast table. With default `NJ_VLC_FAST_BITS=8`, the
`2560x1440` 4:2:0 production peak is now:

```text
 61,440 bytes  streaming retained row cache
 61,440 bytes  NanoJPEG MCU-row temp buffers
----------
122,880 bytes  tracked peak allocation
```

Implemented direction:

* Store canonical Huffman metadata for custom DHT tables.
* Use a small configurable fast table, for example `NJ_VLC_FAST_BITS=8` or
  `NJ_VLC_FAST_BITS=10`, with a slow fallback for longer codes.
* Keep custom-DHT JPEG support instead of requiring standard-Huffman-only input.
* Do not allocate VLC/Huffman tables through the caller arena.

Acceptance criteria:

* `2560x1440` 4:2:0 production peak allocation is 122,880 bytes, below 270 KiB.
* `jpeg streaming cache bytes` is 61,440 for the 1:1 fast path.
* Production encode does not regress to the old 5,529,600-byte full
  component-plane allocation.
* Strict ffmpeg decode and full CTest remain green.

### 7. Streaming H.264 Output Consumer

After JPEG row streaming and compact Huffman decode, the largest remaining
embedded RAM risk was the one-shot H.264 output buffer used by the JPEG
API/tool. `sh264e_get_max_output_size()` reports the worst-case complete-frame
capacity:

```text
1,024 header bytes + 90 slices * 126,976 bytes = 11,428,864 bytes
```

This is caller-owned output capacity, not JPEG decoder memory. The actual
`2560x1440` JPEG 4:2:0 test bitstream is much smaller, but embedded callers
still need the worst-case capacity when using the one-shot API.

Implemented streaming behavior:

* The public `sh264e_output_consumer_t` callback lets callers consume complete
  or partial Annex B byte-stream chunks outside the library.
* The arena-backed JPEG streaming-output API reuses one small output chunk
  buffer while preserving NAL-unit order and emulation-prevention behavior
  across flush boundaries.
* The production tool default flush buffer is 4,096 bytes.
* Keep file I/O outside the library; tools/tests may implement file consumers.
* Preserve the one-shot API as a convenience wrapper, potentially implemented
  through a memory consumer.

Memory target for `2560x1440` JPEG 4:2:0 embedded path:

| Area | Bytes |
| --- | ---: |
| JPEG work arena | 123,024 |
| JPEG/scaler slice work | 0 for I420, 20,480 for NV12 |
| Reusable H.264 output chunk buffer | 4,096 |
| Encoder heap | 191,048 |
| Static mutable RAM | about 4,529 |
| Total, excluding compressed JPEG input and `.rodata` | about 330 KiB I420 / 350 KiB NV12 budget class |

Acceptance criteria:

* Embedded JPEG encode path using the streaming-output API no longer requires
  the 11,428,864-byte max complete-frame output buffer or a one-slice output
  chunk buffer.
* Consumer callback errors stop encode deterministically.
* Tool/test file output uses a consumer implemented outside the library.
* One-shot API output and streaming-consumer output are byte-for-byte identical
  for representative fixtures.
* Strict ffmpeg decode and memory regression tests remain green.

### 8. Public JPEG API Full-Frame Decode Closeout

The arena-backed APIs and production tool path use MCU-row streaming, but the
heap-backed one-shot wrapper must also share that implementation. The final
public-API memory contract is:

* `sh264e_encode_jpeg_idr`
* `sh264e_encode_jpeg_idr_with_arena`
* `sh264e_encode_jpeg_idr_with_arena_stream`

All three must avoid full decoded JPEG component frames. The difference between
the APIs is only allocation placement and H.264 output delivery:

| API | JPEG allocation model | H.264 output model |
| --- | --- | --- |
| `sh264e_encode_jpeg_idr` | heap-backed streaming row cache | caller-provided complete output buffer |
| `sh264e_encode_jpeg_idr_with_arena` | caller-provided streaming row-cache arena | caller-provided complete output buffer |
| `sh264e_encode_jpeg_idr_with_arena_stream` | caller-provided streaming row-cache arena | reusable output chunk + consumer callback |

Acceptance criteria for the closeout:

* Calling `sh264e_encode_jpeg_idr` on a `2560x1440` 4:2:0 JPEG no longer
  reports the old 5,529,600-byte full component-plane peak.
* Full component-frame decode is not reachable from any public JPEG API.
* One-shot and streaming-output bitstreams remain byte-for-byte identical for
  representative fixtures.
* Tests fail if any public JPEG API regresses to full decoded component-frame
  allocation.

## Test Plan

Documentation-only commit:

```sh
git diff --check
```

Future implementation work:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Integration validation:

* JPEG I420 and NV12 output decode with ffmpeg.
* Memory report compares current component-plane path with MCU-row streaming.
* QEMU smoke/benchmark path runs when ARM toolchain and QEMU are available.

## GitHub Issue Breakdown

Create milestone-sized issues for PM scheduling. Each issue should cite this
file as its source, include dependencies, list acceptance criteria, include
validation commands, and state memory targets where relevant.

Recommended issue sequence:

1. `JPEG streaming: make MCU-row decoder contract production-safe`
2. `JPEG streaming: production rolling row cache and scaler bridge`
3. `JPEG streaming: switch arena-backed JPEG encode to streaming path`
4. `JPEG streaming: memory regression tests and measurement report`
5. `NanoJPEG: reduce static context BSS`
6. `NanoJPEG: replace 16-bit SRAM VLC tables with compact/XIP Huffman decode`
7. `JPEG: add streaming H.264 output consumer API`
8. `JPEG: remove full component-frame decode from public JPEG APIs`

Do not add `help wanted` or `need help` labels unless an issue is actually
blocked by external hardware or tooling.

## Assumptions

* First implementation target is `2560x1440` JPEG 4:2:0.
* Public JPEG API remains stable.
* Component-plane decode may exist only as non-public debug or test code.
* No public JPEG API may touch a full decoded component frame.
* Compressed JPEG input remains caller-owned and image-size dependent.
* Encoder state, H.264 output buffers, and slice work buffers are outside the
  decoded-image memory target.
