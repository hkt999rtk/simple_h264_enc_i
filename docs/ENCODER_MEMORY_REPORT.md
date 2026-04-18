# Encoder Memory Report

Issue #50 records the fixed-v1 encoder heap breakdown used by the memory
optimization roadmap.

## Fixed V1 Breakdown

For the supported `2560x1440` progressive IDR configuration, one encoder
instance reports:

| Block | Bytes | Notes |
| --- | ---: | --- |
| Encoder context/config | 72 | `sh264e_encoder_t` host-side control structure |
| Bitstream scratch | 126,976 | Internal RBSP workspace sized by `sh264e_get_max_slice_output_size` |
| Reconstructed luma slice | 40,960 | `2560 * 16` slice-local reconstructed luma |
| Reconstructed chroma slices | 20,480 | U and V, each `1280 * 8` |
| Neighbor/nonzero state | 2,560 | 4x4 luma nonzero state for one slice |
| Total encoder memory | 191,048 | Sum of the rows above |

The previously documented `about 191,024` byte budget was an estimate. The
measured current 64-bit host total is `191,048` bytes.

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

The JPEG tool prints the same report:

```text
encoder memory total bytes: 191048
encoder context bytes: 72
encoder bitstream scratch bytes: 126976
encoder recon luma bytes: 40960
encoder recon chroma bytes: 20480
encoder neighbor state bytes: 2560
```

`tests/test_api.c` and `tests/run_ffmpeg_integration.py` assert these values so
accidental growth in the major encoder heap blocks is visible in CTest.
