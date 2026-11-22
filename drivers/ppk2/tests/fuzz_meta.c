/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* libFuzzer target (LIBMYBENCH_FUZZ=ON, clang). */

#include "libmybench/ppk2.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* First byte picks a split point so partial-line handling is exercised too. */
    ppk2_meta_parser *p = ppk2_meta_parser_new();
    size_t cut = size ? data[0] % (size + 1) : 0;
    size_t a = ppk2_meta_parser_feed(p, data, cut);
    if (a == cut)
        ppk2_meta_parser_feed(p, data + cut, size - cut);
    ppk2_cal cal;
    ppk2_cal_from_metadata(&cal, ppk2_meta_parser_result(p));
    for (unsigned r = 0; r < 8; r++)
        (void)ppk2_cal_current(&cal, 1234, r, 3.3);
    ppk2_meta_parser_free(p);
    return 0;
}
