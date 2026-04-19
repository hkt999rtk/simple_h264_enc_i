# FreeRTOS Cortex-M7 Memory Notes

The library is designed for caller-owned memory. FreeRTOS applications should
allocate arenas and output buffers from known memory regions that match the
board's SRAM, DTCM, cache, DMA, and task-stack policy.

## Encoder Arena

For the supported fixed v1 configuration, `sh264e_encoder_get_work_size`
currently reports a 303-byte caller arena requirement for
`sh264e_encoder_create_with_arena`.

`sh264e_encoder_get_memory_report` reports the encoder-owned breakdown:

```text
context/control bytes: 40
bitstream scratch bytes: 256
reconstructed luma bytes: 0
reconstructed chroma bytes: 0
neighbor state bytes: 0
total bytes: 296
```

The arena size is slightly larger than the total to allow internal alignment of
the opaque encoder control structure.

## Output Buffers

The caller-buffer APIs require enough output capacity for the requested header,
slice, or complete frame. Query:

```text
sh264e_get_max_header_output_size
sh264e_get_max_slice_output_size
sh264e_get_max_output_size
```

For embedded JPEG paths, prefer the streaming output consumer APIs. They allow a
small reusable output chunk buffer and synchronous forwarding to flash, storage,
DMA, or a ring buffer instead of requiring a full-frame H.264 output buffer.

## Raw Resize Work Buffer

For raw I420 or NV12 source frames, use:

```text
sh264e_resize_get_slice_buffer_size
sh264e_resize_make_slice
```

The caller owns the work buffer and the source frame. The resize helper returns
one encoder slice at a time.

## JPEG Decoder Arena

For contiguous JPEG input, size the decoder arena with:

```text
sh264e_jpeg_get_work_size
```

For pull-only sequential JPEG input, size the decoder arena with:

```text
sh264e_jpeg_source_get_work_size
```

Both sizing paths parse the JPEG input. Source-reader sizing consumes the
reader, so reset or recreate the source before the next sizing call or encode
call.

## JPEG Slice Work Buffer

The conservative worst-case query is:

```text
sh264e_jpeg_get_slice_buffer_size
```

For a specific JPEG and output pixel format, use:

```text
sh264e_jpeg_get_slice_work_size
sh264e_jpeg_source_get_slice_work_size
```

`sh264e_jpeg_get_last_slice_work_bytes` reports the effective slice staging
used by the last JPEG encode.

## JPEG Streaming Cache Diagnostics

The production JPEG path uses MCU-row streaming rather than full-image decode.
Use these public diagnostics after an encode:

```text
sh264e_jpeg_get_last_allocation_stats
sh264e_jpeg_get_last_streaming_cache_bytes
```

The reported peak allocation excludes caller-owned compressed JPEG input,
encoder arena, H.264 output buffers, RTOS task stacks, driver buffers, and any
application-level queues.

## Allocation Policy

The package is built with `NJ_USE_LIBC=0` and no private JPEG test hooks.
Production FreeRTOS integrations should decide explicitly where each buffer
lives:

* encoder arena
* JPEG decoder arena
* raw resize or JPEG slice work buffer
* streaming output chunk buffer
* source frame or compressed JPEG input
* RTOS task stack and driver/DMA buffers

Keep cache maintenance and DMA ownership outside the library boundary.
