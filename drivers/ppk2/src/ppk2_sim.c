/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* A simulated PPK2 at the byte level: it takes the same commands and produces real metadata
 * text and sample words, so the simulator path runs the real parser and decoder. */

#include "libmybench/ppk2.h"
#include "text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PENDING_CAP 2048
#define ADC_MAX 16383u
#define ADC_SWITCH 16000.0 /* move to a lower-resistance range above this code */

struct ppk2_sim {
    ppk2_cal cal;
    char pending[PENDING_CAP];
    size_t pending_len, pending_pos;
    int sampling;
    int gate;
    ppk2_supply supply;
    unsigned mv;
    uint64_t index;
    uint8_t seq;
    ppk2_sim_current_fn current_fn;
    ppk2_sim_logic_fn logic_fn;
    void *user;
};

static double default_current(void *user, uint64_t i)
{
    (void)user;
    (void)i;
    return 1e-3;
}

static uint8_t default_logic(void *user, uint64_t i)
{
    (void)user;
    return (uint8_t)(i / 1000u);
}

ppk2_sim *ppk2_sim_new(const ppk2_cal *cal)
{
    ppk2_sim *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    if (cal) {
        s->cal = *cal;
    } else {
        ppk2_cal_default(&s->cal);
        s->cal.missing = 0; /* a unit that reports the fallback values explicitly */
    }
    s->mv = 3300;
    ppk2_sim_set_load(s, NULL, NULL, NULL);
    return s;
}

void ppk2_sim_free(ppk2_sim *sim) { free(sim); }

void ppk2_sim_set_load(ppk2_sim *sim, ppk2_sim_current_fn current, ppk2_sim_logic_fn logic, void *user)
{
    sim->current_fn = current ? current : default_current;
    sim->logic_fn = logic ? logic : default_logic;
    sim->user = user;
}

static void append(ppk2_sim *s, const char *text)
{
    size_t n = strlen(text);
    if (s->pending_len + n <= PENDING_CAP) {
        memcpy(s->pending + s->pending_len, text, n);
        s->pending_len += n;
    }
}

static void queue_metadata(ppk2_sim *s)
{
    static const char *const keys[PPK2_COEF_COUNT] = {"R", "GS", "GI", "O", "S", "I", "UG"};
    char line[64], num[32];

    snprintf(line, sizeof line, "Calibrated: %s\n", ppk2_cal_fallback_count(&s->cal) ? "false" : "true");
    append(s, line);
    append(s, "HW: simulated\n");
    snprintf(line, sizeof line, "VDD: %u\n", s->mv);
    append(s, line);
    append(s, "IA: 0\n");
    for (unsigned k = 0; k < PPK2_COEF_COUNT; k++) {
        for (unsigned r = 0; r < PPK2_RANGES; r++) {
            if (s->cal.missing & PPK2_COEF_BIT(k, r))
                continue;
            double v = (s->cal.zero & PPK2_COEF_BIT(k, r)) ? 0.0 : s->cal.coef[k][r];
            if (lmb_format_double(num, sizeof num, v) < 0)
                continue;
            snprintf(line, sizeof line, "%s%u: %s\n", keys[k], r, num);
            append(s, line);
        }
    }
    append(s, "END\n");
}

int ppk2_sim_write(ppk2_sim *sim, const uint8_t *data, size_t len)
{
    if (!sim || !data || len == 0)
        return PPK2_ERR_INVALID_ARG;
    switch (data[0]) {
    case 0x19:
        if (len != 1)
            return PPK2_ERR_FORMAT;
        queue_metadata(sim);
        return PPK2_OK;
    case 0x11:
        if (len != 2 || (data[1] != 1 && data[1] != 2))
            return PPK2_ERR_FORMAT;
        sim->supply = (ppk2_supply)data[1];
        return PPK2_OK;
    case 0x0D:
        if (len != 3)
            return PPK2_ERR_FORMAT;
        sim->mv = ((unsigned)data[1] << 8) | data[2];
        return PPK2_OK;
    case 0x0C:
        if (len != 2 || data[1] > 1)
            return PPK2_ERR_FORMAT;
        sim->gate = data[1];
        return PPK2_OK;
    case 0x06:
    case 0x07:
        if (len != 1)
            return PPK2_ERR_FORMAT;
        sim->sampling = data[0] == 0x06;
        return PPK2_OK;
    case 0x25:
        /* User gain: accepted and ignored, the host applies calibration (see CLEANROOM.md). */
        return len == 6 ? PPK2_OK : PPK2_ERR_FORMAT;
    default:
        return PPK2_ERR_FORMAT;
    }
}

/* Inverse of the current formula for one range: the x that gives amps. */
static double solve_x(const ppk2_cal *c, unsigned r, double amps, double volts)
{
    const double gs = c->coef[PPK2_COEF_GS][r], gi = c->coef[PPK2_COEF_GI][r];
    const double ug = c->coef[PPK2_COEF_UG][r];
    const double g = amps / ug - c->coef[PPK2_COEF_S][r] * volts - c->coef[PPK2_COEF_I][r];
    const double linear = gi != 0.0 ? g / gi : 0.0;
    if (fabs(gs) < 1e-30)
        return linear;
    const double disc = gi * gi + 4.0 * gs * g;
    if (disc < 0.0)
        return -gi / (2.0 * gs);
    const double a = (-gi + sqrt(disc)) / (2.0 * gs);
    const double b = (-gi - sqrt(disc)) / (2.0 * gs);
    return fabs(a - linear) <= fabs(b - linear) ? a : b;
}

static unsigned encode_current(const ppk2_sim *s, double amps, unsigned *range_out)
{
    const double volts = s->mv / 1000.0;
    for (unsigned r = 0; r < PPK2_RANGES; r++) {
        const double x = solve_x(&s->cal, r, amps, volts);
        const double a = (x * 163840.0 * s->cal.coef[PPK2_COEF_R][r] / 1.8 + s->cal.coef[PPK2_COEF_O][r]) / 4.0;
        if (a <= ADC_SWITCH || r == PPK2_RANGES - 1) {
            double q = floor(a + 0.5);
            if (q < 0.0)
                q = 0.0;
            if (q > ADC_MAX)
                q = ADC_MAX;
            *range_out = r;
            return (unsigned)q;
        }
    }
    *range_out = PPK2_RANGES - 1;
    return ADC_MAX;
}

double ppk2_sim_quantize(const ppk2_sim *sim, double amps, unsigned *range_out)
{
    unsigned r;
    unsigned adc = encode_current(sim, amps, &r);
    if (range_out)
        *range_out = r;
    return ppk2_cal_current(&sim->cal, adc, r, sim->mv / 1000.0);
}

size_t ppk2_sim_read(ppk2_sim *sim, uint8_t *out, size_t cap, uint64_t words_due)
{
    size_t n = 0;
    while (n < cap && sim->pending_pos < sim->pending_len)
        out[n++] = (uint8_t)sim->pending[sim->pending_pos++];
    if (sim->pending_pos == sim->pending_len)
        sim->pending_pos = sim->pending_len = 0;
    if (sim->pending_len) /* reply not fully read yet: no samples behind it */
        return n;

    while (sim->sampling && words_due > 0 && cap - n >= 4) {
        /* The DUT gate sits after the shunts: with it open, no current flows in either mode. */
        const double amps = sim->gate ? sim->current_fn(sim->user, sim->index) : 0.0;
        ppk2_word w;
        unsigned range;
        w.adc = (uint16_t)encode_current(sim, amps, &range);
        w.range = (uint8_t)range;
        w.aux = 0;
        w.seq = sim->seq;
        w.logic = sim->logic_fn(sim->user, sim->index);
        const uint32_t word = ppk2_word_encode(w);
        out[n++] = (uint8_t)word;
        out[n++] = (uint8_t)(word >> 8);
        out[n++] = (uint8_t)(word >> 16);
        out[n++] = (uint8_t)(word >> 24);
        sim->seq = (uint8_t)((sim->seq + 1) & 0x3F);
        sim->index++;
        words_due--;
    }
    return n;
}

void ppk2_sim_drop(ppk2_sim *sim, uint64_t n)
{
    sim->index += n;
    sim->seq = (uint8_t)((sim->seq + n) & 0x3F);
}

uint64_t ppk2_sim_index(const ppk2_sim *sim) { return sim->index; }
