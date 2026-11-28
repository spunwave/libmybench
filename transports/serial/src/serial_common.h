/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Shared by the platform files: bounded string copy and a sorted port list. */
#ifndef LIBMYBENCH_SERIAL_COMMON_H
#define LIBMYBENCH_SERIAL_COMMON_H

#include "libmybench/serial.h"

#include <stddef.h>

void lmb_serial_copy(char *dst, size_t cap, const char *src);

typedef struct lmb_serial_list_builder {
    lmb_serial_port *out;
    size_t cap;
    lmb_serial_port *all; /* every port found, grown as needed; NULL after an allocation failure */
    size_t count, alloc;
    size_t lost;          /* ports counted but not kept (allocation failure) */
} lmb_serial_list_builder;

void lmb_serial_list_begin(lmb_serial_list_builder *b, lmb_serial_port *out, size_t cap);
void lmb_serial_list_add(lmb_serial_list_builder *b, const lmb_serial_port *port);
/* Sorts by path, copies the first cap entries out, frees the rest. Returns the number found. */
size_t lmb_serial_list_end(lmb_serial_list_builder *b);

#endif
