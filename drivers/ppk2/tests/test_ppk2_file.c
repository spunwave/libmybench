/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* .ppk2 file tests: metadata.json, write -> read round trips, archives built by hand (as another
 * program might write them), damaged input. */

#include "libmybench/ppk2.h"

#include "miniz.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PPK2_TEST_TMPDIR
#define PPK2_TEST_TMPDIR "."
#endif

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                              \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                             \
    do {                                                                               \
        long long a_ = (long long)(a), b_ = (long long)(b);                            \
        g_checks++;                                                                    \
        if (a_ != b_) {                                                                \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, \
                    a_, b_);                                                           \
        }                                                                              \
    } while (0)

static int parse(const char *json, ppk2_file_info *info)
{
    return ppk2_file_parse_metadata(json, strlen(json), info);
}

static void test_metadata_parse(void)
{
    ppk2_file_info info;
    CHECK_EQ_INT(parse("{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":100000}}", &info), PPK2_OK);
    CHECK(info.samples_per_second == 100000.0);
    CHECK(isnan(info.start_time_ms));

    /* Whitespace, a byte order mark, other keys, nested values, key order. */
    const char *rich = "\xEF\xBB\xBF {\n  \"metadata\" : { \"note\": \"a \\\"quoted\\\" \\u00e9\",\n"
                       "    \"list\": [1, -2.5e3, true, false, null, {\"samplesPerSecond\": 7}],\n"
                       "    \"samplesPerSecond\": 1e5, \"startSystemTime\": 1764288000123 },\n"
                       "  \"formatVersion\": 2.0, \"extra\": {\"formatVersion\": 9} }\n";
    CHECK_EQ_INT(parse(rich, &info), PPK2_OK);
    CHECK(info.samples_per_second == 100000.0);
    CHECK(info.start_time_ms == 1764288000123.0);

    CHECK_EQ_INT(parse("{\"formatVersion\":1,\"metadata\":{\"samplesPerSecond\":100000}}", &info), PPK2_ERR_UNSUPPORTED);
    CHECK_EQ_INT(parse("{\"formatVersion\":3,\"metadata\":{\"samplesPerSecond\":100000}}", &info), PPK2_ERR_UNSUPPORTED);

    const char *bad[] = {
        "",
        "[]",
        "{\"formatVersion\":2}",                                                /* no rate */
        "{\"metadata\":{\"samplesPerSecond\":100000}}",                         /* no version */
        "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":0}}",          /* zero rate */
        "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":-5}}",
        "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":\"100000\"}}", /* a string */
        "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":1e999}}",      /* overflows */
        "{\"formatVersion\":2,\"samplesPerSecond\":100000}",                    /* wrong level */
        "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":100000}} x",   /* trailing garbage */
        "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":100000},}",    /* trailing comma */
        "{\"formatVersion\":02,\"metadata\":{\"samplesPerSecond\":100000}}",    /* leading zero */
        "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":1.}}",
        "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":100000}",      /* unclosed */
        "{\"formatVersio\\u006e\":2,\"metadata\":{\"samplesPerSecond\":100000}}", /* escaped key: not matched */
        "{\"a\":\"\\x\"}",
        "{\"a\":\"\\u12\"}",
        "{\"a\":tru}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const int rc = parse(bad[i], &info);
        if (rc != PPK2_ERR_FORMAT)
            fprintf(stderr, "bad metadata %zu: rc %d\n", i, rc);
        CHECK_EQ_INT(rc, PPK2_ERR_FORMAT);
    }

    /* Nesting beyond the limit is refused rather than recursing without bound. */
    char deep[400];
    size_t n = 0;
    n += (size_t)snprintf(deep, sizeof deep, "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":1},\"x\":");
    for (int i = 0; i < 100; i++)
        deep[n++] = '[';
    for (int i = 0; i < 100; i++)
        deep[n++] = ']';
    deep[n++] = '}';
    CHECK_EQ_INT(ppk2_file_parse_metadata(deep, n, &info), PPK2_ERR_FORMAT);

    /* An embedded NUL is not whitespace. */
    const char nul[] = "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":1}}\0";
    CHECK_EQ_INT(ppk2_file_parse_metadata(nul, sizeof nul - 1, &info), PPK2_ERR_FORMAT);
}

static void test_metadata_format(void)
{
    char buf[200];
    int n = ppk2_file_format_metadata(buf, sizeof buf, 100000.0, NAN);
    CHECK(n > 0);
    CHECK(strcmp(buf, "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":100000}}") == 0);
    n = ppk2_file_format_metadata(buf, sizeof buf, 12500.5, 1764288000123.0);
    CHECK(strcmp(buf, "{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":12500.5,\"startSystemTime\":1764288000123}}") == 0);
    CHECK_EQ_INT(n, (int)strlen(buf));
    ppk2_file_info info;
    CHECK_EQ_INT(ppk2_file_parse_metadata(buf, (size_t)n, &info), PPK2_OK);
    CHECK(info.samples_per_second == 12500.5 && info.start_time_ms == 1764288000123.0);
    CHECK_EQ_INT(ppk2_file_format_metadata(buf, 10, 100000.0, NAN), PPK2_ERR_NO_SPACE);
    CHECK_EQ_INT(ppk2_file_format_metadata(buf, sizeof buf, 0.0, NAN), PPK2_ERR_INVALID_ARG);
    CHECK_EQ_INT(ppk2_file_format_metadata(buf, sizeof buf, NAN, NAN), PPK2_ERR_INVALID_ARG);

    /* A decimal-comma locale must not change the text (written) or the value (read). */
    if (setlocale(LC_NUMERIC, "de_DE.UTF-8") || setlocale(LC_NUMERIC, "fr_FR.UTF-8") ||
        setlocale(LC_NUMERIC, "nl_NL.UTF-8")) {
        n = ppk2_file_format_metadata(buf, sizeof buf, 12500.5, NAN);
        CHECK(n > 0 && strstr(buf, "12500.5") != NULL);
        CHECK_EQ_INT(parse("{\"formatVersion\":2,\"metadata\":{\"samplesPerSecond\":12500.5}}", &info), PPK2_OK);
        CHECK(info.samples_per_second == 12500.5);
        setlocale(LC_NUMERIC, "C");
    }
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

/* Reads one entry of a ZIP file into the heap (NULL if absent). */
static void *zip_entry(const char *path, const char *name, size_t *len)
{
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, path, 0))
        return NULL;
    void *p = mz_zip_reader_extract_file_to_heap(&z, name, len, 0);
    mz_zip_reader_end(&z);
    return p;
}

static void test_roundtrip(void)
{
    const char *path = PPK2_TEST_TMPDIR "/roundtrip.ppk2";
    const size_t n = 250001; /* several internal chunks and a ragged end */
    float *ua = malloc(n * sizeof *ua);
    uint16_t *st = malloc(n * sizeof *st);
    CHECK(ua && st);
    if (!ua || !st)
        return;
    for (size_t i = 0; i < n; i++) {
        ua[i] = (float)(1.0 + (double)(i % 1000) * 0.25);
        st[i] = ppk2_logic_states_from_mask((uint8_t)i);
    }
    ua[0] = 1.0f;
    st[0] = ppk2_logic_states_from_mask(0x01); /* the report's golden record */
    st[1] = 0;                                  /* all channels missing */
    st[2] = 0xFFFF;                             /* all "both" */

    ppk2_file_writer *w = NULL;
    CHECK_EQ_INT(ppk2_file_create(path, 100000.0, 1764288000123.0, &w), PPK2_OK);
    CHECK(w != NULL);
    if (!w)
        return;
    char part[512];
    snprintf(part, sizeof part, "%s.part", path);
    CHECK(file_exists(part));
    /* Uneven appends, one without logic states. */
    CHECK_EQ_INT(ppk2_file_append(w, ua, st, 1), PPK2_OK);
    CHECK_EQ_INT(ppk2_file_append(w, ua + 1, st + 1, 99999), PPK2_OK);
    CHECK_EQ_INT(ppk2_file_append(w, ua + 100000, st + 100000, n - 100000), PPK2_OK);
    CHECK_EQ_INT(ppk2_file_append(w, NULL, NULL, 0), PPK2_OK);
    CHECK(ppk2_file_written(w) == n);
    CHECK_EQ_INT(ppk2_file_finish(w), PPK2_OK);
    CHECK(!file_exists(part));

    /* What another reader sees: the two entries, and the golden first record. */
    size_t len = 0;
    char *meta = zip_entry(path, "metadata.json", &len);
    CHECK(meta && len > 0);
    if (meta)
        CHECK(memcmp(meta, "{\"formatVersion\":2,", 19) == 0);
    mz_free(meta);
    uint8_t *raw = zip_entry(path, "session.raw", &len);
    CHECK(raw && len == n * PPK2_RECORD_SIZE);
    if (raw) {
        static const uint8_t golden[6] = {0x00, 0x00, 0x80, 0x3F, 0x55, 0x56};
        CHECK(memcmp(raw, golden, 6) == 0);
    }
    mz_free(raw);

    ppk2_file_reader *r = NULL;
    CHECK_EQ_INT(ppk2_file_open(path, &r), PPK2_OK);
    CHECK(r != NULL);
    if (!r)
        return;
    const ppk2_file_info *info = ppk2_file_get_info(r);
    CHECK(info->samples_per_second == 100000.0);
    CHECK(info->start_time_ms == 1764288000123.0);
    CHECK(info->records == n);
    CHECK_EQ_INT(info->partial_tail, 0);
    CHECK_EQ_INT(info->has_minimap, 0);
    float got_ua[777];
    uint16_t got_st[777];
    size_t total = 0, got = 0, bad = 0;
    do {
        CHECK_EQ_INT(ppk2_file_read(r, got_ua, got_st, 777, &got), PPK2_OK);
        for (size_t i = 0; i < got; i++)
            if (got_ua[i] != ua[total + i] || got_st[i] != st[total + i])
                bad++;
        total += got;
    } while (got);
    CHECK(total == n);
    CHECK_EQ_INT(bad, 0);
    CHECK_EQ_INT(ppk2_file_read(r, got_ua, got_st, 777, &got), PPK2_OK);
    CHECK_EQ_INT(got, 0);
    ppk2_file_close(r);
    remove(path);
    free(ua);
    free(st);
}

static void test_empty_and_abort(void)
{
    const char *path = PPK2_TEST_TMPDIR "/empty.ppk2";
    ppk2_file_writer *w = NULL;
    CHECK_EQ_INT(ppk2_file_create(path, 100000.0, NAN, &w), PPK2_OK);
    CHECK_EQ_INT(ppk2_file_finish(w), PPK2_OK);
    ppk2_file_reader *r = NULL;
    CHECK_EQ_INT(ppk2_file_open(path, &r), PPK2_OK);
    if (r) {
        CHECK(ppk2_file_get_info(r)->records == 0);
        CHECK(isnan(ppk2_file_get_info(r)->start_time_ms));
        size_t got = 1;
        float v;
        CHECK_EQ_INT(ppk2_file_read(r, &v, NULL, 1, &got), PPK2_OK);
        CHECK_EQ_INT(got, 0);
        ppk2_file_close(r);
    }
    remove(path);

    const char *apath = PPK2_TEST_TMPDIR "/aborted.ppk2";
    remove(apath);
    CHECK_EQ_INT(ppk2_file_create(apath, 100000.0, NAN, &w), PPK2_OK);
    float one = 1.0f;
    CHECK_EQ_INT(ppk2_file_append(w, &one, NULL, 1), PPK2_OK);
    ppk2_file_abort(w);
    char part[512];
    snprintf(part, sizeof part, "%s.part", apath);
    CHECK(!file_exists(part));
    CHECK(!file_exists(apath));

    CHECK_EQ_INT(ppk2_file_create(PPK2_TEST_TMPDIR "/no-such-dir/x.ppk2", 100000.0, NAN, &w), PPK2_ERR_IO);
    CHECK_EQ_INT(ppk2_file_create(apath, 0.0, NAN, &w), PPK2_ERR_INVALID_ARG);
}

/* An archive as another program might write it: entries stored or deflated, in any order, a
 * minimap, a partial record at the end. */
static void *foreign_archive(size_t *len, const char *meta, int with_session, size_t records, size_t tail,
                             mz_uint level)
{
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_writer_init_heap(&z, 0, 0))
        return NULL;
    uint8_t *raw = calloc(records * PPK2_RECORD_SIZE + tail + 1, 1);
    for (size_t i = 0; i < records; i++)
        ppk2_record_encode((float)i * 0.5f, ppk2_logic_states_from_mask((uint8_t)(0x80 >> (i % 8))),
                           raw + i * PPK2_RECORD_SIZE);
    const uint8_t minimap[16] = {0};
    int ok = mz_zip_writer_add_mem(&z, "minimap.raw", minimap, sizeof minimap, level);
    if (with_session)
        ok = ok && mz_zip_writer_add_mem(&z, "session.raw", raw, records * PPK2_RECORD_SIZE + tail, level);
    if (meta)
        ok = ok && mz_zip_writer_add_mem(&z, "metadata.json", meta, strlen(meta), level);
    void *buf = NULL;
    ok = ok && mz_zip_writer_finalize_heap_archive(&z, &buf, len);
    mz_zip_writer_end(&z);
    free(raw);
    return ok ? buf : NULL;
}

static void test_foreign_archives(void)
{
    const char *meta = "{\"metadata\":{\"samplesPerSecond\":100000,\"startSystemTime\":5},\"formatVersion\":2}";
    const mz_uint levels[2] = {MZ_NO_COMPRESSION, MZ_DEFAULT_LEVEL};
    for (int k = 0; k < 2; k++) {
        size_t len = 0;
        void *zip = foreign_archive(&len, meta, 1, 7, 4, levels[k]);
        CHECK(zip != NULL);
        if (!zip)
            continue;
        ppk2_file_reader *r = NULL;
        CHECK_EQ_INT(ppk2_file_open_mem(zip, len, &r), PPK2_OK);
        if (r) {
            const ppk2_file_info *info = ppk2_file_get_info(r);
            CHECK(info->records == 7);
            CHECK_EQ_INT(info->partial_tail, 4);
            CHECK_EQ_INT(info->has_minimap, 1);
            CHECK(info->start_time_ms == 5.0);
            float v[3];
            uint16_t s[3];
            size_t got, total = 0;
            int ok = 1;
            do {
                CHECK_EQ_INT(ppk2_file_read(r, v, s, 3, &got), PPK2_OK);
                for (size_t i = 0; i < got; i++) {
                    const size_t idx = total + i;
                    ok &= v[i] == (float)idx * 0.5f;
                    ok &= ppk2_logic_state(s[i], (unsigned)(7 - idx % 8)) == PPK2_LOGIC_HIGH;
                    ok &= ppk2_logic_state(s[i], (unsigned)((8 - idx % 8) % 8)) == PPK2_LOGIC_LOW;
                }
                total += got;
            } while (got);
            CHECK(ok);
            CHECK_EQ_INT(total, 7);
            ppk2_file_close(r);
        }
        mz_free(zip);
    }

    /* Only logic states wanted. */
    size_t len = 0;
    void *zip = foreign_archive(&len, meta, 1, 5, 0, MZ_NO_COMPRESSION);
    ppk2_file_reader *r = NULL;
    CHECK_EQ_INT(ppk2_file_open_mem(zip, len, &r), PPK2_OK);
    if (r) {
        uint16_t s[8];
        size_t got;
        CHECK_EQ_INT(ppk2_file_read(r, NULL, s, 8, &got), PPK2_OK);
        CHECK_EQ_INT(got, 5);
        CHECK_EQ_INT(ppk2_file_read(r, NULL, NULL, 8, &got), PPK2_ERR_INVALID_ARG);
        ppk2_file_close(r);
    }

    /* Truncated archive: the central directory is at the end, so it no longer opens. */
    CHECK_EQ_INT(ppk2_file_open_mem(zip, len - 10, &r), PPK2_ERR_FORMAT);
    /* Corrupted stored data is detected by the CRC check only when the entry ends; flip a byte
     * of the metadata instead, which is read whole at open. */
    mz_free(zip);

    zip = foreign_archive(&len, NULL, 1, 3, 0, MZ_NO_COMPRESSION);
    CHECK_EQ_INT(ppk2_file_open_mem(zip, len, &r), PPK2_ERR_FORMAT); /* no metadata.json */
    mz_free(zip);
    zip = foreign_archive(&len, meta, 0, 0, 0, MZ_NO_COMPRESSION);
    CHECK_EQ_INT(ppk2_file_open_mem(zip, len, &r), PPK2_ERR_FORMAT); /* no session.raw */
    mz_free(zip);
    zip = foreign_archive(&len, "{\"formatVersion\":1,\"metadata\":{\"samplesPerSecond\":1}}", 1, 3, 0,
                          MZ_NO_COMPRESSION);
    CHECK_EQ_INT(ppk2_file_open_mem(zip, len, &r), PPK2_ERR_UNSUPPORTED);
    mz_free(zip);

    const char junk[] = "this is not a zip archive at all, just text";
    CHECK_EQ_INT(ppk2_file_open_mem(junk, sizeof junk, &r), PPK2_ERR_FORMAT);
    CHECK_EQ_INT(ppk2_file_open(PPK2_TEST_TMPDIR "/does-not-exist.ppk2", &r), PPK2_ERR_IO);
    CHECK_EQ_INT(ppk2_file_open(NULL, &r), PPK2_ERR_INVALID_ARG);
    CHECK_EQ_INT(ppk2_file_open_mem(NULL, 0, &r), PPK2_ERR_INVALID_ARG);
}

int main(void)
{
    test_metadata_parse();
    test_metadata_format();
    test_roundtrip();
    test_empty_and_abort();
    test_foreign_archives();
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
