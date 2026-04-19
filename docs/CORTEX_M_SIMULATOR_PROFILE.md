# Cortex-M Simulator Profile Validation

This document defines the local macOS simulator validation path for Cortex-M
DWT and idle behavior. It is a diagnostic harness only. Simulator observations
must not be reported as Cortex-M4/M7 performance data and must stay separate
from both real-board DWT captures and QEMU proxy timing rows.

## Scope

The supported simulator path uses the existing QEMU dependency:

* `qemu-system-arm`
* `arm-none-eabi-gcc`
* CMake and Python 3

The probe target is `sh264e_qemu_sim_profile_m7`. It builds a freestanding
Cortex-M7 image that does not include the encoder library and does not require
bare-metal C library headers. The probe verifies simulator behavior that the
full benchmark firmware depends on:

* DWT `CYCCNT` increments during a deterministic active loop.
* `WFI` can sleep until a configured SysTick interrupt wakes the core.
* DWT `CYCCNT` behavior during the idle wait is observable, zero/stubbed, or
  unsupported.

## macOS Setup

Install the expected tools with Homebrew or an equivalent package manager:

```sh
brew install cmake qemu arm-none-eabi-gcc
```

Then configure and build the QEMU-enabled tree:

```sh
cmake -S . -B build-qemu -DSH264E_BUILD_QEMU_TESTS=ON
cmake --build build-qemu --target sh264e_qemu_sim_profile_m7
```

Run the simulator profile harness:

```sh
python3 tests/run_qemu_sim_profile.py \
  --build-dir build-qemu \
  --target sh264e_qemu_sim_profile_m7
```

The runner emits a Markdown report with explicit simulator-only labels. A
nonzero active-loop `CYCCNT` means DWT is usable for simulator diagnostics. A
zero active-loop value means the simulator stubs or disables CYCCNT. The WFI
row reports whether the simulator resumed from SysTick, returned without the
expected interrupt, stayed blocked until timeout, or omitted the required
metadata.

## Initial macOS QEMU Observation

Captured on 2026-04-19 with QEMU emulator version 10.2.1 on
macOS-26.3-arm64-arm-64bit:

| Probe | Observation | Raw value |
| --- | --- | --- |
| DWT `CYCCNT` active work | Zero/stubbed: active work completed but `CYCCNT` stayed zero | `active_cycles=0` |
| `WFI` idle wait | Observable wake from SysTick, but `CYCCNT` stayed zero during idle wait | `wfi_cycles=0`, `systick_count=1` |

This means the tested macOS QEMU path is useful for boot, semihosting, WFI
wake, and control-flow diagnostics, but it is not useful for DWT cycle
measurement. It cannot distinguish active work from idle wait by `CYCCNT`.

## Full Benchmark Firmware Gap

The existing encoder and scaler QEMU firmware targets remain the correctness
preflight for benchmark code:

```sh
cmake --build build-qemu --target \
  sh264e_qemu_scaler_bench_m7 \
  sh264e_qemu_scaler_bench_m7_portable \
  sh264e_qemu_encoder_bench_m7 \
  sh264e_qemu_encoder_bench_m7_portable
ctest --test-dir build-qemu -R 'sh264e_qemu_(scaler|encoder)_bench_m7' --output-on-failure
```

Some macOS `arm-none-eabi-gcc` packages do not include the Newlib header set
needed by those library firmware targets. In that case CMake prints:

```text
Skipping QEMU firmware tests: arm-none-eabi-gcc cannot compile <stdlib.h>
```

That is a host toolchain packaging gap, not a simulator result. Install a
bare-metal toolchain that can compile `<stdlib.h>` to run the full encoder and
scaler firmware locally. The Linux validation workflow installs the required
Newlib package and remains the CI path for those targets.

## Reporting Policy

Do not put simulator profile rows in the real-board result table. If a report
is copied into `docs/CORTEX_M_SCALER_BENCHMARKS.md`, label it as simulator
validation only and keep it separate from:

* real-board DWT cycle rows, which are authoritative for performance claims
* QEMU proxy timing rows, which are host/QEMU wall-time trend metrics

Issue #93 remains the real-board baseline item. This simulator harness only
answers whether the local simulator can support preflight diagnostics for DWT
and WFI behavior.
