# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Martin Dolez

set(_ppk2_dev_sources)
if(TARGET libmybench_serial)
    set(_ppk2_dev_sources src/ppk2_dev.c)   # the device layer needs the serial transport
endif()
libmybench_module(ppk2 SOURCES
    ${_ppk2_dev_sources}
    src/ppk2_ctl.c
    src/ppk2_family.c
    src/ppk2_file.c
    src/ppk2_meta.c
    src/ppk2_sim.c
    src/ppk2_stream.c
    src/text.c)
# The device-model adapter (ppk2_family.c): facets over ppk2_dev, and the simulator.
target_link_libraries(libmybench_ppk2 PUBLIC libmybench::core)
if(TARGET libmybench_serial)
    target_link_libraries(libmybench_ppk2 PUBLIC libmybench::serial)
    target_compile_definitions(libmybench_ppk2 PRIVATE LMB_PPK2_HAVE_SERIAL)
endif()
# .ppk2 archives: the vendored miniz, compiled into this library (no zlib-compatible names).
target_sources(libmybench_ppk2 PRIVATE $<TARGET_OBJECTS:libmybench_miniz>)
target_include_directories(libmybench_ppk2 PRIVATE ${PROJECT_SOURCE_DIR}/third_party/miniz)
target_compile_definitions(libmybench_ppk2 PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

if(LIBMYBENCH_BUILD_TESTS)
    add_executable(ppk2_tests tests/test_ppk2.c src/text.c)
    target_include_directories(ppk2_tests PRIVATE src)
    target_link_libraries(ppk2_tests PRIVATE libmybench::ppk2)
    libmybench_warnings(ppk2_tests)
    add_test(NAME ppk2 COMMAND ppk2_tests)

    # .ppk2 files. Builds hand-made archives with miniz, so it compiles its own copy (the
    # library's is hidden in shared builds).
    add_executable(ppk2_file_tests tests/test_ppk2_file.c $<TARGET_OBJECTS:libmybench_miniz>)
    target_include_directories(ppk2_file_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/miniz)
    # Its own folder: ppk2_tool_roundtrip writes roundtrip.ppk2 too, and ctest -j runs both at once.
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/file-tests)
    target_compile_definitions(ppk2_file_tests PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES
        PPK2_TEST_TMPDIR="${CMAKE_CURRENT_BINARY_DIR}/file-tests")
    target_link_libraries(ppk2_file_tests PRIVATE libmybench::ppk2)
    libmybench_warnings(ppk2_file_tests)
    add_test(NAME ppk2_file COMMAND ppk2_file_tests)

    # The device layer against a simulated PPK2 behind a pseudo-terminal (POSIX only).
    if(UNIX AND TARGET libmybench_serial)
        find_package(Threads REQUIRED)
        add_executable(ppk2_dev_tests tests/test_ppk2_dev.c tools/sim_port.c)
        target_include_directories(ppk2_dev_tests PRIVATE tools)
        target_link_libraries(ppk2_dev_tests PRIVATE libmybench::ppk2 Threads::Threads)
        libmybench_warnings(ppk2_dev_tests)
        add_test(NAME ppk2_dev COMMAND ppk2_dev_tests)
        set_tests_properties(ppk2_dev PROPERTIES TIMEOUT 60)
    endif()
    # The family adapter on the simulator: facets, streaming, error paths.
    add_executable(ppk2_family_tests tests/test_ppk2_family.c)
    target_link_libraries(ppk2_family_tests PRIVATE libmybench::ppk2)
    if(UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(ppk2_family_tests PRIVATE Threads::Threads)
    endif()
    libmybench_warnings(ppk2_family_tests)
    add_test(NAME ppk2_family COMMAND ppk2_family_tests)
    set_tests_properties(ppk2_family PROPERTIES TIMEOUT 60)
endif()

if(LIBMYBENCH_BUILD_TOOLS)
    add_executable(ppk2-tool tools/ppk2_tool.c)
    target_link_libraries(ppk2-tool PRIVATE libmybench::ppk2)
    if(UNIX)
        find_package(Threads REQUIRED)
        target_sources(ppk2-tool PRIVATE tools/sim_port.c)
        target_link_libraries(ppk2-tool PRIVATE Threads::Threads)
    endif()
    libmybench_warnings(ppk2-tool)
    install(TARGETS ppk2-tool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(LIBMYBENCH_BUILD_TESTS)
        # Round trip through the tool: simulate a capture, decode it, check the summary line.
        add_test(NAME ppk2_tool_roundtrip COMMAND ${CMAKE_COMMAND}
            -DTOOL=$<TARGET_FILE:ppk2-tool> -DWORK=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tool_roundtrip.cmake)
    endif()
endif()

if(LIBMYBENCH_FUZZ)
    foreach(f meta stream file)
        add_executable(ppk2_fuzz_${f} tests/fuzz_${f}.c)
        target_link_libraries(ppk2_fuzz_${f} PRIVATE libmybench::ppk2)
        target_compile_options(ppk2_fuzz_${f} PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_options(ppk2_fuzz_${f} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
endif()
