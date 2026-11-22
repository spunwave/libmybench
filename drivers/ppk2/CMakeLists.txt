# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Martin Dolez

libmybench_module(ppk2 SOURCES
    src/ppk2_ctl.c
    src/ppk2_meta.c
    src/ppk2_sim.c
    src/ppk2_stream.c
    src/text.c)

if(LIBMYBENCH_BUILD_TESTS)
    add_executable(ppk2_tests tests/test_ppk2.c src/text.c)
    target_include_directories(ppk2_tests PRIVATE src)
    target_link_libraries(ppk2_tests PRIVATE libmybench::ppk2)
    libmybench_warnings(ppk2_tests)
    add_test(NAME ppk2 COMMAND ppk2_tests)
endif()

if(LIBMYBENCH_BUILD_TOOLS)
    add_executable(ppk2-tool tools/ppk2_tool.c)
    target_link_libraries(ppk2-tool PRIVATE libmybench::ppk2)
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
    foreach(f meta stream)
        add_executable(ppk2_fuzz_${f} tests/fuzz_${f}.c)
        target_link_libraries(ppk2_fuzz_${f} PRIVATE libmybench::ppk2)
        target_compile_options(ppk2_fuzz_${f} PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_options(ppk2_fuzz_${f} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
endif()
