/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * libmybench device model: what an application sees. A context holds the registered device
 * families; it lists the devices present, opens one, and hands out its capability facets
 * (facets.h). Nothing here knows a particular family. Families plug in through driver.h.
 *
 * Read docs/DEVICE-MODEL.md for the rules in full. In short:
 *
 *   ABI      One integer, LMB_ABI_VERSION. Only an exact match works (lmb_context_new). Within a
 *            version the API only grows: new functions, new enum values, new members at the end
 *            of a facet vtable (each vtable carries its size).
 *   Errors   Every function returns an lmb_status (0 or negative), never throws, never aborts.
 *   Memory   The library allocates devices and contexts; the caller frees them with the matching
 *            *_close / *_free. Facet pointers belong to the device and die with it.
 *   Threads  A context is thread-safe once its families are registered. A device may be used from
 *            several threads, but each facet from one thread at a time (see DEVICE-MODEL.md).
 */
#ifndef LIBMYBENCH_DEVICE_H
#define LIBMYBENCH_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#include "libmybench/export.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The ABI this header describes. Bumped only by an incompatible change. */
#define LMB_ABI_VERSION 1u

/** The ABI the loaded library was built with (check it against LMB_ABI_VERSION at start-up). */
LMB_API uint32_t lmb_abi_version(void);

typedef enum lmb_status {
    LMB_OK = 0,
    LMB_ERR_INVALID_ARG = -1,  /**< bad argument (NULL, out of range, unknown enum value) */
    LMB_ERR_ABI = -2,          /**< the caller and the library (or a driver) disagree on the ABI */
    LMB_ERR_NOT_FOUND = -3,    /**< no such device */
    LMB_ERR_NO_FACET = -4,     /**< the device does not have that facet */
    LMB_ERR_UNSUPPORTED = -5,  /**< the facet exists but not this operation, or this setting */
    LMB_ERR_STATE = -6,        /**< not allowed now (not started, already started, wrong order) */
    LMB_ERR_BUSY = -7,         /**< the device is open already, here or in another program */
    LMB_ERR_ACCESS = -8,       /**< the system refused access (permissions) */
    LMB_ERR_DISCONNECTED = -9, /**< the device went away; close it */
    LMB_ERR_TIMEOUT = -10,     /**< the device did not answer in time (reads return OK with 0 items instead) */
    LMB_ERR_IO = -11,          /**< transport or protocol failure */
    LMB_ERR_NO_MEM = -12,
    LMB_ERR_EXISTS = -13,      /**< duplicate registration */
    LMB_ERR_OVERFLOW = -14     /**< data was lost because the caller did not read fast enough */
} lmb_status;

/** Description of an lmb_status (static string, never NULL). */
LMB_API const char *lmb_strerror(int code);

/** Capability kinds. Values are stable; new kinds are appended. */
typedef enum lmb_facet_kind {
    LMB_FACET_POWER_SUPPLY = 1,   /**< source voltage, output switch, measure-only mode */
    LMB_FACET_CURRENT_STREAM = 2, /**< continuous current at a fixed rate */
    LMB_FACET_LOGIC_INPUT = 3,    /**< digital lines sampled on the device's timeline */
    LMB_FACET_ANALOG_INPUT = 4,   /**< voltage channels, hardware-timed or on demand */
    LMB_FACET_ANALOG_OUTPUT = 5,  /**< voltage outputs, hardware-timed or on demand */
    LMB_FACET_DIGITAL_IO = 6,     /**< static digital lines with a direction each */
    LMB_FACET_SIM_CONTROL = 7,    /**< simulated devices only: load, lost samples, unplug and replug */
    LMB_FACET_KIND_END_
} lmb_facet_kind;

/** Bit of a facet in lmb_device_info.facets. */
#define LMB_FACET_BIT(kind) (1u << (unsigned)(kind))

/** Name of a facet kind ("current-stream"), "?" for an unknown value. */
LMB_API const char *lmb_facet_name(int kind);

/* ------------------------------------------------------------------------------------------ */
/* Descriptors                                                                                */

/**
 * What enumeration says about a device. Arrays of these are exchanged with the library, so the
 * layout is frozen for the whole ABI version (the reserved bytes are for additions that fit).
 * All strings are UTF-8, NUL-terminated.
 */
typedef struct lmb_device_info {
    char id[64];        /**< stable key to open the device with ("ppk2:ABC123", "sim:ppk2"); opaque */
    char family[32];    /**< driver family ("ppk2") */
    char name[64];      /**< display name */
    char serial[64];    /**< serial number, "" when unknown */
    char transport[32]; /**< "usb-serial", "usb", "sim", ... */
    uint32_t facets;    /**< LMB_FACET_BIT mask of what lmb_device_facet will return */
    int32_t simulated;  /**< 1 for a simulator, 0 for hardware */
    uint32_t reserved[8];
} lmb_device_info;

/** lmb_enumerate flags. 0 means everything. */
#define LMB_ENUM_NO_SIMULATED 0x1u /**< skip simulators */
#define LMB_ENUM_ONLY_SIMULATED 0x2u

/* ------------------------------------------------------------------------------------------ */
/* Context: the registry                                                                      */

typedef struct lmb_context lmb_context;
typedef struct lmb_device lmb_device;

/**
 * New empty context. abi_version must be LMB_ABI_VERSION (as the application was compiled
 * against it): LMB_ERR_ABI otherwise, and *out stays NULL. Register families next
 * (lmb_register_builtin_families, or one family's own register function).
 */
LMB_API int lmb_context_new(uint32_t abi_version, lmb_context **out);
/** Close the devices still open, destroy the families, free. NULL is fine. */
LMB_API void lmb_context_free(lmb_context *ctx);

/**
 * List the devices present now, from every family (or only those matching the flags). Writes
 * up to cap entries into out; *count gets the total found, which may exceed cap (call with
 * cap 0 and out NULL to size the array). Each call rescans.
 */
LMB_API int lmb_enumerate(lmb_context *ctx, uint32_t flags, lmb_device_info *out, size_t cap, size_t *count);

/**
 * Open a device by lmb_device_info.id. LMB_ERR_NOT_FOUND for an id no family claims or a
 * device that is gone, LMB_ERR_BUSY when it is open already (in this context, or another
 * program holds it). On error *out is NULL. A device powers nothing and starts nothing by itself.
 */
LMB_API int lmb_device_open(lmb_context *ctx, const char *id, lmb_device **out);

/**
 * Stop whatever the device is doing, put it in its safe state (a supply is switched off, streams
 * stop) and release it. Never fails; NULL is fine. The handle and every facet pointer obtained
 * from it are invalid afterwards. Not to be called while another thread is inside a call on the
 * same device.
 */
LMB_API void lmb_device_close(lmb_device *dev);

/** Descriptor of an open device (what enumeration reported when it was opened). */
LMB_API int lmb_device_get_info(const lmb_device *dev, lmb_device_info *out);

/**
 * LMB_OK while the device is usable, LMB_ERR_DISCONNECTED once it went away (every facet call
 * then returns that too). lmb_poll_events reports a change of it as LMB_EVENT_STATUS.
 */
LMB_API int lmb_device_status(lmb_device *dev);

/**
 * The facet of this kind: *out points at its vtable (the matching type in facets.h, for example
 * const lmb_current_stream *), owned by the device and valid until lmb_device_close. The same
 * pointer comes back on every call. LMB_ERR_NO_FACET when the device does not have it (check
 * lmb_device_info.facets to know in advance), LMB_ERR_INVALID_ARG for a kind outside the enum.
 * Typed wrappers for each facet are in facets.h.
 */
LMB_API int lmb_device_facet(lmb_device *dev, lmb_facet_kind kind, const void **out);

/* ------------------------------------------------------------------------------------------ */
/* Hot-plug                                                                                   */

/**
 * Polled events (lmb_poll_events): what changed since the previous call. Polling needs no thread
 * of the library's own, which is what a single-threaded host (WebAssembly) and a Qt timer want.
 */
typedef enum lmb_event_type {
    LMB_EVENT_ARRIVED = 1, /**< a device is listed that was not before */
    LMB_EVENT_LEFT = 2,    /**< a listed device is gone */
    LMB_EVENT_STATUS = 3   /**< an open device's lmb_device_status changed (it went away, or is back) */
} lmb_event_type;

typedef struct lmb_event {
    uint32_t size;     /**< sizeof(lmb_event): set by the caller before lmb_poll_events, the library writes only what fits */
    uint32_t type;     /**< lmb_event_type; an unknown value is something newer than the caller */
    int32_t status;    /**< LMB_EVENT_STATUS: the lmb_status of the open device now (LMB_OK or LMB_ERR_DISCONNECTED) */
    uint32_t reserved0;
    lmb_device_info info; /**< the device (for LEFT, as last listed) */
    uint32_t reserved[8];
} lmb_event;

/**
 * Rescan (like lmb_enumerate, simulators included) and report the differences as events, oldest
 * first: up to cap events into out, *count gets how many were written (cap 0 and out NULL: *count
 * is how many are pending). Events that do not fit stay queued for the next call. The first call
 * reports every device present as ARRIVED, so a caller needs no separate lmb_enumerate. Every
 * open device whose status changed since the last call gets a STATUS event. Each element of out
 * must have `size` set to sizeof(lmb_event). Safe from any thread (calls are serialised).
 */
LMB_API int lmb_poll_events(lmb_context *ctx, lmb_event *out, size_t cap, size_t *count);

typedef enum lmb_hotplug_event {
    LMB_HOTPLUG_ARRIVED = 1,
    LMB_HOTPLUG_LEFT = 2
} lmb_hotplug_event;

/**
 * Called when a device arrives or leaves, with its descriptor. It runs on a thread owned by the
 * library, never concurrently with itself, and must not call lmb_device_close, lmb_context_free
 * or block for long: post to your own queue.
 */
typedef void (*lmb_hotplug_fn)(void *user, lmb_hotplug_event event, const lmb_device_info *info);

/**
 * Subscribe (cb non-NULL) or unsubscribe (cb NULL). Not implemented in this version: always
 * LMB_ERR_UNSUPPORTED. Use lmb_poll_events.
 */
LMB_API int lmb_set_hotplug(lmb_context *ctx, lmb_hotplug_fn cb, void *user);

/* ------------------------------------------------------------------------------------------ */
/* Built-in families                                                                          */

/**
 * Register every family this build of the library contains (those enabled with
 * LIBMYBENCH_DRIVER_*), so an application needs no family-specific code. Lives in the
 * libmybench::builtin target. Families from outside (a closed driver) register themselves
 * through their own function.
 */
LMB_API int lmb_register_builtin_families(lmb_context *ctx);

#ifdef __cplusplus
}
#endif

#endif
