# Cortex-M Benchmark Capture

This document is the handoff artifact for Cortex-M scaler and encoder
benchmarking. It keeps QEMU preflight results separate from real-board timing
so cycle counts are not confused with emulator behavior.

## Scaler Scope

The benchmark firmware in `tests/qemu_scaler_bench.c` exercises
`sh264e_resize_make_slice` on deterministic NV12 source rows. The default target
uses the `1280x720 -> 2560x1440` exact 2x path. Additional QEMU targets compile
the same firmware for 1:1 bypass, `5120x2880 -> 2560x1440` exact 0.5x, and a
`1920x1080 -> 2560x1440` general bilinear path. The firmware retains only the
source rows reached by the measured slice window, so benchmark-only buffers do
not represent library persistent SRAM. Each run generates `BENCH_ITERS` encoder
slices and writes:

* `sh264e_bench_checksum` - nonzero correctness guard for the generated slices.
* `sh264e_bench_cycles` - DWT cycle count around the resize loop.

Portable and DSP builds must produce the same checksum for the same target
fixture. Treat a checksum mismatch as a correctness failure before comparing
cycles.

## Encoder Scope

The benchmark firmware in `tests/qemu_encoder_bench.c` exercises the
independent-MB progressive H.264 encoder path on eight deterministic raw input
rows. The default target uses I420 caller-buffer output and keeps the historical
`0x5926e2e5` checksum assertion. Additional QEMU targets compile the same
fixture for I420 streaming-consumer output and NV12 caller-buffer output. Each
run writes:

* `sh264e_bench_checksum` - correctness guard over the generated Annex B bytes.
* `sh264e_bench_cycles` - DWT cycle count around `sh264e_begin_idr` plus eight `sh264e_encode_idr_slice` calls.

Portable and DSP-capable builds must produce the same checksum for the same
target fixture. The default I420 caller-buffer QEMU firmware checks the current
expected checksum `0x5926e2e5`; expanded fixtures keep deterministic nonzero
checksums exported through `sh264e_bench_checksum`. Treat any checksum mismatch
or zero checksum as a correctness failure before comparing cycles.

## Expanded Coverage Target

The current benchmark firmware is intentionally small, but the next performance
batch needs broader coverage before more hot-path tuning lands. New benchmark
coverage should keep the existing checksum-first policy and should not change
public APIs or library persistent SRAM budgets.

Encoder coverage should include:

* I420 caller-buffer progressive encode.
* I420 streaming-consumer progressive encode.
* NV12 progressive encode.

Scaler coverage should include:

* 1:1 bypass.
* Exact `1280x720 -> 2560x1440` 2x scaling.
* Exact `5120x2880 -> 2560x1440` 0.5x scaling.
* A general bilinear case that does not use an exact-ratio fast path.

JPEG coverage includes benchmark-only QEMU firmware that feeds deterministic
component rows into the same MCU-row streaming slice builder and streaming H.264
consumer used by the production JPEG path. The QEMU fixture covers exact 2x and
0.5x resize/encode paths without embedding large compressed JPEG fixtures.
Integration tests remain responsible for compressed JPEG parser coverage.
Benchmark-only buffers are allowed in firmware, but any library memory-budget
change must update the memory reports and tests.

## QEMU Preflight

Use QEMU to verify that firmware builds, boots, exits through semihosting, and
keeps checksum behavior stable. Do not use QEMU cycle counts as tuning-quality
performance data for scaler or encoder optimizations.

```sh
cmake -S . -B build-qemu -DSH264E_BUILD_QEMU_TESTS=ON
cmake --build build-qemu --target sh264e_qemu_scaler_half_bench_m4 sh264e_qemu_encoder_i420_stream_bench_m4
ctest --test-dir build-qemu -R 'sh264e_qemu_(scaler|encoder)_(smoke|bench)_(m4|m7)' --output-on-failure
```

CMake intentionally skips these firmware targets when `arm-none-eabi-gcc` or
`qemu-system-arm` is missing, or when the installed bare-metal toolchain cannot
compile the standard headers used by the library.

## QEMU Proxy Timing

QEMU proxy timing is a host-side wall-time trend metric for repeated QEMU
firmware runs. It is separate from QEMU preflight correctness and from
real-board DWT cycle capture:

* QEMU preflight correctness means build, boot, semihosting exit, and checksum
  validation through the firmware exit code.
* QEMU proxy timing means repeated host/QEMU wall-time measurements for the
  same firmware target on the same host setup.
* Real-board DWT cycles remain the only authoritative Cortex-M performance
  data for optimization claims.

Proxy timing reports must label every value as QEMU proxy timing or
preflight-only wall time. Do not call proxy timing Cortex-M4/M7 cycles, do not
convert it into cycles per slice, and do not use it as a merge-blocking
absolute threshold unless a later issue explicitly defines that policy.

The proxy timing runner should report one row per benchmark target and variant
with these fields:

| Field | Required value |
| --- | --- |
| QEMU version | `qemu-system-arm --version` first line |
| Host OS / CPU | OS version plus CPU model or concise host identifier |
| Target firmware | CMake target name, for example `sh264e_qemu_encoder_bench_m4` |
| Machine | QEMU machine model, for example `mps2-an386` or `mps2-an500` |
| CPU model | QEMU CPU model, for example `cortex-m4` or `cortex-m7` |
| Variant | default DSP-capable build or `SH264E_DISABLE_ARM_DSP` portable C |
| Repeat count | Number of QEMU executions included in the statistics |
| Median wall time | Median elapsed host wall time for successful runs |
| Min wall time | Fastest elapsed host wall time for successful runs |
| Max wall time | Slowest elapsed host wall time for successful runs |
| Checksum status | Pass/fail as determined by firmware exit code |

The runner must treat any nonzero QEMU/firmware exit as a correctness failure
for that row. Failed rows should not contribute to median/min/max timing unless
a later issue defines an explicit failed-run reporting policy.

After building the QEMU benchmark firmware, run the proxy timing runner from
the repository root:

```sh
python3 tests/run_qemu_proxy_timing.py --build-dir build-qemu --repeat 7
```

The default target set includes Cortex-M4 and Cortex-M7 DSP-capable and
portable-C rows for scaler exact 2x, scaler 1:1 bypass, scaler exact 0.5x,
scaler general bilinear, encoder I420 caller-buffer, encoder I420
streaming-consumer, encoder NV12 caller-buffer, JPEG stream exact 2x, and JPEG
stream exact 0.5x fixtures. Use
`--list-targets` to print the exact target names and metadata for the current
checkout.

Use `--target <name>` to run a subset, `--format csv` for machine-readable
output, and `--list-targets` to print the known target metadata without running
QEMU. The runner exits nonzero if any selected firmware exits nonzero.

For the NV12 chroma pair-sum optimization, compare the before/after trend using
the `sh264e_qemu_encoder_nv12_bench_m4`,
`sh264e_qemu_encoder_nv12_bench_m4_portable`,
`sh264e_qemu_encoder_nv12_bench_m7`, and
`sh264e_qemu_encoder_nv12_bench_m7_portable` proxy timing rows. If the local
ARM toolchain cannot build those ELFs, record the missing-tool output with the
host CTest result and let the Ubuntu QEMU validation remain the correctness
preflight.

For the luma macroblock-analysis batching optimization, compare the
before/after trend using the I420 caller-buffer encoder rows
`sh264e_qemu_encoder_bench_m4`, `sh264e_qemu_encoder_bench_m4_portable`,
`sh264e_qemu_encoder_bench_m7`, and
`sh264e_qemu_encoder_bench_m7_portable`; plus the I420 streaming-consumer rows
as controls for the output callback path. The optimization changes source
analysis structure only, so bitstream checksum parity remains the correctness
gate.

For the fixed `nC=0` luma residual-writer specialization, compare the
before/after trend using the same I420 caller-buffer encoder rows. The
specialization changes CAVLC syntax emission control flow only, so bitstream
checksum parity remains the correctness gate and no scaler/JPEG rows are
expected to move.

For the exact-ratio row-kernel scaler optimization, compare the before/after
trend using the scaler exact 2x rows
`sh264e_qemu_scaler_bench_m4`, `sh264e_qemu_scaler_bench_m4_portable`,
`sh264e_qemu_scaler_bench_m7`, and
`sh264e_qemu_scaler_bench_m7_portable`; the scaler exact 0.5x rows
`sh264e_qemu_scaler_half_bench_m4`,
`sh264e_qemu_scaler_half_bench_m4_portable`,
`sh264e_qemu_scaler_half_bench_m7`, and
`sh264e_qemu_scaler_half_bench_m7_portable`; plus the JPEG streaming exact 2x
and 0.5x rows. The optimization is expected to move exact-ratio scaler rows
while leaving 1:1 bypass and general bilinear rows as controls.

For the general bilinear row-invariant hoisting optimization, compare the
before/after trend using the scaler general bilinear rows
`sh264e_qemu_scaler_general_bench_m4`,
`sh264e_qemu_scaler_general_bench_m4_portable`,
`sh264e_qemu_scaler_general_bench_m7`, and
`sh264e_qemu_scaler_general_bench_m7_portable`. The 1:1 bypass and exact-ratio
rows are controls and should remain checksum-identical.

For the JPEG row-cache ring-buffer optimization, compare the before/after trend
using the JPEG stream exact 2x and exact 0.5x rows plus the host ffmpeg
integration memory metrics. The optimization should reduce SRAM copy bandwidth
inside the streaming decoder bridge without changing the reported row-cache byte
capacity or effective slice-work bytes.

For the caller-buffer direct Annex B writer optimization, compare the
caller-buffer encoder rows against the existing streaming-consumer rows. The
caller-buffer rows should remain byte-identical to the streaming-consumer output
for the same fixture. If the remaining encoder RBSP scratch accounting changes,
update `docs/ENCODER_MEMORY_REPORT.md` and the corresponding memory regression
tests in the same change.

For exact-ratio phase-unrolled scaler kernels, compare the scaler exact 2x and
exact 0.5x rows plus the JPEG streaming exact-ratio rows. The implementation
preserves the same half-pixel bilinear output as the quarter-step row kernels
while replacing per-pixel phase derivation with fixed 2x phase pairs and 0.5x
half-phase row traversal; general bilinear rows are controls.

## Simulator DWT And WFI Profile

Use `docs/CORTEX_M_SIMULATOR_PROFILE.md` when validating local macOS simulator
DWT and idle behavior. The `sh264e_qemu_sim_profile_m7` probe is intentionally
separate from encoder/scaler benchmark firmware and from QEMU proxy timing. It
checks whether simulator-observed DWT `CYCCNT` advances during active work and
whether `WFI` resumes from a configured SysTick interrupt.

```sh
cmake -S . -B build-qemu -DSH264E_BUILD_QEMU_TESTS=ON
cmake --build build-qemu --target sh264e_qemu_sim_profile_m7
python3 tests/run_qemu_sim_profile.py --build-dir build-qemu
```

Simulator profile output is diagnostic only. Do not report it as Cortex-M
cycles, do not convert it to cycles per slice, and keep it separate from both
the proxy timing table and the real-board DWT result table.

## Real-Board Capture

Build the same benchmark variants, then flash or load the generated ELF for the
target board. If semihosting is unavailable on the board, set a debugger
breakpoint after `run_benchmark` returns in `Reset_Handler` or on entry to
`semihost_exit` before reading the globals.

Required reads:

```gdb
p/x sh264e_bench_checksum
p/u sh264e_bench_cycles
p/u sh264e_bench_cycles / 8
```

The final expression reports cycles per resized scaler slice for
`tests/qemu_scaler_bench.c` or cycles per encoded input row for
`tests/qemu_encoder_bench.c`; both currently use an eight-iteration benchmark
window.

Record these metadata fields with every result:

| Field | Required value |
| --- | --- |
| Board | Board name and revision |
| Core | Cortex-M core model, for example M4F or M7 |
| CPU clock | Frequency used for the run |
| Compiler | `arm-none-eabi-gcc` version or equivalent |
| Flags | Full optimization and CPU flags |
| Variant | default DSP-capable build or `SH264E_DISABLE_ARM_DSP` |
| Memory placement | SRAM/TCM/flash placement for code, source buffers, and work buffer |
| Cache state | I-cache/D-cache state, if the target has caches |
| Checksum | `sh264e_bench_checksum` value |
| Cycles | `sh264e_bench_cycles` value |

## Result Table

Fill one row per board, core, compiler, and variant. Leave QEMU rows marked as
preflight-only if they are included for traceability.

| Date | Source | Board / machine | Core | Clock MHz | Cache | Memory placement | Compiler / flags | Variant | Checksum | Total cycles | Cycles / slice | Notes |
| --- | --- | --- | --- | ---: | --- | --- | --- | --- | ---: | ---: | ---: | --- |
| TBD | QEMU preflight | `mps2-an386` | Cortex-M4 | N/A | N/A | emulator default | `arm-none-eabi-gcc -O2 -mcpu=cortex-m4 -mthumb` | DSP-capable | TBD | preflight only | preflight only | firmware boot/checksum only |
| TBD | QEMU preflight | `mps2-an386` | Cortex-M4 | N/A | N/A | emulator default | `arm-none-eabi-gcc -O2 -mcpu=cortex-m4 -mthumb -DSH264E_DISABLE_ARM_DSP` | portable C | TBD | preflight only | preflight only | checksum must match DSP-capable row |
| TBD | QEMU preflight | `mps2-an500` | Cortex-M7 | N/A | N/A | emulator default | `arm-none-eabi-gcc -O2 -mcpu=cortex-m7 -mthumb` | DSP-capable | TBD | preflight only | preflight only | firmware boot/checksum only |
| TBD | QEMU preflight | `mps2-an500` | Cortex-M7 | N/A | N/A | emulator default | `arm-none-eabi-gcc -O2 -mcpu=cortex-m7 -mthumb -DSH264E_DISABLE_ARM_DSP` | portable C | TBD | preflight only | preflight only | checksum must match DSP-capable row |
| TBD | QEMU preflight | `mps2-an386` | Cortex-M4 | N/A | N/A | emulator default | `arm-none-eabi-gcc -O2 -mcpu=cortex-m4 -mthumb` | encoder DSP-capable | TBD | preflight only | preflight only | firmware boot/checksum only |
| TBD | QEMU preflight | `mps2-an386` | Cortex-M4 | N/A | N/A | emulator default | `arm-none-eabi-gcc -O2 -mcpu=cortex-m4 -mthumb -DSH264E_DISABLE_ARM_DSP` | encoder portable C | TBD | preflight only | preflight only | checksum must match DSP-capable row |
| TBD | QEMU preflight | `mps2-an500` | Cortex-M7 | N/A | N/A | emulator default | `arm-none-eabi-gcc -O2 -mcpu=cortex-m7 -mthumb` | encoder DSP-capable | TBD | preflight only | preflight only | firmware boot/checksum only |
| TBD | QEMU preflight | `mps2-an500` | Cortex-M7 | N/A | N/A | emulator default | `arm-none-eabi-gcc -O2 -mcpu=cortex-m7 -mthumb -DSH264E_DISABLE_ARM_DSP` | encoder portable C | TBD | preflight only | preflight only | checksum must match DSP-capable row |

## Proxy Timing Result Table

Proxy timing rows belong in a separate table from real-board DWT rows. This
table is a schema for the runner/reporting issues; values are host/QEMU wall
times, not Cortex-M cycles.

Initial baseline rows were captured from QEMU benchmark ELFs built with
`arm-none-eabi-gcc (GCC) 15.2.0` using the CMake QEMU firmware flags
`-O2 -ffreestanding -fno-builtin -ffunction-sections -fdata-sections
-nostdlib`, the target-specific `-mcpu`/`-mthumb` settings shown below, and
`SH264E_DISABLE_ARM_DSP` for portable C rows. The local Homebrew cross compiler
is configured without a bundled C library header set, so this capture used a
temporary minimal C declaration header path while still linking the firmware as
freestanding `-nostdlib` images.

| Date | QEMU version | Host OS / CPU | Target firmware | Machine | CPU model | Variant | Repeats | Median wall time | Min wall time | Max wall time | Checksum status | Notes |
| --- | --- | --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- | --- |
| 2026-04-19 | QEMU emulator version 10.2.1 | macOS-26.3-arm64-arm-64bit / Apple M1 Pro | `sh264e_qemu_scaler_bench_m4` | `mps2-an386` | `cortex-m4` | DSP-capable | 7 | 0.038981s | 0.037587s | 0.044042s | pass | QEMU proxy timing only; not Cortex-M cycles |
| 2026-04-19 | QEMU emulator version 10.2.1 | macOS-26.3-arm64-arm-64bit / Apple M1 Pro | `sh264e_qemu_scaler_bench_m4_portable` | `mps2-an386` | `cortex-m4` | portable C | 7 | 0.038051s | 0.035880s | 0.038912s | pass | QEMU proxy timing only; not Cortex-M cycles |
| 2026-04-19 | QEMU emulator version 10.2.1 | macOS-26.3-arm64-arm-64bit / Apple M1 Pro | `sh264e_qemu_scaler_bench_m7` | `mps2-an500` | `cortex-m7` | DSP-capable | 7 | 0.040218s | 0.038007s | 0.040551s | pass | QEMU proxy timing only; not Cortex-M cycles |
| 2026-04-19 | QEMU emulator version 10.2.1 | macOS-26.3-arm64-arm-64bit / Apple M1 Pro | `sh264e_qemu_scaler_bench_m7_portable` | `mps2-an500` | `cortex-m7` | portable C | 7 | 0.039237s | 0.037282s | 0.042193s | pass | QEMU proxy timing only; not Cortex-M cycles |
| 2026-04-19 | QEMU emulator version 10.2.1 | macOS-26.3-arm64-arm-64bit / Apple M1 Pro | `sh264e_qemu_encoder_bench_m4` | `mps2-an386` | `cortex-m4` | DSP-capable | 7 | 0.075284s | 0.060355s | 0.077442s | pass | QEMU proxy timing only; not Cortex-M cycles |
| 2026-04-19 | QEMU emulator version 10.2.1 | macOS-26.3-arm64-arm-64bit / Apple M1 Pro | `sh264e_qemu_encoder_bench_m4_portable` | `mps2-an386` | `cortex-m4` | portable C | 7 | 0.069209s | 0.056350s | 0.069765s | pass | QEMU proxy timing only; not Cortex-M cycles |
| 2026-04-19 | QEMU emulator version 10.2.1 | macOS-26.3-arm64-arm-64bit / Apple M1 Pro | `sh264e_qemu_encoder_bench_m7` | `mps2-an500` | `cortex-m7` | DSP-capable | 7 | 0.053135s | 0.049608s | 0.056042s | pass | QEMU proxy timing only; not Cortex-M cycles |
| 2026-04-19 | QEMU emulator version 10.2.1 | macOS-26.3-arm64-arm-64bit / Apple M1 Pro | `sh264e_qemu_encoder_bench_m7_portable` | `mps2-an500` | `cortex-m7` | portable C | 7 | 0.052251s | 0.050608s | 0.052608s | pass | QEMU proxy timing only; not Cortex-M cycles |

## Tuning Decision

After real-board rows are available, compare cycles per slice for the portable C
and DSP-capable variants on the same board and compiler. Keep the DSP path as-is
only if it gives a measurable target-board speedup and preserves checksum parity;
otherwise open a follow-up for hand tuning or removal.
