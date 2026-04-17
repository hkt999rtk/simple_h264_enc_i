cmake_minimum_required(VERSION 3.20)

project(simple_h264_enc_i VERSION 0.1.0 LANGUAGES C)

option(SH264E_BUILD_TOOLS "Build command-line tools" ON)
option(SH264E_BUILD_TESTS "Build tests" ON)
option(SH264E_BUILD_QEMU_TESTS "Build Cortex-M QEMU firmware tests when the toolchain is usable" ON)

add_library(simple_h264_enc_i STATIC
    src/sh264e.c
    src/nanojpeg.c
)

target_include_directories(simple_h264_enc_i
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(simple_h264_enc_i PUBLIC c_std_99)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(simple_h264_enc_i PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
    )
elseif(MSVC)
    target_compile_options(simple_h264_enc_i PRIVATE /W4)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(src/nanojpeg.c PROPERTIES
        COMPILE_OPTIONS "-Wno-conversion;-Wno-shadow"
    )
endif()
set_property(SOURCE src/nanojpeg.c APPEND PROPERTY COMPILE_DEFINITIONS NJ_USE_LIBC=0)

if(SH264E_BUILD_TOOLS)
    add_executable(sh264e_encode_file
        tools/sh264e_encode_file.c
    )
    target_link_libraries(sh264e_encode_file PRIVATE simple_h264_enc_i)
    target_compile_features(sh264e_encode_file PRIVATE c_std_99)

    add_executable(sh264e_encode_file_progressive
        tools/sh264e_encode_file_progressive.c
    )
    target_link_libraries(sh264e_encode_file_progressive PRIVATE simple_h264_enc_i)
    target_compile_features(sh264e_encode_file_progressive PRIVATE c_std_99)

    add_executable(sh264e_resize_encode_progressive
        tools/sh264e_resize_encode_progressive.c
    )
    target_link_libraries(sh264e_resize_encode_progressive PRIVATE simple_h264_enc_i)
    target_compile_features(sh264e_resize_encode_progressive PRIVATE c_std_99)

    add_executable(sh264e_encode_jpeg
        tools/sh264e_encode_jpeg.c
    )
    target_link_libraries(sh264e_encode_jpeg PRIVATE simple_h264_enc_i)
    target_compile_features(sh264e_encode_jpeg PRIVATE c_std_99)

    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(sh264e_encode_file PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
        target_compile_options(sh264e_encode_file_progressive PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
        target_compile_options(sh264e_resize_encode_progressive PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
        target_compile_options(sh264e_encode_jpeg PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
    elseif(MSVC)
        target_compile_options(sh264e_encode_file PRIVATE /W4)
        target_compile_options(sh264e_encode_file_progressive PRIVATE /W4)
        target_compile_options(sh264e_resize_encode_progressive PRIVATE /W4)
        target_compile_options(sh264e_encode_jpeg PRIVATE /W4)
    endif()
endif()

if(SH264E_BUILD_TESTS)
    enable_testing()

    add_executable(sh264e_test_api
        tests/test_api.c
    )
    target_link_libraries(sh264e_test_api PRIVATE simple_h264_enc_i)
    target_compile_features(sh264e_test_api PRIVATE c_std_99)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(sh264e_test_api PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
    elseif(MSVC)
        target_compile_options(sh264e_test_api PRIVATE /W4)
    endif()

    add_test(NAME sh264e_api COMMAND sh264e_test_api)
    add_test(
        NAME sh264e_no_file_io_in_library
        COMMAND ${CMAKE_COMMAND}
            -DSH264E_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_no_file_io.cmake
    )
    add_test(
        NAME sh264e_jpeg_no_rgb_path
        COMMAND ${CMAKE_COMMAND}
            -DSH264E_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_jpeg_no_rgb_path.cmake
    )

    find_package(Python3 COMPONENTS Interpreter)
    find_program(SH264E_FFPROBE ffprobe)
    find_program(SH264E_FFMPEG ffmpeg)
    find_program(SH264E_CJPEG cjpeg)
    find_program(SH264E_ARM_NONE_EABI_GCC arm-none-eabi-gcc)
    find_program(SH264E_QEMU_SYSTEM_ARM qemu-system-arm)
    set(SH264E_CAN_BUILD_QEMU_TESTS OFF)
    if(SH264E_BUILD_QEMU_TESTS AND SH264E_ARM_NONE_EABI_GCC AND SH264E_QEMU_SYSTEM_ARM)
        set(SH264E_ARM_STDLIB_PROBE ${CMAKE_CURRENT_BINARY_DIR}/sh264e_arm_stdlib_probe.c)
        file(WRITE ${SH264E_ARM_STDLIB_PROBE}
            "#include <stdlib.h>\nint main(void) { return 0; }\n"
        )
        execute_process(
            COMMAND ${SH264E_ARM_NONE_EABI_GCC}
                -x c
                -c
                ${SH264E_ARM_STDLIB_PROBE}
                -o ${CMAKE_CURRENT_BINARY_DIR}/sh264e_arm_stdlib_probe.o
            RESULT_VARIABLE SH264E_ARM_STDLIB_PROBE_RESULT
            OUTPUT_QUIET
            ERROR_QUIET
        )
        if(SH264E_ARM_STDLIB_PROBE_RESULT EQUAL 0)
            set(SH264E_CAN_BUILD_QEMU_TESTS ON)
        else()
            message(STATUS "Skipping QEMU firmware tests: arm-none-eabi-gcc cannot compile <stdlib.h>")
        endif()
    elseif(SH264E_BUILD_QEMU_TESTS)
        message(STATUS "Skipping QEMU firmware tests: arm-none-eabi-gcc or qemu-system-arm was not found")
    endif()
    if(Python3_Interpreter_FOUND AND SH264E_FFPROBE AND SH264E_FFMPEG AND SH264E_BUILD_TOOLS)
        set(SH264E_FFMPEG_INTEGRATION_ARGS
            --encoder $<TARGET_FILE:sh264e_encode_file>
            --progressive-encoder $<TARGET_FILE:sh264e_encode_file_progressive>
            --resize-encoder $<TARGET_FILE:sh264e_resize_encode_progressive>
            --jpeg-encoder $<TARGET_FILE:sh264e_encode_jpeg>
            --ffprobe ${SH264E_FFPROBE}
            --ffmpeg ${SH264E_FFMPEG}
            --workdir ${CMAKE_CURRENT_BINARY_DIR}/integration
        )
        if(SH264E_CJPEG)
            list(APPEND SH264E_FFMPEG_INTEGRATION_ARGS --cjpeg ${SH264E_CJPEG})
        endif()
        add_test(
            NAME sh264e_ffmpeg_integration
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ffmpeg_integration.py
                ${SH264E_FFMPEG_INTEGRATION_ARGS}
        )
    endif()

    if(SH264E_CAN_BUILD_QEMU_TESTS)
        function(sh264e_add_qemu_firmware TARGET_NAME SOURCE CPU MACHINE)
            set(extra_compile_options "")
            foreach(def IN LISTS ARGN)
                list(APPEND extra_compile_options -D${def})
            endforeach()
            set(ELF_PATH ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf)
            add_custom_command(
                OUTPUT ${ELF_PATH}
                COMMAND ${SH264E_ARM_NONE_EABI_GCC}
                    -mcpu=${CPU}
                    -mthumb
                    -O2
                    -ffreestanding
                    -fno-builtin
                    -ffunction-sections
                    -fdata-sections
                    -DNJ_USE_LIBC=0
                    ${extra_compile_options}
                    -I${CMAKE_CURRENT_SOURCE_DIR}/include
                    -T${CMAKE_CURRENT_SOURCE_DIR}/tests/qemu_cortexm.ld
                    -nostdlib
                    -Wl,--gc-sections
                    ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/sh264e.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/nanojpeg.c
                    -lgcc
                    -o ${ELF_PATH}
                DEPENDS
                    ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/qemu_cortexm.ld
                    ${CMAKE_CURRENT_SOURCE_DIR}/include/sh264e.h
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/sh264e.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/nanojpeg.c
                VERBATIM
            )
            add_custom_target(${TARGET_NAME} ALL DEPENDS ${ELF_PATH})
            add_test(
                NAME ${TARGET_NAME}
                COMMAND ${SH264E_QEMU_SYSTEM_ARM}
                    -M ${MACHINE}
                    -cpu ${CPU}
                    -kernel ${ELF_PATH}
                    -semihosting
                    -nographic
                    -monitor none
                    -serial none
            )
            set_tests_properties(${TARGET_NAME} PROPERTIES TIMEOUT 10)
        endfunction()

        sh264e_add_qemu_firmware(
            sh264e_qemu_scaler_smoke_m3
            tests/qemu_scaler_smoke.c
            cortex-m3
            mps2-an385
        )
        sh264e_add_qemu_firmware(
            sh264e_qemu_scaler_smoke_m4
            tests/qemu_scaler_smoke.c
            cortex-m4
            mps2-an386
        )
        sh264e_add_qemu_firmware(
            sh264e_qemu_scaler_smoke_m7
            tests/qemu_scaler_smoke.c
            cortex-m7
            mps2-an500
        )
        sh264e_add_qemu_firmware(
            sh264e_qemu_scaler_bench_m4
            tests/qemu_scaler_bench.c
            cortex-m4
            mps2-an386
        )
        sh264e_add_qemu_firmware(
            sh264e_qemu_scaler_bench_m4_portable
            tests/qemu_scaler_bench.c
            cortex-m4
            mps2-an386
            SH264E_DISABLE_ARM_DSP
        )
        sh264e_add_qemu_firmware(
            sh264e_qemu_scaler_bench_m7
            tests/qemu_scaler_bench.c
            cortex-m7
            mps2-an500
        )
        sh264e_add_qemu_firmware(
            sh264e_qemu_scaler_bench_m7_portable
            tests/qemu_scaler_bench.c
            cortex-m7
            mps2-an500
            SH264E_DISABLE_ARM_DSP
        )
    endif()
endif()
