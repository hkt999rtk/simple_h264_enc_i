cmake_minimum_required(VERSION 3.20)

project(simple_h264_enc_i VERSION 0.1.0 LANGUAGES C)

option(SH264E_BUILD_TOOLS "Build command-line tools" ON)
option(SH264E_BUILD_TESTS "Build tests" ON)

add_library(simple_h264_enc_i STATIC
    src/sh264e.c
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

if(SH264E_BUILD_TOOLS)
    add_executable(sh264e_encode_file
        tools/sh264e_encode_file.c
    )
    target_link_libraries(sh264e_encode_file PRIVATE simple_h264_enc_i)
    target_compile_features(sh264e_encode_file PRIVATE c_std_99)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(sh264e_encode_file PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
    elseif(MSVC)
        target_compile_options(sh264e_encode_file PRIVATE /W4)
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

    find_package(Python3 COMPONENTS Interpreter)
    find_program(SH264E_FFPROBE ffprobe)
    find_program(SH264E_FFMPEG ffmpeg)
    if(Python3_Interpreter_FOUND AND SH264E_FFPROBE AND SH264E_FFMPEG AND SH264E_BUILD_TOOLS)
        add_test(
            NAME sh264e_ffmpeg_integration
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ffmpeg_integration.py
                --encoder $<TARGET_FILE:sh264e_encode_file>
                --ffprobe ${SH264E_FFPROBE}
                --ffmpeg ${SH264E_FFMPEG}
                --workdir ${CMAKE_CURRENT_BINARY_DIR}/integration
        )
    endif()
endif()
