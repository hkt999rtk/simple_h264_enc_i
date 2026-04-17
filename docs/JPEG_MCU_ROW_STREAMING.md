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
`sh264e_encode_jpeg --streaming-prototype` for a 1280x720 4:2:0 I420 output
fixture. A public streaming API should wait until the prototype proves the
row-cache contract across the rest of the supported JPEG matrix.

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
| 1280x720 4:2:2 | 30,720 | 2 MCU rows | 61,440 |
| 1280x720 4:4:4 | 30,720 | 2 MCU rows | 61,440 |
| 5120x2880 4:2:0 | 122,880 | 3 MCU rows | 368,640 |
| 5120x2880 4:4:4 | 122,880 | 5 MCU rows | 614,400 |

The exact retained-row count should be computed from the scaler's fixed-point
source mapping, then rounded up to the JPEG component MCU-row height. These
figures exclude the existing 61,440-byte encoder slice work buffer and H.264
output buffer, both of which are already caller-controlled.

## Validation Plan

The prototype adds a tool/test-only path before replacing the default JPEG
encoder:

* Generate a `1280x720` `yuvj420p` JPEG fixture with ffmpeg.
* Encode through the streaming prototype to Annex B H.264.
* Decode the H.264 with ffmpeg and verify the same stream metadata as the
  component-plane path.
* Decode both the streaming and component-plane H.264 outputs to raw `yuv420p`
  and compare SHA-256 hashes for the first 1280x720 4:2:0 I420 fixture.
* Confirm the NanoJPEG allocation peak is below the 1,382,400-byte full
  component-plane allocation for the same fixture.
* When `cjpeg` is available, generate a 4:2:0 fixture with DRI/RST restart
  markers and run the same streaming-vs-component decoded-frame comparison.
* Keep the component-plane arena path as the compatibility fallback until the
  streaming path covers grayscale, 4:2:0, 4:2:2, and 4:4:4.
