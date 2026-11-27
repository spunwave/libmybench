/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* .ppk2 capture files, format version 2 (report: "Capture interchange"): a ZIP archive with
 * metadata.json and session.raw (6-byte records). minimap.raw is neither read nor written: its
 * layout is not in the report (CLEANROOM.md, open question 5). */

#include "libmybench/ppk2.h"

#include "text.h"

#include "miniz.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define META_NAME "metadata.json"
#define SESSION_NAME "session.raw"
#define MINIMAP_NAME "minimap.raw"
#define META_MAX (1u << 20) /* metadata.json larger than this is refused */
#define JSON_DEPTH_MAX 32

/* ------------------------------------------------------------------------------------------ */
/* metadata.json: a small strict JSON reader that keeps three numbers                          */

typedef struct json {
    const char *p, *end;
    int depth;
    int have_version, have_rate, have_start;
    double version, rate, start;
} json;

enum { AT_OTHER, AT_ROOT, AT_META };

static void json_ws(json *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
        j->p++;
}

/* A string token. *plain is set when it has no escapes, so [*s, *s + *n) is its text. */
static int json_string(json *j, const char **s, size_t *n, int *plain)
{
    if (j->p >= j->end || *j->p != '"')
        return -1;
    j->p++;
    *s = j->p;
    *plain = 1;
    while (j->p < j->end) {
        const unsigned char c = (unsigned char)*j->p;
        if (c == '"') {
            *n = (size_t)(j->p - *s);
            j->p++;
            return 0;
        }
        if (c < 0x20)
            return -1;
        if (c == '\\') {
            *plain = 0;
            if (++j->p >= j->end)
                return -1;
            const char e = *j->p;
            if (e == 'u') {
                for (int k = 1; k <= 4; k++)
                    if (j->p + k >= j->end || !strchr("0123456789abcdefABCDEF", j->p[k]) || !j->p[k])
                        return -1;
                j->p += 4;
            } else if (!strchr("\"\\/bfnrt", e) || !e) {
                return -1;
            }
        }
        j->p++;
    }
    return -1;
}

static int json_number(json *j, double *out)
{
    const char *start = j->p;
    if (j->p < j->end && *j->p == '-')
        j->p++;
    if (j->p >= j->end)
        return -1;
    if (*j->p == '0') {
        j->p++;
    } else if (*j->p >= '1' && *j->p <= '9') {
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
            j->p++;
    } else {
        return -1;
    }
    if (j->p < j->end && *j->p == '.') {
        j->p++;
        if (j->p >= j->end || *j->p < '0' || *j->p > '9')
            return -1;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
            j->p++;
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        j->p++;
        if (j->p < j->end && (*j->p == '+' || *j->p == '-'))
            j->p++;
        if (j->p >= j->end || *j->p < '0' || *j->p > '9')
            return -1;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
            j->p++;
    }
    char buf[64];
    const size_t n = (size_t)(j->p - start);
    if (n >= sizeof buf) {
        /* Valid but absurdly long: keep the syntax check, drop the value. */
        *out = NAN;
        return 0;
    }
    memcpy(buf, start, n);
    buf[n] = '\0';
    if (lmb_parse_double(buf, out) != 0)
        *out = NAN; /* overflow to infinity: syntactically fine, not a usable value */
    return 0;
}

static int json_literal(json *j, const char *word)
{
    const size_t n = strlen(word);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, word, n) != 0)
        return -1;
    j->p += n;
    return 0;
}

static int key_is(const char *s, size_t n, int plain, const char *want)
{
    return plain && n == strlen(want) && memcmp(s, want, n) == 0;
}

static int json_value(json *j, int at, const char *key, size_t keylen, int keyplain);

static int json_object(json *j, int at)
{
    j->p++; /* '{' */
    json_ws(j);
    if (j->p < j->end && *j->p == '}') {
        j->p++;
        return 0;
    }
    for (;;) {
        const char *k;
        size_t kn;
        int kp;
        json_ws(j);
        if (json_string(j, &k, &kn, &kp) != 0)
            return -1;
        json_ws(j);
        if (j->p >= j->end || *j->p != ':')
            return -1;
        j->p++;
        json_ws(j);
        if (json_value(j, at, k, kn, kp) != 0)
            return -1;
        json_ws(j);
        if (j->p < j->end && *j->p == ',') {
            j->p++;
            continue;
        }
        if (j->p < j->end && *j->p == '}') {
            j->p++;
            return 0;
        }
        return -1;
    }
}

static int json_array(json *j)
{
    j->p++; /* '[' */
    json_ws(j);
    if (j->p < j->end && *j->p == ']') {
        j->p++;
        return 0;
    }
    for (;;) {
        json_ws(j);
        if (json_value(j, AT_OTHER, NULL, 0, 0) != 0)
            return -1;
        json_ws(j);
        if (j->p < j->end && *j->p == ',') {
            j->p++;
            continue;
        }
        if (j->p < j->end && *j->p == ']') {
            j->p++;
            return 0;
        }
        return -1;
    }
}

/* One value. `at` is where its parent object sits, `key` its name there (NULL in arrays). */
static int json_value(json *j, int at, const char *key, size_t keylen, int keyplain)
{
    if (j->p >= j->end)
        return -1;
    int rc;
    const char c = *j->p;
    if (c == '{' || c == '[') {
        if (++j->depth > JSON_DEPTH_MAX)
            return -1;
        if (c == '{') {
            int child = AT_OTHER;
            if (at == AT_ROOT && key && key_is(key, keylen, keyplain, "metadata"))
                child = AT_META;
            rc = json_object(j, child);
        } else {
            rc = json_array(j);
        }
        j->depth--;
        return rc;
    }
    if (c == '"') {
        const char *s;
        size_t n;
        int plain;
        return json_string(j, &s, &n, &plain);
    }
    if (c == 't')
        return json_literal(j, "true");
    if (c == 'f')
        return json_literal(j, "false");
    if (c == 'n')
        return json_literal(j, "null");
    double v;
    if (json_number(j, &v) != 0)
        return -1;
    if (!key)
        return 0;
    /* A repeated key: the last one wins, as in most JSON readers. */
    if (at == AT_ROOT && key_is(key, keylen, keyplain, "formatVersion")) {
        j->have_version = 1;
        j->version = v;
    } else if (at == AT_META && key_is(key, keylen, keyplain, "samplesPerSecond")) {
        j->have_rate = 1;
        j->rate = v;
    } else if (at == AT_META && key_is(key, keylen, keyplain, "startSystemTime")) {
        j->have_start = 1;
        j->start = v;
    }
    return 0;
}

int ppk2_file_parse_metadata(const char *text, size_t len, ppk2_file_info *info)
{
    if (!text || !info)
        return PPK2_ERR_INVALID_ARG;
    json j;
    memset(&j, 0, sizeof j);
    j.p = text;
    j.end = text + len;
    /* A UTF-8 byte order mark is tolerated. */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        j.p += 3;
    json_ws(&j);
    if (j.p >= j.end || *j.p != '{')
        return PPK2_ERR_FORMAT;
    j.depth = 1;
    if (json_object(&j, AT_ROOT) != 0)
        return PPK2_ERR_FORMAT;
    json_ws(&j);
    if (j.p != j.end)
        return PPK2_ERR_FORMAT;
    if (!j.have_version || !j.have_rate)
        return PPK2_ERR_FORMAT;
    if (j.version != 2.0)
        return PPK2_ERR_UNSUPPORTED;
    if (!isfinite(j.rate) || j.rate <= 0.0)
        return PPK2_ERR_FORMAT;
    memset(info, 0, sizeof *info);
    info->samples_per_second = j.rate;
    info->start_time_ms = NAN;
    if (j.have_start && isfinite(j.start))
        info->start_time_ms = j.start;
    return PPK2_OK;
}

int ppk2_file_format_metadata(char *buf, size_t cap, double samples_per_second, double start_time_ms)
{
    if (!buf || !isfinite(samples_per_second) || samples_per_second <= 0.0)
        return PPK2_ERR_INVALID_ARG;
    char rate[32], start[32];
    if (lmb_format_double(rate, sizeof rate, samples_per_second) < 0)
        return PPK2_ERR_INVALID_ARG;
    int n;
    if (isfinite(start_time_ms)) {
        /* Whole milliseconds, like Date.now(): %.0f of an integer-valued double is exact. */
        n = snprintf(start, sizeof start, "%.0f", floor(start_time_ms));
        if (n < 0 || (size_t)n >= sizeof start)
            return PPK2_ERR_INVALID_ARG;
        n = snprintf(buf, cap, "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":%s,\"startSystemTime\":%s}}",
                     rate, start);
    } else {
        n = snprintf(buf, cap, "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":%s}}", rate);
    }
    if (n < 0 || (size_t)n >= cap)
        return PPK2_ERR_NO_SPACE;
    return n;
}

/* ------------------------------------------------------------------------------------------ */
/* Reading                                                                                     */

struct ppk2_file_reader {
    mz_zip_archive zip;
    mz_zip_reader_extract_iter_state *iter;
    ppk2_file_info info;
    uint64_t remaining; /* complete records not yet returned */
    uint8_t buf[6 * 2048];
};

static int reader_init(ppk2_file_reader *r)
{
    const int meta = mz_zip_reader_locate_file(&r->zip, META_NAME, NULL, 0);
    const int session = mz_zip_reader_locate_file(&r->zip, SESSION_NAME, NULL, 0);
    if (meta < 0 || session < 0)
        return PPK2_ERR_FORMAT;
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&r->zip, (mz_uint)meta, &st) || st.m_uncomp_size > META_MAX)
        return PPK2_ERR_FORMAT;
    size_t len = 0;
    char *text = mz_zip_reader_extract_to_heap(&r->zip, (mz_uint)meta, &len, 0);
    if (!text)
        return mz_zip_get_last_error(&r->zip) == MZ_ZIP_ALLOC_FAILED ? PPK2_ERR_NO_MEM : PPK2_ERR_FORMAT;
    const int rc = ppk2_file_parse_metadata(text, len, &r->info);
    mz_free(text);
    if (rc != PPK2_OK)
        return rc;
    if (!mz_zip_reader_file_stat(&r->zip, (mz_uint)session, &st))
        return PPK2_ERR_FORMAT;
    r->info.records = st.m_uncomp_size / PPK2_RECORD_SIZE;
    r->info.partial_tail = (unsigned)(st.m_uncomp_size % PPK2_RECORD_SIZE);
    r->info.has_minimap = mz_zip_reader_locate_file(&r->zip, MINIMAP_NAME, NULL, 0) >= 0;
    r->remaining = r->info.records;
    r->iter = mz_zip_reader_extract_iter_new(&r->zip, (mz_uint)session, 0);
    if (!r->iter)
        return mz_zip_get_last_error(&r->zip) == MZ_ZIP_ALLOC_FAILED ? PPK2_ERR_NO_MEM : PPK2_ERR_FORMAT;
    return PPK2_OK;
}

static int reader_open(ppk2_file_reader **out, const char *path, const void *data, size_t len)
{
    if (!out || (!path && !data))
        return PPK2_ERR_INVALID_ARG;
    *out = NULL;
    ppk2_file_reader *r = calloc(1, sizeof *r);
    if (!r)
        return PPK2_ERR_NO_MEM;
    mz_bool ok = path ? mz_zip_reader_init_file(&r->zip, path, 0) : mz_zip_reader_init_mem(&r->zip, data, len, 0);
    if (!ok) {
        const mz_zip_error e = mz_zip_get_last_error(&r->zip);
        free(r);
        return e == MZ_ZIP_FILE_OPEN_FAILED ? PPK2_ERR_IO : e == MZ_ZIP_ALLOC_FAILED ? PPK2_ERR_NO_MEM : PPK2_ERR_FORMAT;
    }
    const int rc = reader_init(r);
    if (rc != PPK2_OK) {
        ppk2_file_close(r);
        return rc;
    }
    *out = r;
    return PPK2_OK;
}

int ppk2_file_open(const char *path, ppk2_file_reader **out)
{
    return reader_open(out, path, NULL, 0);
}

int ppk2_file_open_mem(const void *data, size_t len, ppk2_file_reader **out)
{
    if (!data)
        return PPK2_ERR_INVALID_ARG;
    return reader_open(out, NULL, data, len);
}

const ppk2_file_info *ppk2_file_get_info(const ppk2_file_reader *r)
{
    return r ? &r->info : NULL;
}

int ppk2_file_read(ppk2_file_reader *r, float *current_ua, uint16_t *logic_states, size_t cap, size_t *got)
{
    if (!r || !got || (cap && !current_ua && !logic_states))
        return PPK2_ERR_INVALID_ARG;
    *got = 0;
    while (*got < cap && r->remaining) {
        size_t want = cap - *got;
        if (want > sizeof r->buf / PPK2_RECORD_SIZE)
            want = sizeof r->buf / PPK2_RECORD_SIZE;
        if (want > r->remaining)
            want = (size_t)r->remaining;
        const size_t bytes = want * PPK2_RECORD_SIZE;
        /* The iterator returns short counts only at the end of the entry or on an error. */
        if (mz_zip_reader_extract_iter_read(r->iter, r->buf, bytes) != bytes)
            return PPK2_ERR_FORMAT;
        for (size_t i = 0; i < want; i++) {
            float ua;
            uint16_t st;
            ppk2_record_decode(r->buf + i * PPK2_RECORD_SIZE, &ua, &st);
            if (current_ua)
                current_ua[*got + i] = ua;
            if (logic_states)
                logic_states[*got + i] = st;
        }
        *got += want;
        r->remaining -= want;
    }
    return PPK2_OK;
}

void ppk2_file_close(ppk2_file_reader *r)
{
    if (!r)
        return;
    if (r->iter)
        mz_zip_reader_extract_iter_free(r->iter); /* also checks the CRC; the result is not needed here */
    mz_zip_reader_end(&r->zip);
    free(r);
}

/* ------------------------------------------------------------------------------------------ */
/* Writing                                                                                     */

struct ppk2_file_writer {
    char *path;      /* the archive */
    char *part;      /* session records while recording: <path>.part */
    FILE *f;
    double rate, start_ms;
    uint64_t records;
    uint8_t buf[6 * 2048];
};

int ppk2_file_create(const char *path, double samples_per_second, double start_time_ms, ppk2_file_writer **out)
{
    if (!path || !out || !isfinite(samples_per_second) || samples_per_second <= 0.0)
        return PPK2_ERR_INVALID_ARG;
    *out = NULL;
    ppk2_file_writer *w = calloc(1, sizeof *w);
    if (!w)
        return PPK2_ERR_NO_MEM;
    const size_t n = strlen(path);
    w->path = malloc(n + 1);
    w->part = malloc(n + 6);
    if (!w->path || !w->part) {
        free(w->path);
        free(w->part);
        free(w);
        return PPK2_ERR_NO_MEM;
    }
    memcpy(w->path, path, n + 1);
    memcpy(w->part, path, n);
    memcpy(w->part + n, ".part", 6);
    w->rate = samples_per_second;
    w->start_ms = start_time_ms;
    w->f = fopen(w->part, "wb");
    if (!w->f) {
        free(w->path);
        free(w->part);
        free(w);
        return PPK2_ERR_IO;
    }
    *out = w;
    return PPK2_OK;
}

int ppk2_file_append(ppk2_file_writer *w, const float *current_ua, const uint16_t *logic_states, size_t n)
{
    if (!w || (n && !current_ua))
        return PPK2_ERR_INVALID_ARG;
    if (!w->f)
        return PPK2_ERR_STATE; /* an earlier write failed */
    while (n) {
        size_t k = n < sizeof w->buf / PPK2_RECORD_SIZE ? n : sizeof w->buf / PPK2_RECORD_SIZE;
        for (size_t i = 0; i < k; i++)
            ppk2_record_encode(current_ua[i], logic_states ? logic_states[i] : 0, w->buf + i * PPK2_RECORD_SIZE);
        if (fwrite(w->buf, PPK2_RECORD_SIZE, k, w->f) != k) {
            fclose(w->f);
            w->f = NULL;
            return PPK2_ERR_IO;
        }
        w->records += k;
        current_ua += k;
        if (logic_states)
            logic_states += k;
        n -= k;
    }
    return PPK2_OK;
}

uint64_t ppk2_file_written(const ppk2_file_writer *w)
{
    return w ? w->records : 0;
}

static void writer_free(ppk2_file_writer *w)
{
    if (w->f)
        fclose(w->f);
    free(w->path);
    free(w->part);
    free(w);
}

int ppk2_file_finish(ppk2_file_writer *w)
{
    if (!w)
        return PPK2_ERR_INVALID_ARG;
    if (!w->f) {
        remove(w->part);
        writer_free(w);
        return PPK2_ERR_IO;
    }
    const int closed = fclose(w->f);
    w->f = NULL;
    char meta[160];
    int rc = closed == 0 ? ppk2_file_format_metadata(meta, sizeof meta, w->rate, w->start_ms) : PPK2_ERR_IO;
    if (rc >= 0) {
        const size_t meta_len = (size_t)rc;
        rc = PPK2_OK;
        mz_zip_archive zip;
        memset(&zip, 0, sizeof zip);
        /* Zip64 only when needed: session.raw passes 4 GiB after about two hours at 100 kS/s. */
        if (!mz_zip_writer_init_file_v2(&zip, w->path, 0, 0)) {
            rc = PPK2_ERR_IO;
        } else {
            if (!mz_zip_writer_add_mem(&zip, META_NAME, meta, meta_len, MZ_BEST_SPEED) ||
                !mz_zip_writer_add_file(&zip, SESSION_NAME, w->part, NULL, 0, MZ_BEST_SPEED) ||
                !mz_zip_writer_finalize_archive(&zip))
                rc = mz_zip_get_last_error(&zip) == MZ_ZIP_ALLOC_FAILED ? PPK2_ERR_NO_MEM : PPK2_ERR_IO;
            if (!mz_zip_writer_end(&zip) && rc == PPK2_OK)
                rc = PPK2_ERR_IO;
            if (rc != PPK2_OK)
                remove(w->path);
        }
    }
    remove(w->part);
    writer_free(w);
    return rc;
}

void ppk2_file_abort(ppk2_file_writer *w)
{
    if (!w)
        return;
    if (w->f) {
        fclose(w->f);
        w->f = NULL;
    }
    remove(w->part);
    writer_free(w);
}
