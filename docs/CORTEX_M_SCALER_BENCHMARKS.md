# Cortex-M Benchmark Capture

This document is the handoff artifact for Cortex-M scaler and encoder
benchmarking. It keeps QEMU preflight results separate from real-board timing
so cycle counts are not confused with emulator behavior.

## Scaler Scope

The benchmark firmware in `tests/qemu_scaler_bench.c` exercises
`sh264e_resize_make_slice` on a deterministic 1280x720 NV12 source. That
geometry uses the exact 2x scaler fast path. Each run generates `BENCH_ITERS`
encoder slices and writes:

* `sh264e_bench_checksum` - nonzero correctness guard for the generated slices.
* `sh264e_bench_cycles` - DWT cycle count around the resize loop.

Portable and DSP builds must produce the same checksum for the same target
fixture. Treat a checksum mismatch as a correctness failure before comparing
cycles.

## Encoder Scope

The benchmark firmware in `tests/qemu_encoder_bench.c` exercises the
independent-MB progressive H.264 encoder path on eight deterministic raw I420
input rows. Each run writes:

* `sh264e_bench_checksum` - correctness guard over the generated Annex B bytes.
* `sh264e_bench_cycles` - DWT cycle count around `sh264e_begin_idr` plus eight `sh264e_encode_idr_slice` calls.

Portable and DSP-capable builds must produce the same checksum for the same
target fixture. The QEMU firmware checks the current expected checksum
`0x5926e2e5`; treat any checksum mismatch as a correctness failure before
comparing cycles.

## QEMU Preflight

Use QEMU to verify that firmware builds, boots, exits through semihosting, and
keeps checksum behavior stable. Do not use QEMU cycle counts as tuning-quality
performance data for scaler or encoder optimizations.

```sh
cmake -S . -B build-qemu -DSH264E_BUILD_QEMU_TESTS=ON
cmake --build build-qemu --target \
  sh264e_qemu_scaler_bench_m4 \
  sh264e_qemu_scaler_bench_m4_portable \
  sh264e_qemu_scaler_bench_m7 \
  sh264e_qemu_scaler_bench_m7_portable \
  sh264e_qemu_encoder_bench_m4 \
  sh264e_qemu_encoder_bench_m4_portable \
  sh264e_qemu_encoder_bench_m7 \
  sh264e_qemu_encoder_bench_m7_portable
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

The default target set is:

* `sh264e_qemu_scaler_bench_m4`
* `sh264e_qemu_scaler_bench_m4_portable`
* `sh264e_qemu_scaler_bench_m7`
* `sh264e_qemu_scaler_bench_m7_portable`
* `sh264e_qemu_encoder_bench_m4`
* `sh264e_qemu_encoder_bench_m4_portable`
* `sh264e_qemu_encoder_bench_m7`
* `sh264e_qemu_encoder_bench_m7_portable`

Use `--target <name>` to run a subset, `--format csv` for machine-readable
output, and `--list-targets` to print the known target metadata without running
QEMU. The runner exits nonzero if any selected firmware exits nonzero.

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
