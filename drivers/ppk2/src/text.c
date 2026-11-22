/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "text.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static const double k_pow10[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_space(char c) { return c == ' ' || c == '\t'; }

int lmb_parse_double(const char *s, double *out)
{
    const char *p = s;
    while (is_space(*p))
        p++;

    int neg = 0;
    if (*p == '+' || *p == '-')
        neg = (*p++ == '-');

    uint64_t mant = 0;
    int sig = 0;      /* significant digits kept in mant */
    long exp10 = 0;   /* decimal exponent applied to mant */
    int any = 0;

    while (is_digit(*p)) {
        any = 1;
        if (sig < 19) {
            mant = mant * 10u + (uint64_t)(*p - '0');
            if (mant)
                sig++;
        } else {
            exp10++;
        }
        p++;
    }
    if (*p == '.') {
        p++;
        while (is_digit(*p)) {
            any = 1;
            if (sig < 19) {
                mant = mant * 10u + (uint64_t)(*p - '0');
                if (mant)
                    sig++;
                exp10--;
            }
            p++;
        }
    }
    if (!any)
        return -1;

    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0;
        if (*q == '+' || *q == '-')
            eneg = (*q++ == '-');
        if (!is_digit(*q))
            return -1;
        long e = 0;
        while (is_digit(*q)) {
            if (e < 100000)
                e = e * 10 + (*q - '0');
            q++;
        }
        exp10 += eneg ? -e : e;
        p = q;
    }
    while (is_space(*p))
        p++;
    if (*p != '\0')
        return -1;

    double v = (double)mant;
    if (mant != 0) {
        if (exp10 > 400)
            return -1;
        if (exp10 < -400)
            exp10 = -400;
        long e = exp10;
        while (e > 22) {
            v *= 1e22;
            e -= 22;
        }
        while (e < -22) {
            v /= 1e22;
            e += 22;
        }
        if (e >= 0)
            v *= k_pow10[e];
        else
            v /= k_pow10[-e];
    }
    if (!isfinite(v))
        return -1;
    *out = neg ? -v : v;
    return 0;
}

int lmb_format_double(char *buf, size_t cap, double v)
{
    int n = snprintf(buf, cap, "%.10g", v);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    /* %g never groups thousands, so a ',' can only be a locale's decimal point. */
    for (int i = 0; i < n; i++)
        if (buf[i] == ',')
            buf[i] = '.';
    return n;
}
