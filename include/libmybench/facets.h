/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Capability facets: the vtables lmb_device_facet returns. A device has the facets its hardware
 * has, and an instrument asks for the ones it needs.
 *
 * Shape of a facet:
 *   - It starts with abi_version, size (sizeof the struct as the driver was built) and self.
 *     Call every function with self as its first argument: f->start(f->self).
 *   - Functions may be added at the end of a vtable in later releases. Check LMB_HAS(f, member)
 *     before calling a member that is newer than your oldest supported release; a driver also
 *     leaves a member NULL for what the hardware cannot do (LMB_HAS is then false).
 *   - Every function returns an lmb_status. Reads return LMB_OK with *got = 0 after a timeout.
 *   - Quantities are SI doubles (volts, amperes, hertz, seconds). Time is never a clock: streams
 *     give sample indexes on the device's timeline, and time is index / sample_rate_hz.
 *   - Info structs are filled by the facet: set `size` to sizeof before the call.
 *
 * Implemented so far: PowerSupply, CurrentStream, LogicInput (the PPK2 driver). AnalogInput,
 * AnalogOutput and DigitalIO are the shape the NI myDAQ needs; no driver in this repository has
 * them yet, and they may still change before 1.0 when the myDAQ adapter exercises them.
 */
#ifndef LIBMYBENCH_FACETS_H
#define LIBMYBENCH_FACETS_H

#include "libmybench/device.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initializer of an info or state struct the facet fills: sets `size`, zeroes the rest
 *  (lmb_stream_info si = LMB_SIZED(lmb_stream_info);). Designated, so it is warning-free. */
#define LMB_SIZED(type) {.size = sizeof(type)}

/** True when vtable f is large enough to contain member and the driver set it. */
#define LMB_HAS(f, member) \
    ((f) && (size_t)((const char *)&(f)->member - (const char *)(f)) + sizeof((f)->member) <= (f)->size && (f)->member)

/* ------------------------------------------------------------------------------------------ */
/* PowerSupply: the device powers the DUT, or only measures                                    */

typedef enum lmb_supply_mode {
    LMB_SUPPLY_NONE = 0,     /**< not chosen yet */
    LMB_SUPPLY_EXTERNAL = 1, /**< the DUT has its own supply; the device only measures (PPK2 ammeter mode) */
    LMB_SUPPLY_INTERNAL = 2  /**< the device sources voltage_v to the DUT (PPK2 source mode) */
} lmb_supply_mode;

typedef struct lmb_power_supply_info {
    uint32_t size;
    uint32_t modes;    /**< bit (1u << lmb_supply_mode) for every mode available */
    double min_v;      /**< internal supply range, 0 when there is none */
    double max_v;
    double step_v;     /**< 0: continuous or unknown */
    uint32_t reserved[4];
} lmb_power_supply_info;

typedef struct lmb_power_supply_state {
    uint32_t size;
    int32_t mode;      /**< lmb_supply_mode */
    double voltage_v;  /**< the setting, NAN before one was set */
    int32_t output_on; /**< the DUT is connected to the supply (or, in external mode, to the measurement path) */
    uint32_t reserved[4];
} lmb_power_supply_state;

typedef struct lmb_power_supply {
    uint32_t abi_version;
    uint32_t size;
    void *self;
    int (*get_info)(void *self, lmb_power_supply_info *out);
    /** Choose the mode. The output is switched off first (a mode change never happens with the DUT
     *  connected). LMB_ERR_STATE while a stream is running. */
    int (*set_mode)(void *self, lmb_supply_mode mode);
    /** Set the voltage; *applied (may be NULL) gets the value after clamping to the range and the
     *  driver's cap. In external mode it still matters: some devices need it for their current
     *  calculation. LMB_ERR_STATE while a stream is running. */
    int (*set_voltage)(void *self, double volts, double *applied);
    /** Connect (on != 0) or disconnect the DUT. LMB_ERR_STATE when mode or voltage are not set yet. */
    int (*set_output)(void *self, int on);
    int (*get_state)(void *self, lmb_power_supply_state *out);
} lmb_power_supply;

/* ------------------------------------------------------------------------------------------ */
/* Streams: CurrentStream and LogicInput                                                       */
/*
 * A device that samples several things at once (the PPK2 samples current and eight logic lines
 * in the same 32-bit word) has one acquisition and one timeline. Each stream facet has its own
 * start, stop and read position: starting a second facet joins the running acquisition, and
 * samples carry their index on the shared timeline so readers can be aligned. The acquisition
 * stops when the last facet stops. Index 0 is the first sample after the acquisition started.
 */

#define LMB_SAMPLE_GAP 0x1u     /**< samples were lost right before this one (see `lost`) */
#define LMB_SAMPLE_INVALID 0x2u /**< the value is not valid (current_a is NAN, e.g. an invalid range) */

/** One current sample. Frozen layout (arrays of it cross the ABI). */
typedef struct lmb_current_sample {
    double current_a;
    uint32_t flags;   /**< LMB_SAMPLE_* */
    uint32_t lost;    /**< with LMB_SAMPLE_GAP: samples lost before this one (a lower bound), saturating */
} lmb_current_sample;

/** One snapshot of the logic lines. Frozen layout. */
typedef struct lmb_logic_sample {
    uint32_t levels;  /**< bit n = line n high */
    uint32_t flags;   /**< LMB_SAMPLE_* */
    uint32_t lost;
    uint32_t reserved;
} lmb_logic_sample;

typedef struct lmb_stream_stats {
    uint32_t size;
    uint32_t reserved0;
    uint64_t delivered; /**< samples handed to the reader since start */
    uint64_t gaps;      /**< LMB_SAMPLE_GAP events */
    uint64_t lost;      /**< samples known lost: on the wire, or dropped because the reader fell behind */
    uint64_t reserved[4];
} lmb_stream_stats;

typedef struct lmb_stream_info {
    uint32_t size;
    uint32_t flags;           /**< LMB_STREAM_* */
    double sample_rate_hz;    /**< nominal */
    uint32_t lines;           /**< logic lines (LogicInput), 0 for a current stream */
    uint32_t reserved[5];
} lmb_stream_info;

#define LMB_STREAM_GAP_DETECTION 0x1u /**< the device can tell that samples were lost */

typedef struct lmb_current_stream {
    uint32_t abi_version;
    uint32_t size;
    void *self;
    int (*get_info)(void *self, lmb_stream_info *out);
    /** Start (or join) the acquisition. LMB_ERR_STATE if this facet is started already. */
    int (*start)(void *self);
    /** Leave the acquisition; it ends with the last facet. Idempotent. Pending samples are dropped. */
    int (*stop)(void *self);
    /**
     * Pull up to cap samples, waiting up to timeout_ms (0: don't wait) for the first one. *got is 0
     * after a timeout. *first_index (may be NULL) gets the timeline index of out[0]. Samples come in
     * order; when the reader falls behind by more than the library's buffer (about a second) the
     * oldest are dropped and the next sample carries LMB_SAMPLE_GAP. LMB_ERR_STATE if not started.
     */
    int (*read)(void *self, lmb_current_sample *out, size_t cap, int timeout_ms, size_t *got, uint64_t *first_index);
    int (*get_stats)(void *self, lmb_stream_stats *out);
} lmb_current_stream;

typedef struct lmb_logic_input {
    uint32_t abi_version;
    uint32_t size;
    void *self;
    int (*get_info)(void *self, lmb_stream_info *out);
    int (*start)(void *self);
    int (*stop)(void *self);
    int (*read)(void *self, lmb_logic_sample *out, size_t cap, int timeout_ms, size_t *got, uint64_t *first_index);
    int (*get_stats)(void *self, lmb_stream_stats *out);
} lmb_logic_input;

/* ------------------------------------------------------------------------------------------ */
/* AnalogInput: shaped for the myDAQ (AI0/AI1 at 200 kS/s, audio in). Not implemented yet.      */

typedef struct lmb_analog_input_info {
    uint32_t size;
    uint32_t channels;        /**< channel count; channel numbers are 0..channels-1 */
    double max_rate_hz;       /**< per channel */
    double channel_skew_s;    /**< channel k is sampled k * skew after channel 0 in a scan (0: simultaneous) */
    uint32_t range_count;
    double ranges_v[8];       /**< symmetric full scales, +-ranges_v[i] */
    uint32_t reserved[4];
} lmb_analog_input_info;

typedef struct lmb_analog_input_config {
    uint32_t size;
    uint32_t channel_mask;    /**< bit n = channel n; samples are interleaved in channel order */
    double range_v;           /**< full scale for all selected channels; the nearest not smaller is used */
    double rate_hz;           /**< per channel; coerced, the result is returned by configure */
    int32_t continuous;       /**< 0: finite, `samples_per_channel` per start; 1: until stop */
    uint64_t samples_per_channel;
    uint32_t reserved[4];
} lmb_analog_input_config;

typedef struct lmb_analog_input {
    uint32_t abi_version;
    uint32_t size;
    void *self;
    int (*get_info)(void *self, lmb_analog_input_info *out);
    /** Reserve channels and set the timing; *actual_rate_hz gets the coerced rate. LMB_ERR_BUSY if reserved elsewhere. */
    int (*configure)(void *self, const lmb_analog_input_config *cfg, double *actual_rate_hz);
    int (*start)(void *self);
    int (*stop)(void *self);
    /** Pull up to cap_frames frames (one value per selected channel each, volts). LMB_ERR_OVERFLOW when
     *  continuous data was lost (the call still returns what is there, in *got). */
    int (*read)(void *self, double *out, size_t cap_frames, int timeout_ms, size_t *got_frames);
    /** One on-demand value, without a configured task. LMB_ERR_BUSY while a task owns the channel. */
    int (*read_static)(void *self, uint32_t channel, double range_v, double *volts);
} lmb_analog_input;

/* ------------------------------------------------------------------------------------------ */
/* AnalogOutput: shaped for the myDAQ (AO0/AO1, audio out). Not implemented yet.               */

typedef struct lmb_analog_output_info {
    uint32_t size;
    uint32_t channels;
    double max_rate_hz;
    uint32_t range_count;
    double ranges_v[8];
    uint32_t reserved[4];
} lmb_analog_output_info;

typedef struct lmb_analog_output_config {
    uint32_t size;
    uint32_t channel_mask;
    double range_v;
    double rate_hz;
    int32_t continuous;
    int32_t regenerate;       /**< continuous: 1 repeats the buffer written before start; 0 streams (write while running) */
    uint32_t reserved[4];
} lmb_analog_output_config;

typedef struct lmb_analog_output {
    uint32_t abi_version;
    uint32_t size;
    void *self;
    int (*get_info)(void *self, lmb_analog_output_info *out);
    int (*configure)(void *self, const lmb_analog_output_config *cfg, double *actual_rate_hz);
    /** Frames of interleaved volts. Before start: sets the buffer. Streaming: appends, waiting up to timeout_ms for room. */
    int (*write)(void *self, const double *data, size_t frames, int timeout_ms, size_t *written_frames);
    int (*start)(void *self);
    int (*stop)(void *self);    /**< outputs keep their last value */
    /** Finite: wait until every sample was generated; *done is 0 after a timeout. */
    int (*wait_done)(void *self, int timeout_ms, int *done);
    int (*write_static)(void *self, uint32_t channel, double volts);
} lmb_analog_output;

/* ------------------------------------------------------------------------------------------ */
/* DigitalIO: shaped for the myDAQ (DIO 0..7). Not implemented yet.                            */

typedef struct lmb_digital_io_info {
    uint32_t size;
    uint32_t lines;           /**< line numbers 0..lines-1, at most 32 */
    uint32_t input_only_mask; /**< lines that cannot be outputs (0: all can) */
    uint32_t reserved[5];
} lmb_digital_io_info;

typedef struct lmb_digital_io {
    uint32_t abi_version;
    uint32_t size;
    void *self;
    int (*get_info)(void *self, lmb_digital_io_info *out);
    /** Lines with a 1 in mask become outputs (output != 0) or inputs. */
    int (*set_direction)(void *self, uint32_t mask, int output);
    int (*get_direction)(void *self, uint32_t *outputs);
    /** Drive the output lines in mask to the bits of value. */
    int (*write)(void *self, uint32_t value, uint32_t mask);
    /** Levels of all lines. */
    int (*read)(void *self, uint32_t *levels);
} lmb_digital_io;

/* ------------------------------------------------------------------------------------------ */
/* Typed lookups                                                                              */

#define LMB_FACET_GETTER_(fn, kind, type)                                                       \
    static inline int fn(lmb_device *dev, const type **out)                                    \
    {                                                                                           \
        const void *p = NULL;                                                                   \
        const int rc = lmb_device_facet(dev, (kind), &p);                                       \
        if (out)                                                                                \
            *out = (rc == LMB_OK) ? (const type *)p : NULL;                                     \
        return rc;                                                                              \
    }

LMB_FACET_GETTER_(lmb_device_power_supply, LMB_FACET_POWER_SUPPLY, lmb_power_supply)
LMB_FACET_GETTER_(lmb_device_current_stream, LMB_FACET_CURRENT_STREAM, lmb_current_stream)
LMB_FACET_GETTER_(lmb_device_logic_input, LMB_FACET_LOGIC_INPUT, lmb_logic_input)
LMB_FACET_GETTER_(lmb_device_analog_input, LMB_FACET_ANALOG_INPUT, lmb_analog_input)
LMB_FACET_GETTER_(lmb_device_analog_output, LMB_FACET_ANALOG_OUTPUT, lmb_analog_output)
LMB_FACET_GETTER_(lmb_device_digital_io, LMB_FACET_DIGITAL_IO, lmb_digital_io)

#ifdef __cplusplus
}
#endif

#endif
