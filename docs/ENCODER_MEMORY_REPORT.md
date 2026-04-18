# Encoder Memory Report

Issue #50 originally recorded the fixed-v1 encoder heap breakdown used by the
memory optimization roadmap. Issue #63 changes the fixed-v1 encoder to
independent one-macroblock IDR slices, removing row-level reconstructed-neighbor
state from the encoder arena.

## Fixed V1 Breakdown

For the supported `2560x1440` progressive IDR configuration, one encoder
instance reports:

| Block | Bytes | Notes |
| --- | ---: | --- |
| Encoder context/config | 40 | `sh264e_encoder_t` host-side control structure |
| Bitstream scratch | 2,048 | Internal RBSP workspace for one macroblock-slice NALU |
| Reconstructed luma slice | 0 | Fixed unavailable-neighbor prediction uses no reconstructed storage |
| Reconstructed chroma slices | 0 | Fixed unavailable-neighbor prediction uses no reconstructed storage |
| Neighbor/nonzero state | 0 | CAVLC `nC` is fixed to 0 |
| Total encoder memory | 2,088 | Sum of the rows above |
| Caller-provided encoder arena | 2,095 | Total plus worst-case control-structure alignment padding |

The measured current 64-bit host total is `2,088` bytes. The removed row-level
state accounts for the 64,000-byte reduction from the previous 191,048-byte
baseline, with an additional bitstream scratch reduction because the RBSP
workspace now only needs to hold one macroblock slice.

## Independent-MB Result

The independent macroblock slice mode eliminates these blocks:

| Block | Former bytes | Current bytes |
| --- | ---: | ---: |
| Reconstructed luma slice | 40,960 | 0 |
| Reconstructed chroma slices | 20,480 | 0 |
| Neighbor/nonzero state | 2,560 | 0 |
| Removable subtotal | 64,000 | 0 |

The current bitstream emits one H.264 slice per macroblock so the decoder treats
left/top macroblocks as unavailable. Encoder-side row reconstructed-neighbor
prediction and row CAVLC neighbor context are therefore removed.

## Scope

This report is only encoder-owned memory. It excludes:

* caller-owned compressed JPEG input
* JPEG decoder arena and NanoJPEG row-cache allocations
* JPEG/scaler slice work buffer
* H.264 output buffers or streaming output chunk buffers
* `.rodata` and NanoJPEG compact Huffman metadata

## Diagnostics

`sh264e_encoder_get_memory_report` is a documented diagnostic/stat API. It
validates the supplied encoder config and returns the sub-block sizes above
without creating an encoder.

`sh264e_encoder_get_work_size` reports the caller arena size needed by
`sh264e_encoder_create_with_arena`. The arena-backed creation path accepts
misaligned caller memory by aligning the opaque encoder control structure inside
the supplied block, then carving the byte-addressed encoder work buffers after
it. `sh264e_encoder_destroy` resets no caller memory and does not free the arena.

The JPEG tool prints the same report:

```text
encoder memory total bytes: 2088
encoder arena work bytes: 2095
encoder context bytes: 40
encoder bitstream scratch bytes: 2048
encoder recon luma bytes: 0
encoder recon chroma bytes: 0
encoder neighbor state bytes: 0
```

`tests/test_api.c` and `tests/run_ffmpeg_integration.py` assert these values so
accidental growth in the major encoder heap blocks is visible in CTest.
