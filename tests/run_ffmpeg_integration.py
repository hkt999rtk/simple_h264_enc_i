#!/usr/bin/env python3
import argparse
import subprocess
from pathlib import Path

WIDTH = 2560
HEIGHT = 1440
SMALL_WIDTH = 1280
SMALL_HEIGHT = 720


def run(cmd):
    subprocess.run(cmd, check=True)


def run_capture(cmd):
    return subprocess.run(cmd, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def run_expect_fail(cmd, stderr_contains=None):
    result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode == 0:
        raise RuntimeError(f"expected command to fail: {cmd}")
    if stderr_contains is not None and stderr_contains not in result.stderr:
        raise RuntimeError(f"expected stderr to contain {stderr_contains!r}; got {result.stderr!r}")


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


def make_i420_sized(path, width, height):
    y_size = width * height
    c_size = (width // 2) * (height // 2)
    with path.open("wb") as f:
        for y in range(height):
            f.write(bytes(((x // 6 + y // 5) & 0xFF) for x in range(width)))
        for y in range(height // 2):
            f.write(bytes((72 + (x // 16 + y // 18) % 72) & 0xFF for x in range(width // 2)))
        for y in range(height // 2):
            f.write(bytes((176 + (x // 20 + y // 14) % 56) & 0xFF for x in range(width // 2)))
    assert path.stat().st_size == y_size + 2 * c_size


def make_nv12_sized(path, width, height):
    y_size = width * height
    uv_size = width * (height // 2)
    with path.open("wb") as f:
        for y in range(height):
            f.write(bytes(((x // 7 + y // 4) & 0xFF) for x in range(width)))
        for y in range(height // 2):
            row = bytearray()
            for x in range(width // 2):
                row.append((88 + (x // 12 + y // 15) % 64) & 0xFF)
                row.append((168 + (x // 18 + y // 10) % 64) & 0xFF)
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


def validate_image_pix_fmt(ffprobe, image, expected_pix_fmt):
    probe = subprocess.run(
        [
            ffprobe,
            "-v",
            "error",
            "-show_entries",
            "stream=pix_fmt",
            "-of",
            "default=nw=1:nk=1",
            str(image),
        ],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    got = probe.stdout.strip()
    if got != expected_pix_fmt:
        raise RuntimeError(f"{image} pix_fmt: got {got}, expected {expected_pix_fmt}")


def run_case(args, fmt, make_input):
    yuv = Path(args.workdir) / f"input_{fmt}.yuv"
    bitstream = Path(args.workdir) / f"output_{fmt}.h264"
    make_input(yuv)
    run([args.encoder, "--format", fmt, str(yuv), str(bitstream)])
    validate_bitstream(args.ffprobe, args.ffmpeg, bitstream)


def make_color_jpeg(args, path, pix_fmt):
    run([
        args.ffmpeg,
        "-y",
        "-f",
        "lavfi",
        "-i",
        f"testsrc2=size={SMALL_WIDTH}x{SMALL_HEIGHT}:rate=1",
        "-frames:v",
        "1",
        "-pix_fmt",
        pix_fmt,
        "-q:v",
        "3",
        str(path),
    ])
    validate_image_pix_fmt(args.ffprobe, path, pix_fmt)


def encode_jpeg(args, fmt, jpeg_input, bitstream):
    result = run_capture([args.jpeg_encoder, "--format", fmt, str(jpeg_input), str(bitstream)])
    if "jpeg current allocation bytes: 0" not in result.stdout:
        raise RuntimeError(f"JPEG encoder did not release tracked allocations: {result.stdout!r}")
    if "jpeg work arena bytes:" not in result.stdout:
        raise RuntimeError(f"JPEG encoder did not report work arena size: {result.stdout!r}")
    for line in result.stdout.splitlines():
        if line.startswith("jpeg peak allocation bytes:"):
            if int(line.rsplit(" ", 1)[1]) == 0:
                raise RuntimeError(f"JPEG encoder reported zero peak allocation bytes: {result.stdout!r}")
            return
    raise RuntimeError(f"JPEG encoder did not report peak allocation bytes: {result.stdout!r}")


def encode_jpeg_streaming_prototype(args, jpeg_input, bitstream):
    result = run_capture([
        args.jpeg_encoder,
        "--streaming-prototype",
        "--format",
        "i420",
        str(jpeg_input),
        str(bitstream),
    ])
    if "jpeg current allocation bytes: 0" not in result.stdout:
        raise RuntimeError(f"streaming JPEG prototype leaked tracked allocations: {result.stdout!r}")
    peak = None
    cache = None
    for line in result.stdout.splitlines():
        if line.startswith("jpeg peak allocation bytes:"):
            peak = int(line.rsplit(" ", 1)[1])
        if line.startswith("jpeg streaming cache bytes:"):
            cache = int(line.rsplit(" ", 1)[1])
    if peak is None or peak >= 1382400:
        raise RuntimeError(f"streaming JPEG prototype did not reduce NanoJPEG allocation peak: {result.stdout!r}")
    if cache is None or cache == 0:
        raise RuntimeError(f"streaming JPEG prototype did not report cache bytes: {result.stdout!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--encoder", required=True)
    parser.add_argument("--progressive-encoder", required=True)
    parser.add_argument("--resize-encoder", required=True)
    parser.add_argument("--jpeg-encoder", required=True)
    parser.add_argument("--ffprobe", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--workdir", required=True)
    args = parser.parse_args()

    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    run_case(args, "i420", make_i420)
    run_case(args, "nv12", make_nv12)

    progressive_i420 = workdir / "output_i420_progressive.h264"
    run([args.progressive_encoder, "--format", "i420", str(workdir / "input_i420.yuv"), str(progressive_i420)])
    validate_bitstream(args.ffprobe, args.ffmpeg, progressive_i420)

    progressive_nv12 = workdir / "output_nv12_progressive.h264"
    run([args.progressive_encoder, "--format", "nv12", str(workdir / "input_nv12.yuv"), str(progressive_nv12)])
    validate_bitstream(args.ffprobe, args.ffmpeg, progressive_nv12)

    small_i420 = workdir / "input_i420_720p.yuv"
    make_i420_sized(small_i420, SMALL_WIDTH, SMALL_HEIGHT)
    resized_i420 = workdir / "output_i420_resize_progressive.h264"
    run([
        args.resize_encoder,
        "--format",
        "i420",
        "--src-width",
        str(SMALL_WIDTH),
        "--src-height",
        str(SMALL_HEIGHT),
        str(small_i420),
        str(resized_i420),
    ])
    validate_bitstream(args.ffprobe, args.ffmpeg, resized_i420)

    small_nv12 = workdir / "input_nv12_720p.yuv"
    make_nv12_sized(small_nv12, SMALL_WIDTH, SMALL_HEIGHT)
    resized_nv12 = workdir / "output_nv12_resize_progressive.h264"
    run([
        args.resize_encoder,
        "--format",
        "nv12",
        "--src-width",
        str(SMALL_WIDTH),
        "--src-height",
        str(SMALL_HEIGHT),
        str(small_nv12),
        str(resized_nv12),
    ])
    validate_bitstream(args.ffprobe, args.ffmpeg, resized_nv12)

    run_expect_fail([
        args.resize_encoder,
        "--format",
        "i420",
        "--src-width",
        "1281",
        "--src-height",
        str(SMALL_HEIGHT),
        str(small_i420),
        str(workdir / "invalid_odd_width.h264"),
    ])
    run_expect_fail([
        args.resize_encoder,
        "--format",
        "badfmt",
        "--src-width",
        str(SMALL_WIDTH),
        "--src-height",
        str(SMALL_HEIGHT),
        str(small_i420),
        str(workdir / "invalid_format.h264"),
    ])

    for pix_fmt in ("yuvj420p", "yuvj422p", "yuvj444p"):
        jpeg_input = workdir / f"input_720p_{pix_fmt}.jpg"
        make_color_jpeg(args, jpeg_input, pix_fmt)
        if pix_fmt == "yuvj420p":
            run_expect_fail([
                args.jpeg_encoder,
                "--test-arena-shrink",
                "1",
                "--format",
                "i420",
                str(jpeg_input),
                str(workdir / "output_jpeg_arena_too_small.h264"),
            ], stderr_contains="buffer too small")
            streaming_output = workdir / "output_jpeg_streaming_yuvj420p_i420.h264"
            encode_jpeg_streaming_prototype(args, jpeg_input, streaming_output)
            validate_bitstream(args.ffprobe, args.ffmpeg, streaming_output)
        for fmt in ("i420", "nv12"):
            jpeg_output = workdir / f"output_jpeg_{pix_fmt}_{fmt}.h264"
            encode_jpeg(args, fmt, jpeg_input, jpeg_output)
            validate_bitstream(args.ffprobe, args.ffmpeg, jpeg_output)

    grayscale_jpeg_input = workdir / "input_gray_720p.jpg"
    run([
        args.ffmpeg,
        "-y",
        "-f",
        "lavfi",
        "-i",
        f"testsrc2=size={SMALL_WIDTH}x{SMALL_HEIGHT}:rate=1",
        "-frames:v",
        "1",
        "-vf",
        "format=gray",
        "-pix_fmt",
        "gray",
        "-q:v",
        "3",
        str(grayscale_jpeg_input),
    ])
    # Some FFmpeg MJPEG builds encode gray sources as yuvj444p, so only the
    # color subsampling fixtures assert exact JPEG component layout.
    for fmt in ("i420", "nv12"):
        grayscale_jpeg_output = workdir / f"output_jpeg_gray_{fmt}.h264"
        encode_jpeg(args, fmt, grayscale_jpeg_input, grayscale_jpeg_output)
        validate_bitstream(args.ffprobe, args.ffmpeg, grayscale_jpeg_output)


if __name__ == "__main__":
    main()
