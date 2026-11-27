/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * libmybench PPK2 driver: protocol core for the Nordic Power Profiler Kit II (USB 1915:C00A).
 *
 * Written only from the clean-room interoperability report (see drivers/ppk2/CLEANROOM.md).
 *
 * This module does no I/O. It turns commands into the exact bytes to write, and bytes read from
 * the measurement port into metadata, calibration and samples. A transport (serial port, Web
 * Serial, a capture file, the simulator below) moves the bytes. Nothing here allocates except
 * the *_new() functions, nothing blocks, and nothing reads a clock, so every path is testable
 * with fixed byte vectors and the same code runs on desktop, WebAssembly and microcontrollers.
 *
 * Overview
 *   ppk2_ctl          command builder that enforces the safe power order (gate off before a
 *                     supply change, supply selected before the gate goes on, gate off on close).
 *   ppk2_meta_parser  incremental parser for the metadata reply ("key: value" lines up to END).
 *   ppk2_cal          per-unit calibration, with the report's fallbacks and which ones were used.
 *   ppk2_stream       sample-stream decoder: 32-bit words split anywhere across reads, sequence
 *                     gap detection, current in amperes.
 *   ppk2_record_*     the 6-byte records of a .ppk2 capture's session.raw.
 *   ppk2_sim          a simulated PPK2 that accepts the same commands and produces real wire bytes.
 *   ppk2_file_*       .ppk2 capture files (ZIP: metadata.json + session.raw), read and written.
 *                     Unlike the rest, these do file I/O (stdio), or read from memory.
 *
 * Errors: functions returning int return PPK2_OK (0) or a negative ppk2_status.
 *
 * Threading: no global state. An object is used from one thread at a time.
 *
 * ABI: 0.x releases make no ABI promise. From 1.0, a change to a public struct bumps the major
 * version. Stateful objects are opaque for that reason.
 */
#ifndef LIBMYBENCH_PPK2_H
#define LIBMYBENCH_PPK2_H

#include <stddef.h>
#include <stdint.h>

#include "libmybench/export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PPK2_USB_VID 0x1915
#define PPK2_USB_PID 0xC00A

#define PPK2_SAMPLE_RATE 100000u /**< nominal records per second (one 32-bit word each) */
#define PPK2_RANGES 5            /**< shunt ranges 0-4, in order of decreasing resistance */
#define PPK2_VOLTAGE_MIN_MV 800
#define PPK2_VOLTAGE_MAX_MV 5000

typedef enum ppk2_status {
    PPK2_OK = 0,
    PPK2_ERR_INVALID_ARG = -1, /**< bad argument */
    PPK2_ERR_STATE = -2,       /**< not allowed in the current state (see ppk2_ctl) */
    PPK2_ERR_NO_SPACE = -3,    /**< the output buffer is too small */
    PPK2_ERR_FORMAT = -4,      /**< malformed input */
    PPK2_ERR_NO_MEM = -5,      /**< allocation failed */
    PPK2_ERR_IO = -6,          /**< a file or port could not be opened, read or written */
    PPK2_ERR_UNSUPPORTED = -7, /**< well-formed but not supported (e.g. another .ppk2 version) */
    PPK2_ERR_TIMEOUT = -8,     /**< the device did not answer in time */
    PPK2_ERR_DISCONNECTED = -9 /**< the device went away */
} ppk2_status;

/** Description of a ppk2_status (static string, never NULL). */
LMB_API const char *ppk2_strerror(int code);

/* ------------------------------------------------------------------------------------------ */
/* Commands and the power sequence                                                            */

typedef enum ppk2_supply {
    PPK2_SUPPLY_NONE = 0,    /**< not selected yet (state after ppk2_ctl_new) */
    PPK2_SUPPLY_AMMETER = 1, /**< external input powers the DUT, the PPK2 measures (11 01) */
    PPK2_SUPPLY_SOURCE = 2   /**< the internal regulated source powers the DUT (11 02) */
} ppk2_supply;

/** One command. Send each frame as its own write, in order, with no terminator. */
typedef struct ppk2_frame {
    uint8_t len;
    uint8_t bytes[3];
} ppk2_frame;

#define PPK2_TX_MAX 4

/** The frames a ppk2_ctl call wants written. Calls clear it first. */
typedef struct ppk2_tx {
    size_t count;
    ppk2_frame frames[PPK2_TX_MAX];
} ppk2_tx;

typedef struct ppk2_ctl ppk2_ctl;

/**
 * New command builder. cap_mv limits the source voltage below the hardware maximum
 * (0 = PPK2_VOLTAGE_MAX_MV). The gate starts in an unknown state, so the first call that
 * selects a supply or turns the gate on writes "gate off" first.
 */
LMB_API ppk2_ctl *ppk2_ctl_new(unsigned cap_mv);
LMB_API void ppk2_ctl_free(ppk2_ctl *ctl);

/**
 * Ask for the metadata reply (19). PPK2_ERR_STATE unless the stream is known to be stopped:
 * after opening the port (or after ppk2_ctl_invalidate) call ppk2_ctl_stop and drain the port
 * first, since a unit left streaming by a previous program would mix samples into the reply.
 */
LMB_API int ppk2_ctl_request_metadata(ppk2_ctl *ctl, ppk2_tx *tx);

/**
 * Select the supply. When the gate is on or unknown, it is turned off first: a supply change
 * never happens with the DUT connected. Selecting the current supply again sends nothing
 * unless the gate state is unknown.
 */
LMB_API int ppk2_ctl_select_supply(ppk2_ctl *ctl, ppk2_supply supply, ppk2_tx *tx);

/**
 * Set the source voltage (0D hi lo). The value is clamped to 800 mV .. the cap; *applied_mv
 * (may be NULL) gets what was sent. The setting also enters the current calculation in
 * ammeter mode, so set it in both modes.
 */
LMB_API int ppk2_ctl_set_voltage(ppk2_ctl *ctl, unsigned mv, unsigned *applied_mv, ppk2_tx *tx);

/**
 * Connect (on != 0) or disconnect the DUT (0C 01 / 0C 00). Turning it on needs a selected
 * supply, and a set voltage when the supply is the internal source; otherwise PPK2_ERR_STATE.
 * Turning it off is always allowed.
 */
LMB_API int ppk2_ctl_set_power(ppk2_ctl *ctl, int on, ppk2_tx *tx);

/** Start (06) / stop (07) the sample stream. Stop leaves the gate as it is. */
LMB_API int ppk2_ctl_start(ppk2_ctl *ctl, ppk2_tx *tx);
LMB_API int ppk2_ctl_stop(ppk2_ctl *ctl, ppk2_tx *tx);

/** Before closing the port: stop (if sampling or unknown) and gate off, always. */
LMB_API int ppk2_ctl_shutdown(ppk2_ctl *ctl, ppk2_tx *tx);

/** A write failed or its result is unknown: assume nothing about the gate or the stream. */
LMB_API void ppk2_ctl_invalidate(ppk2_ctl *ctl);

LMB_API ppk2_supply ppk2_ctl_supply(const ppk2_ctl *ctl);
LMB_API int ppk2_ctl_power_on(const ppk2_ctl *ctl);  /**< 1 on, 0 off, -1 unknown */
LMB_API int ppk2_ctl_sampling(const ppk2_ctl *ctl);  /**< 1 sampling, 0 idle, -1 unknown */
/** The voltage setting in volts (the V of the current formula), or NAN before one was set. */
LMB_API double ppk2_ctl_source_volts(const ppk2_ctl *ctl);

/* ------------------------------------------------------------------------------------------ */
/* Metadata and calibration                                                                   */

/** Coefficient kinds, each with one value per range (keys R0..R4, GS0..GS4 and so on). */
typedef enum ppk2_coef {
    PPK2_COEF_R = 0, /**< shunt resistance, ohms */
    PPK2_COEF_GS,
    PPK2_COEF_GI,
    PPK2_COEF_O,
    PPK2_COEF_S,
    PPK2_COEF_I,
    PPK2_COEF_UG,
    PPK2_COEF_COUNT
} ppk2_coef;

/** Bit of a coefficient in ppk2_metadata.present and the ppk2_cal masks. */
#define PPK2_COEF_BIT(kind, range) (UINT64_C(1) << ((unsigned)(kind) * PPK2_RANGES + (unsigned)(range)))

typedef struct ppk2_metadata {
    int calibrated;   /**< 1 / 0 from the Calibrated key, -1 when absent or unreadable */
    double vdd_mv;    /**< VDD, NAN when absent */
    double ia;        /**< IA (amplifier offset), NAN when absent */
    char hw[32];      /**< HW, "" when absent */
    char mode[32];    /**< mode, "" when absent */
    double coef[PPK2_COEF_COUNT][PPK2_RANGES];
    uint64_t present; /**< PPK2_COEF_BIT set for every coefficient that was present and readable */
    uint32_t unknown_keys; /**< well-formed lines with a key not listed above */
    uint32_t bad_lines;    /**< lines that are not "key: value", values that don't parse, over-long lines */
} ppk2_metadata;

typedef struct ppk2_meta_parser ppk2_meta_parser;

LMB_API ppk2_meta_parser *ppk2_meta_parser_new(void);
LMB_API void ppk2_meta_parser_free(ppk2_meta_parser *p);
LMB_API void ppk2_meta_parser_reset(ppk2_meta_parser *p);

/**
 * Feed bytes read after the metadata request. Returns how many were consumed: all of them,
 * unless the END line is among them, in which case consumption stops right after it and the
 * rest belongs to the next parser. Numbers are read the same way whatever the C locale.
 */
LMB_API size_t ppk2_meta_parser_feed(ppk2_meta_parser *p, const uint8_t *data, size_t len);
/** 1 once the END line was seen. */
LMB_API int ppk2_meta_parser_done(const ppk2_meta_parser *p);
/** What was parsed so far (complete after done). */
LMB_API const ppk2_metadata *ppk2_meta_parser_result(const ppk2_meta_parser *p);

typedef struct ppk2_cal {
    double coef[PPK2_COEF_COUNT][PPK2_RANGES];
    uint64_t missing; /**< coefficients that fell back because the key was absent */
    uint64_t zero;    /**< coefficients that fell back because the value was zero (only kinds
                           whose fallback is not zero: R, GS, GI, UG) */
} ppk2_cal;

/** The report's fallback set: R = 1031.64, 101.65, 10.15, 0.94, 0.043; GS = GI = UG = 1; O = S = I = 0. */
LMB_API void ppk2_cal_default(ppk2_cal *cal);
/** Calibration of one unit from its metadata. A missing or zero coefficient keeps its fallback. */
LMB_API void ppk2_cal_from_metadata(ppk2_cal *cal, const ppk2_metadata *meta);
/** Number of coefficients that fell back (missing or zero). 0 for a fully calibrated unit. */
LMB_API unsigned ppk2_cal_fallback_count(const ppk2_cal *cal);
/**
 * Current in amperes for one ADC value in one range, before any display filtering.
 * source_volts is the voltage setting (ppk2_ctl_source_volts), also in ammeter mode.
 * NAN when range > 4 or source_volts is NAN.
 */
LMB_API double ppk2_cal_current(const ppk2_cal *cal, unsigned adc, unsigned range, double source_volts);

/* ------------------------------------------------------------------------------------------ */
/* Sample stream                                                                              */

typedef struct ppk2_word {
    uint16_t adc;  /**< bits 0-13 */
    uint8_t range; /**< bits 14-16; 5-7 are invalid */
    uint8_t aux;   /**< bit 17, auxiliary USB power sense (unconfirmed on hardware) */
    uint8_t seq;   /**< bits 18-23, increments modulo 64 */
    uint8_t logic; /**< bits 24-31, D0 in bit 0 */
} ppk2_word;

LMB_API ppk2_word ppk2_word_decode(uint32_t word);
LMB_API uint32_t ppk2_word_encode(ppk2_word w);

#define PPK2_SAMPLE_AUX 0x01u       /**< auxiliary USB sense bit set */
#define PPK2_SAMPLE_GAP 0x02u       /**< sequence jump: samples were lost right before this one */
#define PPK2_SAMPLE_BAD_RANGE 0x04u /**< range code 5-7: current is NAN, logic is still valid */

typedef struct ppk2_sample {
    double current_a; /**< NAN for a bad range or when the stream has no calibration */
    uint16_t adc;
    uint8_t range;
    uint8_t logic;    /**< D0 in bit 0 */
    uint8_t flags;    /**< PPK2_SAMPLE_* */
    uint8_t missing;  /**< with PPK2_SAMPLE_GAP: samples lost, modulo 64 (a lower bound) */
} ppk2_sample;

typedef struct ppk2_stream_stats {
    uint64_t words;     /**< words decoded */
    uint64_t gaps;      /**< sequence jumps */
    uint64_t missing;   /**< samples known lost (sum of the jumps, each modulo 64) */
    uint64_t bad_range; /**< words with range 5-7 */
} ppk2_stream_stats;

typedef struct ppk2_stream ppk2_stream;

LMB_API ppk2_stream *ppk2_stream_new(void);
LMB_API void ppk2_stream_free(ppk2_stream *s);
/** Forget carried bytes and the sequence: call after stop, drain and restart. Keeps the statistics. */
LMB_API void ppk2_stream_reset(ppk2_stream *s);
/** Calibration and voltage setting used for current_a (copied). cal NULL: current_a is NAN. */
LMB_API void ppk2_stream_set_calibration(ppk2_stream *s, const ppk2_cal *cal, double source_volts);

/**
 * Decode bytes into at most cap samples. *consumed gets the bytes used; it is less than len
 * only when out filled up, so call again with the rest. Up to 3 trailing bytes are carried
 * to the next call. Returns the number of samples written.
 */
LMB_API size_t ppk2_stream_decode(ppk2_stream *s, const uint8_t *data, size_t len, size_t *consumed,
                                  ppk2_sample *out, size_t cap);
LMB_API void ppk2_stream_get_stats(const ppk2_stream *s, ppk2_stream_stats *stats);

/**
 * Losses the sequence check can't see (multiples of 64, such as a whole 512-word block): the
 * words expected for elapsed_s at the nominal rate, minus those received and those known lost.
 * USB timing jitter makes small values meaningless; compare it over seconds, not milliseconds.
 */
LMB_API uint64_t ppk2_possible_undetected_loss(uint64_t words_received, uint64_t missing_detected,
                                               double elapsed_s);

/* ------------------------------------------------------------------------------------------ */
/* .ppk2 capture records (session.raw)                                                         */

#define PPK2_RECORD_SIZE 6

/** Two-bit logic state of one channel in a record. */
enum {
    PPK2_LOGIC_MISSING = 0,
    PPK2_LOGIC_LOW = 1,
    PPK2_LOGIC_HIGH = 2,
    PPK2_LOGIC_BOTH = 3
};

/** Record: current in microamperes (binary32 LE), then logic states (u16 big-endian, 2 bits per channel). */
LMB_API void ppk2_record_encode(float current_ua, uint16_t logic_states, uint8_t out[PPK2_RECORD_SIZE]);
LMB_API void ppk2_record_decode(const uint8_t in[PPK2_RECORD_SIZE], float *current_ua, uint16_t *logic_states);
/** States for a plain D0-D7 mask: high or low on every channel. */
LMB_API uint16_t ppk2_logic_states_from_mask(uint8_t mask);
LMB_API unsigned ppk2_logic_state(uint16_t states, unsigned channel);

/* ------------------------------------------------------------------------------------------ */
/* .ppk2 capture files (format version 2)                                                       */

/*
 * A ZIP archive holding metadata.json ({"formatVersion": 2, "metadata": {"samplesPerSecond": ...,
 * "startSystemTime": ...}}) and session.raw (the records above, record i at time
 * i / samplesPerSecond). minimap.raw, the desktop app's overview, is neither read nor written:
 * its layout is not documented in the clean-room report. Whether the desktop app opens a file
 * without it is an open question (drivers/ppk2/CLEANROOM.md).
 */

typedef struct ppk2_file_info {
    double samples_per_second;
    double start_time_ms;  /**< startSystemTime (Unix epoch, ms), NAN when absent */
    uint64_t records;      /**< complete records in session.raw */
    unsigned partial_tail; /**< bytes after the last complete record (0-5), not returned */
    int has_minimap;       /**< the archive has a minimap.raw (ignored) */
} ppk2_file_info;

typedef struct ppk2_file_reader ppk2_file_reader;

/** Open a capture. PPK2_ERR_IO (can't open), _FORMAT (not a .ppk2), _UNSUPPORTED (formatVersion != 2). */
LMB_API int ppk2_file_open(const char *path, ppk2_file_reader **out);
/** Same, from a file in memory; data must stay valid until ppk2_file_close. */
LMB_API int ppk2_file_open_mem(const void *data, size_t len, ppk2_file_reader **out);
LMB_API const ppk2_file_info *ppk2_file_get_info(const ppk2_file_reader *r);
/**
 * The next records, in order: up to cap of them into current_ua and/or logic_states (either
 * may be NULL). *got is 0 at the end. PPK2_ERR_FORMAT if the archive is damaged (CRC errors are
 * not detected until the whole entry has been read).
 */
LMB_API int ppk2_file_read(ppk2_file_reader *r, float *current_ua, uint16_t *logic_states, size_t cap,
                           size_t *got);
LMB_API void ppk2_file_close(ppk2_file_reader *r);

typedef struct ppk2_file_writer ppk2_file_writer;

/**
 * Start a capture file at path. Records go to "<path>.part" while recording, and
 * ppk2_file_finish writes the archive. start_time_ms: Unix epoch in ms, or NAN to leave it out.
 */
LMB_API int ppk2_file_create(const char *path, double samples_per_second, double start_time_ms,
                             ppk2_file_writer **out);
/** Append n records. logic_states may be NULL (all channels "missing"). */
LMB_API int ppk2_file_append(ppk2_file_writer *w, const float *current_ua, const uint16_t *logic_states, size_t n);
/** Records appended so far. */
LMB_API uint64_t ppk2_file_written(const ppk2_file_writer *w);
/** Write the archive, remove the .part file and free w (also on error). */
LMB_API int ppk2_file_finish(ppk2_file_writer *w);
/** Drop the capture: remove the .part file and free w. */
LMB_API void ppk2_file_abort(ppk2_file_writer *w);

/**
 * metadata.json by itself, for other containers and for tests. parse: PPK2_ERR_FORMAT (not JSON,
 * or formatVersion / samplesPerSecond missing or invalid), PPK2_ERR_UNSUPPORTED (another version).
 * format: returns the length written (without the terminating NUL) or a negative ppk2_status.
 */
LMB_API int ppk2_file_parse_metadata(const char *text, size_t len, ppk2_file_info *info);
LMB_API int ppk2_file_format_metadata(char *buf, size_t cap, double samples_per_second, double start_time_ms);

/* ------------------------------------------------------------------------------------------ */
/* Simulated PPK2                                                                             */

typedef struct ppk2_sim ppk2_sim;

/** Current drawn by the simulated DUT in amperes, and the D0-D7 levels, at sample index i. */
typedef double (*ppk2_sim_current_fn)(void *user, uint64_t i);
typedef uint8_t (*ppk2_sim_logic_fn)(void *user, uint64_t i);

/**
 * A simulated unit with the given calibration (NULL: the fallback set). Coefficients marked
 * in cal->missing are left out of its metadata reply, like a unit that lacks them. The
 * default load draws 1 mA with D0-D7 counting up every 1000 samples.
 */
LMB_API ppk2_sim *ppk2_sim_new(const ppk2_cal *cal);
LMB_API void ppk2_sim_free(ppk2_sim *sim);
LMB_API void ppk2_sim_set_load(ppk2_sim *sim, ppk2_sim_current_fn current, ppk2_sim_logic_fn logic, void *user);

/** Bytes written to the measurement port. Every command must arrive in one call (one write). */
LMB_API int ppk2_sim_write(ppk2_sim *sim, const uint8_t *data, size_t len);

/**
 * Bytes the port has to read: a pending metadata reply first, then, while sampling, at most
 * words_due sample words (the caller paces time). Returns the bytes written to out.
 */
LMB_API size_t ppk2_sim_read(ppk2_sim *sim, uint8_t *out, size_t cap, uint64_t words_due);

/** Lose n samples without sending them (gap injection for tests). */
LMB_API void ppk2_sim_drop(ppk2_sim *sim, uint64_t n);
/** Index of the next sample. */
LMB_API uint64_t ppk2_sim_index(const ppk2_sim *sim);
/** The current the simulated wire actually encodes for a load (after ADC rounding), for tests. */
LMB_API double ppk2_sim_quantize(const ppk2_sim *sim, double amps, unsigned *range_out);

#ifdef __cplusplus
}
#endif

#endif
