# cmake/FindMbedTLS.cmake
# Locate MbedTLS include paths and libraries

# If MbedTLSConfig.cmake is present on the system, prefer upstream config
find_package(MbedTLS CONFIG QUIET)
if (TARGET MbedTLS::mbedcrypto)
    set(MbedTLS_FOUND TRUE)
    return()
endif()

find_path(MBEDTLS_INCLUDE_DIR
    NAMES mbedtls/version.h mbedtls/ssl.h mbedtls/pk.h
    PATHS /usr/include /usr/local/include
)

find_library(MBEDCRYPTO_LIBRARY
    NAMES mbedcrypto
    PATHS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/local/lib
)

find_library(MBEDX509_LIBRARY
    NAMES mbedx509
    PATHS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/local/lib
)

find_library(MBEDTLS_LIBRARY
    NAMES mbedtls
    PATHS /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/local/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MbedTLS
    DEFAULT_MSG
    MBEDTLS_INCLUDE_DIR
    MBEDCRYPTO_LIBRARY
)

if (MbedTLS_FOUND)
    if (NOT TARGET MbedTLS::mbedcrypto)
        add_library(MbedTLS::mbedcrypto UNKNOWN IMPORTED)
        set_target_properties(MbedTLS::mbedcrypto PROPERTIES
            IMPORTED_LOCATION "${MBEDCRYPTO_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MBEDTLS_INCLUDE_DIR}"
        )
    endif()
    if (MBEDX509_LIBRARY AND NOT TARGET MbedTLS::mbedx509)
        add_library(MbedTLS::mbedx509 UNKNOWN IMPORTED)
        set_target_properties(MbedTLS::mbedx509 PROPERTIES
            IMPORTED_LOCATION "${MBEDX509_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MBEDTLS_INCLUDE_DIR}"
        )
    endif()
    if (MBEDTLS_LIBRARY AND NOT TARGET MbedTLS::mbedtls)
        add_library(MbedTLS::mbedtls UNKNOWN IMPORTED)
        set_target_properties(MbedTLS::mbedtls PROPERTIES
            IMPORTED_LOCATION "${MBEDTLS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MBEDTLS_INCLUDE_DIR}"
        )
    endif()
endif()
