if(NOT DEFINED SH264E_PROJECT_DIR)
    message(FATAL_ERROR "SH264E_PROJECT_DIR is required")
endif()

file(READ "${SH264E_PROJECT_DIR}/include/sh264e.h" public_header)
file(READ "${SH264E_PROJECT_DIR}/src/sh264e.c" sh264e_source)
file(READ "${SH264E_PROJECT_DIR}/tools/sh264e_encode_jpeg.c" jpeg_tool)
file(READ "${SH264E_PROJECT_DIR}/tests/sh264e_jpeg_test_hooks.h" test_hooks)

if(jpeg_tool MATCHES "(void|size_t|sh264e_status_t)[ \t]+sh264e_")
    message(FATAL_ERROR "JPEG tool must not forward-declare sh264e private diagnostics")
endif()

if(NOT jpeg_tool MATCHES "#include \"sh264e_jpeg_test_hooks\\.h\"")
    message(FATAL_ERROR "JPEG test-only hooks must be declared by the test hook header")
endif()

if(NOT jpeg_tool MATCHES "SH264E_ENABLE_JPEG_TEST_HOOKS")
    message(FATAL_ERROR "JPEG diagnostic test flags must be build-gated")
endif()

if(NOT public_header MATCHES "sh264e_jpeg_get_last_streaming_cache_bytes[ \t\r\n]*\\(")
    message(FATAL_ERROR "Streaming cache stats must be intentionally declared in the public API")
endif()

if(NOT test_hooks MATCHES "sh264e_jpeg_set_test_allocation_limit[ \t\r\n]*\\(")
    message(FATAL_ERROR "Allocation-limit hook must be test-only")
endif()

if(NOT sh264e_source MATCHES "#if SH264E_ENABLE_JPEG_TEST_HOOKS[ \t\r\n]+void[ \t\r\n]+sh264e_jpeg_set_test_allocation_limit")
    message(FATAL_ERROR "Allocation-limit implementation must be test-hook gated")
endif()

if(NOT sh264e_source MATCHES "#if SH264E_ENABLE_JPEG_TEST_HOOKS[ \t\r\n]+sh264e_status_t[ \t\r\n]+sh264e_encode_jpeg_idr_streaming_prototype")
    message(FATAL_ERROR "Streaming prototype implementation must be test-hook gated")
endif()

if(sh264e_source MATCHES "sh264e_jpeg_get_streaming_work_size[ \t\r\n]*\\(")
    message(FATAL_ERROR "Use public sh264e_jpeg_get_work_size instead of private streaming work-size helper")
endif()
