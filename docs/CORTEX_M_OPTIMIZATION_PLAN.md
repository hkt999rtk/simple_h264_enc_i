# Cortex-M Optimization Plan

This document is the source of truth for the next Cortex-M4/M7 optimization
batch. Implementation issues must reference this document, the documentation
commit that introduced or updated it, and the dependent implementation work.

## Baseline Dependency

PR #73, `h264: shrink mb bitstream scratch`, is the required baseline for this
batch. Developers must wait for PR #73 to land if it is still open, then
fetch/pull latest `main` before starting implementation. That PR reduces the
one-macroblock RBSP scratch size and removes a CAVLC level-code search loop, so
it affects encoder memory reports, one-shot output sizing, and bit-writer
follow-up work.

## Optimization Policy

* Public APIs remain stable unless a specific issue explicitly allows a change.
* Persistent SRAM must not increase unless the issue explicitly justifies it and
  updates the memory reports and tests.
* Cortex-M DSP paths must be guarded at compile time and must keep a portable C
  fallback. Defining `SH264E_DISABLE_ARM_DSP` must force the portable path.
* Output remains bit-exact by default. If an optimization cannot preserve
  checksum parity, open a separate documentation/design issue before changing
  the checksum policy.
* QEMU is a build, boot, semihosting, and checksum preflight only. Real
  Cortex-M hardware with DWT cycle capture is required for performance claims.

## Roadmap

1. Add H.264 encoder DWT benchmark firmware for Cortex-M4/M7.
2. Optimize H.264 luma/chroma DC residual sums with `USAD8` or equivalent DSP
   guarded code.
3. Rewrite the H.264 bit writer with a byte-oriented accumulator.
4. Batch Annex B streaming output runs that do not need emulation-prevention
   insertion.
5. Add exact scaler fast paths for `1280x720 -> 2560x1440` and
   `5120x2880 -> 2560x1440` for I420 and NV12.
6. Reduce the `2560x1440` JPEG 4:2:0 1:1 NV12 slice-work staging requirement.
7. Evaluate exact DSP vertical blend optimization for the bilinear scaler.

## Issue Dependency Graph

```text
PR #73 h264 scratch/CAVLC cleanup
  -> H.264 encoder DWT benchmark firmware
      -> USAD8 DC residual sums
      -> bit writer accumulator
          -> Annex B batch streaming writer

PR #73 h264 scratch/CAVLC cleanup
  -> scaler 2x / 0.5x fast paths
      -> exact DSP vertical blend evaluation

PR #73 h264 scratch/CAVLC cleanup
  -> JPEG/NV12 1:1 zero-slice-work path
```

## Validation Policy

Every implementation issue must run the normal host validation:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
git diff --check
```

When the ARM bare-metal toolchain and QEMU are available, Cortex-M issues must
also run the relevant QEMU firmware tests:

```sh
cmake -S . -B build-qemu -DSH264E_BUILD_QEMU_TESTS=ON
cmake --build build-qemu
ctest --test-dir build-qemu -R 'qemu|sh264e_qemu' --output-on-failure
```

Performance claims must include real-board DWT cycle data with board, core,
clock, compiler, flags, cache state, memory placement, checksum, total cycles,
and cycles per encoded or resized slice.

## Current Benchmark Firmware

The QEMU preflight set now includes scaler smoke/benchmark firmware and H.264
progressive encoder benchmark firmware. The encoder benchmark uses eight
deterministic I420 input rows, caller-provided encoder arena placement, and the
independent-MB progressive path. It exports the same globals as the scaler
benchmark:

* `sh264e_bench_checksum` - correctness guard over the generated Annex B bytes.
* `sh264e_bench_cycles` - DWT cycle count around `sh264e_begin_idr` plus eight input-row encode calls.

The encoder QEMU firmware currently expects checksum `0x5926e2e5` for both
default DSP-capable and portable builds. QEMU rows are correctness/preflight
only. Real-board captures must compare the default DSP-capable and
`SH264E_DISABLE_ARM_DSP` portable variants before using cycle data for
optimization decisions.

## Scaler Fast-Path Status

The raw resize API includes exact fixed-ratio fast paths for
`1280x720 -> 2560x1440` and `5120x2880 -> 2560x1440`. These paths bypass the
general axis mapper but keep the same half-pixel bilinear samples and rounding,
so I420 and NV12 slices remain byte-exact with the reference scaler.
