/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Metadata reply and calibration (report: "Calibration and current units"). */

#include "libmybench/ppk2.h"
#include "text.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LINE_CAP 192

struct ppk2_meta_parser {
    ppk2_metadata meta;
    char line[LINE_CAP];
    size_t len;
    int overflow;
    int done;
};

static const char *const k_coef_keys[PPK2_COEF_COUNT] = {"R", "GS", "GI", "O", "S", "I", "UG"};
static const double k_fallback_r[PPK2_RANGES] = {1031.64, 101.65, 10.15, 0.94, 0.043};
#define ALL_COEF_BITS ((UINT64_C(1) << (PPK2_COEF_COUNT * PPK2_RANGES)) - 1)

static void meta_init(ppk2_metadata *m)
{
    memset(m, 0, sizeof *m);
    m->calibrated = -1;
    m->vdd_mv = NAN;
    m->ia = NAN;
}

ppk2_meta_parser *ppk2_meta_parser_new(void)
{
    ppk2_meta_parser *p = malloc(sizeof *p);
    if (p)
        ppk2_meta_parser_reset(p);
    return p;
}

void ppk2_meta_parser_free(ppk2_meta_parser *p) { free(p); }

void ppk2_meta_parser_reset(ppk2_meta_parser *p)
{
    meta_init(&p->meta);
    p->len = 0;
    p->overflow = 0;
    p->done = 0;
}

int ppk2_meta_parser_done(const ppk2_meta_parser *p) { return p->done; }
const ppk2_metadata *ppk2_meta_parser_result(const ppk2_meta_parser *p) { return &p->meta; }

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
    return s;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static void copy_text(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int coef_key(const char *key, unsigned *kind, unsigned *range)
{
    size_t n = strlen(key);
    if (n < 2)
        return 0;
    char last = key[n - 1];
    if (last < '0' || last > '4')
        return 0;
    for (unsigned k = 0; k < PPK2_COEF_COUNT; k++) {
        size_t kl = strlen(k_coef_keys[k]);
        if (kl == n - 1 && memcmp(key, k_coef_keys[k], kl) == 0) {
            *kind = k;
            *range = (unsigned)(last - '0');
            return 1;
        }
    }
    return 0;
}

static void handle_line(ppk2_meta_parser *p)
{
    ppk2_metadata *m = &p->meta;
    p->line[p->len] = '\0';
    char *line = trim(p->line);
    if (*line == '\0')
        return;
    if (strcmp(line, "END") == 0) {
        p->done = 1;
        return;
    }
    char *colon = strchr(line, ':');
    if (!colon) {
        m->bad_lines++;
        return;
    }
    *colon = '\0';
    char *key = trim(line);
    char *value = trim(colon + 1);
    double v;
    unsigned kind, range;

    if (strcmp(key, "Calibrated") == 0) {
        if (ieq(value, "true") || ieq(value, "1") || ieq(value, "yes"))
            m->calibrated = 1;
        else if (ieq(value, "false") || ieq(value, "0") || ieq(value, "no"))
            m->calibrated = 0;
        else
            m->bad_lines++;
    } else if (strcmp(key, "HW") == 0) {
        copy_text(m->hw, sizeof m->hw, value);
    } else if (strcmp(key, "mode") == 0) {
        copy_text(m->mode, sizeof m->mode, value);
    } else if (strcmp(key, "VDD") == 0) {
        if (lmb_parse_double(value, &v) == 0)
            m->vdd_mv = v;
        else
            m->bad_lines++;
    } else if (strcmp(key, "IA") == 0) {
        if (lmb_parse_double(value, &v) == 0)
            m->ia = v;
        else
            m->bad_lines++;
    } else if (coef_key(key, &kind, &range)) {
        if (lmb_parse_double(value, &v) == 0) {
            m->coef[kind][range] = v;
            m->present |= PPK2_COEF_BIT(kind, range);
        } else {
            m->bad_lines++;
        }
    } else {
        m->unknown_keys++;
    }
}

size_t ppk2_meta_parser_feed(ppk2_meta_parser *p, const uint8_t *data, size_t len)
{
    if (!p || p->done || !data)
        return 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '\n') {
            if (!p->overflow)
                handle_line(p);
            p->len = 0;
            p->overflow = 0;
            if (p->done)
                return i + 1;
            continue;
        }
        if (p->overflow)
            continue;
        if (c == '\0' || p->len >= LINE_CAP - 1) {
            /* A NUL can't be part of a text line: treat the line as garbage, like an over-long one. */
            p->overflow = 1;
            p->meta.bad_lines++;
            continue;
        }
        p->line[p->len++] = (char)c;
    }
    return len;
}

void ppk2_cal_default(ppk2_cal *cal)
{
    for (unsigned r = 0; r < PPK2_RANGES; r++) {
        cal->coef[PPK2_COEF_R][r] = k_fallback_r[r];
        cal->coef[PPK2_COEF_GS][r] = 1.0;
        cal->coef[PPK2_COEF_GI][r] = 1.0;
        cal->coef[PPK2_COEF_O][r] = 0.0;
        cal->coef[PPK2_COEF_S][r] = 0.0;
        cal->coef[PPK2_COEF_I][r] = 0.0;
        cal->coef[PPK2_COEF_UG][r] = 1.0;
    }
    cal->missing = ALL_COEF_BITS;
    cal->zero = 0;
}

static int fallback_is_nonzero(unsigned kind)
{
    return kind == PPK2_COEF_R || kind == PPK2_COEF_GS || kind == PPK2_COEF_GI || kind == PPK2_COEF_UG;
}

void ppk2_cal_from_metadata(ppk2_cal *cal, const ppk2_metadata *meta)
{
    ppk2_cal_default(cal);
    for (unsigned k = 0; k < PPK2_COEF_COUNT; k++) {
        for (unsigned r = 0; r < PPK2_RANGES; r++) {
            uint64_t bit = PPK2_COEF_BIT(k, r);
            if (!(meta->present & bit))
                continue;
            cal->missing &= ~bit;
            double v = meta->coef[k][r];
            if (v == 0.0) {
                /* Desktop compatibility: zero leaves the fallback in effect. */
                if (fallback_is_nonzero(k))
                    cal->zero |= bit;
                continue;
            }
            cal->coef[k][r] = v;
        }
    }
}

unsigned ppk2_cal_fallback_count(const ppk2_cal *cal)
{
    uint64_t m = cal->missing | cal->zero;
    unsigned n = 0;
    while (m) {
        m &= m - 1;
        n++;
    }
    return n;
}

double ppk2_cal_current(const ppk2_cal *cal, unsigned adc, unsigned range, double source_volts)
{
    if (range >= PPK2_RANGES || isnan(source_volts))
        return NAN;
    const double r = cal->coef[PPK2_COEF_R][range];
    const double o = cal->coef[PPK2_COEF_O][range];
    const double x = (4.0 * adc - o) * 1.8 / (163840.0 * r);
    return cal->coef[PPK2_COEF_UG][range] *
           (cal->coef[PPK2_COEF_GS][range] * x * x + cal->coef[PPK2_COEF_GI][range] * x +
            cal->coef[PPK2_COEF_S][range] * source_volts + cal->coef[PPK2_COEF_I][range]);
}
