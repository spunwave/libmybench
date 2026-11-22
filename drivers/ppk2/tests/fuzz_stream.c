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
    ppk2_stream *s = ppk2_stream_new();
    ppk2_cal cal;
    ppk2_cal_default(&cal);
    ppk2_stream_set_calibration(s, &cal, 3.3);
    ppk2_sample out[7];
    size_t off = 0;
    /* Uneven chunk sizes taken from the data itself. */
    while (off < size) {
        size_t chunk = 1 + data[off] % 13;
        if (chunk > size - off)
            chunk = size - off;
        size_t done = 0;
        while (done < chunk) {
            size_t used;
            ppk2_stream_decode(s, data + off + done, chunk - done, &used, out, 7);
            done += used;
        }
        off += chunk;
    }
    ppk2_stream_free(s);
    return 0;
}
