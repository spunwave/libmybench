# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Martin Dolez

# The device model: registry, enumeration, open/close, facet lookup. No driver code. Its public
# headers (device.h, facets.h, driver.h) are in the shared include/libmybench/.
libmybench_module(core SOURCES src/core.c src/sys.c)

if(LIBMYBENCH_BUILD_TESTS)
    add_executable(core_tests tests/test_core.c)
    target_link_libraries(core_tests PRIVATE libmybench::core)
    libmybench_warnings(core_tests)
    add_test(NAME core COMMAND core_tests)
endif()
