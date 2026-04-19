#!/usr/bin/env python3
"""Run QEMU benchmark firmware repeatedly and report proxy wall-time rows."""

from __future__ import annotations

import argparse
import csv
import datetime as _datetime
import platform
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Sequence, Tuple


@dataclass(frozen=True)
class Target:
    name: str
    machine: str
    cpu: str
    variant: str


TARGETS: Tuple[Target, ...] = (
    Target("sh264e_qemu_scaler_bench_m4", "mps2-an386", "cortex-m4", "DSP-capable"),
    Target("sh264e_qemu_scaler_bench_m4_portable", "mps2-an386", "cortex-m4", "portable C"),
    Target("sh264e_qemu_scaler_bench_m7", "mps2-an500", "cortex-m7", "DSP-capable"),
    Target("sh264e_qemu_scaler_bench_m7_portable", "mps2-an500", "cortex-m7", "portable C"),
    Target("sh264e_qemu_encoder_bench_m4", "mps2-an386", "cortex-m4", "DSP-capable"),
    Target("sh264e_qemu_encoder_bench_m4_portable", "mps2-an386", "cortex-m4", "portable C"),
    Target("sh264e_qemu_encoder_bench_m7", "mps2-an500", "cortex-m7", "DSP-capable"),
    Target("sh264e_qemu_encoder_bench_m7_portable", "mps2-an500", "cortex-m7", "portable C"),
)


@dataclass
class Row:
    date: str
    qemu_version: str
    host: str
    target: Target
    repeats: int
    median: Optional[float]
    minimum: Optional[float]
    maximum: Optional[float]
    checksum_status: str
    notes: str


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run Cortex-M QEMU benchmark firmware repeatedly and report QEMU "
            "proxy timing as host/QEMU wall time, not real Cortex-M cycles."
        )
    )
    parser.add_argument(
        "--build-dir",
        default="build-qemu",
        help="CMake build directory containing sh264e_qemu_* .elf files.",
    )
    parser.add_argument(
        "--qemu",
        default=None,
        help="Path to qemu-system-arm. Defaults to PATH lookup.",
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=7,
        help="Number of successful QEMU executions to time per target.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=10.0,
        help="Per-run timeout in seconds.",
    )
    parser.add_argument(
        "--target",
        action="append",
        choices=[target.name for target in TARGETS],
        help="Target to run. May be repeated. Defaults to all benchmark targets.",
    )
    parser.add_argument(
        "--allow-missing",
        action="store_true",
        help="Skip missing target ELF files instead of failing.",
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "csv"),
        default="markdown",
        help="Output format.",
    )
    parser.add_argument(
        "--list-targets",
        action="store_true",
        help="List known benchmark targets and exit without running QEMU.",
    )
    args = parser.parse_args(argv)
    if args.repeat <= 0:
        parser.error("--repeat must be positive")
    if args.timeout <= 0.0:
        parser.error("--timeout must be positive")
    return args


def selected_targets(names: Optional[Sequence[str]]) -> List[Target]:
    if not names:
        return list(TARGETS)
    by_name = {target.name: target for target in TARGETS}
    return [by_name[name] for name in names]


def target_elf(build_dir: Path, target: Target) -> Path:
    return build_dir / f"{target.name}.elf"


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
    first_line = result.stdout.splitlines()[0] if result.stdout.splitlines() else ""
    return first_line.strip() or "unknown"


def host_cpu_name() -> str:
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
            if result.stdout.strip():
                return result.stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            pass
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith(("model name", "Hardware", "Processor")):
                _, _, value = line.partition(":")
                if value.strip():
                    return value.strip()
    return platform.processor() or platform.machine() or "unknown CPU"


def host_summary() -> str:
    return f"{platform.platform()} / {host_cpu_name()}"


def qemu_command(qemu: str, target: Target, elf: Path) -> List[str]:
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


def run_target(qemu: str, target: Target, elf: Path, repeat: int, timeout: float) -> Row:
    timings: List[float] = []
    for run_index in range(1, repeat + 1):
        command = qemu_command(qemu, target, elf)
        start = time.perf_counter()
        try:
            result = subprocess.run(
                command,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            return make_row(
                target,
                repeat,
                median=None,
                minimum=None,
                maximum=None,
                checksum_status="fail",
                notes=f"timeout on run {run_index}",
            )
        elapsed = time.perf_counter() - start
        if result.returncode != 0:
            return make_row(
                target,
                repeat,
                median=None,
                minimum=None,
                maximum=None,
                checksum_status="fail",
                notes=f"exit {result.returncode} on run {run_index}",
            )
        timings.append(elapsed)

    return make_row(
        target,
        repeat,
        median=statistics.median(timings),
        minimum=min(timings),
        maximum=max(timings),
        checksum_status="pass",
        notes="QEMU proxy timing only; not Cortex-M cycles",
    )


def make_row(
    target: Target,
    repeats: int,
    median: Optional[float],
    minimum: Optional[float],
    maximum: Optional[float],
    checksum_status: str,
    notes: str,
) -> Row:
    return Row(
        date=_datetime.date.today().isoformat(),
        qemu_version=make_row.qemu_version,
        host=make_row.host,
        target=target,
        repeats=repeats,
        median=median,
        minimum=minimum,
        maximum=maximum,
        checksum_status=checksum_status,
        notes=notes,
    )


make_row.qemu_version = "unknown"
make_row.host = "unknown"


def format_seconds(value: Optional[float]) -> str:
    if value is None:
        return "N/A"
    return f"{value:.6f}s"


def markdown_cell(value: object) -> str:
    return str(value).replace("\\", "\\\\").replace("|", "\\|").replace("\n", " ")


def print_markdown(rows: Iterable[Row]) -> None:
    print("# QEMU Proxy Timing")
    print()
    print("Values are host/QEMU wall time only. They are not Cortex-M cycles.")
    print()
    print(
        "| Date | QEMU version | Host OS / CPU | Target firmware | Machine | CPU model | "
        "Variant | Repeats | Median wall time | Min wall time | Max wall time | "
        "Checksum status | Notes |"
    )
    print("| --- | --- | --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- | --- |")
    for row in rows:
        print(
            f"| {row.date} | {markdown_cell(row.qemu_version)} | {markdown_cell(row.host)} | "
            f"`{row.target.name}` | `{row.target.machine}` | `{row.target.cpu}` | "
            f"{markdown_cell(row.target.variant)} | "
            f"{row.repeats} | {format_seconds(row.median)} | {format_seconds(row.minimum)} | "
            f"{format_seconds(row.maximum)} | {row.checksum_status} | {markdown_cell(row.notes)} |"
        )


def print_csv(rows: Iterable[Row]) -> None:
    writer = csv.writer(sys.stdout)
    writer.writerow(
        [
            "date",
            "qemu_version",
            "host_os_cpu",
            "target_firmware",
            "machine",
            "cpu_model",
            "variant",
            "repeats",
            "median_wall_time_seconds",
            "min_wall_time_seconds",
            "max_wall_time_seconds",
            "checksum_status",
            "notes",
        ]
    )
    for row in rows:
        writer.writerow(
            [
                row.date,
                row.qemu_version,
                row.host,
                row.target.name,
                row.target.machine,
                row.target.cpu,
                row.target.variant,
                row.repeats,
                "" if row.median is None else f"{row.median:.9f}",
                "" if row.minimum is None else f"{row.minimum:.9f}",
                "" if row.maximum is None else f"{row.maximum:.9f}",
                row.checksum_status,
                row.notes,
            ]
        )


def print_targets(build_dir: Path, targets: Iterable[Target]) -> None:
    for target in targets:
        print(
            f"{target.name}\t{target.machine}\t{target.cpu}\t"
            f"{target.variant}\t{target_elf(build_dir, target)}"
        )


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    build_dir = Path(args.build_dir)
    targets = selected_targets(args.target)

    if args.list_targets:
        print_targets(build_dir, targets)
        return 0

    qemu = args.qemu or shutil.which("qemu-system-arm")
    if not qemu:
        print("error: qemu-system-arm was not found; pass --qemu", file=sys.stderr)
        return 2

    make_row.qemu_version = qemu_version(qemu)
    make_row.host = host_summary()

    rows: List[Row] = []
    missing: List[Path] = []
    for target in targets:
        elf = target_elf(build_dir, target)
        if not elf.exists():
            if args.allow_missing:
                continue
            missing.append(elf)
            continue
        rows.append(run_target(qemu, target, elf, args.repeat, args.timeout))

    if missing:
        print("error: missing target ELF files:", file=sys.stderr)
        for path in missing:
            print(f"  {path}", file=sys.stderr)
        return 2

    if not rows:
        print("error: no target ELF files were available to run", file=sys.stderr)
        return 2

    if args.format == "csv":
        print_csv(rows)
    else:
        print_markdown(rows)

    return 1 if any(row.checksum_status != "pass" for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
