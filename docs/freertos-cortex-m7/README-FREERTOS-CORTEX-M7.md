# FreeRTOS Cortex-M7 Package

This package contains the production `simple_h264_enc_i` static library for
FreeRTOS Cortex-M7 applications. It is a developer package, not a complete
board support package.

## Contents

```text
include/sh264e.h
lib/libsimple_h264_enc_i.a
docs/README-FREERTOS-CORTEX-M7.md
docs/API.md
docs/MEMORY.md
build-info.txt
manifest.txt
```

The library is built only from:

* `src/sh264e.c`
* `src/nanojpeg.c`
* `include/sh264e.h`

The package does not include tools, tests, host binaries, Android artifacts, or
iOS artifacts.

## Choose The ABI

Use the soft-float package when the consuming FreeRTOS application is compiled
without a hard-float ABI:

```text
simple_h264_enc_i-${VERSION}-freertos-cortex-m7-soft.tar.gz
```

Use the hard-float package when the application is compiled for the common
Cortex-M7 FPv5 single-precision ABI:

```text
simple_h264_enc_i-${VERSION}-freertos-cortex-m7-fpv5-sp-hard.tar.gz
```

The application and every linked static library must use compatible CPU,
Thumb, FPU, and float ABI settings. Do not mix `-mfloat-abi=soft` and
`-mfloat-abi=hard` objects in the same firmware image.

## Compiler Flags

The package library is built with `arm-none-eabi-gcc` using:

```sh
-mcpu=cortex-m7
-mthumb
-O2
-ffreestanding
-fno-builtin
-ffunction-sections
-fdata-sections
-DNJ_USE_LIBC=0
-Iinclude
```

The ABI-specific flags are either:

```sh
-mfloat-abi=soft
```

or:

```sh
-mfpu=fpv5-sp-d16
-mfloat-abi=hard
```

Use matching flags in the consuming firmware project. Keep
`-ffunction-sections` and `-fdata-sections` enabled if the final firmware link
uses `--gc-sections`.

## Integration Steps

1. Copy or reference `include/sh264e.h` from the package in the firmware include
   path.
2. Add `lib/libsimple_h264_enc_i.a` to the firmware link.
3. Compile application code with matching Cortex-M7 and float ABI flags.
4. Provide all input, output, JPEG arena, encoder arena, and slice-work buffers
   from application-owned memory.
5. Route encoded Annex B bytes from the streaming output consumer to flash,
   storage, DMA, or a caller-owned ring buffer.

The library performs no file I/O and does not own board drivers, RTOS tasks,
DMA policy, flash layout, camera input, or retry policy.

## Production Policy

The package is built with the production policy:

```text
SH264E_BUILD_TOOLS=OFF
SH264E_BUILD_TESTS=OFF
SH264E_ENABLE_JPEG_TEST_HOOKS=OFF
NJ_USE_LIBC=0
```

Private JPEG fault-injection hooks, prototype streaming symbols, and NanoJPEG
full-image decode entry points are rejected by the package symbol audit.

## Metadata

`build-info.txt` records the version, git commit, compiler path and version,
archive tools, target flags, source files, and production policy.

`manifest.txt` records SHA-256 checksums for the package payload files.
