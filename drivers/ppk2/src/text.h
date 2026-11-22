/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Locale-independent number text. strtod() and printf("%g") follow LC_NUMERIC, and a Qt app
 * sets it from the user's environment, so "1.5" would not parse on a French or Dutch desktop. */
#ifndef LIBMYBENCH_PPK2_TEXT_H
#define LIBMYBENCH_PPK2_TEXT_H

#include <stddef.h>

/* Parses the whole string ("-12.5e-3", surrounding spaces allowed). 0 on success, -1 otherwise
 * (empty, trailing garbage, or not finite). */
int lmb_parse_double(const char *s, double *out);

/* Formats with up to 10 significant digits and '.' as the decimal point. Returns the length,
 * or -1 if it doesn't fit. */
int lmb_format_double(char *buf, size_t cap, double v);

#endif
