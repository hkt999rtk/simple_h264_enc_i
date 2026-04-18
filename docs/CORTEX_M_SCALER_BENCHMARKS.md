# Cortex-M Scaler Benchmark Capture

This document is the handoff artifact for Cortex-M scaler benchmarking. It
keeps QEMU preflight results separate from real-board timing so cycle counts
are not confused with emulator behavior.

## Scope

The benchmark firmware in `tests/qemu_scaler_bench.c` exercises
`sh264e_resize_make_slice` on a deterministic 1280x720 NV12 source. That
geometry uses the exact 2x scaler fast path. Each run generates `BENCH_ITERS`
encoder slices and writes:

* `sh264e_bench_checksum` - nonzero correctness guard for the generated slices.
* `sh264e_bench_cycles` - DWT cycle count around the resize loop.

Portable and DSP builds must produce the same checksum for the same target
fixture. Treat a checksum mismatch as a correctness failure before comparing
cycles.

Future H.264 encoder benchmark firmware must use the same QEMU preflight and
real-board DWT capture policy. Encoder benchmarks should export checksum and
cycle globals for the independent-MB progressive encode path, and should compare
portable C and DSP-capable builds only after checksum parity is established.

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
  sh264e_qemu_scaler_bench_m7_portable
ctest --test-dir build-qemu -R 'sh264e_qemu_scaler_(smoke|bench)_(m4|m7)' --output-on-failure
```

CMake intentionally skips these firmware targets when `arm-none-eabi-gcc` or
`qemu-system-arm` is missing, or when the installed bare-metal toolchain cannot
compile the standard headers used by the library.

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

The final expression reports cycles per resized encoder slice because
`BENCH_ITERS` is currently `8`.

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

## Tuning Decision

After real-board rows are available, compare cycles per slice for the portable C
and DSP-capable variants on the same board and compiler. Keep the DSP path as-is
only if it gives a measurable target-board speedup and preserves checksum parity;
otherwise open a follow-up for hand tuning or removal.
