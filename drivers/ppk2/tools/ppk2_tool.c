/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* ppk2-tool: offline PPK2 tool. It works on raw byte captures (what the measurement port sent:
 * an optional metadata reply, then sample words), which makes it the first hardware bring-up aid
 * too (capture with any serial terminal, decode here), and on .ppk2 capture files. */

#include "libmybench/ppk2.h"
#include "libmybench/ppk2_dev.h"

#if !defined(_WIN32)
#include "sim_port.h"
#include <signal.h>
#include <unistd.h>
#endif
#include <time.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static void usage(void)
{
    fputs("usage:\n"
          "  ppk2-tool sim [--seconds S] [--amps A] [--mv MV] [--drop N]   simulated capture to stdout\n"
          "  ppk2-tool decode [--mv MV] [--ppk2 OUT] FILE|-                 capture -> CSV on stdout\n"
          "                                                                  (or a .ppk2 file with --ppk2)\n"
          "  ppk2-tool meta FILE|-                                           print metadata + calibration\n"
          "  ppk2-tool info FILE.ppk2                                        what a .ppk2 file holds\n"
          "  ppk2-tool export FILE.ppk2                                      .ppk2 -> CSV on stdout\n"
          "  ppk2-tool list                                                  PPK2 serial ports\n"
          "  ppk2-tool capture --port P [--ammeter] [--mv MV] [--power] [--seconds S] [--ppk2 OUT]\n"
          "                                                                  live capture -> CSV or .ppk2\n"
          "  ppk2-tool sim-port [--amps A]                                   simulated PPK2 on a\n"
          "                                                                  pseudo-terminal (not Windows)\n"
          "\n"
          "A capture is the raw bytes from the measurement port: an optional metadata reply ending\n"
          "in END, then 32-bit sample words. --mv is the source voltage setting used for the current\n"
          "(default: VDD from the metadata). --drop loses N samples halfway, to test gap reporting.\n"
          "In a .ppk2 file, lost samples become records with no current and every logic channel\n"
          "\"missing\", so time stays aligned.\n"
          "\n"
          "capture: source mode at --mv (default 3300) unless --ammeter. The DUT is connected only\n"
          "with --power, and always disconnected at the end. A PPK2 has two serial ports; which one\n"
          "measures is not known yet: try each (the console port times out).\n",
          stderr);
}

static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
#ifdef _WIN32
    if (f == stdin)
        _setmode(_fileno(stdin), _O_BINARY);
#endif
    size_t cap = 1 << 16, n = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        if (n == cap) {
            uint8_t *bigger = realloc(buf, cap * 2);
            if (!bigger) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = bigger;
            cap *= 2;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0)
            break;
    }
    if (f != stdin)
        fclose(f);
    *len = n;
    return buf;
}

static const char *const k_coef_names[PPK2_COEF_COUNT] = {"R", "GS", "GI", "O", "S", "I", "UG"};

static void print_fallbacks(const ppk2_cal *cal, FILE *out)
{
    unsigned n = ppk2_cal_fallback_count(cal);
    if (n == 0) {
        fprintf(out, "calibration: every coefficient from the unit\n");
        return;
    }
    fprintf(out, "calibration: %u coefficient(s) use the fallback:", n);
    for (unsigned k = 0; k < PPK2_COEF_COUNT; k++)
        for (unsigned r = 0; r < PPK2_RANGES; r++) {
            uint64_t bit = PPK2_COEF_BIT(k, r);
            if (cal->missing & bit)
                fprintf(out, " %s%u(missing)", k_coef_names[k], r);
            else if (cal->zero & bit)
                fprintf(out, " %s%u(zero)", k_coef_names[k], r);
        }
    fputc('\n', out);
}

/* Splits a capture into metadata and samples. Returns the offset of the first sample byte. */
static size_t split_capture(const uint8_t *buf, size_t len, ppk2_meta_parser *p)
{
    size_t used = ppk2_meta_parser_feed(p, buf, len);
    if (ppk2_meta_parser_done(p))
        return used;
    ppk2_meta_parser_reset(p); /* no END: the capture has no metadata */
    return 0;
}

static int cmd_meta(const char *path)
{
    size_t len;
    uint8_t *buf = read_all(path, &len);
    if (!buf)
        return 1;
    ppk2_meta_parser *p = ppk2_meta_parser_new();
    if (split_capture(buf, len, p) == 0 && !ppk2_meta_parser_done(p)) {
        fprintf(stderr, "%s: no metadata reply (no END line)\n", path);
        free(buf);
        ppk2_meta_parser_free(p);
        return 1;
    }
    const ppk2_metadata *m = ppk2_meta_parser_result(p);
    printf("Calibrated: %s\nHW: %s\nmode: %s\nVDD: %g mV\nIA: %g\n",
           m->calibrated == 1 ? "true" : m->calibrated == 0 ? "false" : "(absent)",
           m->hw[0] ? m->hw : "(absent)", m->mode[0] ? m->mode : "(absent)", m->vdd_mv, m->ia);
    for (unsigned k = 0; k < PPK2_COEF_COUNT; k++) {
        printf("%-2s", k_coef_names[k]);
        for (unsigned r = 0; r < PPK2_RANGES; r++)
            if (m->present & PPK2_COEF_BIT(k, r))
                printf("  %14.8g", m->coef[k][r]);
            else
                printf("  %14s", "-");
        putchar('\n');
    }
    printf("unknown keys: %u, bad lines: %u\n", m->unknown_keys, m->bad_lines);
    ppk2_cal cal;
    ppk2_cal_from_metadata(&cal, m);
    print_fallbacks(&cal, stdout);
    ppk2_meta_parser_free(p);
    free(buf);
    return 0;
}

static int cmd_decode(const char *path, double mv, const char *ppk2_out)
{
    size_t len;
    uint8_t *buf = read_all(path, &len);
    if (!buf)
        return 1;
    ppk2_meta_parser *p = ppk2_meta_parser_new();
    size_t off = split_capture(buf, len, p);
    const size_t sample_bytes = len - off;
    ppk2_cal cal;
    if (ppk2_meta_parser_done(p)) {
        const ppk2_metadata *m = ppk2_meta_parser_result(p);
        ppk2_cal_from_metadata(&cal, m);
        if (isnan(mv) && !isnan(m->vdd_mv)) {
            mv = m->vdd_mv;
            fprintf(stderr, "voltage setting: %g mV taken from VDD (pass --mv if that is not the setting)\n", mv);
        }
    } else {
        ppk2_cal_default(&cal);
        fprintf(stderr, "no metadata in the capture: fallback calibration\n");
    }
    print_fallbacks(&cal, stderr);
    if (isnan(mv))
        fprintf(stderr, "no voltage setting known: current left empty (pass --mv)\n");

    ppk2_stream *s = ppk2_stream_new();
    ppk2_stream_set_calibration(s, &cal, mv / 1000.0);
    ppk2_file_writer *w = NULL;
    if (ppk2_out) {
        const int rc = ppk2_file_create(ppk2_out, PPK2_SAMPLE_RATE, NAN, &w);
        if (rc != PPK2_OK) {
            fprintf(stderr, "%s: %s\n", ppk2_out, ppk2_strerror(rc));
            ppk2_stream_free(s);
            ppk2_meta_parser_free(p);
            free(buf);
            return 1;
        }
    } else {
        printf("index,time_s,current_a,range,adc,logic,flags\n");
    }
    int write_rc = PPK2_OK;
    uint64_t index = 0;
    ppk2_sample out[4096];
    while (off < len) {
        size_t used;
        size_t n = ppk2_stream_decode(s, buf + off, len - off, &used, out, 4096);
        off += used;
        if (w) {
            float ua[1];
            uint16_t st[1];
            for (size_t k = 0; k < n && write_rc == PPK2_OK; k++) {
                /* Lost samples become records with no current and every channel "missing", so
                 * the time of each record stays right. NAN (bad range, no calibration) is
                 * stored as is. */
                if (out[k].flags & PPK2_SAMPLE_GAP) {
                    const float hole = NAN;
                    for (unsigned m = 0; m < out[k].missing && write_rc == PPK2_OK; m++)
                        write_rc = ppk2_file_append(w, &hole, NULL, 1);
                }
                ua[0] = (float)(out[k].current_a * 1e6);
                st[0] = ppk2_logic_states_from_mask(out[k].logic);
                if (write_rc == PPK2_OK)
                    write_rc = ppk2_file_append(w, ua, st, 1);
            }
        }
        for (size_t k = 0; k < n && !w; k++) {
            if (out[k].flags & PPK2_SAMPLE_GAP)
                index += out[k].missing;
            printf("%llu,%.5f,", (unsigned long long)index, (double)index / PPK2_SAMPLE_RATE);
            if (!isnan(out[k].current_a))
                printf("%.6e", out[k].current_a);
            printf(",%u,%u,0x%02X,%s%s%s\n", out[k].range, out[k].adc, out[k].logic,
                   out[k].flags & PPK2_SAMPLE_GAP ? "gap " : "",
                   out[k].flags & PPK2_SAMPLE_BAD_RANGE ? "bad-range " : "",
                   out[k].flags & PPK2_SAMPLE_AUX ? "aux" : "");
            index++;
        }
        if (used == 0)
            break; /* trailing partial word */
    }
    ppk2_stream_stats st;
    ppk2_stream_get_stats(s, &st);
    fprintf(stderr, "summary: words=%llu gaps=%llu missing=%llu bad_range=%llu trailing_bytes=%zu\n",
            (unsigned long long)st.words, (unsigned long long)st.gaps, (unsigned long long)st.missing,
            (unsigned long long)st.bad_range, sample_bytes % 4);
    int rc = 0;
    if (w) {
        const uint64_t records = ppk2_file_written(w);
        if (write_rc == PPK2_OK)
            write_rc = ppk2_file_finish(w);
        else
            ppk2_file_abort(w);
        if (write_rc != PPK2_OK) {
            fprintf(stderr, "%s: %s\n", ppk2_out, ppk2_strerror(write_rc));
            rc = 1;
        } else {
            fprintf(stderr, "wrote %s: %llu records\n", ppk2_out, (unsigned long long)records);
        }
    }
    ppk2_stream_free(s);
    ppk2_meta_parser_free(p);
    free(buf);
    return rc;
}

static ppk2_file_reader *open_ppk2(const char *path)
{
    ppk2_file_reader *r = NULL;
    const int rc = ppk2_file_open(path, &r);
    if (rc != PPK2_OK)
        fprintf(stderr, "%s: %s\n", path, ppk2_strerror(rc));
    return r;
}

static int cmd_info(const char *path)
{
    ppk2_file_reader *r = open_ppk2(path);
    if (!r)
        return 1;
    const ppk2_file_info *info = ppk2_file_get_info(r);
    printf("samples per second: %g\n", info->samples_per_second);
    if (isnan(info->start_time_ms))
        printf("start time: (absent)\n");
    else
        printf("start time: %.0f ms since 1970-01-01 UTC\n", info->start_time_ms);
    printf("records: %llu (%.3f s)\n", (unsigned long long)info->records,
           (double)info->records / info->samples_per_second);
    if (info->partial_tail)
        printf("partial record at the end: %u bytes (ignored)\n", info->partial_tail);
    printf("minimap.raw: %s\n", info->has_minimap ? "present (ignored)" : "absent");

    /* One pass for the summary: current statistics over the finite values. */
    float ua[4096];
    uint16_t st[4096];
    size_t got;
    double sum = 0.0, lo = INFINITY, hi = -INFINITY;
    uint64_t finite = 0;
    int rc;
    while ((rc = ppk2_file_read(r, ua, st, 4096, &got)) == PPK2_OK && got)
        for (size_t i = 0; i < got; i++)
            if (isfinite(ua[i])) {
                sum += ua[i];
                lo = ua[i] < lo ? ua[i] : lo;
                hi = ua[i] > hi ? ua[i] : hi;
                finite++;
            }
    if (rc != PPK2_OK)
        fprintf(stderr, "%s: %s while reading records\n", path, ppk2_strerror(rc));
    if (finite)
        printf("current: mean %.6g uA, min %.6g uA, max %.6g uA (%llu finite values)\n", sum / (double)finite,
               lo, hi, (unsigned long long)finite);
    ppk2_file_close(r);
    return rc == PPK2_OK ? 0 : 1;
}

static int cmd_export(const char *path)
{
    ppk2_file_reader *r = open_ppk2(path);
    if (!r)
        return 1;
    const double rate = ppk2_file_get_info(r)->samples_per_second;
    printf("index,time_s,current_a,d0,d1,d2,d3,d4,d5,d6,d7\n");
    static const char *const k_state[4] = {"", "0", "1", "both"};
    float ua[4096];
    uint16_t st[4096];
    size_t got;
    uint64_t index = 0;
    int rc;
    while ((rc = ppk2_file_read(r, ua, st, 4096, &got)) == PPK2_OK && got)
        for (size_t i = 0; i < got; i++, index++) {
            printf("%llu,%.6f,", (unsigned long long)index, (double)index / rate);
            if (isfinite(ua[i]))
                printf("%.6e", (double)ua[i] * 1e-6);
            for (unsigned c = 0; c < 8; c++)
                printf(",%s", k_state[ppk2_logic_state(st[i], c)]);
            putchar('\n');
        }
    if (rc != PPK2_OK)
        fprintf(stderr, "%s: %s while reading records\n", path, ppk2_strerror(rc));
    ppk2_file_close(r);
    return rc == PPK2_OK ? 0 : 1;
}

static int cmd_list(void)
{
    lmb_serial_port ports[16];
    size_t n = 0;
    const int rc = ppk2_dev_find(ports, 16, &n);
    if (rc != PPK2_OK) {
        fprintf(stderr, "list: %s\n", ppk2_dev_strerror(rc));
        return 1;
    }
    if (n == 0)
        fprintf(stderr, "no PPK2 found (USB %04x:%04x)\n", PPK2_USB_VID, PPK2_USB_PID);
    for (size_t i = 0; i < n && i < 16; i++) {
        printf("%s  interface %d", ports[i].path, ports[i].usb_interface);
        if (ports[i].serial_number[0])
            printf("  serial %s", ports[i].serial_number);
        if (ports[i].description[0])
            printf("  %s", ports[i].description);
        putchar('\n');
    }
    return 0;
}

static volatile int g_interrupted;

#if !defined(_WIN32)
static void on_signal(int sig)
{
    (void)sig;
    g_interrupted = 1;
}
#endif

static double wall_ms(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)(ts.tv_nsec / 1000000);
}

static int cmd_capture(const char *port, int ammeter, unsigned mv, int power, double seconds, const char *ppk2_out)
{
    ppk2_dev *d = NULL;
    int rc = ppk2_dev_open(port, 0, &d);
    if (rc != PPK2_OK) {
        fprintf(stderr, "%s: %s\n", port, ppk2_dev_strerror(rc));
        return 1;
    }
    print_fallbacks(ppk2_dev_calibration(d), stderr);
#if !defined(_WIN32)
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#endif
    unsigned applied = 0;
    rc = ppk2_dev_select_supply(d, ammeter ? PPK2_SUPPLY_AMMETER : PPK2_SUPPLY_SOURCE);
    if (rc == PPK2_OK)
        rc = ppk2_dev_set_voltage(d, mv, &applied); /* enters the current formula in both modes */
    if (rc == PPK2_OK && power)
        rc = ppk2_dev_set_power(d, 1);
    ppk2_file_writer *w = NULL;
    if (rc == PPK2_OK && ppk2_out)
        rc = ppk2_file_create(ppk2_out, PPK2_SAMPLE_RATE, wall_ms(), &w);
    if (rc == PPK2_OK)
        rc = ppk2_dev_start(d);
    if (rc != PPK2_OK) {
        fprintf(stderr, "%s: %s\n", port, ppk2_dev_strerror(rc));
        ppk2_file_abort(w);
        ppk2_dev_close(d);
        return 1;
    }
    fprintf(stderr, "%s mode, %u mV, DUT %s; capturing %g s (Ctrl-C stops)\n", ammeter ? "ammeter" : "source",
            applied, power ? "connected" : "not connected", seconds);
    if (!w)
        printf("index,time_s,current_a,range,logic,flags\n");
    const uint64_t total = (uint64_t)(seconds * PPK2_SAMPLE_RATE + 0.5);
    uint64_t index = 0, received = 0;
    ppk2_sample s[4096];
    int idle = 0;
    while (received < total && !g_interrupted && rc == PPK2_OK) {
        size_t got = 0;
        size_t want = total - received < 4096 ? (size_t)(total - received) : 4096;
        rc = ppk2_dev_read(d, s, want, 200, &got);
        if (rc != PPK2_OK)
            break;
        if (got == 0 && ++idle >= 10) {
            rc = PPK2_ERR_TIMEOUT; /* 2 s without a sample */
            break;
        }
        if (got)
            idle = 0;
        for (size_t k = 0; k < got && rc == PPK2_OK; k++) {
            if (s[k].flags & PPK2_SAMPLE_GAP) {
                index += s[k].missing;
                const float hole = NAN;
                for (unsigned m = 0; w && m < s[k].missing && rc == PPK2_OK; m++)
                    rc = ppk2_file_append(w, &hole, NULL, 1);
            }
            if (w) {
                const float ua = (float)(s[k].current_a * 1e6);
                const uint16_t st = ppk2_logic_states_from_mask(s[k].logic);
                if (rc == PPK2_OK)
                    rc = ppk2_file_append(w, &ua, &st, 1);
            } else {
                printf("%llu,%.5f,", (unsigned long long)index, (double)index / PPK2_SAMPLE_RATE);
                if (!isnan(s[k].current_a))
                    printf("%.6e", s[k].current_a);
                printf(",%u,0x%02X,%s%s\n", s[k].range, s[k].logic, s[k].flags & PPK2_SAMPLE_GAP ? "gap " : "",
                       s[k].flags & PPK2_SAMPLE_BAD_RANGE ? "bad-range" : "");
            }
            index++;
        }
        received += got;
    }
    ppk2_stream_stats st;
    ppk2_dev_stats(d, &st);
    ppk2_dev_close(d); /* stops and disconnects the DUT whatever happened */
    int status = 0;
    if (rc != PPK2_OK) {
        fprintf(stderr, "capture: %s\n", ppk2_dev_strerror(rc));
        status = 1;
    }
    if (w) {
        const uint64_t n = ppk2_file_written(w);
        const int frc = ppk2_file_finish(w);
        if (frc != PPK2_OK) {
            fprintf(stderr, "%s: %s\n", ppk2_out, ppk2_strerror(frc));
            status = 1;
        } else {
            fprintf(stderr, "wrote %s: %llu records\n", ppk2_out, (unsigned long long)n);
        }
    }
    fprintf(stderr, "summary: words=%llu gaps=%llu missing=%llu bad_range=%llu%s\n", (unsigned long long)st.words,
            (unsigned long long)st.gaps, (unsigned long long)st.missing, (unsigned long long)st.bad_range,
            g_interrupted ? " (interrupted)" : "");
    return status;
}

typedef struct {
    double amps;
} sim_load;

static double sim_current(void *user, uint64_t i)
{
    /* The set level with a 10 % ripple at 50 Hz, like a DUT doing periodic work. */
    const double a = ((sim_load *)user)->amps;
    return a * (1.0 + 0.1 * sin(2.0 * 3.14159265358979 * 50.0 * (double)i / PPK2_SAMPLE_RATE));
}

static int send(ppk2_sim *sim, const ppk2_tx *tx)
{
    for (size_t i = 0; i < tx->count; i++)
        if (ppk2_sim_write(sim, tx->frames[i].bytes, tx->frames[i].len) != PPK2_OK)
            return -1;
    return 0;
}

/* Writes everything the simulator sends until `words` samples have gone out. */
static void pump(ppk2_sim *sim, uint64_t words)
{
    uint8_t buf[4096];
    for (;;) {
        const uint64_t before = ppk2_sim_index(sim);
        const size_t n = ppk2_sim_read(sim, buf, sizeof buf, words);
        if (n == 0)
            break;
        fwrite(buf, 1, n, stdout);
        words -= ppk2_sim_index(sim) - before;
    }
}

static int cmd_sim(double seconds, double amps, unsigned mv, uint64_t drop)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    ppk2_sim *sim = ppk2_sim_new(NULL);
    ppk2_ctl *ctl = ppk2_ctl_new(0);
    sim_load load = {amps};
    ppk2_sim_set_load(sim, sim_current, NULL, &load);
    ppk2_tx tx;
    int rc = 0;
    rc |= ppk2_ctl_stop(ctl, &tx) || send(sim, &tx);
    rc |= ppk2_ctl_set_voltage(ctl, mv, NULL, &tx) || send(sim, &tx);
    rc |= ppk2_ctl_request_metadata(ctl, &tx) || send(sim, &tx);
    rc |= ppk2_ctl_select_supply(ctl, PPK2_SUPPLY_SOURCE, &tx) || send(sim, &tx);
    rc |= ppk2_ctl_set_power(ctl, 1, &tx) || send(sim, &tx);
    rc |= ppk2_ctl_start(ctl, &tx) || send(sim, &tx);
    if (rc) {
        fprintf(stderr, "simulator refused a command\n");
        return 1;
    }
    const uint64_t total = (uint64_t)(seconds * PPK2_SAMPLE_RATE + 0.5);
    const uint64_t half = total / 2;
    pump(sim, half);
    if (drop)
        ppk2_sim_drop(sim, drop);
    pump(sim, total - half);
    ppk2_ctl_shutdown(ctl, &tx);
    send(sim, &tx);
    ppk2_ctl_free(ctl);
    ppk2_sim_free(sim);
    return 0;
}

#if !defined(_WIN32)
static int cmd_sim_port(double amps)
{
    ppk2_sim *sim = ppk2_sim_new(NULL);
    static sim_load load;
    load.amps = amps;
    ppk2_sim_set_load(sim, sim_current, NULL, &load);
    char path[256];
    sim_port *port = NULL;
    if (!sim || sim_port_start(sim, path, sizeof path, &port) != 0) {
        perror("sim-port");
        ppk2_sim_free(sim);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("%s\n", path);
    fflush(stdout);
    fprintf(stderr, "simulated PPK2 (%g A load) on %s; Ctrl-C stops\n", amps, path);
    while (!g_interrupted)
        pause();
    sim_port_stop(port);
    ppk2_sim_free(sim);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *cmd = argv[1];
    double seconds = 1.0, amps = 1e-3, mv = NAN;
    uint64_t drop = 0;
    const char *path = NULL, *ppk2_out = NULL, *port = NULL;
    int ammeter = 0, power = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--amps") == 0 && i + 1 < argc)
            amps = atof(argv[++i]);
        else if (strcmp(argv[i], "--mv") == 0 && i + 1 < argc)
            mv = atof(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = argv[++i];
        else if (strcmp(argv[i], "--ammeter") == 0)
            ammeter = 1;
        else if (strcmp(argv[i], "--power") == 0)
            power = 1;
        else if (strcmp(argv[i], "--ppk2") == 0 && i + 1 < argc)
            ppk2_out = argv[++i];
        else if (strcmp(argv[i], "--drop") == 0 && i + 1 < argc)
            drop = strtoull(argv[++i], NULL, 10);
        else if (!path && (argv[i][0] != '-' || strcmp(argv[i], "-") == 0))
            path = argv[i];
        else {
            usage();
            return 2;
        }
    }
    if (strcmp(cmd, "sim") == 0)
        return cmd_sim(seconds, amps, isnan(mv) ? 3300u : (unsigned)mv, drop);
    if (strcmp(cmd, "decode") == 0 && path)
        return cmd_decode(path, mv, ppk2_out);
    if (strcmp(cmd, "meta") == 0 && path)
        return cmd_meta(path);
    if (strcmp(cmd, "list") == 0)
        return cmd_list();
    if (strcmp(cmd, "capture") == 0 && port)
        return cmd_capture(port, ammeter, isnan(mv) ? 3300u : (unsigned)mv, power, seconds, ppk2_out);
#if !defined(_WIN32)
    if (strcmp(cmd, "sim-port") == 0)
        return cmd_sim_port(amps);
#endif
    if (strcmp(cmd, "info") == 0 && path)
        return cmd_info(path);
    if (strcmp(cmd, "export") == 0 && path)
        return cmd_export(path);
    usage();
    return 2;
}
