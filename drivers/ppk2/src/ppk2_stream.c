/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Sample words and .ppk2 records (report: "Sample framing and loss", "capture interchange"). */

#include "libmybench/ppk2.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct ppk2_stream {
    uint8_t carry[3];
    uint8_t ncarry;
    int have_seq;
    uint8_t last_seq;
    int has_cal;
    ppk2_cal cal;
    double volts;
    ppk2_stream_stats stats;
};

ppk2_word ppk2_word_decode(uint32_t w)
{
    ppk2_word d;
    d.adc = (uint16_t)(w & 0x3FFFu);
    d.range = (uint8_t)((w >> 14) & 0x7u);
    d.aux = (uint8_t)((w >> 17) & 0x1u);
    d.seq = (uint8_t)((w >> 18) & 0x3Fu);
    d.logic = (uint8_t)(w >> 24);
    return d;
}

uint32_t ppk2_word_encode(ppk2_word w)
{
    return (uint32_t)(w.adc & 0x3FFFu) | ((uint32_t)(w.range & 0x7u) << 14) |
           ((uint32_t)(w.aux & 0x1u) << 17) | ((uint32_t)(w.seq & 0x3Fu) << 18) |
           ((uint32_t)w.logic << 24);
}

ppk2_stream *ppk2_stream_new(void)
{
    ppk2_stream *s = calloc(1, sizeof *s);
    if (s)
        s->volts = NAN;
    return s;
}

void ppk2_stream_free(ppk2_stream *s) { free(s); }

void ppk2_stream_reset(ppk2_stream *s)
{
    s->ncarry = 0;
    s->have_seq = 0;
}

void ppk2_stream_set_calibration(ppk2_stream *s, const ppk2_cal *cal, double source_volts)
{
    s->has_cal = cal != NULL;
    if (cal)
        s->cal = *cal;
    s->volts = source_volts;
}

static void emit(ppk2_stream *s, uint32_t word, ppk2_sample *out)
{
    ppk2_word w = ppk2_word_decode(word);
    out->adc = w.adc;
    out->range = w.range;
    out->logic = w.logic;
    out->flags = w.aux ? PPK2_SAMPLE_AUX : 0;
    out->missing = 0;

    if (s->have_seq) {
        uint8_t expected = (uint8_t)((s->last_seq + 1) & 0x3F);
        if (w.seq != expected) {
            out->flags |= PPK2_SAMPLE_GAP;
            out->missing = (uint8_t)((w.seq - expected) & 0x3F);
            if (out->missing == 0) /* can't happen when seq != expected; kept for clarity */
                out->missing = 64;
            s->stats.gaps++;
            s->stats.missing += out->missing;
        }
    }
    s->have_seq = 1;
    s->last_seq = w.seq;

    if (w.range >= PPK2_RANGES) {
        out->flags |= PPK2_SAMPLE_BAD_RANGE;
        out->current_a = NAN;
        s->stats.bad_range++;
    } else {
        out->current_a = s->has_cal ? ppk2_cal_current(&s->cal, w.adc, w.range, s->volts) : NAN;
    }
    s->stats.words++;
}

size_t ppk2_stream_decode(ppk2_stream *s, const uint8_t *data, size_t len, size_t *consumed,
                          ppk2_sample *out, size_t cap)
{
    size_t i = 0, n = 0;
    if (!s || (!data && len) || (!out && cap)) {
        if (consumed)
            *consumed = 0;
        return 0;
    }
    while (n < cap && s->ncarry + (len - i) >= 4) {
        uint8_t b[4];
        size_t k = 0;
        for (; k < s->ncarry; k++)
            b[k] = s->carry[k];
        while (k < 4)
            b[k++] = data[i++];
        s->ncarry = 0;
        uint32_t word = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        emit(s, word, &out[n++]);
    }
    if (s->ncarry + (len - i) < 4) {
        /* Fewer than 4 bytes left: keep them for the next call. */
        while (i < len)
            s->carry[s->ncarry++] = data[i++];
    }
    if (consumed)
        *consumed = i;
    return n;
}

void ppk2_stream_get_stats(const ppk2_stream *s, ppk2_stream_stats *stats) { *stats = s->stats; }

uint64_t ppk2_possible_undetected_loss(uint64_t words_received, uint64_t missing_detected, double elapsed_s)
{
    if (!(elapsed_s > 0.0) || !isfinite(elapsed_s))
        return 0;
    double expected = floor(elapsed_s * PPK2_SAMPLE_RATE + 0.5);
    double got = (double)words_received + (double)missing_detected;
    return expected > got ? (uint64_t)(expected - got) : 0;
}

void ppk2_record_encode(float current_ua, uint16_t logic_states, uint8_t out[PPK2_RECORD_SIZE])
{
    uint32_t u;
    memcpy(&u, &current_ua, sizeof u);
    out[0] = (uint8_t)u;
    out[1] = (uint8_t)(u >> 8);
    out[2] = (uint8_t)(u >> 16);
    out[3] = (uint8_t)(u >> 24);
    out[4] = (uint8_t)(logic_states >> 8);
    out[5] = (uint8_t)logic_states;
}

void ppk2_record_decode(const uint8_t in[PPK2_RECORD_SIZE], float *current_ua, uint16_t *logic_states)
{
    uint32_t u = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    if (current_ua)
        memcpy(current_ua, &u, sizeof u);
    if (logic_states)
        *logic_states = (uint16_t)(((unsigned)in[4] << 8) | in[5]);
}

uint16_t ppk2_logic_states_from_mask(uint8_t mask)
{
    uint16_t states = 0;
    for (unsigned ch = 0; ch < 8; ch++) {
        unsigned v = (mask >> ch) & 1u ? PPK2_LOGIC_HIGH : PPK2_LOGIC_LOW;
        states |= (uint16_t)(v << (2 * ch));
    }
    return states;
}

unsigned ppk2_logic_state(uint16_t states, unsigned channel)
{
    return channel < 8 ? (states >> (2 * channel)) & 3u : PPK2_LOGIC_MISSING;
}
