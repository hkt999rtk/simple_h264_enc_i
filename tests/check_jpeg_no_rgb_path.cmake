if(NOT DEFINED SH264E_SRC_DIR)
    message(FATAL_ERROR "SH264E_SRC_DIR is required")
endif()

file(READ "${SH264E_SRC_DIR}/sh264e.c" sh264e_source)
file(READ "${SH264E_SRC_DIR}/nanojpeg.c" nanojpeg_source)

if(NOT sh264e_source MATCHES "njDecodeMcuRows[ \t\r\n]*\\(")
    message(FATAL_ERROR "JPEG encode path must use NanoJPEG MCU-row streaming decode")
endif()

if(sh264e_source MATCHES "njDecodeComponents[ \t\r\n]*\\(")
    message(FATAL_ERROR "Public JPEG encode path must not use NanoJPEG full component-plane decode")
endif()

if(sh264e_source MATCHES "njGetImage[ \t\r\n]*\\(")
    message(FATAL_ERROR "JPEG encode path must not consume NanoJPEG RGB image output")
endif()

if(NOT nanojpeg_source MATCHES "decode_mcu_rows_only")
    message(FATAL_ERROR "NanoJPEG MCU-row decode guard is missing")
endif()

if(NOT nanojpeg_source MATCHES "#define NJ_ENABLE_FULL_IMAGE_DECODE 0")
    message(FATAL_ERROR "NanoJPEG full-image/component decode must default to disabled")
endif()

if(NOT nanojpeg_source MATCHES "#if NJ_ENABLE_FULL_IMAGE_DECODE[ \t\r\n]+nj_result_t[ \t\r\n]+njDecode[ \t\r\n]*\\(")
    message(FATAL_ERROR "NanoJPEG full-image decode entry point must be debug-gated")
endif()

if(NOT nanojpeg_source MATCHES "#if NJ_ENABLE_FULL_IMAGE_DECODE[ \t\r\n]+int[ \t\r\n]+njIsColor[ \t\r\n]*\\(")
    message(FATAL_ERROR "NanoJPEG RGB image accessors must be debug-gated")
endif()

if(NOT nanojpeg_source MATCHES "#if NJ_ENABLE_FULL_IMAGE_DECODE[ \t\r\n]+njConvert[ \t\r\n]*\\(")
    message(FATAL_ERROR "NanoJPEG RGB conversion must be debug-gated")
endif()

if(NOT nanojpeg_source MATCHES "#else[ \t\r\n]+return NJ_UNSUPPORTED;[ \t\r\n]+#endif")
    message(FATAL_ERROR "NanoJPEG normal build must reject full-image decode")
endif()
