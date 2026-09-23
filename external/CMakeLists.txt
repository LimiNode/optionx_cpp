# external/CMakeLists.txt
cmake_minimum_required(VERSION 3.18)
project(ExternalDependencies LANGUAGES C CXX)

option(BUILD_SHARED_LIBS "Build shared libraries by default" OFF)

if(NOT DEFINED OPTIONX_DEPS_OUTPUT_DIR)
    set(OPTIONX_DEPS_OUTPUT_DIR "${CMAKE_BINARY_DIR}")
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic
    )
    add_link_options(
        -static-libgcc
        -static-libstdc++
    )

    if(MINGW)
        add_link_options(
            -static-libwinpthread
        )
    endif()

elseif (MSVC)
    add_compile_options(
        /W4
        /wd4201  # anonymous union/struct
        /wd4324  # padding warning
    )
    # Статическая линковка — глобально:
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

set(EXTERNAL_CMAKE_DIR ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(${EXTERNAL_CMAKE_DIR}/install-headers.cmake)
include(${EXTERNAL_CMAKE_DIR}/install-static-libs.cmake)
include(${EXTERNAL_CMAKE_DIR}/install-runtime-dlls.cmake)

# mdbx
include(${EXTERNAL_CMAKE_DIR}/libmdbx-wrapper.cmake)

if(WIN32)
# openssl
install_headers_to_include(openssl ${CMAKE_CURRENT_LIST_DIR}/openssl-win64-v3.5.8/include/openssl)
# The 3.5.8 snapshot ships its x64 import/static libraries in the MD tree.
install_static_libs_to_lib(${CMAKE_CURRENT_LIST_DIR}/openssl-win64-v3.5.8/lib/VC/x64/MD)
install_runtime_dlls_to_bin(${CMAKE_CURRENT_LIST_DIR}/openssl-win64-v3.5.8/bin)

# curl
install_headers_to_include(curl ${CMAKE_CURRENT_LIST_DIR}/curl-8.22.0_1-win64-mingw/include/curl)

file(GLOB CURL_STATIC_LIBS "${CMAKE_CURRENT_LIST_DIR}/curl-8.22.0_1-win64-mingw/lib/*.a")
# Удаляем libssl.a и libcrypto.a
list(FILTER CURL_STATIC_LIBS EXCLUDE REGEX ".*libssl.a$")
list(FILTER CURL_STATIC_LIBS EXCLUDE REGEX ".*libcrypto.a$")
install_lib_files_to_lib(${CURL_STATIC_LIBS})
# install_static_libs_to_lib(${CMAKE_CURRENT_LIST_DIR}/curl-8.22.0_1-win64-mingw/lib)
install_runtime_dlls_to_bin(${CMAKE_CURRENT_LIST_DIR}/curl-8.22.0_1-win64-mingw/bin)
set(CURL_CA_BUNDLE "${CMAKE_CURRENT_LIST_DIR}/curl-8.22.0_1-win64-mingw/bin/curl-ca-bundle.crt")
if(EXISTS "${CURL_CA_BUNDLE}")
    file(COPY
        "${CURL_CA_BUNDLE}"
        DESTINATION ${OPTIONX_DEPS_OUTPUT_DIR}/bin
    )
endif()
else()
    # Non-Windows builds use system OpenSSL/CURL targets from the root
    # tests/examples link configuration.
endif()

# AES
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/AES)
install_headers_to_include("." ${CMAKE_CURRENT_LIST_DIR}/AES/src)

# googletest
include(${EXTERNAL_CMAKE_DIR}/libgtest-wrapper.cmake)
# install_static_libs_to_lib(${GTEST_DIR}/build)

# header-only
install_headers_to_include("."             ${CMAKE_CURRENT_LIST_DIR}/time-shield-cpp/include)
install_headers_to_include(logit_cpp       ${CMAKE_CURRENT_LIST_DIR}/log-it-cpp/include/logit_cpp)
install_headers_to_include("."             ${CMAKE_CURRENT_LIST_DIR}/asio/asio/include)
install_headers_to_include(nlohmann        ${CMAKE_CURRENT_LIST_DIR}/json/include/nlohmann)
install_headers_to_include("."             ${CMAKE_CURRENT_LIST_DIR}/Simple-Web-Server)
install_headers_to_include("."             ${CMAKE_CURRENT_LIST_DIR}/Simple-WebSocket-Server)
install_headers_to_include("."             ${CMAKE_CURRENT_LIST_DIR}/kurlyk/include)
install_headers_to_include("."             ${CMAKE_CURRENT_LIST_DIR}/mdbx-containers/include)
install_headers_to_include("."             ${CMAKE_CURRENT_LIST_DIR}/SimpleNamedPipe/include)
install_headers_to_include("."             ${CMAKE_CURRENT_LIST_DIR}/uni-algo/include)

add_custom_target(all_libs_built ALL
    COMMENT "All libraries and headers have been prepared."
)
