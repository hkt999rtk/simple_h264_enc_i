#!/usr/bin/env python3
import argparse
import subprocess
from pathlib import Path

WIDTH = 2560
HEIGHT = 1440


def run(cmd):
    subprocess.run(cmd, check=True)


def make_i420(path):
    y_size = WIDTH * HEIGHT
    c_size = (WIDTH // 2) * (HEIGHT // 2)
    with path.open("wb") as f:
        for y in range(HEIGHT):
            f.write(bytes(((x // 10 + y // 8) & 0xFF) for x in range(WIDTH)))
        for _ in range(HEIGHT // 2):
            f.write(bytes((80 + (x // 32) % 64) & 0xFF for x in range(WIDTH // 2)))
        for y in range(HEIGHT // 2):
            f.write(bytes((180 + (y // 24) % 48) & 0xFF for _ in range(WIDTH // 2)))
    assert path.stat().st_size == y_size + 2 * c_size


def make_nv12(path):
    y_size = WIDTH * HEIGHT
    uv_size = WIDTH * (HEIGHT // 2)
    with path.open("wb") as f:
        for y in range(HEIGHT):
            f.write(bytes(((x // 16 + y // 12) & 0xFF) for x in range(WIDTH)))
        for y in range(HEIGHT // 2):
            row = bytearray()
            for x in range(WIDTH // 2):
                row.append((96 + (x // 24) % 32) & 0xFF)
                row.append((176 + (y // 20) % 48) & 0xFF)
            f.write(row)
    assert path.stat().st_size == y_size + uv_size


def validate_bitstream(ffprobe, ffmpeg, bitstream):
    probe = subprocess.run(
        [
            ffprobe,
            "-v",
            "error",
            "-show_entries",
            "stream=codec_name,profile,width,height,pix_fmt",
            "-of",
            "default=nw=1",
            str(bitstream),
        ],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    expected = {
        "codec_name=h264",
        "profile=Constrained Baseline",
        "width=2560",
        "height=1440",
        "pix_fmt=yuv420p",
    }
    got = set(line.strip() for line in probe.stdout.splitlines() if line.strip())
    missing = expected - got
    if missing:
        raise RuntimeError(f"ffprobe output missing {sorted(missing)}; got {sorted(got)}")

    run([ffmpeg, "-v", "error", "-i", str(bitstream), "-f", "null", "-"])


def run_case(args, fmt, make_input):
    yuv = Path(args.workdir) / f"input_{fmt}.yuv"
    bitstream = Path(args.workdir) / f"output_{fmt}.h264"
    make_input(yuv)
    run([args.encoder, "--format", fmt, str(yuv), str(bitstream)])
    validate_bitstream(args.ffprobe, args.ffmpeg, bitstream)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--encoder", required=True)
    parser.add_argument("--ffprobe", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--workdir", required=True)
    args = parser.parse_args()

    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    run_case(args, "i420", make_i420)
    run_case(args, "nv12", make_nv12)


if __name__ == "__main__":
    main()
