cmake_minimum_required(VERSION 3.21)

set(REQUESTER_PYTHON "python3" CACHE STRING "Python interpreter for requester verification")
set(REQUESTER_CLANG_FORMAT "clang-format" CACHE STRING "clang-format executable for requester verification")
set(REQUESTER_CPPCHECK "cppcheck" CACHE STRING "cppcheck executable for requester verification")
set(REQUESTER_CLANG_TIDY "clang-tidy-21" CACHE STRING "clang-tidy executable for requester verification")
set(REQUESTER_VERIFY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../tools/verify_sources.py")
set(REQUESTER_VERIFY_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/verify_sources.json")
set(WGNX_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/libs/wireguard-nx/common" CACHE PATH
    "Path to the WireGuard NX shared IPC headers")

add_custom_target(format-check
    COMMAND "${REQUESTER_PYTHON}" "${REQUESTER_VERIFY_SOURCES}" format
            --root "${CMAKE_CURRENT_SOURCE_DIR}"
            --config "${REQUESTER_VERIFY_CONFIG}"
            --formatter "${REQUESTER_CLANG_FORMAT}"
    VERBATIM)

add_custom_target(static-check
    COMMAND "${REQUESTER_PYTHON}" "${REQUESTER_VERIFY_SOURCES}" static
            --root "${CMAKE_CURRENT_SOURCE_DIR}"
            --config "${REQUESTER_VERIFY_CONFIG}"
            --analyzer "${REQUESTER_CPPCHECK}"
    VERBATIM)

add_custom_target(tidy-check
    COMMAND "${CMAKE_COMMAND}" -E env "WGNX_COMMON=${WGNX_COMMON}"
            "${REQUESTER_PYTHON}" "${REQUESTER_VERIFY_SOURCES}" tidy
            --root "${CMAKE_CURRENT_SOURCE_DIR}"
            --config "${REQUESTER_VERIFY_CONFIG}"
            --tidy "${REQUESTER_CLANG_TIDY}"
    VERBATIM)

option(REQUESTER_BUILD_HOST_TESTS "Build requester host-only tests" OFF)

if(REQUESTER_BUILD_HOST_TESTS)
    project(requester_host_tests LANGUAGES CXX)
    enable_testing()
    add_executable(requester_bsd_outcome_tests
        src/bsd_system_udp_outcome.cpp
        test/bsd_system_udp_outcome_tests.cpp)
    target_include_directories(requester_bsd_outcome_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(requester_bsd_outcome_tests PRIVATE cxx_std_23)
    target_compile_options(requester_bsd_outcome_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME requester_bsd_outcome_tests COMMAND requester_bsd_outcome_tests)
    return()
endif()

set(BOREALIS_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/libs/borealis/library")
set(PLATFORM_SWITCH ON CACHE BOOL "Build for Nintendo Switch" FORCE)
set(USE_DEKO3D ON CACHE BOOL "Use Borealis' Deko3D backend" FORCE)

include("${BOREALIS_LIBRARY}/cmake/commonOption.cmake")
include("${BOREALIS_LIBRARY}/cmake/toolchain.cmake")

project(requester LANGUAGES C CXX)

set(APP_TITLE "NX Reversing Requester")
set(APP_AUTHOR "chrisbraucker")
set(APP_VERSION "0.1.0")
set(APP_TITLEID "010000000000EAD2")
set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png")
set(APP_PROJECT_URL "https://github.com/chrisbraucker/nx-reverse")
set(BOREALIS_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/borealis/resources")
set(REQUESTER_ROMFS_DIR "${CMAKE_BINARY_DIR}/romfs")
option(REQUESTER_PACKET_DIAGNOSTICS
       "Emit requester packet-granularity diagnostics" OFF)
file(GLOB BOREALIS_SYSTEM_RESOURCES CONFIGURE_DEPENDS
    "${BOREALIS_RESOURCES}/img/sys/*")

if(NOT EXISTS "${WGNX_COMMON}/include/wgnx/tunnel_protocol.hpp")
    message(FATAL_ERROR
        "WGNX_COMMON must contain the current WireGuard NX tunnel IPC headers. "
        "Use -DWGNX_COMMON=/path/to/wireguard-nx.git/common for a parallel checkout.")
endif()

execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE REQUESTER_BUILD_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(REQUESTER_BUILD_ID STREQUAL "")
    set(REQUESTER_BUILD_ID "unknown")
endif()

add_subdirectory("${BOREALIS_LIBRARY}" "${CMAKE_BINARY_DIR}/borealis")
find_package(Threads REQUIRED)

file(GLOB REQUESTER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
program_target(requester "${REQUESTER_SOURCES}")
set_target_properties(requester PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

target_include_directories(requester PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/include"
    "${WGNX_COMMON}/include"
    ${APP_PLATFORM_INCLUDE})
target_compile_definitions(requester PRIVATE
    TARGET="requester"
    APP_TITLE="${APP_TITLE}"
    APP_AUTHOR="${APP_AUTHOR}"
    APP_PROJECT_URL="${APP_PROJECT_URL}"
    NXRV_VERSION="${APP_VERSION}"
    NXRV_BUILD_ID="${REQUESTER_BUILD_ID}"
    REQUESTER_PACKET_DIAGNOSTICS=$<BOOL:${REQUESTER_PACKET_DIAGNOSTICS}>)
target_compile_options(requester PRIVATE
    -g
    -Wall
    -Werror
    -Wno-error=deprecated-literal-operator
    -O2
    -ffunction-sections
    -fdata-sections
    -march=armv8-a+crc+crypto
    -mtune=cortex-a57
    -mtp=soft
    ${APP_PLATFORM_OPTION})
target_link_options(requester PRIVATE
    -g
    -Wl,--gc-sections
    -Wl,-Map,requester.map)
target_link_libraries(requester PRIVATE borealis curl z nx m Threads::Threads
                      ${APP_PLATFORM_LIB})

if(USE_DEKO3D)
    gen_dksh("${REQUESTER_ROMFS_DIR}/shaders")
endif()

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/requester.nro"
    COMMAND "${NX_NACPTOOL_EXE}" --create "${APP_TITLE}" "${APP_AUTHOR}"
            "${APP_VERSION}" requester.nacp --titleid=${APP_TITLEID}
    # Clear the previously broad Borealis resource staging without removing
    # the Deko3D shaders generated in the RomFS directory at configure time.
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${REQUESTER_ROMFS_DIR}/.gitignore"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${REQUESTER_ROMFS_DIR}/font"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${REQUESTER_ROMFS_DIR}/i18n"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${REQUESTER_ROMFS_DIR}/icon"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${REQUESTER_ROMFS_DIR}/img"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${REQUESTER_ROMFS_DIR}/material"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${REQUESTER_ROMFS_DIR}/opencc"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${REQUESTER_ROMFS_DIR}/xml"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${REQUESTER_ROMFS_DIR}/i18n/en-US"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${REQUESTER_ROMFS_DIR}/material"
    # Borealis Switch runtime requirements for the current requester UI.
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${BOREALIS_RESOURCES}/i18n/en-US/hints.json"
            "${REQUESTER_ROMFS_DIR}/i18n/en-US/hints.json"
    # Borealis uses this complete directory for cursor and system-status UI.
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${BOREALIS_RESOURCES}/img/sys"
            "${REQUESTER_ROMFS_DIR}/img/sys"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${BOREALIS_RESOURCES}/material/MaterialIcons-Regular.ttf"
            "${REQUESTER_ROMFS_DIR}/material/MaterialIcons-Regular.ttf"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${REQUESTER_ROMFS_DIR}/img"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${APP_ICON}"
            "${REQUESTER_ROMFS_DIR}/img/requester.png"
    COMMAND "${NX_ELF2NRO_EXE}" requester.elf requester.nro --icon=${APP_ICON}
            --nacp=requester.nacp --romfsdir=${REQUESTER_ROMFS_DIR}
    DEPENDS requester
            "${APP_ICON}"
            "${BOREALIS_RESOURCES}/i18n/en-US/hints.json"
            ${BOREALIS_SYSTEM_RESOURCES}
            "${BOREALIS_RESOURCES}/material/MaterialIcons-Regular.ttf"
    VERBATIM)
add_custom_target(requester_nro ALL DEPENDS "${CMAKE_BINARY_DIR}/requester.nro")

add_custom_command(TARGET requester_nro POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/out"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_BINARY_DIR}/requester.nro"
            "${CMAKE_CURRENT_SOURCE_DIR}/out/requester.nro")
