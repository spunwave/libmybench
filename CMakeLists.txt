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
include(cmake/LibmybenchModule.cmake)

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

if(LIBMYBENCH_DRIVER_PPK2)
    add_subdirectory(drivers/ppk2)
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
