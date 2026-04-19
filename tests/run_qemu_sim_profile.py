#!/usr/bin/env python3
"""Characterize Cortex-M simulator DWT and WFI behavior."""

from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional, Sequence


@dataclass(frozen=True)
class ProbeTarget:
    name: str
    machine: str
    cpu: str
    elf_name: str


TARGETS = (
    ProbeTarget(
        "sh264e_qemu_sim_profile_m7",
        "mps2-an500",
        "cortex-m7",
        "sh264e_qemu_sim_profile_m7.elf",
    ),
)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run a Cortex-M simulator profiling probe and label DWT/WFI "
            "observations as simulator validation only."
        )
    )
    parser.add_argument("--build-dir", default="build-qemu")
    parser.add_argument("--qemu", default=None, help="Path to qemu-system-arm.")
    parser.add_argument("--target", choices=[target.name for target in TARGETS], default=TARGETS[0].name)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--list-targets", action="store_true")
    args = parser.parse_args(argv)
    if args.timeout <= 0.0:
        parser.error("--timeout must be positive")
    return args


def selected_target(name: str) -> ProbeTarget:
    for target in TARGETS:
        if target.name == name:
            return target
    raise AssertionError(name)


def qemu_version(qemu: str) -> str:
    try:
        result = subprocess.run(
            [qemu, "--version"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=5.0,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"unavailable ({exc})"
    return result.stdout.splitlines()[0].strip() if result.stdout.splitlines() else "unknown"


def host_summary() -> str:
    cpu = platform.processor() or platform.machine() or "unknown CPU"
    if sys.platform == "darwin":
        try:
            result = subprocess.run(
                ["sysctl", "-n", "machdep.cpu.brand_string"],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=2.0,
            )
            cpu = result.stdout.strip() or cpu
        except (OSError, subprocess.TimeoutExpired):
            pass
    return f"{platform.platform()} / {cpu}"


def qemu_command(qemu: str, target: ProbeTarget, elf: Path) -> list[str]:
    return [
        qemu,
        "-M",
        target.machine,
        "-cpu",
        target.cpu,
        "-kernel",
        str(elf),
        "-semihosting",
        "-nographic",
        "-monitor",
        "none",
        "-serial",
        "none",
    ]


def parse_probe_output(text: str) -> Dict[str, str]:
    values: Dict[str, str] = {}
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            values[key.strip()] = value.strip()
    return values


def parse_u32(values: Dict[str, str], key: str) -> Optional[int]:
    value = values.get(key)
    if value is None:
        return None
    try:
        return int(value, 10)
    except ValueError:
        return None


def characterize_dwt(active_cycles: Optional[int]) -> str:
    if active_cycles is None:
        return "unsupported: active_cycles was not reported"
    if active_cycles == 0:
        return "zero/stubbed: active work completed but CYCCNT stayed zero"
    return "usable for simulator diagnostics: CYCCNT advanced during active work"


def characterize_wfi(values: Dict[str, str], timed_out: bool) -> str:
    if timed_out:
        if values.get("wfi_probe") == "begin":
            return "blocked: WFI did not resume before timeout"
        return "unsupported: probe timed out before WFI observation"
    if values.get("wfi_probe") != "end":
        return "unsupported: WFI completion marker was not reported"
    systick_count = parse_u32(values, "systick_count")
    wfi_cycles = parse_u32(values, "wfi_cycles")
    if systick_count is None or wfi_cycles is None:
        return "unstable: WFI completed but idle metadata was incomplete"
    if systick_count == 0:
        return "unstable: WFI returned without the configured SysTick wake signal"
    if wfi_cycles == 0:
        return "zero/stubbed: WFI woke from SysTick but CYCCNT stayed zero"
    return "observable: WFI woke from SysTick and CYCCNT advanced during idle wait"


def print_report(
    target: ProbeTarget,
    qemu: str,
    values: Dict[str, str],
    timed_out: bool,
    returncode: Optional[int],
) -> None:
    active_cycles = parse_u32(values, "active_cycles")
    wfi_cycles = parse_u32(values, "wfi_cycles")
    systick_count = parse_u32(values, "systick_count")
    print("# Cortex-M Simulator Profile")
    print()
    print("This report is simulator validation only. It is not Cortex-M performance data.")
    print()
    print(f"- QEMU version: {qemu_version(qemu)}")
    print(f"- Host: {host_summary()}")
    print(f"- Target firmware: `{target.name}`")
    print(f"- Machine / CPU: `{target.machine}` / `{target.cpu}`")
    print(f"- Exit status: {'timeout' if timed_out else returncode}")
    print()
    print("| Probe | Observation | Raw value |")
    print("| --- | --- | --- |")
    print(f"| DWT CYCCNT active work | {characterize_dwt(active_cycles)} | {active_cycles if active_cycles is not None else 'N/A'} |")
    print(f"| WFI idle wait | {characterize_wfi(values, timed_out)} | cycles={wfi_cycles if wfi_cycles is not None else 'N/A'}, systick_count={systick_count if systick_count is not None else 'N/A'} |")
    print()
    print("Keep these rows separate from real-board DWT captures and QEMU proxy timing rows.")


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    build_dir = Path(args.build_dir)
    target = selected_target(args.target)
    if args.list_targets:
        for item in TARGETS:
            print(f"{item.name}\t{item.machine}\t{item.cpu}\t{build_dir / item.elf_name}")
        return 0

    qemu = args.qemu or shutil.which("qemu-system-arm")
    if not qemu:
        print("error: qemu-system-arm was not found; pass --qemu", file=sys.stderr)
        return 2

    elf = build_dir / target.elf_name
    if not elf.exists():
        print(f"error: missing simulator profile ELF: {elf}", file=sys.stderr)
        return 2

    timed_out = False
    returncode: Optional[int] = None
    stdout = ""
    stderr = ""
    try:
        result = subprocess.run(
            qemu_command(qemu, target, elf),
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=args.timeout,
        )
        returncode = result.returncode
        stdout = result.stdout
        stderr = result.stderr
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""

    values = parse_probe_output(stdout + "\n" + stderr)
    print_report(target, qemu, values, timed_out, returncode)

    if timed_out and values.get("wfi_probe") == "begin":
        return 0
    if timed_out:
        return 1
    return 0 if returncode == 0 and "active_cycles" in values else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
