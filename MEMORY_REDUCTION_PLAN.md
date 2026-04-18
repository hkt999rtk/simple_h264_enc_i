# JPEG Memory Reduction Plan

## Goal

Reduce the current `2560x1440` JPEG 4:2:0 -> H.264 production working memory by
removing the full decoded component-frame dependency.

This document is the focused implementation handoff for making MCU-row
streaming JPEG decode the production path. `SPACE_REDUCTION.md` remains the
broader DRAM/SRAM history and roadmap.

Public JPEG API shape should remain stable for this work. In particular,
`sh264e_encode_jpeg_idr_with_arena` should eventually use the streaming path
internally, while preserving caller-provided arena, slice-work, and output
buffer ownership.

## Current Production Memory

The production JPEG path currently decodes the complete JPEG image into full
Y/Cb/Cr component planes, then scales and encodes one H.264 slice at a time:

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
| Encoder internal heap | about 191,024 | One encoder instance |
| H.264 header + reusable slice output buffers | 128,000 | Caller-owned |
| JPEG/scaler output slice work buffer | 61,440 | Caller-owned |
| Total | about 6.14 MiB | Excludes compressed JPEG input |

The encoder and scaler are already slice-oriented. The memory problem is the
full decoded JPEG component frame.

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
| Target MCU-row streaming cache | about 184,320 |
| Expected reduction | about 5.10 MiB |

The target excludes compressed JPEG input, encoder state, H.264 output buffers,
and the existing 61,440-byte output slice work buffer.

## Production Behavior

The production API should keep the existing shape:

* `sh264e_encode_jpeg_idr_with_arena` remains the deterministic embedded entry
  point.
* The caller still owns the JPEG input buffer, JPEG arena, slice-work buffer,
  and H.264 output buffer.
* `sh264e_encode_jpeg_idr` may remain a heap-backed convenience wrapper.
* Component-plane decode may remain temporarily for debug or fallback while the
  streaming matrix is being proven, but the production path should not require
  full component-frame allocation.

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
  122,880 bytes for 1280x720 4:4:4, and 184,320 bytes for 2560x1440 4:2:0.
* The JPEG tool and integration matrix also assert the scaled output slice work
  buffer remains 61,440 bytes.

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

Selected implementation direction:

* Default to arena-backed/dynamic VLC tables with `NJ_DYNAMIC_VLC=1`.
* Keep `NJ_DYNAMIC_VLC=0` as a build-time escape hatch for projects that prefer
  the original static-table model.
* Route the dynamic tables through the existing NanoJPEG allocation shim so
  heap and caller-provided arena paths both report the 524,288-byte table block
  in their work-size/peak-allocation accounting.

Acceptance criteria:

* Cortex-M object-size report shows reduced fixed SRAM usage.
* JPEG correctness tests still pass.

### 6. Compact or XIP Huffman Decode Follow-Up

The static BSS reduction moved the 524,288-byte VLC table block out of fixed
SRAM, but it still appears in decode-time arena peak allocation. For
`2560x1440` 4:2:0, the current production peak is:

```text
524,288 bytes  dynamic NanoJPEG VLC tables
184,320 bytes  streaming retained row cache
 61,440 bytes  NanoJPEG MCU-row temp buffers
----------
770,048 bytes  tracked peak allocation
```

The row-cache memory target is met. The next target is to replace the 16-bit
SRAM VLC lookup tables with a compact Huffman representation and optional
XIP-friendly standard-Huffman fast path.

Preferred direction:

* Store canonical Huffman metadata for custom DHT tables.
* Use a small configurable fast table, for example `NJ_VLC_FAST_BITS=8` or
  `NJ_VLC_FAST_BITS=10`, with a slow fallback for longer codes.
* Optionally add const precomputed standard-Huffman tables that can live in
  flash/XIP for controlled JPEG sources.
* Keep custom-DHT JPEG support unless a build option explicitly selects
  standard-Huffman-only behavior.

Acceptance criteria:

* `2560x1440` 4:2:0 production peak allocation drops below 270 KiB.
* `jpeg streaming cache bytes` remains 184,320 unless intentionally remeasured.
* Production encode does not regress to the old 5,529,600-byte full
  component-plane allocation.
* Strict ffmpeg decode and full CTest remain green.

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

Do not add `help wanted` or `need help` labels unless an issue is actually
blocked by external hardware or tooling.

## Assumptions

* First implementation target is `2560x1440` JPEG 4:2:0.
* Public JPEG API remains stable.
* The component-plane path can remain temporarily for debug or fallback.
* Compressed JPEG input remains caller-owned and image-size dependent.
* Encoder state, H.264 output buffers, and slice work buffers are outside the
  decoded-image memory target.
