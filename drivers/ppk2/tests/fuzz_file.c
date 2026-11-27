/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* libFuzzer target (LIBMYBENCH_FUZZ=ON, clang): .ppk2 archives and metadata.json. */

#include "libmybench/ppk2.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    ppk2_file_info info;
    (void)ppk2_file_parse_metadata((const char *)data, size, &info);

    ppk2_file_reader *r = NULL;
    if (ppk2_file_open_mem(data, size, &r) == PPK2_OK) {
        float ua[64];
        uint16_t st[64];
        size_t got;
        unsigned rounds = 0;
        while (ppk2_file_read(r, ua, st, 64, &got) == PPK2_OK && got && ++rounds < 1000)
            ;
        ppk2_file_close(r);
    }
    return 0;
}
