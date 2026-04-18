# Independent Macroblock Slice Mode Status

## Implemented Behavior

The H.264 encoder uses independent macroblock slices instead of row-slice
reconstructed-neighbor intra prediction:

* IDR-only
* fixed 16x16 macroblocks
* one H.264 slice per macroblock
* no left/top reconstructed-neighbor reference
* no reconstructed slice buffer
* lower CPU, SRAM, and SRAM bandwidth at the cost of larger bitstreams

This is a deliberate Cortex-M tradeoff. The encoder is intended to be simple,
deterministic, and low-memory rather than compression efficient.

Issues #63, #64, #65, #66, and #67 implemented and validated this mode. The
implementation uses fixed unavailable-neighbor residual prediction and fixed
CAVLC `nC = 0`; it retains no row-level or per-macroblock reconstructed neighbor
state.

## Former Row-Slice Baseline

The progressive input API consumed one macroblock row per call before this
change, and that public call shape remains unchanged:

```text
sh264e_begin_idr
90x sh264e_encode_idr_slice   // each call consumes one 2560x16 input row
sh264e_end_idr
```

Former bitstream shape:

```text
SPS
PPS
90 IDR slice NALUs
```

Each former IDR slice contained one horizontal macroblock row, or 160
macroblocks. That model uses reconstructed left/top samples inside a slice for
DC prediction and CAVLC neighbor context.

Former encoder-owned memory included:

| Block | Bytes |
| --- | ---: |
| Reconstructed luma slice | 40,960 |
| Reconstructed chroma slices | 20,480 |
| Neighbor/nonzero state | 2,560 |
| Removed subtotal | 64,000 |

## Current Bitstream Shape

The progressive input API remains row-oriented, while the H.264 slice output
inside each row is now macroblock-oriented:

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

The implementation preserves the public progressive API shape:

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

Implemented encoder behavior:

* no luma reconstructed-neighbor prediction
* no chroma reconstructed-neighbor prediction
* no reconstructed pixel writeback
* no reconstructed slice buffer clears
* unavailable macroblock-neighbor intra behavior at each one-MB slice boundary
* CAVLC `nC` fixed to 0 because no neighbor nonzero context is available

The residual path may remain DC-only:

```text
read source 4x4 luma block
predict against fixed boundary predictor
compute one DC-like residual level
quantize
write CAVLC residual with nC = 0
```

Chroma may continue to use one simplified DC-like residual per 8x8 block.

## Memory Result

The implementation removes these encoder-owned blocks:

| Block | Former bytes | Current bytes |
| --- | ---: | ---: |
| Reconstructed luma slice | 40,960 | 0 |
| Reconstructed chroma slices | 20,480 | 0 |
| Neighbor/nonzero state | 2,560 | 0 |
| Removed subtotal | 64,000 | 0 |

The encoder still needs:

* opaque context/config
* bitstream/RBSP scratch unless the streaming output writer removes or shrinks
  it further
* any small temporary per-macroblock local variables

Independent-MB mode also reduced the worst-case bitstream scratch requirement
because the encoder no longer needs a full macroblock-row slice RBSP.

## CPU and SRAM Bandwidth Result

The implementation eliminates:

* per-slice `memset` of reconstructed luma/chroma buffers
* per-block reads from reconstructed left/top samples
* per-block reconstructed pixel writes
* nonzero-neighbor state updates and reads

Result:

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

## Implemented Steps

1. Documentation and tests distinguish input rows from H.264 slices.
2. The IDR writer has one helper that emits exactly one macroblock as one
   IDR slice NALU.
3. `sh264e_encode_idr_slice` loops over 160 macroblocks and emits 160 IDR
   slice NALUs for the submitted input row.
4. `first_mb_in_slice = row_index * 160 + mb_x`.
5. Reconstructed-neighbor prediction is replaced with fixed boundary prediction.
6. Reconstructed luma/chroma buffers are removed from encoder state.
7. Neighbor/nonzero state is removed from encoder state.
8. CAVLC `nC` is fixed to 0.
9. Max output sizing and streaming output buffer assumptions are updated.
10. Memory reports and regression tests are updated.

## Validation

Validation used for the final status:

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
* implemented encoder arena size is 303 bytes on the current 64-bit host

## Non-Goals

This plan does not add:

* P/B frames
* motion estimation
* CABAC
* rate control
* multiple intra prediction modes
* deblocking tuning
* general-purpose H.264 compression efficiency
