if(NOT DEFINED SH264E_SRC_DIR)
    message(FATAL_ERROR "SH264E_SRC_DIR is required")
endif()

file(GLOB_RECURSE SH264E_LIBRARY_FILES
    "${SH264E_SRC_DIR}/*.c"
    "${SH264E_SRC_DIR}/*.h"
)

foreach(path IN LISTS SH264E_LIBRARY_FILES)
    file(READ "${path}" contents)
    foreach(func open read write close fopen fread fwrite fclose)
        if(contents MATCHES "(^|[^A-Za-z0-9_])${func}[ \t\r\n]*\\(")
            message(FATAL_ERROR "Forbidden file I/O call '${func}' found in library file: ${path}")
        endif()
    endforeach()
endforeach()
