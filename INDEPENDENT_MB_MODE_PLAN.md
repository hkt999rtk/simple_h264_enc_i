# Independent Macroblock Slice Mode Plan

## Goal

Change the H.264 encoder direction from row-slice intra prediction to
independent macroblock slices:

* IDR-only
* fixed 16x16 macroblocks
* one H.264 slice per macroblock
* no left/top reconstructed-neighbor reference
* no reconstructed slice buffer
* lower CPU, SRAM, and SRAM bandwidth at the cost of larger bitstreams

This is a deliberate Cortex-M tradeoff. The encoder is intended to be simple,
deterministic, and low-memory rather than compression efficient.

Issue #63 implements the bitstream shape and memory removal in this plan. The
implementation keeps only per-macroblock predictor/nonzero state on the stack so
the existing intra-4x4 CAVLC syntax remains decoder-compatible inside each
one-macroblock slice; it does not retain row-level reconstructed-neighbor state.

## Current Baseline

The current progressive input API consumes one macroblock row per call:

```text
sh264e_begin_idr
90x sh264e_encode_idr_slice   // each call consumes one 2560x16 input row
sh264e_end_idr
```

Current bitstream shape:

```text
SPS
PPS
90 IDR slice NALUs
```

Each current IDR slice contains one horizontal macroblock row, or 160
macroblocks. That model uses reconstructed left/top samples inside a slice for
DC prediction and CAVLC neighbor context.

Current encoder-owned memory includes:

| Block | Bytes |
| --- | ---: |
| Reconstructed luma slice | 40,960 |
| Reconstructed chroma slices | 20,480 |
| Neighbor/nonzero state | 2,560 |
| Total removable target | 64,000 |

## Target Bitstream Shape

Keep the progressive input API row-oriented, but change the H.264 slice output
inside each row:

```text
SPS
PPS
IDR Slice for MB 0
IDR Slice for MB 1
...
IDR Slice for MB 14399
```

For a fixed `2560x1440` frame:

```text
mb_width  = 2560 / 16 = 160
mb_height = 1440 / 16 = 90
mb_count  = 160 * 90 = 14,400
```

Each macroblock is encoded as one complete IDR slice NALU. The slice header
uses:

```text
first_mb_in_slice = mb_y * 160 + mb_x
```

This makes left and top macroblocks unavailable to the decoder because they are
outside the current slice. Encoder and decoder therefore agree that no
reconstructed-neighbor prediction is used.

## API Behavior

Avoid large public API churn for the first implementation:

* `sh264e_begin_idr` still emits SPS/PPS.
* `sh264e_encode_idr_slice` still consumes one horizontal input macroblock row.
* `sh264e_end_idr` still validates exactly 90 submitted input rows.
* Each `sh264e_encode_idr_slice` call now emits 160 IDR slice NALUs, not one.

This preserves the low-bandwidth caller input model while changing the H.264
slice granularity.

Documentation and tests must stop assuming one input slice call equals one H.264
slice NALU. The clearer terminology is:

* **input row**: one caller-provided `2560x16` luma / `8` chroma-row block
* **H.264 slice**: one macroblock slice NALU

## Prediction and Residual

Target encoder behavior:

* no row-level luma reconstructed-neighbor prediction
* no row-level chroma reconstructed-neighbor prediction
* no reconstructed pixel writeback outside temporary one-MB stack state
* no reconstructed slice buffer clears
* unavailable macroblock-neighbor intra behavior at each one-MB slice boundary
* CAVLC context must not read row-level neighbor state; temporary within-MB
  context may be used when required by the chosen syntax

The residual path may remain DC-only:

```text
read source 4x4 luma block
predict against fixed boundary predictor
compute one DC-like residual level
quantize
write CAVLC residual with nC = 0
```

Chroma may continue to use one simplified DC-like residual per 8x8 block.

## Memory Target

The first implementation should remove these encoder-owned blocks:

| Block | Current bytes | Target |
| --- | ---: | ---: |
| Reconstructed luma slice | 40,960 | 0 |
| Reconstructed chroma slices | 20,480 | 0 |
| Neighbor/nonzero state | 2,560 | 0 |
| Removable subtotal | 64,000 | 0 |

The encoder will still need:

* opaque context/config
* bitstream/RBSP scratch unless the streaming output writer removes or shrinks
  it further
* any small temporary per-macroblock local variables

If #48 streaming H.264 output is active, this independent-MB mode should also
help reduce the worst-case bitstream scratch requirement because the encoder no
longer needs a full macroblock-row slice RBSP.

## CPU and SRAM Bandwidth Target

The implementation should eliminate:

* per-slice `memset` of reconstructed luma/chroma buffers
* per-block reads from reconstructed left/top samples
* per-block reconstructed pixel writes
* nonzero-neighbor state updates and reads

Expected result:

* lower CPU cycles per macroblock
* lower SRAM read/write bandwidth
* simpler scheduling on Cortex-M
* larger output bitstream due to 14,400 slice headers and weaker prediction

## Bitstream Cost

The cost is intentional:

* IDR slice count grows from 90 to 14,400.
* Slice header overhead increases substantially.
* Compression efficiency drops because prediction no longer tracks local image
  continuity.
* Decoder must parse many more slices.

This tradeoff is acceptable for the target use case if CPU/SRAM is more
important than bitstream size.

## Implementation Steps

1. Update documentation and tests to distinguish input rows from H.264 slices.
2. Refactor the IDR writer so one helper emits exactly one macroblock as one
   IDR slice NALU.
3. In `sh264e_encode_idr_slice`, loop over 160 macroblocks and emit 160 IDR
   slice NALUs for the submitted input row.
4. Set `first_mb_in_slice = row_index * 160 + mb_x`.
5. Replace reconstructed-neighbor prediction with fixed boundary prediction.
6. Remove reconstructed luma/chroma buffers from encoder state.
7. Remove neighbor/nonzero state from encoder state.
8. Fix CAVLC `nC` to 0.
9. Update max output sizing and streaming output buffer assumptions.
10. Update memory reports and regression tests.

## Validation

Required validation:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
git diff --check
```

Functional expectations:

* SPS/PPS remain first.
* One frame contains exactly 14,400 IDR slice NALUs.
* `first_mb_in_slice` covers `0..14399`.
* `sh264e_encode_idr_slice` still accepts exactly 90 input rows.
* Strict ffmpeg decode succeeds:

  ```sh
  ffmpeg -v error -xerror -i output.h264 -f null -
  ```

Memory expectations:

* encoder recon luma bytes report as 0 or disappear
* encoder recon chroma bytes report as 0 or disappear
* encoder neighbor state bytes report as 0 or disappear
* total encoder arena size drops by about 64,000 bytes before considering any
  bitstream scratch reductions
* implemented encoder arena size is 2,127 bytes on the current 64-bit host

## Non-Goals

This plan does not add:

* P/B frames
* motion estimation
* CABAC
* rate control
* multiple intra prediction modes
* deblocking tuning
* general-purpose H.264 compression efficiency
