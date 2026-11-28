# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Martin Dolez
#
# libmybench: the open device library of MYBENCH. Plain C11, no Qt. Each device family is its
# own target under drivers/, switched on and off with LIBMYBENCH_DRIVER_<FAMILY>.
cmake_minimum_required(VERSION 3.16)
project(libmybench VERSION 0.1.0 LANGUAGES C)

set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

option(LIBMYBENCH_DRIVER_PPK2 "Build the Nordic PPK2 driver" ON)
option(LIBMYBENCH_TRANSPORT_SERIAL "Build the serial-port transport (needed by the PPK2 device layer)" ON)
option(LIBMYBENCH_BUILD_TESTS "Build the unit tests" ON)
option(LIBMYBENCH_BUILD_TOOLS "Build the command-line tools" ON)
option(LIBMYBENCH_WERROR "Treat warnings as errors" OFF)
option(LIBMYBENCH_FUZZ "Build libFuzzer targets (clang only)" OFF)

if(NOT CMAKE_C_STANDARD)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)
endif()
set(CMAKE_C_VISIBILITY_PRESET hidden)

include(GNUInstallDirs)

# Windows finds DLLs next to the executable, not through an rpath: with BUILD_SHARED_LIBS the
# tests and tools would miss the libraries built in other folders, and the "DLL not found" dialog
# blocks a headless run forever. Put every executable and DLL of this build in one bin/ folder.
if(WIN32 AND CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR AND NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()
include(cmake/LibmybenchModule.cmake)

if(LIBMYBENCH_FUZZ)
    # Coverage feedback from the library code itself, not only from the fuzz harnesses.
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()

# Headers shared by every module (export macro).
add_library(libmybench_base INTERFACE)
add_library(libmybench::base ALIAS libmybench_base)
set_target_properties(libmybench_base PROPERTIES EXPORT_NAME base)
target_include_directories(libmybench_base INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
install(TARGETS libmybench_base EXPORT libmybenchTargets)
install(DIRECTORY include/libmybench DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(LIBMYBENCH_BUILD_TESTS)
    enable_testing()
endif()

# Vendored miniz (MIT): ZIP for file formats. An object library, built without our warning flags
# and linked into the modules that need it, so it is never a separate library to install.
add_library(libmybench_miniz OBJECT third_party/miniz/miniz.c)
target_include_directories(libmybench_miniz PRIVATE third_party/miniz)
target_compile_definitions(libmybench_miniz PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES _LARGEFILE64_SOURCE)
set_target_properties(libmybench_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
if(MSVC)
    target_compile_options(libmybench_miniz PRIVATE /w)
else()
    target_compile_options(libmybench_miniz PRIVATE -w)
endif()

add_subdirectory(core)
if(LIBMYBENCH_TRANSPORT_SERIAL)
    add_subdirectory(transports/serial)
endif()
if(LIBMYBENCH_DRIVER_PPK2)
    add_subdirectory(drivers/ppk2)
endif()
add_subdirectory(builtin)
if(LIBMYBENCH_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

include(CMakePackageConfigHelpers)
install(EXPORT libmybenchTargets NAMESPACE libmybench::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmybench)
configure_package_config_file(cmake/libmybenchConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/libmybenchConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmybench)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/libmybenchConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libmybenchConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/libmybenchConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libmybench)
install(FILES LICENSE LICENSING.md DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/libmybench)
