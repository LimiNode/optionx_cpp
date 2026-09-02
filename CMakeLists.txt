# При генерации Code::Blocks проекта с MinGW можно использовать так:
# cmake -G "CodeBlocks - MinGW Makefiles" -S . -B build-cb
cmake_minimum_required(VERSION 3.18)
project(optionx_cpp LANGUAGES CXX)

if(MSVC)
    add_compile_options(/Zc:__cplusplus)
endif()

if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

set(OPTIONX_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(OPTIONX_TOP_LEVEL ON)
endif()

# Папка для статических библиотек (.a, .lib)
if(OPTIONX_TOP_LEVEL)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib CACHE PATH "")

# Папка для динамических библиотек (.so, .dll)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib CACHE PATH "")
endif()

if(OPTIONX_TOP_LEVEL)
    get_property(OPTIONX_HAS_BUILD_DEPS CACHE OPTIONX_BUILD_DEPS PROPERTY TYPE SET)
    get_property(OPTIONX_HAS_LEGACY_BUILD_DEPS CACHE BUILD_DEPS PROPERTY TYPE SET)
    if(OPTIONX_HAS_LEGACY_BUILD_DEPS AND NOT OPTIONX_HAS_BUILD_DEPS)
        set(OPTIONX_BUILD_DEPS "${BUILD_DEPS}" CACHE BOOL "Build bundled optionx dependencies" FORCE)
    endif()

    get_property(OPTIONX_HAS_BUILD_EXAMPLES CACHE OPTIONX_BUILD_EXAMPLES PROPERTY TYPE SET)
    get_property(OPTIONX_HAS_LEGACY_BUILD_EXAMPLES CACHE BUILD_EXAMPLES PROPERTY TYPE SET)
    if(OPTIONX_HAS_LEGACY_BUILD_EXAMPLES AND NOT OPTIONX_HAS_BUILD_EXAMPLES)
        set(OPTIONX_BUILD_EXAMPLES "${BUILD_EXAMPLES}" CACHE BOOL "Build optionx examples" FORCE)
    endif()

    get_property(OPTIONX_HAS_BUILD_TESTS CACHE OPTIONX_BUILD_TESTS PROPERTY TYPE SET)
    get_property(OPTIONX_HAS_LEGACY_BUILD_TESTS CACHE BUILD_TESTS PROPERTY TYPE SET)
    if(OPTIONX_HAS_LEGACY_BUILD_TESTS AND NOT OPTIONX_HAS_BUILD_TESTS)
        set(OPTIONX_BUILD_TESTS "${BUILD_TESTS}" CACHE BOOL "Build optionx tests" FORCE)
    endif()

    get_property(OPTIONX_HAS_DEPS_BUILD_DIR CACHE OPTIONX_DEPS_BUILD_DIR PROPERTY TYPE SET)
    get_property(OPTIONX_HAS_LEGACY_DEPS_BUILD_DIR CACHE DEPS_BUILD_DIR PROPERTY TYPE SET)
    if(OPTIONX_HAS_LEGACY_DEPS_BUILD_DIR AND NOT OPTIONX_HAS_DEPS_BUILD_DIR)
        set(OPTIONX_DEPS_BUILD_DIR "${DEPS_BUILD_DIR}" CACHE PATH "Path to prebuilt optionx dependencies" FORCE)
    endif()
endif()

option(OPTIONX_BUILD_DEPS "Build bundled optionx dependencies" OFF)
option(OPTIONX_BUILD_EXAMPLES "Build optionx examples" OFF)
option(OPTIONX_BUILD_TESTS "Build optionx tests" OFF)
option(
    OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS
    "Link selected bridge smoke tests without the full optionx_cpp dependency graph"
    OFF)
set(OPTIONX_DEPS_BUILD_DIR "" CACHE PATH "Path to prebuilt optionx dependencies")

if(OPTIONX_TOP_LEVEL)
    set(BUILD_DEPS "${OPTIONX_BUILD_DEPS}" CACHE BOOL "Legacy alias for OPTIONX_BUILD_DEPS" FORCE)
    set(BUILD_EXAMPLES "${OPTIONX_BUILD_EXAMPLES}" CACHE BOOL "Legacy alias for OPTIONX_BUILD_EXAMPLES" FORCE)
    set(BUILD_TESTS "${OPTIONX_BUILD_TESTS}" CACHE BOOL "Legacy alias for OPTIONX_BUILD_TESTS" FORCE)
    set(DEPS_BUILD_DIR "${OPTIONX_DEPS_BUILD_DIR}" CACHE PATH "Legacy alias for OPTIONX_DEPS_BUILD_DIR" FORCE)
endif()

set(LOGIT_BASE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Base path for LOGIT logs")
file(TO_CMAKE_PATH "${LOGIT_BASE_PATH}" LOGIT_BASE_PATH_FWD)

function(optionx_get_network_link_libraries out_var)
    set(network_libs)

    if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
        list(APPEND network_libs
            OpenSSL::SSL
            OpenSSL::Crypto
        )
    elseif(WIN32 AND MSVC)
        list(APPEND network_libs
            libssl
            libcrypto
        )
    elseif(WIN32)
        list(APPEND network_libs
            ssl
            crypto
        )
    else()
        find_package(OpenSSL REQUIRED)
        list(APPEND network_libs
            OpenSSL::SSL
            OpenSSL::Crypto
        )
    endif()

    if(TARGET CURL::libcurl)
        list(APPEND network_libs CURL::libcurl)
    elseif(NOT WIN32)
        find_package(CURL REQUIRED)
        list(APPEND network_libs CURL::libcurl)
    elseif(NOT MSVC)
        set(network_libs
            ${network_libs}
            curl
        )
    endif()

    set(${out_var} ${network_libs} PARENT_SCOPE)
endfunction()

function(optionx_link_preferred_dependency target raw_name)
    foreach(candidate IN LISTS ARGN)
        if(TARGET ${candidate})
            target_link_libraries(${target} INTERFACE ${candidate})
            return()
        endif()
    endforeach()

    target_link_libraries(${target} INTERFACE ${raw_name})
endfunction()

function(optionx_link_existing_dependencies target)
    foreach(candidate IN LISTS ARGN)
        if(TARGET ${candidate})
            target_link_libraries(${target} INTERFACE ${candidate})
        endif()
    endforeach()
endfunction()

set(OPTIONX_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(OPTIONX_COMPILE_DEFINITIONS
    ASIO_STANDALONE
    KURLYK_OAUTH_SUPPORT=0
    UNI_ALGO_STATIC_DATA
)

set(OPTIONX_WINDOWS_SYSTEM_LIBS
    ws2_32
    wsock32
    crypt32
    shell32
    ole32
    ntdll
    bcrypt
)

add_library(optionx_cpp INTERFACE)
add_library(optionx_cpp::optionx_cpp ALIAS optionx_cpp)

target_include_directories(optionx_cpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/SimpleNamedPipe/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/Simple-Web-Server>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/uni-algo/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(optionx_cpp INTERFACE cxx_std_17)
target_compile_definitions(optionx_cpp INTERFACE ${OPTIONX_COMPILE_DEFINITIONS})

if(OPTIONX_BUILD_DEPS)
    set(OPTIONX_DEPS_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    add_subdirectory(external)
elseif(OPTIONX_DEPS_BUILD_DIR)
    set(OPTIONX_DEPS_OUTPUT_DIR "${OPTIONX_DEPS_BUILD_DIR}")
endif()

if(OPTIONX_DEPS_OUTPUT_DIR)
    target_include_directories(optionx_cpp INTERFACE
        $<BUILD_INTERFACE:${OPTIONX_DEPS_OUTPUT_DIR}/include>
    )
    target_link_directories(optionx_cpp INTERFACE
        $<BUILD_INTERFACE:${OPTIONX_DEPS_OUTPUT_DIR}/lib>
        $<BUILD_INTERFACE:${OPTIONX_DEPS_OUTPUT_DIR}/bin>
    )
endif()

optionx_get_network_link_libraries(OPTIONX_NETWORK_LIBS)
target_link_libraries(optionx_cpp INTERFACE ${OPTIONX_NETWORK_LIBS})

optionx_link_preferred_dependency(optionx_cpp mdbx
    mdbx::mdbx-static
    mdbx-static
    mdbx::mdbx
    mdbx
)
optionx_link_preferred_dependency(optionx_cpp AES AES)
optionx_link_existing_dependencies(optionx_cpp
    time_shield::time_shield
    log-it-cpp::log-it-cpp
    mdbx_containers::mdbx_containers
    nlohmann_json::nlohmann_json
    kurlyk
    simple-websocket-server
)

if(WIN32)
    target_link_libraries(optionx_cpp INTERFACE ${OPTIONX_WINDOWS_SYSTEM_LIBS})
endif()

if(OPTIONX_BUILD_EXAMPLES)
    if(OPTIONX_BUILD_DEPS)
        set(EXAMPLE_BUILD_LIBS_DIR ${OPTIONX_DEPS_OUTPUT_DIR})
    else()
        if(NOT OPTIONX_DEPS_BUILD_DIR)
            message(FATAL_ERROR "OPTIONX_BUILD_DEPS is OFF, but OPTIONX_DEPS_BUILD_DIR is not set")
        endif()
        set(EXAMPLE_BUILD_LIBS_DIR ${OPTIONX_DEPS_BUILD_DIR})
    endif()

    set(EXAMPLE_INCLUDE_DIRS
        ${OPTIONX_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/time-shield-cpp/include
    )

    set(EXAMPLE_DEPS_INCLUDE_DIRS
        ${EXAMPLE_BUILD_LIBS_DIR}/include
    )

    set(EXAMPLE_LIBRARY_DIRS
        ${EXAMPLE_BUILD_LIBS_DIR}/lib
        ${EXAMPLE_BUILD_LIBS_DIR}/bin
    )

    file(GLOB EXAMPLE_DLL_FILES "${EXAMPLE_LIBRARY_DIRS}/*.dll")

    set(EXAMPLE_DEFINES
        ${OPTIONX_COMPILE_DEFINITIONS}
    )

    set(EXAMPLE_LIBS)

    add_executable(trade_record_db_example examples/trade_record_db_example.cpp)

    target_include_directories(trade_record_db_example PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
    )

    target_link_directories(trade_record_db_example PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        trade_record_db_example PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    target_link_libraries(trade_record_db_example PRIVATE ${EXAMPLE_LIBS} optionx_cpp)

    if(OPTIONX_BUILD_DEPS)
        add_dependencies(trade_record_db_example mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET trade_record_db_example POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:trade_record_db_example>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET trade_record_db_example POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:trade_record_db_example>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(account_info_hub_example examples/account_info_hub_example.cpp)

    target_include_directories(account_info_hub_example PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
    )

    target_link_directories(account_info_hub_example PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        account_info_hub_example PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    target_link_libraries(account_info_hub_example PRIVATE ${EXAMPLE_LIBS} optionx_cpp)

    if(OPTIONX_BUILD_DEPS)
        add_dependencies(account_info_hub_example mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET account_info_hub_example POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:account_info_hub_example>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET account_info_hub_example POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:account_info_hub_example>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(trading_condition_hub_example examples/trading_condition_hub_example.cpp)

    target_include_directories(trading_condition_hub_example PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
    )

    target_link_directories(trading_condition_hub_example PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        trading_condition_hub_example PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    target_link_libraries(trading_condition_hub_example PRIVATE ${EXAMPLE_LIBS} optionx_cpp)

    if(OPTIONX_BUILD_DEPS)
        add_dependencies(trading_condition_hub_example mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET trading_condition_hub_example POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:trading_condition_hub_example>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET trading_condition_hub_example POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:trading_condition_hub_example>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(market_data_hub_example examples/market_data_hub_example.cpp)

    target_include_directories(market_data_hub_example PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
    )

    target_link_directories(market_data_hub_example PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        market_data_hub_example PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    target_link_libraries(market_data_hub_example PRIVATE ${EXAMPLE_LIBS} optionx_cpp)

    if(OPTIONX_BUILD_DEPS)
        add_dependencies(market_data_hub_example mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET market_data_hub_example POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:market_data_hub_example>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET market_data_hub_example POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:market_data_hub_example>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(market_data_router_example examples/market_data_router_example.cpp)

    target_include_directories(market_data_router_example PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
    )

    target_link_directories(market_data_router_example PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        market_data_router_example PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    target_link_libraries(market_data_router_example PRIVATE ${EXAMPLE_LIBS} optionx_cpp)

    if(OPTIONX_BUILD_DEPS)
        add_dependencies(market_data_router_example mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET market_data_router_example POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:market_data_router_example>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET market_data_router_example POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:market_data_router_example>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(intrade_market_data_example examples/intrade_market_data_example.cpp)

    target_include_directories(intrade_market_data_example PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
    )

    target_link_directories(intrade_market_data_example PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        intrade_market_data_example PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    if(MINGW)
        target_compile_options(intrade_market_data_example PRIVATE -Wa,-mbig-obj)
    endif()
    target_link_libraries(intrade_market_data_example PRIVATE ${EXAMPLE_LIBS} optionx_cpp)

    if(OPTIONX_BUILD_DEPS)
        add_dependencies(intrade_market_data_example mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET intrade_market_data_example POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:intrade_market_data_example>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET intrade_market_data_example POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:intrade_market_data_example>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(tradingview_extension_bridge_smoke examples/tradingview_extension_bridge_smoke.cpp)

    target_include_directories(tradingview_extension_bridge_smoke PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/Simple-Web-Server
    )

    target_link_directories(tradingview_extension_bridge_smoke PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        tradingview_extension_bridge_smoke PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    if(MINGW)
        target_compile_options(tradingview_extension_bridge_smoke PRIVATE -Wa,-mbig-obj)
    endif()
    target_link_libraries(tradingview_extension_bridge_smoke PRIVATE ${EXAMPLE_LIBS} optionx_cpp)

    if(OPTIONX_BUILD_DEPS)
        add_dependencies(tradingview_extension_bridge_smoke mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET tradingview_extension_bridge_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:tradingview_extension_bridge_smoke>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET tradingview_extension_bridge_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:tradingview_extension_bridge_smoke>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(named_pipe_bridge_smoke examples/named_pipe_bridge_smoke.cpp)
    target_compile_features(named_pipe_bridge_smoke PRIVATE cxx_std_17)

    target_include_directories(named_pipe_bridge_smoke PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
    )

    target_link_directories(named_pipe_bridge_smoke PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        named_pipe_bridge_smoke PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    if(MINGW)
        target_compile_options(named_pipe_bridge_smoke PRIVATE -Wa,-mbig-obj)
    endif()
    if(OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        target_link_libraries(named_pipe_bridge_smoke PRIVATE ${EXAMPLE_LIBS})
        if(WIN32)
            target_link_libraries(named_pipe_bridge_smoke PRIVATE ${OPTIONX_WINDOWS_SYSTEM_LIBS})
        endif()
    else()
        target_link_libraries(named_pipe_bridge_smoke PRIVATE ${EXAMPLE_LIBS} optionx_cpp)
    endif()

    if(OPTIONX_BUILD_DEPS AND NOT OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        add_dependencies(named_pipe_bridge_smoke mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET named_pipe_bridge_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:named_pipe_bridge_smoke>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET named_pipe_bridge_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:named_pipe_bridge_smoke>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(bot_binary_bridge_smoke examples/bot_binary_bridge_smoke.cpp)
    target_compile_features(bot_binary_bridge_smoke PRIVATE cxx_std_17)

    target_include_directories(bot_binary_bridge_smoke PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/Simple-Web-Server
    )

    target_link_directories(bot_binary_bridge_smoke PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        bot_binary_bridge_smoke PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    if(MINGW)
        target_compile_options(bot_binary_bridge_smoke PRIVATE -Wa,-mbig-obj)
    elseif(MSVC)
        target_compile_options(bot_binary_bridge_smoke PRIVATE /bigobj)
    endif()
    if(OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        target_link_libraries(
            bot_binary_bridge_smoke PRIVATE
            ${EXAMPLE_LIBS}
            ${OPTIONX_NETWORK_LIBS}
        )
        if(WIN32)
            target_link_libraries(bot_binary_bridge_smoke PRIVATE ${OPTIONX_WINDOWS_SYSTEM_LIBS})
        endif()
    else()
        target_link_libraries(bot_binary_bridge_smoke PRIVATE ${EXAMPLE_LIBS} optionx_cpp)
    endif()

    if(OPTIONX_BUILD_DEPS AND NOT OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        add_dependencies(bot_binary_bridge_smoke mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET bot_binary_bridge_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:bot_binary_bridge_smoke>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET bot_binary_bridge_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:bot_binary_bridge_smoke>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(bot_binary_command_builder_smoke examples/bot_binary_command_builder_smoke.cpp)
    target_compile_features(bot_binary_command_builder_smoke PRIVATE cxx_std_17)

    target_include_directories(bot_binary_command_builder_smoke PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
    )

    target_link_directories(bot_binary_command_builder_smoke PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        bot_binary_command_builder_smoke PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    if(MINGW)
        target_compile_options(bot_binary_command_builder_smoke PRIVATE -Wa,-mbig-obj)
    endif()
    if(OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        target_link_libraries(bot_binary_command_builder_smoke PRIVATE ${EXAMPLE_LIBS})
        if(WIN32)
            target_link_libraries(bot_binary_command_builder_smoke PRIVATE ${OPTIONX_WINDOWS_SYSTEM_LIBS})
        endif()
    else()
        target_link_libraries(bot_binary_command_builder_smoke PRIVATE ${EXAMPLE_LIBS} optionx_cpp)
    endif()

    if(OPTIONX_BUILD_DEPS AND NOT OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        add_dependencies(bot_binary_command_builder_smoke mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET bot_binary_command_builder_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:bot_binary_command_builder_smoke>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET bot_binary_command_builder_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:bot_binary_command_builder_smoke>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(metatrader_file_bridge_smoke examples/metatrader_file_bridge_smoke.cpp)
    target_compile_features(metatrader_file_bridge_smoke PRIVATE cxx_std_17)

    target_include_directories(metatrader_file_bridge_smoke PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
    )

    target_link_directories(metatrader_file_bridge_smoke PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        metatrader_file_bridge_smoke PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    if(MINGW)
        target_compile_options(metatrader_file_bridge_smoke PRIVATE -Wa,-mbig-obj)
    endif()
    if(OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        target_link_libraries(metatrader_file_bridge_smoke PRIVATE ${EXAMPLE_LIBS})
        if(WIN32)
            target_link_libraries(metatrader_file_bridge_smoke PRIVATE ${OPTIONX_WINDOWS_SYSTEM_LIBS})
        endif()
    else()
        target_link_libraries(metatrader_file_bridge_smoke PRIVATE ${EXAMPLE_LIBS} optionx_cpp)
    endif()

    if(OPTIONX_BUILD_DEPS AND NOT OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        add_dependencies(metatrader_file_bridge_smoke mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET metatrader_file_bridge_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:metatrader_file_bridge_smoke>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET metatrader_file_bridge_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:metatrader_file_bridge_smoke>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(metatrader_file_command_writer_smoke examples/metatrader_file_command_writer_smoke.cpp)
    target_compile_features(metatrader_file_command_writer_smoke PRIVATE cxx_std_17)

    target_include_directories(metatrader_file_command_writer_smoke PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
    )

    target_link_directories(metatrader_file_command_writer_smoke PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        metatrader_file_command_writer_smoke PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    if(MINGW)
        target_compile_options(metatrader_file_command_writer_smoke PRIVATE -Wa,-mbig-obj)
    endif()
    if(OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        target_link_libraries(metatrader_file_command_writer_smoke PRIVATE ${EXAMPLE_LIBS})
        if(WIN32)
            target_link_libraries(metatrader_file_command_writer_smoke PRIVATE ${OPTIONX_WINDOWS_SYSTEM_LIBS})
        endif()
    else()
        target_link_libraries(metatrader_file_command_writer_smoke PRIVATE ${EXAMPLE_LIBS} optionx_cpp)
    endif()

    if(OPTIONX_BUILD_DEPS AND NOT OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        add_dependencies(metatrader_file_command_writer_smoke mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET metatrader_file_command_writer_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:metatrader_file_command_writer_smoke>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET metatrader_file_command_writer_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:metatrader_file_command_writer_smoke>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(metatrader_file_end_to_end_smoke examples/metatrader_file_end_to_end_smoke.cpp)
    target_compile_features(metatrader_file_end_to_end_smoke PRIVATE cxx_std_17)

    target_include_directories(metatrader_file_end_to_end_smoke PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
    )

    target_link_directories(metatrader_file_end_to_end_smoke PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        metatrader_file_end_to_end_smoke PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    if(MINGW)
        target_compile_options(metatrader_file_end_to_end_smoke PRIVATE -Wa,-mbig-obj)
    endif()
    if(OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        target_link_libraries(metatrader_file_end_to_end_smoke PRIVATE ${EXAMPLE_LIBS})
        if(WIN32)
            target_link_libraries(metatrader_file_end_to_end_smoke PRIVATE ${OPTIONX_WINDOWS_SYSTEM_LIBS})
        endif()
    else()
        target_link_libraries(metatrader_file_end_to_end_smoke PRIVATE ${EXAMPLE_LIBS} optionx_cpp)
    endif()

    if(OPTIONX_BUILD_DEPS AND NOT OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        add_dependencies(metatrader_file_end_to_end_smoke mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET metatrader_file_end_to_end_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:metatrader_file_end_to_end_smoke>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET metatrader_file_end_to_end_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:metatrader_file_end_to_end_smoke>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(telegram_signal_bridge_smoke examples/telegram_signal_bridge_smoke.cpp)
    target_compile_features(telegram_signal_bridge_smoke PRIVATE cxx_std_17)
    target_include_directories(telegram_signal_bridge_smoke PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
    )
    target_link_directories(telegram_signal_bridge_smoke PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        telegram_signal_bridge_smoke PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    if(MINGW)
        target_compile_options(telegram_signal_bridge_smoke PRIVATE -Wa,-mbig-obj)
    elseif(MSVC)
        target_compile_options(telegram_signal_bridge_smoke PRIVATE /bigobj)
    endif()
    target_link_libraries(telegram_signal_bridge_smoke PRIVATE ${EXAMPLE_LIBS} optionx_cpp)

    add_executable(telegram_archive_parser_smoke examples/telegram_archive_parser_smoke.cpp)
    target_compile_features(telegram_archive_parser_smoke PRIVATE cxx_std_17)
    target_include_directories(telegram_archive_parser_smoke PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
    )
    target_link_directories(telegram_archive_parser_smoke PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        telegram_archive_parser_smoke PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    if(MINGW)
        target_compile_options(telegram_archive_parser_smoke PRIVATE -Wa,-mbig-obj)
    elseif(MSVC)
        target_compile_options(telegram_archive_parser_smoke PRIVATE /bigobj)
    endif()
    target_link_libraries(telegram_archive_parser_smoke PRIVATE ${EXAMPLE_LIBS} optionx_cpp)

    if(NOT TARGET tiny-process-library::tiny-process-library)
        add_subdirectory(
            ${CMAKE_CURRENT_SOURCE_DIR}/external/tg-client-stdio/external/cpp/tiny-process-library
            EXCLUDE_FROM_ALL
        )
    endif()

    add_executable(telegram_live_bridge_smoke examples/telegram_live_bridge_smoke.cpp)
    target_compile_features(telegram_live_bridge_smoke PRIVATE cxx_std_17)
    target_include_directories(telegram_live_bridge_smoke PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/tg-client-stdio/include
    )
    target_link_directories(telegram_live_bridge_smoke PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        telegram_live_bridge_smoke PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    if(MINGW)
        target_compile_options(telegram_live_bridge_smoke PRIVATE -Wa,-mbig-obj)
    elseif(MSVC)
        target_compile_options(telegram_live_bridge_smoke PRIVATE /bigobj)
    endif()
    target_link_libraries(telegram_live_bridge_smoke PRIVATE
        ${EXAMPLE_LIBS}
        optionx_cpp
        tiny-process-library::tiny-process-library
    )

    add_executable(protocol_v1_bridge_smoke examples/protocol_v1_bridge_smoke.cpp)
    target_compile_features(protocol_v1_bridge_smoke PRIVATE cxx_std_17)

    target_include_directories(protocol_v1_bridge_smoke PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/SimpleNamedPipe/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/Simple-Web-Server
    )

    target_link_directories(protocol_v1_bridge_smoke PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        protocol_v1_bridge_smoke PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    if(MINGW)
        target_compile_options(protocol_v1_bridge_smoke PRIVATE -Wa,-mbig-obj)
    elseif(MSVC)
        target_compile_options(protocol_v1_bridge_smoke PRIVATE /bigobj)
    endif()
    if(OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        target_link_libraries(
            protocol_v1_bridge_smoke PRIVATE
            ${EXAMPLE_LIBS}
            ${OPTIONX_NETWORK_LIBS}
        )
        if(WIN32)
            target_link_libraries(protocol_v1_bridge_smoke PRIVATE ${OPTIONX_WINDOWS_SYSTEM_LIBS})
        endif()
    else()
        target_link_libraries(protocol_v1_bridge_smoke PRIVATE ${EXAMPLE_LIBS} optionx_cpp)
    endif()

    if(OPTIONX_BUILD_DEPS AND NOT OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        add_dependencies(protocol_v1_bridge_smoke mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET protocol_v1_bridge_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:protocol_v1_bridge_smoke>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET protocol_v1_bridge_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:protocol_v1_bridge_smoke>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()

    add_executable(protocol_v1_named_pipe_bridge_smoke examples/protocol_v1_named_pipe_bridge_smoke.cpp)
    target_compile_features(protocol_v1_named_pipe_bridge_smoke PRIVATE cxx_std_17)

    target_include_directories(protocol_v1_named_pipe_bridge_smoke PRIVATE
        ${EXAMPLE_INCLUDE_DIRS}
        ${EXAMPLE_DEPS_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/Simple-Web-Server
        ${CMAKE_CURRENT_SOURCE_DIR}/external/SimpleNamedPipe/include
    )

    target_link_directories(protocol_v1_named_pipe_bridge_smoke PRIVATE ${EXAMPLE_LIBRARY_DIRS})
    target_compile_definitions(
        protocol_v1_named_pipe_bridge_smoke PRIVATE
        ${EXAMPLE_DEFINES}
        LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
    )
    if(MINGW)
        target_compile_options(protocol_v1_named_pipe_bridge_smoke PRIVATE -Wa,-mbig-obj)
    elseif(MSVC)
        target_compile_options(protocol_v1_named_pipe_bridge_smoke PRIVATE /bigobj)
    endif()
    if(OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        target_link_libraries(
            protocol_v1_named_pipe_bridge_smoke PRIVATE
            ${EXAMPLE_LIBS}
            ${OPTIONX_NETWORK_LIBS}
        )
        if(WIN32)
            target_link_libraries(protocol_v1_named_pipe_bridge_smoke PRIVATE ${OPTIONX_WINDOWS_SYSTEM_LIBS})
        endif()
    else()
        target_link_libraries(protocol_v1_named_pipe_bridge_smoke PRIVATE ${EXAMPLE_LIBS} optionx_cpp)
    endif()

    if(OPTIONX_BUILD_DEPS AND NOT OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        add_dependencies(protocol_v1_named_pipe_bridge_smoke mdbx-static AES)
    endif()

    foreach(dll ${EXAMPLE_DLL_FILES})
        add_custom_command(TARGET protocol_v1_named_pipe_bridge_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll}" "$<TARGET_FILE_DIR:protocol_v1_named_pipe_bridge_smoke>"
        )
    endforeach()

    if(WIN32)
        add_custom_command(TARGET protocol_v1_named_pipe_bridge_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DOPTIONX_RUNTIME_DLL_DIR="${EXAMPLE_BUILD_LIBS_DIR}/bin"
                -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:protocol_v1_named_pipe_bridge_smoke>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        )
    endif()
endif()

if(OPTIONX_BUILD_TESTS)
    if(OPTIONX_BUILD_DEPS)
        set(BUILD_LIBS_DIR ${OPTIONX_DEPS_OUTPUT_DIR})
    else()
        if(NOT OPTIONX_DEPS_BUILD_DIR)
            message(FATAL_ERROR "OPTIONX_BUILD_DEPS is OFF, but OPTIONX_DEPS_BUILD_DIR is not set")
        endif()
        set(BUILD_LIBS_DIR ${OPTIONX_DEPS_BUILD_DIR})
    endif()

    set(PROJECT_INCLUDE_DIRS
        ${OPTIONX_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/external/SimpleNamedPipe/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/Simple-Web-Server
        ${CMAKE_CURRENT_SOURCE_DIR}/external/time-shield-cpp/include
    )

    set(DEPS_INCLUDE_DIRS
        ${BUILD_LIBS_DIR}/include
    )

    set(PROJECT_LIBRARY_DIRS
        ${BUILD_LIBS_DIR}/lib
        ${BUILD_LIBS_DIR}/bin
    )

    file(GLOB DLL_FILES "${PROJECT_LIBRARY_DIRS}/*.dll")

    set(PROJECT_DEFINES
        ${OPTIONX_COMPILE_DEFINITIONS}
    )

    set(PROJECT_LIBS
        gtest
    )

    set(OPTIONX_LIGHTWEIGHT_TESTS
        bridge_host_test
        metatrader_paths_test
        telegram_dto_test
        telegram_signal_parser_test
        telegram_signal_bridge_test
        telegram_worker_source_test
        trading_view_bridge_test
    )
    if(OPTIONX_LIGHTWEIGHT_BRIDGE_SMOKE_TESTS)
        list(APPEND OPTIONX_LIGHTWEIGHT_TESTS
            bot_binary_bridge_test
            bot_binary_protocol_test
            bridge_umbrella_include_test
            metatrader_file_bridge_test
            metatrader_file_command_writer_test
            metatrader_file_config_include_test
            protocol_v1_bridge_test
        )
    endif()

    set(OPTIONX_LIGHTWEIGHT_NETWORK_TESTS
        bot_binary_bridge_test
        bridge_umbrella_include_test
        protocol_v1_bridge_test
        trading_view_bridge_test
    )

    file(GLOB_RECURSE PROJECT_HEADERS
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        include/*.hpp
    )
    add_custom_target(project_headers SOURCES ${PROJECT_HEADERS})

    file(GLOB_RECURSE TEST_SOURCES
        CONFIGURE_DEPENDS
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        tests/*.cpp
    )
    list(SORT TEST_SOURCES)

    set(TEST_SOURCE_STEMS)
    foreach(test_src ${TEST_SOURCES})
        get_filename_component(test_stem "${test_src}" NAME_WE)
        list(APPEND TEST_SOURCE_STEMS "${test_stem}")
    endforeach()

    set(TEST_TARGET_NAMES)
    set(TEST_TARGET_SOURCES)

    # Keep existing basename targets while they are unique. If recursive
    # discovery finds duplicate stems, use path-derived names for that stem.
    foreach(test_src ${TEST_SOURCES})
        get_filename_component(test_name "${test_src}" NAME_WE)
        set(test_name_count 0)
        foreach(test_stem ${TEST_SOURCE_STEMS})
            if(test_stem STREQUAL test_name)
                math(EXPR test_name_count "${test_name_count} + 1")
            endif()
        endforeach()

        if(test_name_count GREATER 1)
            set(test_name "${test_src}")
            string(REGEX REPLACE "\\.[^.]*$" "" test_name "${test_name}")
            string(MAKE_C_IDENTIFIER "${test_name}" test_name)
        endif()

        list(FIND TEST_TARGET_NAMES "${test_name}" existing_test_index)
        if(NOT existing_test_index EQUAL -1)
            list(GET TEST_TARGET_SOURCES ${existing_test_index} existing_test_src)
            message(FATAL_ERROR "Duplicate test target name '${test_name}' from '${test_src}' conflicts with '${existing_test_src}'")
        endif()

        if(TARGET ${test_name})
            message(FATAL_ERROR "Test target name '${test_name}' from '${test_src}' conflicts with an existing CMake target")
        endif()

        list(APPEND TEST_TARGET_NAMES "${test_name}")
        list(APPEND TEST_TARGET_SOURCES "${test_src}")

        add_executable(${test_name} ${test_src})
        target_compile_features(${test_name} PRIVATE cxx_std_17)

        target_include_directories(${test_name} PRIVATE
            ${PROJECT_INCLUDE_DIRS}
            ${DEPS_INCLUDE_DIRS}
        )

        target_link_directories(${test_name} PRIVATE ${PROJECT_LIBRARY_DIRS})
        target_compile_definitions(
            ${test_name} PRIVATE 
            ${PROJECT_DEFINES}
            LOGIT_BASE_PATH="${LOGIT_BASE_PATH_FWD}"
        )
        if(MSVC)
            target_compile_options(${test_name} PRIVATE /bigobj)
        endif()
        list(FIND OPTIONX_LIGHTWEIGHT_TESTS "${test_name}" lightweight_test_index)
        if(NOT lightweight_test_index EQUAL -1)
            target_link_libraries(
                ${test_name} PRIVATE
                ${PROJECT_LIBS}
            )
            list(FIND OPTIONX_LIGHTWEIGHT_NETWORK_TESTS "${test_name}" lightweight_network_test_index)
            if(NOT lightweight_network_test_index EQUAL -1)
                target_link_libraries(${test_name} PRIVATE ${OPTIONX_NETWORK_LIBS})
            endif()
            if(WIN32)
                target_link_libraries(${test_name} PRIVATE ${OPTIONX_WINDOWS_SYSTEM_LIBS})
            endif()
        else()
            target_link_libraries(${test_name} PRIVATE ${PROJECT_LIBS} optionx_cpp)
        endif()

        if(OPTIONX_BUILD_DEPS)
            if(NOT lightweight_test_index EQUAL -1)
                set(test_deps gtest)
            else()
                set(test_deps mdbx-static AES gtest)
            endif()
            foreach(test_dep ${test_deps})
                if(TARGET ${test_dep})
                    add_dependencies(${test_name} ${test_dep})
                endif()
            endforeach()
        endif()

        set_source_files_properties(${PROJECT_HEADERS} PROPERTIES HEADER_FILE_ONLY ON)
        target_sources(${test_name} PRIVATE ${PROJECT_HEADERS})

        foreach(dll ${DLL_FILES})
            add_custom_command(TARGET ${test_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${dll}" "$<TARGET_FILE_DIR:${test_name}>"
            )
        endforeach()

        if(WIN32)
            add_custom_command(TARGET ${test_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    -DOPTIONX_RUNTIME_DLL_DIR="${BUILD_LIBS_DIR}/bin"
                    -DOPTIONX_RUNTIME_TARGET_DIR="$<TARGET_FILE_DIR:${test_name}>"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
            )
        endif()
    endforeach()

    if(TARGET header_only_odr_test)
        target_sources(header_only_odr_test PRIVATE tests/header_only_odr_companion.cc)
    endif()

    if(TARGET intrade_bar_api_response_test)
        target_include_directories(intrade_bar_api_response_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/external/Simple-Web-Server
        )
    endif()
endif()
