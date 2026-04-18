cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED SH264E_PROJECT_DIR)
    message(FATAL_ERROR "SH264E_PROJECT_DIR is required")
endif()
if(NOT DEFINED SH264E_PRODUCTION_BINARY_DIR)
    message(FATAL_ERROR "SH264E_PRODUCTION_BINARY_DIR is required")
endif()
if(NOT DEFINED SH264E_NM)
    message(FATAL_ERROR "SH264E_NM is required")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND}
        -S ${SH264E_PROJECT_DIR}
        -B ${SH264E_PRODUCTION_BINARY_DIR}
        -DSH264E_BUILD_TOOLS=OFF
        -DSH264E_BUILD_TESTS=OFF
        -DSH264E_ENABLE_JPEG_TEST_HOOKS=OFF
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Production profile configure failed\n"
        "${configure_stdout}\n${configure_stderr}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND}
        --build ${SH264E_PRODUCTION_BINARY_DIR}
        --target simple_h264_enc_i
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Production profile build failed\n"
        "${build_stdout}\n${build_stderr}")
endif()

set(production_library_candidates
    "${SH264E_PRODUCTION_BINARY_DIR}/libsimple_h264_enc_i.a"
    "${SH264E_PRODUCTION_BINARY_DIR}/simple_h264_enc_i.lib"
    "${SH264E_PRODUCTION_BINARY_DIR}/Debug/simple_h264_enc_i.lib"
    "${SH264E_PRODUCTION_BINARY_DIR}/Release/simple_h264_enc_i.lib"
)
set(production_library "")
foreach(candidate IN LISTS production_library_candidates)
    if(EXISTS "${candidate}")
        set(production_library "${candidate}")
        break()
    endif()
endforeach()
if(production_library STREQUAL "")
    message(FATAL_ERROR "Production profile did not produce simple_h264_enc_i static library")
endif()

foreach(tool_name
        sh264e_encode_file
        sh264e_encode_file_progressive
        sh264e_resize_encode_progressive
        sh264e_encode_jpeg
        sh264e_test_api
        sh264e_test_jpeg_mcu_rows)
    foreach(suffix "" ".exe")
        if(EXISTS "${SH264E_PRODUCTION_BINARY_DIR}/${tool_name}${suffix}")
            message(FATAL_ERROR "Production profile unexpectedly built ${tool_name}${suffix}")
        endif()
    endforeach()
endforeach()

execute_process(
    COMMAND ${SH264E_NM} -g ${production_library}
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE nm_stdout
    ERROR_VARIABLE nm_stderr
)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR
        "Production symbol inspection failed\n"
        "${nm_stdout}\n${nm_stderr}")
endif()

foreach(required_symbol
        sh264e_encode_jpeg_idr_with_arena_stream
        sh264e_encode_jpeg_source_idr_with_arena_stream
        sh264e_jpeg_source_get_work_size
        sh264e_jpeg_source_get_slice_work_size
        sh264e_jpeg_get_last_allocation_stats
        sh264e_jpeg_get_last_streaming_cache_bytes
        sh264e_jpeg_get_last_slice_work_bytes
        sh264e_encoder_get_memory_report)
    if(NOT nm_stdout MATCHES "_?${required_symbol}([^A-Za-z0-9_]|$)")
        message(FATAL_ERROR "Production symbols are missing public API ${required_symbol}")
    endif()
endforeach()

foreach(forbidden_symbol
        sh264e_jpeg_set_test_allocation_limit
        sh264e_encode_jpeg_idr_streaming_prototype
        sh264e_encode_jpeg_idr_streaming_prototype_with_arena
        njDecodeComponents
        njGetImage
        njGetImageSize
        njIsColor)
    if(nm_stdout MATCHES "_?${forbidden_symbol}([^A-Za-z0-9_]|$)")
        message(FATAL_ERROR "Production symbols expose private symbol ${forbidden_symbol}")
    endif()
endforeach()

if(nm_stdout MATCHES "(^|[^A-Za-z0-9_])_?njDecode([^A-Za-z0-9_]|$)")
    message(FATAL_ERROR "Production symbols expose NanoJPEG full-image njDecode")
endif()
