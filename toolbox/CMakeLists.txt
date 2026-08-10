cmake_minimum_required(VERSION 3.21)

set(TOOLBOX_PYTHON "python3" CACHE STRING "Python interpreter for toolbox verification")
set(TOOLBOX_CLANG_FORMAT "clang-format" CACHE STRING "clang-format executable for toolbox verification")
set(TOOLBOX_CPPCHECK "cppcheck" CACHE STRING "cppcheck executable for toolbox verification")
set(TOOLBOX_CLANG_TIDY "clang-tidy-21" CACHE STRING "clang-tidy executable for toolbox verification")
set(TOOLBOX_VERIFY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../tools/verify_sources.py")
set(TOOLBOX_VERIFY_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/verify_sources.json")
set(WGNX_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/libs/wireguard-nx/common" CACHE PATH
    "Path to the WireGuard NX shared IPC headers")

add_custom_target(format-check
    COMMAND "${TOOLBOX_PYTHON}" "${TOOLBOX_VERIFY_SOURCES}" format
            --root "${CMAKE_CURRENT_SOURCE_DIR}"
            --config "${TOOLBOX_VERIFY_CONFIG}"
            --formatter "${TOOLBOX_CLANG_FORMAT}"
    VERBATIM)

add_custom_target(static-check
    COMMAND "${TOOLBOX_PYTHON}" "${TOOLBOX_VERIFY_SOURCES}" static
            --root "${CMAKE_CURRENT_SOURCE_DIR}"
            --config "${TOOLBOX_VERIFY_CONFIG}"
            --analyzer "${TOOLBOX_CPPCHECK}"
    VERBATIM)

add_custom_target(tidy-check
    COMMAND "${CMAKE_COMMAND}" -E env "WGNX_COMMON=${WGNX_COMMON}"
            "${TOOLBOX_PYTHON}" "${TOOLBOX_VERIFY_SOURCES}" tidy
            --root "${CMAKE_CURRENT_SOURCE_DIR}"
            --config "${TOOLBOX_VERIFY_CONFIG}"
            --tidy "${TOOLBOX_CLANG_TIDY}"
    VERBATIM)

option(TOOLBOX_BUILD_HOST_TESTS "Build toolbox host-only tests" OFF)

if(TOOLBOX_BUILD_HOST_TESTS)
    project(toolbox_host_tests LANGUAGES CXX)
    enable_testing()
    add_executable(toolbox_bsd_outcome_tests
        src/bsd_system_udp_outcome.cpp
        test/bsd_system_udp_outcome_tests.cpp)
    target_include_directories(toolbox_bsd_outcome_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(toolbox_bsd_outcome_tests PRIVATE cxx_std_23)
    target_compile_options(toolbox_bsd_outcome_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME toolbox_bsd_outcome_tests COMMAND toolbox_bsd_outcome_tests)
    add_executable(toolbox_udp_workload_metrics_tests test/udp_workload_metrics_tests.cpp)
    target_include_directories(toolbox_udp_workload_metrics_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(toolbox_udp_workload_metrics_tests PRIVATE cxx_std_23)
    target_compile_options(toolbox_udp_workload_metrics_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME toolbox_udp_workload_metrics_tests COMMAND toolbox_udp_workload_metrics_tests)
    add_executable(toolbox_runtime_scenario_tests
        src/runtime_scenario.cpp
        test/runtime_scenario_tests.cpp)
    target_include_directories(toolbox_runtime_scenario_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${WGNX_COMMON}/include")
    target_compile_features(toolbox_runtime_scenario_tests PRIVATE cxx_std_23)
    target_compile_options(toolbox_runtime_scenario_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME toolbox_runtime_scenario_tests COMMAND toolbox_runtime_scenario_tests)
    add_executable(toolbox_runtime_config_tests
        src/runtime_config.cpp
        src/runtime_scenario.cpp
        test/runtime_config_tests.cpp)
    target_include_directories(toolbox_runtime_config_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${WGNX_COMMON}/include")
    target_compile_features(toolbox_runtime_config_tests PRIVATE cxx_std_23)
    target_compile_options(toolbox_runtime_config_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME toolbox_runtime_config_tests COMMAND toolbox_runtime_config_tests)
    return()
endif()

set(BOREALIS_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/libs/borealis/library")
set(PLATFORM_SWITCH ON CACHE BOOL "Build for Nintendo Switch" FORCE)
set(USE_DEKO3D ON CACHE BOOL "Use Borealis' Deko3D backend" FORCE)

include("${BOREALIS_LIBRARY}/cmake/commonOption.cmake")
include("${BOREALIS_LIBRARY}/cmake/toolchain.cmake")

project(toolbox LANGUAGES C CXX)

set(APP_TITLE "NX Reversing Toolbox")
set(APP_AUTHOR "chrisbraucker")
set(APP_VERSION "0.1.0")
set(APP_TITLEID "010000000000EAD2")
set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png")
set(APP_PROJECT_URL "https://github.com/chrisbraucker/nx-reverse")
set(BOREALIS_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/borealis/resources")
set(TOOLBOX_ROMFS_DIR "${CMAKE_BINARY_DIR}/romfs")
option(TOOLBOX_PACKET_DIAGNOSTICS
       "Emit toolbox packet-granularity diagnostics" OFF)
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
    OUTPUT_VARIABLE TOOLBOX_BUILD_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(TOOLBOX_BUILD_ID STREQUAL "")
    set(TOOLBOX_BUILD_ID "unknown")
endif()

add_subdirectory("${BOREALIS_LIBRARY}" "${CMAKE_BINARY_DIR}/borealis")
find_package(Threads REQUIRED)

file(GLOB TOOLBOX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
program_target(toolbox "${TOOLBOX_SOURCES}")
set_target_properties(toolbox PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)

target_include_directories(toolbox PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/include"
    "${WGNX_COMMON}/include"
    ${APP_PLATFORM_INCLUDE})
target_compile_definitions(toolbox PRIVATE
    TARGET="toolbox"
    APP_TITLE="${APP_TITLE}"
    APP_AUTHOR="${APP_AUTHOR}"
    APP_PROJECT_URL="${APP_PROJECT_URL}"
    NXRV_VERSION="${APP_VERSION}"
    NXRV_BUILD_ID="${TOOLBOX_BUILD_ID}"
    TOOLBOX_PACKET_DIAGNOSTICS=$<BOOL:${TOOLBOX_PACKET_DIAGNOSTICS}>)
target_compile_options(toolbox PRIVATE
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
target_link_options(toolbox PRIVATE
    -g
    -Wl,--gc-sections
    -Wl,-Map,toolbox.map)
target_link_libraries(toolbox PRIVATE borealis curl z nx m Threads::Threads
                      ${APP_PLATFORM_LIB})

if(USE_DEKO3D)
    gen_dksh("${TOOLBOX_ROMFS_DIR}/shaders")
endif()

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/toolbox.nro"
    COMMAND "${NX_NACPTOOL_EXE}" --create "${APP_TITLE}" "${APP_AUTHOR}"
            "${APP_VERSION}" toolbox.nacp --titleid=${APP_TITLEID}
    # Clear the previously broad Borealis resource staging without removing
    # the Deko3D shaders generated in the RomFS directory at configure time.
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${TOOLBOX_ROMFS_DIR}/.gitignore"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${TOOLBOX_ROMFS_DIR}/font"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${TOOLBOX_ROMFS_DIR}/i18n"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${TOOLBOX_ROMFS_DIR}/icon"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${TOOLBOX_ROMFS_DIR}/img"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${TOOLBOX_ROMFS_DIR}/material"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${TOOLBOX_ROMFS_DIR}/opencc"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${TOOLBOX_ROMFS_DIR}/xml"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${TOOLBOX_ROMFS_DIR}/i18n/en-US"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${TOOLBOX_ROMFS_DIR}/material"
    # Borealis Switch runtime requirements for the current toolbox UI.
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${BOREALIS_RESOURCES}/i18n/en-US/hints.json"
            "${TOOLBOX_ROMFS_DIR}/i18n/en-US/hints.json"
    # Borealis uses this complete directory for cursor and system-status UI.
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${BOREALIS_RESOURCES}/img/sys"
            "${TOOLBOX_ROMFS_DIR}/img/sys"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${BOREALIS_RESOURCES}/material/MaterialIcons-Regular.ttf"
            "${TOOLBOX_ROMFS_DIR}/material/MaterialIcons-Regular.ttf"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${TOOLBOX_ROMFS_DIR}/img"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${APP_ICON}"
            "${TOOLBOX_ROMFS_DIR}/img/toolbox.png"
    COMMAND "${NX_ELF2NRO_EXE}" toolbox.elf toolbox.nro --icon=${APP_ICON}
            --nacp=toolbox.nacp --romfsdir=${TOOLBOX_ROMFS_DIR}
    DEPENDS toolbox
            "${APP_ICON}"
            "${BOREALIS_RESOURCES}/i18n/en-US/hints.json"
            ${BOREALIS_SYSTEM_RESOURCES}
            "${BOREALIS_RESOURCES}/material/MaterialIcons-Regular.ttf"
    VERBATIM)
add_custom_target(toolbox_nro ALL DEPENDS "${CMAKE_BINARY_DIR}/toolbox.nro")

add_custom_command(TARGET toolbox_nro POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/out"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_BINARY_DIR}/toolbox.nro"
            "${CMAKE_CURRENT_SOURCE_DIR}/out/toolbox.nro")
