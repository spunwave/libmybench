# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Martin Dolez

# libmybench_module(<name> SOURCES ...): one driver library, target libmybench_<name>,
# alias libmybench::<name>, public headers in <dir>/include/libmybench/.
function(libmybench_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    set(tgt libmybench_${name})
    add_library(${tgt} ${ARG_SOURCES})
    add_library(libmybench::${name} ALIAS ${tgt})
    # A module without headers of its own (the core: its headers are the shared include/) has no include/.
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_include_directories(${tgt}
            PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                   $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    endif()
    target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(${tgt} PUBLIC libmybench_base ${ARG_LIBS})
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(${tgt} PRIVATE Threads::Threads)
    endif()
    target_compile_definitions(${tgt} PRIVATE LIBMYBENCH_BUILDING)
    if(NOT BUILD_SHARED_LIBS)
        target_compile_definitions(${tgt} PUBLIC LIBMYBENCH_STATIC)
    endif()
    if(NOT MSVC)
        target_link_libraries(${tgt} PUBLIC m)
    endif()
    libmybench_warnings(${tgt})
    set_target_properties(${tgt} PROPERTIES OUTPUT_NAME mybench-${name} EXPORT_NAME ${name}
        VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
    install(TARGETS ${tgt} EXPORT libmybenchTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include)
        install(DIRECTORY include/libmybench DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    endif()
endfunction()

function(libmybench_warnings tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4)
        target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(LIBMYBENCH_WERROR)
            target_compile_options(${tgt} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
            -Wno-sign-conversion -Wstrict-prototypes -Wmissing-prototypes)
        if(LIBMYBENCH_WERROR)
            target_compile_options(${tgt} PRIVATE -Werror)
        endif()
    endif()
endfunction()
