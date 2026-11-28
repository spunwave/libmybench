# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Martin Dolez

# lmb_register_builtin_families: one place that knows which families this build contains, so an
# application links libmybench::builtin and needs no family-specific code. Each enabled family
# adds a definition and a link here.
libmybench_module(builtin SOURCES builtin.c)
target_link_libraries(libmybench_builtin PUBLIC libmybench::core)
if(TARGET libmybench_ppk2)
    target_link_libraries(libmybench_builtin PUBLIC libmybench::ppk2)
    target_compile_definitions(libmybench_builtin PRIVATE LMB_HAVE_PPK2)
endif()
