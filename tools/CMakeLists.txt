# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Martin Dolez

# lmb-tool: the generic client. It links the built-in family list and uses only the public
# device and facet API.
add_executable(lmb-tool lmb_tool.c)
target_link_libraries(lmb-tool PRIVATE libmybench::builtin)
libmybench_warnings(lmb-tool)
install(TARGETS lmb-tool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(LIBMYBENCH_BUILD_TESTS AND LIBMYBENCH_DRIVER_PPK2)
    add_test(NAME lmb_tool COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:lmb-tool>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tool_smoke.cmake)
    set_tests_properties(lmb_tool PROPERTIES TIMEOUT 60)
endif()
