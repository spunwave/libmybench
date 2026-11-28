/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* The families built into this copy of the library (device.h, lmb_register_builtin_families). */

#include "libmybench/device.h"

#ifdef LMB_HAVE_PPK2
#include "libmybench/ppk2_family.h"
#endif

int lmb_register_builtin_families(lmb_context *ctx)
{
    int rc = LMB_OK;
    (void)ctx;
#ifdef LMB_HAVE_PPK2
    if (rc == LMB_OK)
        rc = lmb_ppk2_register(ctx);
#endif
    return rc;
}
