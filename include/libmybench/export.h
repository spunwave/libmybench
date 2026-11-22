/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Symbol export for every libmybench module. Static builds define LIBMYBENCH_STATIC (the CMake
 * targets do it for their users); shared builds export only what is marked LMB_API. */
#ifndef LIBMYBENCH_EXPORT_H
#define LIBMYBENCH_EXPORT_H

#if defined(LIBMYBENCH_STATIC)
#  define LMB_API
#elif defined(_WIN32)
#  if defined(LIBMYBENCH_BUILDING)
#    define LMB_API __declspec(dllexport)
#  else
#    define LMB_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define LMB_API __attribute__((visibility("default")))
#else
#  define LMB_API
#endif

#endif
