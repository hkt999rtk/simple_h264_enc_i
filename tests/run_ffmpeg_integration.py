#!/usr/bin/env python3
import argparse
import hashlib
import subprocess
from pathlib import Path

WIDTH = 2560
HEIGHT = 1440
SMALL_WIDTH = 1280
SMALL_HEIGHT = 720
LARGE_WIDTH = 2560
LARGE_HEIGHT = 1440
JPEG_SLICE_WORK_BYTES = WIDTH * 16 + (WIDTH // 2) * 8 * 2
JPEG_DIRECT_I420_SLICE_WORK_BYTES = 0
JPEG_DIRECT_NV12_SLICE_WORK_BYTES = WIDTH * 8
JPEG_DIRECT_I420_EFFECTIVE_SLICE_WORK_BYTES = 0
JPEG_DIRECT_NV12_EFFECTIVE_SLICE_WORK_BYTES = WIDTH * 8
STREAMING_CACHE_BYTES_720P = {
    "yuvj420p": 61_440,
    "yuvj422p": 81_920,
    "yuvj444p": 122_880,
}
STREAMING_CACHE_BYTES_1440P_420 = 61_440
FULL_COMPONENT_BYTES_720P = {
    "yuvj420p": 1_382_400,
    "yuvj422p": 1_843_200,
    "yuvj444p": 2_764_800,
}
FULL_COMPONENT_BYTES_1440P_420 = 5_529_600
PRODUCTION_MEMORY_720P_420 = {
    "work": 92_304,
    "peak": 92_160,
    "cache": STREAMING_CACHE_BYTES_720P["yuvj420p"],
}
PRODUCTION_MEMORY_1440P_420 = {
    "work": 123_024,
    "peak": 122_880,
    "cache": STREAMING_CACHE_BYTES_1440P_420,
}
H264_OUTPUT_CONSUMER_CHUNKS_MIN = 2 + (WIDTH // 16) * (HEIGHT // 16)
H264_OUTPUT_CHUNK_BYTES = 4_096
H264_TINY_OUTPUT_CHUNK_BYTES = 7
H264_ONE_SHOT_OUTPUT_BYTES = 44_295_424
JPEG_SOURCE_CHUNK_SIZES = (1, 2, 7, 64, 1024)
ENCODER_CONTEXT_BYTES = 72
ENCODER_BITSTREAM_SCRATCH_BYTES = 2_048
ENCODER_RECON_LUMA_BYTES = 0
ENCODER_RECON_CHROMA_BYTES = 0
ENCODER_NEIGHBOR_STATE_BYTES = 0
ENCODER_MEMORY_TOTAL_BYTES = 2_120
ENCODER_ARENA_WORK_BYTES = 2_127
EMBEDDED_OUTPUT_BUFFER_TOTAL_1440P_420 = (
    PRODUCTION_MEMORY_1440P_420["work"]
    + JPEG_DIRECT_I420_SLICE_WORK_BYTES
    + H264_OUTPUT_CHUNK_BYTES
)


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
    run([ffmpeg, "-v", "error", "-xerror", "-i", str(bitstream), "-f", "null", "-"])


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


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def decode_i420_frame(args, bitstream, raw_output):
    run([
        args.ffmpeg,
        "-y",
        "-v",
        "error",
        "-xerror",
        "-i",
        str(bitstream),
        "-f",
        "rawvideo",
        "-pix_fmt",
        "yuv420p",
        str(raw_output),
    ])
    expected_size = WIDTH * HEIGHT * 3 // 2
    got_size = raw_output.stat().st_size
    if got_size != expected_size:
        raise RuntimeError(
            f"decoded frame size mismatch for {bitstream}: got {got_size}, expected {expected_size}"
        )
    return sha256_file(raw_output)


def compare_decoded_i420(args, reference_bitstream, candidate_bitstream, stem):
    workdir = Path(args.workdir)
    reference_raw = workdir / f"{stem}_reference.yuv"
    candidate_raw = workdir / f"{stem}_candidate.yuv"
    reference_hash = decode_i420_frame(args, reference_bitstream, reference_raw)
    candidate_hash = decode_i420_frame(args, candidate_bitstream, candidate_raw)
    if reference_hash != candidate_hash:
        raise RuntimeError(
            "decoded frame mismatch between component-plane and streaming JPEG paths: "
            f"{reference_hash} != {candidate_hash}"
        )


def run_case(args, fmt, make_input):
    yuv = Path(args.workdir) / f"input_{fmt}.yuv"
    bitstream = Path(args.workdir) / f"output_{fmt}.h264"
    make_input(yuv)
    run([args.encoder, "--format", fmt, str(yuv), str(bitstream)])
    validate_bitstream(args.ffprobe, args.ffmpeg, bitstream)


def make_color_jpeg_sized(args, path, pix_fmt, width, height):
    run([
        args.ffmpeg,
        "-y",
        "-f",
        "lavfi",
        "-i",
        f"testsrc2=size={width}x{height}:rate=1",
        "-frames:v",
        "1",
        "-pix_fmt",
        pix_fmt,
        "-q:v",
        "3",
        str(path),
    ])
    validate_image_pix_fmt(args.ffprobe, path, pix_fmt)


def make_color_jpeg(args, path, pix_fmt):
    make_color_jpeg_sized(args, path, pix_fmt, SMALL_WIDTH, SMALL_HEIGHT)


def make_restart_marker_jpeg(args, path):
    ppm = path.with_suffix(".ppm")
    with ppm.open("wb") as f:
        f.write(f"P6\n{SMALL_WIDTH} {SMALL_HEIGHT}\n255\n".encode("ascii"))
        for y in range(SMALL_HEIGHT):
            row = bytearray()
            for x in range(SMALL_WIDTH):
                row.extend((
                    (x // 5 + y // 3) & 0xFF,
                    (64 + x // 7) & 0xFF,
                    (192 + y // 5) & 0xFF,
                ))
            f.write(row)
    run([
        args.cjpeg,
        "-baseline",
        "-quality",
        "85",
        "-sample",
        "2x2,1x1,1x1",
        "-restart",
        "1",
        "-outfile",
        str(path),
        str(ppm),
    ])
    validate_image_pix_fmt(args.ffprobe, path, "yuvj420p")
    validate_jpeg_restart_markers(path)


def validate_jpeg_restart_markers(path):
    data = path.read_bytes()
    if b"\xff\xdd" not in data:
        raise RuntimeError(f"{path} is missing a JPEG DRI marker")
    restart_count = 0
    for i in range(len(data) - 1):
        if data[i] == 0xFF and 0xD0 <= data[i + 1] <= 0xD7:
            restart_count += 1
    if restart_count == 0:
        raise RuntimeError(f"{path} is missing JPEG RST markers")


def parse_jpeg_metric(stdout, prefix):
    for line in stdout.splitlines():
        if line.startswith(prefix):
            return int(line.rsplit(" ", 1)[1])
    raise RuntimeError(f"JPEG encoder did not report {prefix!r}: {stdout!r}")


def validate_encoder_memory_report(stdout):
    report = {
        "context": parse_jpeg_metric(stdout, "encoder context bytes:"),
        "bitstream": parse_jpeg_metric(stdout, "encoder bitstream scratch bytes:"),
        "luma": parse_jpeg_metric(stdout, "encoder recon luma bytes:"),
        "chroma": parse_jpeg_metric(stdout, "encoder recon chroma bytes:"),
        "neighbor": parse_jpeg_metric(stdout, "encoder neighbor state bytes:"),
        "total": parse_jpeg_metric(stdout, "encoder memory total bytes:"),
    }
    expected = {
        "context": ENCODER_CONTEXT_BYTES,
        "bitstream": ENCODER_BITSTREAM_SCRATCH_BYTES,
        "luma": ENCODER_RECON_LUMA_BYTES,
        "chroma": ENCODER_RECON_CHROMA_BYTES,
        "neighbor": ENCODER_NEIGHBOR_STATE_BYTES,
        "total": ENCODER_MEMORY_TOTAL_BYTES,
    }
    if report != expected:
        raise RuntimeError(f"encoder memory report changed: got {report}, expected {expected}")
    subtotal = (
        report["context"]
        + report["bitstream"]
        + report["luma"]
        + report["chroma"]
        + report["neighbor"]
    )
    if report["total"] != subtotal:
        raise RuntimeError(f"encoder memory total {report['total']} != sub-block sum {subtotal}")
    arena_work = parse_jpeg_metric(stdout, "encoder arena work bytes:")
    if arena_work != ENCODER_ARENA_WORK_BYTES:
        raise RuntimeError(
            f"encoder arena work size changed: got {arena_work}, expected {ENCODER_ARENA_WORK_BYTES}"
        )


def encode_jpeg(args, fmt, jpeg_input, bitstream, extra_args=None,
                expected_cache_bytes=None, max_peak_bytes=None,
                expected_work_bytes=None, expected_peak_bytes=None,
                expected_slice_work_bytes=None,
                expected_effective_slice_work_bytes=None):
    command = [args.jpeg_encoder]
    if extra_args:
        command.extend(extra_args)
    command.extend(["--format", fmt, str(jpeg_input), str(bitstream)])
    result = run_capture(command)
    one_shot_output = extra_args is not None and "--test-one-shot-output" in extra_args
    heap_wrapper_output = extra_args is not None and "--test-allocation-limit" in extra_args
    one_shot_h264_output = one_shot_output or heap_wrapper_output
    if "jpeg current allocation bytes: 0" not in result.stdout:
        raise RuntimeError(f"JPEG encoder did not release tracked allocations: {result.stdout!r}")
    validate_encoder_memory_report(result.stdout)
    if "jpeg work arena bytes:" not in result.stdout:
        raise RuntimeError(f"JPEG encoder did not report work arena size: {result.stdout!r}")
    work = parse_jpeg_metric(result.stdout, "jpeg work arena bytes:")
    if expected_work_bytes is not None and work != expected_work_bytes:
        raise RuntimeError(
            f"JPEG encoder default work arena changed for {jpeg_input}: got {work}, expected {expected_work_bytes}"
        )
    slice_work = parse_jpeg_metric(result.stdout, "jpeg slice work bytes:")
    if expected_slice_work_bytes is None:
        expected_slice_work_bytes = JPEG_SLICE_WORK_BYTES
    if slice_work != expected_slice_work_bytes:
        raise RuntimeError(
            f"JPEG encoder slice work changed: got {slice_work}, expected {expected_slice_work_bytes}"
        )
    effective_slice_work = parse_jpeg_metric(result.stdout, "jpeg effective slice work bytes:")
    if expected_effective_slice_work_bytes is None:
        expected_effective_slice_work_bytes = expected_slice_work_bytes
    if effective_slice_work != expected_effective_slice_work_bytes:
        raise RuntimeError(
            "JPEG encoder effective slice work changed: "
            f"got {effective_slice_work}, expected {expected_effective_slice_work_bytes}"
        )
    peak = parse_jpeg_metric(result.stdout, "jpeg peak allocation bytes:")
    if peak == 0:
        raise RuntimeError(f"JPEG encoder reported zero peak allocation bytes: {result.stdout!r}")
    if expected_peak_bytes is not None and peak != expected_peak_bytes:
        raise RuntimeError(
            f"JPEG encoder default peak changed for {jpeg_input}: got {peak}, expected {expected_peak_bytes}"
        )
    cache = parse_jpeg_metric(result.stdout, "jpeg streaming cache bytes:")
    if cache == 0:
        raise RuntimeError(f"JPEG encoder default path did not report streaming cache bytes: {result.stdout!r}")
    if expected_cache_bytes is not None and cache != expected_cache_bytes:
        raise RuntimeError(
            f"JPEG encoder default row cache changed for {jpeg_input}: got {cache}, expected {expected_cache_bytes}"
        )
    if max_peak_bytes is not None and peak >= max_peak_bytes:
        raise RuntimeError(
            f"JPEG encoder default path used full component-plane memory: got peak {peak}, limit {max_peak_bytes}"
        )
    if not one_shot_h264_output:
        output_buffer = parse_jpeg_metric(result.stdout, "jpeg output buffer bytes:")
        if output_buffer != H264_OUTPUT_CHUNK_BYTES:
            raise RuntimeError(
                f"JPEG default output buffer changed: got {output_buffer}, expected {H264_OUTPUT_CHUNK_BYTES}"
            )
        if output_buffer >= H264_ONE_SHOT_OUTPUT_BYTES:
            raise RuntimeError(
                f"JPEG default path regressed to one-shot output buffering: got {output_buffer}"
            )
        chunks = parse_jpeg_metric(result.stdout, "jpeg output consumer chunks:")
        if chunks < H264_OUTPUT_CONSUMER_CHUNKS_MIN:
            raise RuntimeError(
                f"JPEG output consumer chunk count changed: got {chunks}, expected at least {H264_OUTPUT_CONSUMER_CHUNKS_MIN}"
            )
        chunk_bytes = parse_jpeg_metric(result.stdout, "jpeg output chunk buffer bytes:")
        if chunk_bytes != H264_OUTPUT_CHUNK_BYTES:
            raise RuntimeError(
                f"JPEG output consumer chunk capacity changed: got {chunk_bytes}, expected {H264_OUTPUT_CHUNK_BYTES}"
            )
    else:
        output_buffer = parse_jpeg_metric(result.stdout, "jpeg output buffer bytes:")
        if output_buffer != H264_ONE_SHOT_OUTPUT_BYTES:
            raise RuntimeError(
                f"JPEG one-shot H.264 output capacity changed: got {output_buffer}, expected {H264_ONE_SHOT_OUTPUT_BYTES}"
            )
    return result.stdout


def encode_jpeg_source_chunks(args, fmt, jpeg_input, bitstream, chunk_size,
                              expected_cache_bytes=None,
                              expected_work_bytes=None,
                              expected_peak_bytes=None,
                              expected_slice_work_bytes=None,
                              expected_effective_slice_work_bytes=None):
    stdout = encode_jpeg(
        args,
        fmt,
        jpeg_input,
        bitstream,
        ["--test-jpeg-source-chunk-size", str(chunk_size)],
        expected_cache_bytes=expected_cache_bytes,
        expected_work_bytes=expected_work_bytes,
        expected_peak_bytes=expected_peak_bytes,
        expected_slice_work_bytes=expected_slice_work_bytes,
        expected_effective_slice_work_bytes=expected_effective_slice_work_bytes,
    )
    reported_chunk_size = parse_jpeg_metric(stdout, "jpeg source chunk bytes:")
    if reported_chunk_size != chunk_size:
        raise RuntimeError(
            f"JPEG source chunk size changed: got {reported_chunk_size}, expected {chunk_size}"
        )


def encode_jpeg_streaming_prototype(args, fmt, jpeg_input, bitstream,
                                    expected_cache_bytes=None,
                                    expected_slice_work_bytes=None):
    result = run_capture([
        args.jpeg_encoder,
        "--streaming-prototype",
        "--format",
        fmt,
        str(jpeg_input),
        str(bitstream),
    ])
    if "jpeg current allocation bytes: 0" not in result.stdout:
        raise RuntimeError(f"streaming JPEG prototype leaked tracked allocations: {result.stdout!r}")
    work = parse_jpeg_metric(result.stdout, "jpeg work arena bytes:")
    if work == 0:
        raise RuntimeError(f"streaming JPEG prototype did not report arena work size: {result.stdout!r}")
    slice_work = parse_jpeg_metric(result.stdout, "jpeg slice work bytes:")
    if expected_slice_work_bytes is None:
        expected_slice_work_bytes = JPEG_SLICE_WORK_BYTES
    if slice_work != expected_slice_work_bytes:
        raise RuntimeError(
            f"streaming JPEG prototype slice work changed: got {slice_work}, expected {expected_slice_work_bytes}"
        )
    peak = parse_jpeg_metric(result.stdout, "jpeg peak allocation bytes:")
    if peak >= 1382400:
        raise RuntimeError(f"streaming JPEG prototype did not reduce NanoJPEG allocation peak: {result.stdout!r}")
    cache = parse_jpeg_metric(result.stdout, "jpeg streaming cache bytes:")
    if cache == 0:
        raise RuntimeError(f"streaming JPEG prototype did not report cache bytes: {result.stdout!r}")
    if expected_cache_bytes is not None and cache != expected_cache_bytes:
        raise RuntimeError(
            f"streaming JPEG row cache changed for {jpeg_input}: got {cache}, expected {expected_cache_bytes}"
        )


def encode_jpeg_output_consumer(args, fmt, jpeg_input, bitstream, extra_args=None,
                                expected_chunk_bytes=H264_OUTPUT_CHUNK_BYTES):
    command = [
        args.jpeg_encoder,
        "--test-output-consumer",
    ]
    if extra_args:
        command.extend(extra_args)
    command.extend([
        "--format",
        fmt,
        str(jpeg_input),
        str(bitstream),
    ])
    result = run_capture(command)
    chunks = parse_jpeg_metric(result.stdout, "jpeg output consumer chunks:")
    if chunks < H264_OUTPUT_CONSUMER_CHUNKS_MIN:
        raise RuntimeError(
            f"JPEG output consumer chunk count changed: got {chunks}, expected at least {H264_OUTPUT_CONSUMER_CHUNKS_MIN}"
        )
    chunk_bytes = parse_jpeg_metric(result.stdout, "jpeg output chunk buffer bytes:")
    if chunk_bytes != expected_chunk_bytes:
        raise RuntimeError(
            f"JPEG output consumer chunk capacity changed: got {chunk_bytes}, expected {expected_chunk_bytes}"
        )
    if chunk_bytes >= H264_ONE_SHOT_OUTPUT_BYTES:
        raise RuntimeError(
            f"JPEG output consumer regressed to one-shot output buffering: got {chunk_bytes}"
        )
    if "jpeg current allocation bytes: 0" not in result.stdout:
        raise RuntimeError(f"JPEG output consumer leaked tracked allocations: {result.stdout!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--encoder", required=True)
    parser.add_argument("--progressive-encoder", required=True)
    parser.add_argument("--resize-encoder", required=True)
    parser.add_argument("--jpeg-encoder", required=True)
    parser.add_argument("--ffprobe", required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--cjpeg")
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
            run_expect_fail([
                args.jpeg_encoder,
                "--streaming-prototype",
                "--test-arena-shrink",
                "1",
                "--format",
                "i420",
                str(jpeg_input),
                str(workdir / "output_jpeg_streaming_arena_too_small.h264"),
            ], stderr_contains="buffer too small")
            offset_arena_output = workdir / "output_jpeg_arena_offset_i420.h264"
            encode_jpeg(args, "i420", jpeg_input, offset_arena_output,
                        ["--test-arena-offset", "1"],
                        STREAMING_CACHE_BYTES_720P[pix_fmt],
                        FULL_COMPONENT_BYTES_720P[pix_fmt],
                        PRODUCTION_MEMORY_720P_420["work"],
                        PRODUCTION_MEMORY_720P_420["peak"])
            validate_bitstream(args.ffprobe, args.ffmpeg, offset_arena_output)
        for fmt in ("i420", "nv12"):
            jpeg_output = workdir / f"output_jpeg_{pix_fmt}_{fmt}.h264"
            streaming_output = workdir / f"output_jpeg_streaming_{pix_fmt}_{fmt}.h264"
            expected_work = None
            expected_peak = None
            if pix_fmt == "yuvj420p":
                expected_work = PRODUCTION_MEMORY_720P_420["work"]
                expected_peak = PRODUCTION_MEMORY_720P_420["peak"]
            encode_jpeg(args, fmt, jpeg_input, jpeg_output,
                        expected_cache_bytes=STREAMING_CACHE_BYTES_720P[pix_fmt],
                        max_peak_bytes=FULL_COMPONENT_BYTES_720P[pix_fmt],
                        expected_work_bytes=expected_work,
                        expected_peak_bytes=expected_peak)
            if pix_fmt == "yuvj420p" and fmt == "i420":
                one_shot_output = workdir / "output_jpeg_one_shot_yuvj420p_i420.h264"
                encode_jpeg(args, fmt, jpeg_input, one_shot_output,
                            ["--test-one-shot-output"],
                            STREAMING_CACHE_BYTES_720P[pix_fmt],
                            FULL_COMPONENT_BYTES_720P[pix_fmt],
                            PRODUCTION_MEMORY_720P_420["work"],
                            PRODUCTION_MEMORY_720P_420["peak"])
                validate_bitstream(args.ffprobe, args.ffmpeg, one_shot_output)
                if jpeg_output.read_bytes() != one_shot_output.read_bytes():
                    raise RuntimeError("JPEG default output consumer bitstream differs from one-shot output")
                consumer_output = workdir / "output_jpeg_consumer_yuvj420p_i420.h264"
                encode_jpeg_output_consumer(args, fmt, jpeg_input, consumer_output)
                validate_bitstream(args.ffprobe, args.ffmpeg, consumer_output)
                if consumer_output.read_bytes() != one_shot_output.read_bytes():
                    raise RuntimeError("JPEG memory consumer bitstream differs from one-shot output")
                tiny_consumer_output = workdir / "output_jpeg_consumer_tiny_yuvj420p_i420.h264"
                encode_jpeg_output_consumer(
                    args,
                    fmt,
                    jpeg_input,
                    tiny_consumer_output,
                    ["--test-output-chunk-size", str(H264_TINY_OUTPUT_CHUNK_BYTES)],
                    H264_TINY_OUTPUT_CHUNK_BYTES,
                )
                validate_bitstream(args.ffprobe, args.ffmpeg, tiny_consumer_output)
                if tiny_consumer_output.read_bytes() != one_shot_output.read_bytes():
                    raise RuntimeError("JPEG tiny-chunk consumer bitstream differs from one-shot output")
                for source_chunk_size in JPEG_SOURCE_CHUNK_SIZES:
                    source_output = workdir / (
                        f"output_jpeg_source_chunk_{source_chunk_size}_yuvj420p_i420.h264"
                    )
                    encode_jpeg_source_chunks(
                        args,
                        fmt,
                        jpeg_input,
                        source_output,
                        source_chunk_size,
                        STREAMING_CACHE_BYTES_720P[pix_fmt],
                        PRODUCTION_MEMORY_720P_420["work"],
                        PRODUCTION_MEMORY_720P_420["peak"],
                    )
                    validate_bitstream(args.ffprobe, args.ffmpeg, source_output)
                    if source_output.read_bytes() != one_shot_output.read_bytes():
                        raise RuntimeError(
                            f"JPEG source chunk {source_chunk_size} bitstream differs from one-shot output"
                        )
                run_expect_fail([
                    args.jpeg_encoder,
                    "--test-output-consumer-fail-after",
                    "1",
                    "--format",
                    fmt,
                    str(jpeg_input),
                    str(workdir / "output_jpeg_consumer_fail.h264"),
                ], stderr_contains="internal error")
            encode_jpeg_streaming_prototype(args, fmt, jpeg_input, streaming_output,
                                            STREAMING_CACHE_BYTES_720P[pix_fmt])
            validate_bitstream(args.ffprobe, args.ffmpeg, jpeg_output)
            validate_bitstream(args.ffprobe, args.ffmpeg, streaming_output)
            compare_decoded_i420(args, jpeg_output, streaming_output,
                                 f"output_jpeg_{pix_fmt}_{fmt}_streaming_compare")

    large_jpeg_input = workdir / "input_1440p_yuvj420p.jpg"
    large_jpeg_output = workdir / "output_jpeg_1440p_yuvj420p_i420.h264"
    large_streaming_output = workdir / "output_jpeg_streaming_1440p_yuvj420p_i420.h264"
    make_color_jpeg_sized(args, large_jpeg_input, "yuvj420p", LARGE_WIDTH, LARGE_HEIGHT)
    encode_jpeg(args, "i420", large_jpeg_input, large_jpeg_output,
                expected_cache_bytes=STREAMING_CACHE_BYTES_1440P_420,
                max_peak_bytes=FULL_COMPONENT_BYTES_1440P_420,
                expected_work_bytes=PRODUCTION_MEMORY_1440P_420["work"],
                expected_peak_bytes=PRODUCTION_MEMORY_1440P_420["peak"],
                expected_slice_work_bytes=JPEG_DIRECT_I420_SLICE_WORK_BYTES,
                expected_effective_slice_work_bytes=JPEG_DIRECT_I420_EFFECTIVE_SLICE_WORK_BYTES)
    large_jpeg_output_nv12 = workdir / "output_jpeg_1440p_yuvj420p_nv12.h264"
    encode_jpeg(args, "nv12", large_jpeg_input, large_jpeg_output_nv12,
                expected_cache_bytes=STREAMING_CACHE_BYTES_1440P_420,
                max_peak_bytes=FULL_COMPONENT_BYTES_1440P_420,
                expected_work_bytes=PRODUCTION_MEMORY_1440P_420["work"],
                expected_peak_bytes=PRODUCTION_MEMORY_1440P_420["peak"],
                expected_slice_work_bytes=JPEG_DIRECT_NV12_SLICE_WORK_BYTES,
                expected_effective_slice_work_bytes=JPEG_DIRECT_NV12_EFFECTIVE_SLICE_WORK_BYTES)
    validate_bitstream(args.ffprobe, args.ffmpeg, large_jpeg_output_nv12)
    large_heap_wrapper_output = workdir / "output_jpeg_heap_wrapper_1440p_yuvj420p_i420.h264"
    encode_jpeg(args, "i420", large_jpeg_input, large_heap_wrapper_output,
                ["--test-allocation-limit", "999999999"],
                expected_cache_bytes=STREAMING_CACHE_BYTES_1440P_420,
                max_peak_bytes=FULL_COMPONENT_BYTES_1440P_420,
                expected_peak_bytes=PRODUCTION_MEMORY_1440P_420["peak"],
                expected_slice_work_bytes=JPEG_DIRECT_I420_SLICE_WORK_BYTES,
                expected_effective_slice_work_bytes=JPEG_DIRECT_I420_EFFECTIVE_SLICE_WORK_BYTES)
    validate_bitstream(args.ffprobe, args.ffmpeg, large_heap_wrapper_output)
    if large_heap_wrapper_output.read_bytes() != large_jpeg_output.read_bytes():
        raise RuntimeError("JPEG heap wrapper bitstream differs from arena streaming output")
    if EMBEDDED_OUTPUT_BUFFER_TOTAL_1440P_420 != 127_120:
        raise RuntimeError(
            "2560x1440 JPEG streaming memory subtotal changed: "
            f"got {EMBEDDED_OUTPUT_BUFFER_TOTAL_1440P_420}, expected 127120"
        )
    encode_jpeg_streaming_prototype(args, "i420", large_jpeg_input, large_streaming_output,
                                    STREAMING_CACHE_BYTES_1440P_420,
                                    JPEG_DIRECT_I420_SLICE_WORK_BYTES)
    validate_bitstream(args.ffprobe, args.ffmpeg, large_streaming_output)
    compare_decoded_i420(args, large_jpeg_output, large_streaming_output,
                         "output_jpeg_1440p_yuvj420p_i420_streaming_compare")

    if args.cjpeg:
        restart_jpeg_input = workdir / "input_720p_yuvj420p_restart.jpg"
        make_restart_marker_jpeg(args, restart_jpeg_input)
        restart_streaming_output = workdir / "output_jpeg_streaming_yuvj420p_restart_i420.h264"
        restart_jpeg_output = workdir / "output_jpeg_yuvj420p_restart_i420.h264"
        encode_jpeg_streaming_prototype(args, "i420", restart_jpeg_input, restart_streaming_output,
                                        STREAMING_CACHE_BYTES_720P["yuvj420p"])
        validate_bitstream(args.ffprobe, args.ffmpeg, restart_streaming_output)
        encode_jpeg(args, "i420", restart_jpeg_input, restart_jpeg_output,
                    expected_cache_bytes=STREAMING_CACHE_BYTES_720P["yuvj420p"],
                    max_peak_bytes=FULL_COMPONENT_BYTES_720P["yuvj420p"])
        validate_bitstream(args.ffprobe, args.ffmpeg, restart_jpeg_output)
        compare_decoded_i420(args, restart_jpeg_output, restart_streaming_output,
                             "output_jpeg_yuvj420p_restart_i420_streaming_compare")
        restart_source_output = workdir / "output_jpeg_source_chunk_7_yuvj420p_restart_i420.h264"
        encode_jpeg_source_chunks(
            args,
            "i420",
            restart_jpeg_input,
            restart_source_output,
            7,
            STREAMING_CACHE_BYTES_720P["yuvj420p"],
        )
        validate_bitstream(args.ffprobe, args.ffmpeg, restart_source_output)
        if restart_source_output.read_bytes() != restart_jpeg_output.read_bytes():
            raise RuntimeError("JPEG source restart-marker bitstream differs from memory input")

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
        grayscale_streaming_output = workdir / f"output_jpeg_streaming_gray_{fmt}.h264"
        encode_jpeg(args, fmt, grayscale_jpeg_input, grayscale_jpeg_output)
        encode_jpeg_streaming_prototype(args, fmt, grayscale_jpeg_input, grayscale_streaming_output)
        validate_bitstream(args.ffprobe, args.ffmpeg, grayscale_jpeg_output)
        validate_bitstream(args.ffprobe, args.ffmpeg, grayscale_streaming_output)
        compare_decoded_i420(args, grayscale_jpeg_output, grayscale_streaming_output,
                             f"output_jpeg_gray_{fmt}_streaming_compare")


if __name__ == "__main__":
    main()
