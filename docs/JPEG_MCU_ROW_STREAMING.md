# JPEG MCU-Row Streaming Design

Issue #5 tracks the v2 path for removing the full decoded JPEG component
planes from the JPEG-to-H.264 pipeline. The current decoder is intentionally
simple: `njDecodeScan` decodes every MCU, `njDecodeBlock` writes each 8x8 IDCT
block into `nj.comp[i].pixels`, and the scaler later reads those full component
planes. This document defines the first safe streaming shape before changing
that storage model.

## Current Decode Boundary

The current component-plane path has these properties:

* `njDecodeSOF` computes `mbsizex`, `mbsizey`, component dimensions, and full
  component strides.
* `njDecodeSOF` allocates `c->pixels` for every component using
  `c->stride * nj.mbheight * c->ssy * 8`.
* `njDecodeScan` walks MCUs in raster order and writes decoded blocks directly
  into those full component planes.
* `sh264e_encode_jpeg_idr` scales the decoded components one H.264 output slice
  at a time, where one output slice is 16 luma rows and 8 chroma rows.

The streaming design keeps the existing marker, Huffman, quantization, restart,
and IDCT logic but replaces full component-plane storage with a bounded row
ring that only keeps source rows needed by the scaler.

## Row Cache Lifetime

Streaming should process one JPEG MCU row at a time:

```text
parse headers
allocate component row rings
for each JPEG MCU row:
    decode MCU row into component rings
    while enough source rows exist for the next H.264 output slice:
        scale from component rings into slice work buffer
        encode the H.264 slice
        release source rows no longer needed by later output slices
flush any remaining bottom-edge slices
```

Bilinear scaling needs at most two source rows for one output row per component.
The row ring therefore needs to retain the source rows spanning the next output
slice plus one extra source row for interpolation. Because JPEG emits whole MCU
rows, the retained height is rounded up to each component's MCU-row granularity.

For an output luma slice of 16 rows:

```text
needed_luma_source_rows =
    ceil(16 * source_height / 1440) + 1

cached_luma_rows =
    round_up_to_mcu_rows(needed_luma_source_rows + bottom_edge_guard)
```

Chroma uses the same rule against the destination chroma slice height of 8 rows
and the decoded component's native height. The bottom-edge guard lets the scaler
clamp cleanly when the final source row for an interpolation pair is outside the
image.

## First Prototype Slice

The first implementation slice should be deliberately narrow:

* Baseline sequential JPEG only, preserving the current NanoJPEG scope.
* Grayscale and three-component YCbCr only.
* Power-of-two sampling factors only, matching current NanoJPEG validation.
* Interleaved scans only for the first prototype.
* Source dimensions in the existing supported range:
  `1280x720 <= source <= 5120x2880`, even width and height.
* H.264 output remains the existing 2560x1440 progressive IDR frame.

Unsupported structures should fail before partial output begins. The first
prototype can reject progressive/lossless JPEG, arithmetic coding, CMYK/other
color spaces, non-power-of-two sampling, and non-interleaved multi-scan JPEGs.
Restart markers must continue to reset DC predictors and bitstream state at the
same MCU intervals as the current full-plane decode path.

## MCU-Row Decoder Contract

The internal `njDecodeMcuRows` contract is intentionally narrower than the full
component-plane decoder:

* Supported inputs are baseline sequential JPEGs with either one grayscale
  component or three interleaved YCbCr components.
* Component sampling factors must be nonzero powers of two, matching the
  existing NanoJPEG component-plane validation.
* The decoder emits callbacks in raster MCU-row order after a complete MCU row
  has been decoded into the temporary component row buffers.
* Restart markers keep the existing NanoJPEG behavior: bitstream state is byte
  aligned, marker order is checked, and component DC predictors are reset at the
  configured interval.
* Unsupported SOF markers, arithmetic/Huffman table modes outside the baseline
  scope, non-interleaved scan layouts, CMYK/other component counts, and
  malformed marker lengths return deterministic NanoJPEG error codes before a
  production caller writes output.
* A nonzero callback return stops decoding with `NJ_CALLBACK_ABORT`; integration
  code maps its own callback-side status separately so callers receive the
  original library status when slice encoding or arena checks fail.

## Proposed Internal Shape

Add an internal decode mode that owns the JPEG bitstream state and emits decoded
MCU-row availability to the encoder integration layer:

```c
typedef struct sh264e_jpeg_row_window_t {
    const uint8_t *component_pixels[3];
    ptrdiff_t component_stride[3];
    uint32_t component_y0[3];
    uint32_t component_rows[3];
    int component_count;
} sh264e_jpeg_row_window_t;

typedef sh264e_status_t (*sh264e_jpeg_row_ready_fn)(
    const sh264e_jpeg_row_window_t *window,
    void *user);
```

The first production API does not need to expose this callback publicly. The
current prototype keeps it internal and exercises it through
`sh264e_encode_jpeg --streaming-prototype`. The prototype now covers the
supported JPEG source-size policy for grayscale and YCbCr inputs, I420 and NV12
output, and caller-provided arena allocation for the retained row cache.

## Production Arena Default

The row-window bridge is now the production implementation for the public
caller-provided arena entry point, `sh264e_encode_jpeg_idr_with_arena`, and for
the corresponding `sh264e_jpeg_get_work_size` arena sizing query. The public
function signatures and caller-owned buffer model remain unchanged: callers
still provide the compressed JPEG input, a JPEG work arena, one 61,440-byte
slice work buffer, and the H.264 output buffer.

The heap-backed convenience wrapper, `sh264e_encode_jpeg_idr`, remains
compatible for callers that do not need deterministic arena placement. The
tool-local `--streaming-prototype` flag remains available as an internal
comparison path while the default arena path exercises the same streaming row
cache.

## Memory Estimate

The current component-plane path stores the whole decoded image. From the
measured allocation report:

| Source | Component-plane bytes |
| --- | ---: |
| 1280x720 4:2:0 | 1,382,400 |
| 1280x720 4:2:2 | 1,843,200 |
| 1280x720 4:4:4 | 2,764,800 |
| 2560x1440 4:2:0 | 5,529,600 |

The row-streaming cache is bounded by retained MCU rows instead of full image
height. Approximate component-cache sizes are:

| Source | MCU-row bytes | Retained rows | Approx cache |
| --- | ---: | ---: | ---: |
| 1280x720 4:2:0 | 30,720 | 2 MCU rows | 61,440 |
| 1280x720 4:2:2 | 40,960 | 2 MCU rows | 81,920 |
| 1280x720 4:4:4 | 61,440 | 2 MCU rows | 122,880 |
| 2560x1440 4:2:0 | 61,440 | dynamic + margin | 184,320 |
| 5120x2880 4:2:0 | 122,880 | dynamic + margin | 491,520 |
| 5120x2880 4:4:4 | 122,880 | dynamic + margin | 819,200 |

The exact retained-row count should be computed from the scaler's fixed-point
source mapping, rounded up to the JPEG component MCU-row height, and extended by
one MCU-row margin so slices at row-window boundaries can still sample the
previous row. These figures exclude the existing 61,440-byte encoder slice work
buffer and H.264 output buffer, both of which are already caller-controlled.

## Rolling Row-Cache Bridge

The hidden streaming bridge now derives retained row-cache size from the decoded
component geometry and the existing fixed-point scaler's per-slice source row
window. Each component cache is sized to the largest source row span needed by
any 16-row luma or 8-row chroma output slice, rounded up to that component's
JPEG MCU-row height plus one MCU-row margin. Decoded MCU rows are copied into
the rolling component cache, and slices are encoded as soon as all source rows
for the next output slice are available.

The bridge keeps the public JPEG API unchanged. The arena-backed JPEG encode
path uses this bridge by default, so production arena sizing is based on the
rolling row cache instead of full decoded component planes.

Measured bridge contract:

| Source | Streaming row cache | Slice work buffer |
| --- | ---: | ---: |
| 1280x720 4:2:0 | 61,440 | 61,440 |
| 1280x720 4:2:2 | 81,920 | 61,440 |
| 1280x720 4:4:4 | 122,880 | 61,440 |
| 2560x1440 4:2:0 | 184,320 | 61,440 |

The integration matrix asserts these row-cache values for color JPEG inputs and
asserts that the streaming path continues to use the same 61,440-byte scaled
slice work buffer as the component-plane path.

The production memory regression report in
`docs/JPEG_STREAMING_MEMORY_REPORT.md` records the measured default arena path:
`1280x720` 4:2:0 uses 616,616 bytes of JPEG work arena and `2560x1440` 4:2:0
uses 770,216 bytes, including the dynamic NanoJPEG VLC table block.

## Validation Plan

The prototype adds a tool/test-only path before replacing the default JPEG
encoder:

* Generate `1280x720` `yuvj420p`, `yuvj422p`, and `yuvj444p` JPEG fixtures,
  a `2560x1440` `yuvj420p` JPEG fixture, and a grayscale JPEG fixture with
  ffmpeg.
* Encode through the streaming prototype to Annex B H.264 for I420 and NV12
  output where applicable.
* Decode the H.264 with ffmpeg and verify the same stream metadata as the
  component-plane path.
* Decode both the streaming and component-plane H.264 outputs to raw `yuv420p`
  and compare SHA-256 hashes for each covered fixture.
* Confirm the NanoJPEG allocation peak is below the 1,382,400-byte full
  component-plane allocation for the 4:2:0 fixture and below the corresponding
  full component-plane allocation for 4:2:2 and 4:4:4.
* When `cjpeg` is available, generate a 4:2:0 fixture with DRI/RST restart
  markers and run the same streaming-vs-component decoded-frame comparison.
* Confirm the default arena-backed JPEG path reports nonzero streaming
  row-cache bytes and stays below the full component-plane allocation for the
  covered color fixtures.
