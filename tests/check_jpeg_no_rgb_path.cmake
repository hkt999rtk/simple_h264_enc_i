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

if(NOT nanojpeg_source MATCHES "if[ \t\r\n]*\\(!nj\\.decode_components_only\\)[ \t\r\n]*njConvert[ \t\r\n]*\\(")
    message(FATAL_ERROR "NanoJPEG streaming/component decode must skip RGB conversion")
endif()
