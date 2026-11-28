<!--
SPDX-License-Identifier: MPL-2.0
SPDX-FileCopyrightText: 2026 Martin Dolez
-->
# The device model

An application that wants to talk to bench hardware needs three things: a list of devices, a way to
open one, and a way to ask "what can it do?". This is that, behind a plain C ABI (C11, no Qt, no C++,
no exceptions), so the same code serves a Qt app, a command-line tool, a headless server, Python
bindings and a WebAssembly build.

```
 application ──► device.h / facets.h ──► core ──► driver.h ◄── a device family (ppk2, ...)
                 (what you include)      registry               (what a driver implements)
```

| Header | Who includes it | What |
|---|---|---|
| `libmybench/device.h` | applications | context, enumeration, open/close, facet lookup, error codes, ABI version |
| `libmybench/facets.h` | applications | the facet vtables: PowerSupply, CurrentStream, LogicInput, AnalogInput, AnalogOutput, DigitalIO |
| `libmybench/driver.h` | drivers | the family interface, plus a mutex and a monotonic clock so drivers need no platform code |
| `libmybench/ppk2_family.h` | the PPK2 family | its register function and the simulator controls |

## Using it

```c
lmb_context *ctx;
lmb_context_new(LMB_ABI_VERSION, &ctx);          // fails with LMB_ERR_ABI on a mismatch
lmb_register_builtin_families(ctx);              // the only place that knows which families exist

lmb_device_info list[16]; size_t n;
lmb_enumerate(ctx, 0, list, 16, &n);             // id, family, name, serial, transport, facets

lmb_device *dev;
lmb_device_open(ctx, "sim:ppk2", &dev);

const lmb_power_supply *ps;   const lmb_current_stream *cs;
lmb_device_power_supply(dev, &ps);               // LMB_ERR_NO_FACET if the device lacks it
lmb_device_current_stream(dev, &cs);

ps->set_mode(ps->self, LMB_SUPPLY_EXTERNAL);
ps->set_voltage(ps->self, 3.3, NULL);
ps->set_output(ps->self, 1);

cs->start(cs->self);
lmb_current_sample buf[1024]; size_t got; uint64_t first;
cs->read(cs->self, buf, 1024, 100 /* ms */, &got, &first);
cs->stop(cs->self);
lmb_device_close(dev);                           // powers the DUT off, releases the port
lmb_context_free(ctx);
```

`tools/lmb_tool.c` is a complete client of exactly this API (`lmb-tool list`, `info`, `stream`). It contains
no family-specific code.

## Facets

A facet is a capability, not a device type. A device has the facets its hardware has, and an instrument
asks for the ones it needs. `lmb_device_info.facets` is a bit mask, so a UI can grey out instruments
before opening anything.

| Facet | PPK2 | myDAQ (later) | Status |
|---|---|---|---|
| PowerSupply (mode, voltage, output switch) | source / ammeter, 0.8 to 5 V, DUT gate | not controllable | implemented (PPK2) |
| CurrentStream (current at a fixed rate) | 100 kS/s | - | implemented (PPK2) |
| LogicInput (digital lines on the sample timeline) | D0 to D7 inside the sample word | - | implemented (PPK2) |
| AnalogInput (channels, ranges, rate, finite or continuous, on-demand read) | - | AI0/AI1 at 200 kS/s, audio in | shape only |
| AnalogOutput (same, regenerate or stream) | - | AO0/AO1, audio out | shape only |
| DigitalIO (direction per line, write, read) | - | DIO 0 to 7 | shape only |
| SimControl (load, pulsed load, lost samples, unplug, replug) | simulator only | simulator, later | implemented (PPK2 simulator) |

Not yet shaped, and added when the myDAQ adapter needs them: Counter (edge count, frequency, period,
pulse width, encoders), Dmm (functions and ranges, one blocking reading), FrequencyOutput. They are new
enum values and new headers; nothing existing changes.

How the shapes follow the app's needs today (`mydaq::Device` in MYBENCH): an AI task is
channels + range + rate + finite/continuous, a blocking read of `n` frames with a timeout, a reported
inter-channel skew and an overflow error: that is `lmb_analog_input` (`configure`, `start`, `read`,
`channel_skew_s`, `LMB_ERR_OVERFLOW`). Static single reads (`aiReadStatic`, `aoWriteStatic`) are
`read_static` and `write_static`. The DIO calls are `set_direction`, `write` with a mask, `read`.
The AI/AO shapes are the least certain part: they will be exercised for the first time by the myDAQ
adapter and may change until 1.0.

### Shared timeline

A device that samples several things in one acquisition (the PPK2: current and eight logic lines in one
32-bit word) has one acquisition and one timeline. CurrentStream and LogicInput each have their own
`start`, `stop` and read position. The first facet to start begins the acquisition, the others join it,
and it ends when the last stops. Every read returns `first_index`, the position of its first sample on the
device's timeline (0 is the first sample after the acquisition began), so a reader that joined late, or
two readers in different threads, can be aligned. Time is `index / sample_rate_hz`; the API never hands
out wall-clock timestamps, because a USB device has none.

Lost samples are part of the timeline: when the device reports that `n` samples were lost, the next sample
has `LMB_SAMPLE_GAP` and `lost = n`, and its index skips them. **`lost` is a lower bound**: the PPK2's
sequence number is 6 bits, so a loss of a multiple of 64 samples (a whole 512-sample block, say) goes
unseen, and the count saturates. The timeline index of every sample is exact for the samples that were
seen, so the number really lost between two reads is the jump in `first_index`, and the position of the
gap inside a read is the index of the flagged sample. Use index jumps when the exact count matters; use
`lost` and `LMB_SAMPLE_GAP` to know where the data is not continuous.

**Current flows only with the DUT gate on.** On a PPK2 the DUT gate sits after the shunts: with the
PowerSupply output ("DUT power") off, the current stream reads zero in ammeter mode as well as in source
mode. Starting a stream does not switch the output on, so a client that wants to measure sets the mode and
voltage, switches the output on and then starts (or starts first and switches on: the stream just reads
zero until then). The simulator behaves the same way.

### Simulated devices: the SimControl facet

A simulated device also has the SimControl facet (`lmb_sim_control`, `lmb_device_sim_control`), so an
application, a test or a demo can drive it without including a family header: `set_load` (constant current of
the simulated DUT), `set_load_pulses` (base and peak current, period, duty), `drop` (lose n samples on the
simulated wire), `disconnect` (every later call returns `LMB_ERR_DISCONNECTED`) and `reconnect` (the same
open device works again; restart a stream that was running). Hardware never has the facet, so
`lmb_device_info.facets` tells beforehand whether the controls exist. The family functions
(`lmb_ppk2_sim_set_load` and so on) remain and do the same.

## ABI and versioning

- **One integer**, `LMB_ABI_VERSION` (1). `lmb_context_new` takes the version the application was compiled
  against and fails with `LMB_ERR_ABI` if the library's differs. `lmb_abi_version()` returns the library's.
  A driver passes its own version when it registers, and a facet vtable carries it too.
- **Within a version the API only grows**: new functions, new enum values, new members at the **end** of a
  facet vtable. Nothing is removed, reordered or changed in meaning. An incompatible change bumps
  `LMB_ABI_VERSION` and is a major release.
- **Vtables are versioned by size.** Every facet starts with `abi_version`, `size` (what the driver was
  built with) and `self`. A caller built against older headers sees a prefix of a newer vtable. A caller
  built against newer headers must test `LMB_HAS(facet, member)` before using a member added after its
  oldest supported release. A driver leaves members it cannot implement `NULL`; `LMB_HAS` is false for those.
- **Info and state structs** that a facet fills are initialised by the caller with `LMB_SIZED(type)` (sets
  `size`, zeroes the rest). A facet writes only the members that fit in `size`. Reserved members are zero.
- **Arrays that cross the ABI have a frozen layout** (`lmb_device_info`, `lmb_current_sample`,
  `lmb_logic_sample`): their size can't change within a version, so they carry `reserved` members and new
  information goes into new functions. This is why samples are small fixed structs.
- **Enums**: an application treats an unknown value as "something newer than me". Device kinds, error codes
  and facet kinds are only ever appended. Values are never reused.
- **Strings** are UTF-8, NUL-terminated, in fixed buffers inside the structs (no ownership questions).
- Pre-1.0 releases (the library is 0.x) make no promise on top of this: facet shapes may still change, in
  which case `LMB_ABI_VERSION` goes up.
- Only functions marked `LMB_API` are exported from the shared libraries (`BUILD_SHARED_LIBS=ON`).

## Errors

Every function that can fail returns an `lmb_status`: 0 or a negative code, with `lmb_strerror()` for the
text. Nothing aborts, throws or writes to `stderr`. Outputs are set to NULL or 0 on failure. Codes are
specific enough to act on: `LMB_ERR_BUSY` (open elsewhere), `LMB_ERR_ACCESS` (permissions: show the udev
hint), `LMB_ERR_DISCONNECTED` (close it), `LMB_ERR_STATE` (wrong order or mode), `LMB_ERR_OVERFLOW`
(continuous data was lost), `LMB_ERR_UNSUPPORTED` (the facet exists, this operation or value doesn't).

A read that times out is **not** an error: it returns `LMB_OK` with `*got = 0`. A failure while waiting is.

## Threading

- **Context**: register families first, from one thread, before sharing the context. After that,
  `lmb_enumerate`, `lmb_device_open` and `lmb_device_close` may be called from any thread (the core
  serialises its list of open devices). Opening is not held under a lock, so a slow open does not block
  enumeration.
- **Device**: may be used from several threads. **A facet is used by one thread at a time; different
  facets of the same device may be used concurrently.** The typical shape is a reader thread blocked in
  `CurrentStream.read` while the UI thread calls `PowerSupply.set_output`. The driver does the locking
  between facets, and never holds a lock while waiting for hardware longer than a few milliseconds.
- **Close**: `lmb_device_close` must not run while another thread is inside a call on that device. Stop your
  reader thread first (stop the stream, then join), then close. Closing stops streams and puts the device in
  its safe state; for the PPK2 that means the DUT is disconnected.
- **No callbacks into your code.** Streams are **pull**: you call `read` with a
  timeout and the library never calls you from a thread of its own. Reasons: it matches how the serial and
  USB layers work (`ppk2_dev_read`), it keeps ownership and thread questions out of the ABI, it works the
  same from a Qt worker, a Python thread and a single-threaded WebAssembly main loop, and a push API can be
  built on top by a few lines of application code while the reverse can't be done without a buffer. The cost
  is that someone has to call `read` often enough: at 100 kS/s the PPK2 sends 400 kB/s, and an unread
  device first fills the library's ring of about 1.3 s, then loses samples (reported as a gap, never
  silently).
- A read never waits longer than its timeout. A negative timeout is an error; there is no "forever".

## Hot-plug: polled events

`lmb_poll_events(ctx, out, cap, &n)` rescans (like `lmb_enumerate`) and reports what changed since the previous
call, oldest first, as `lmb_event`s:

| Event | Meaning |
|---|---|
| `LMB_EVENT_ARRIVED` | a device is listed that was not before (the first call reports every device present) |
| `LMB_EVENT_LEFT` | a listed device is gone (`info` is what was listed) |
| `LMB_EVENT_STATUS` | an open device's `lmb_device_status` changed: `status` is `LMB_ERR_DISCONNECTED` or `LMB_OK` again |

Events that do not fit in `cap` stay queued for the next call; `cap` 0 with `out` NULL returns how many are
pending. Each `lmb_event` carries its `size` (set it to `sizeof(lmb_event)` before the call; a caller built
with a shorter struct receives only the prefix that fits). Calls are serialised, so any thread may poll, but
one poller per context is the sensible shape.

Polling needs no thread of the library's own, so it works from a Qt timer, a Python loop and the single
main loop of a WebAssembly build alike; how often to call it is the application's choice (a rescan of
serial ports is cheap, a USB probe may not be). The callback `lmb_set_hotplug` stays declared and returns
`LMB_ERR_UNSUPPORTED`: a library thread calling into the application is exactly what this API avoids.
An open device that disappears returns `LMB_ERR_DISCONNECTED` from every call; the STATUS event is how a
poller learns of it without touching the device.

## Writing a device family

A family is `lmb_family` (`driver.h`): a name, a `user` pointer, and
`enumerate`, `open`, `destroy`. `open` fills an `lmb_device_ops` (`close`, `facet`, `status`, and your own
state in `drv`) and the `lmb_device_info`. Then:

1. **Facet vtables** live in your device struct and are returned from `facet`. Fill `abi_version` with
   `LMB_ABI_VERSION`, `size` with `sizeof` the struct, `self` with your state. Leave unsupported members NULL.
2. **Register explicitly**: expose `int lmb_<family>_register(lmb_context *)` that fills an `lmb_family`
   and calls `lmb_context_register_family`. There are no static constructors (they don't survive static
   libraries or WebAssembly). In-tree families are added to `builtin/` (a definition and a link line);
   a family from another repository (such as the closed myDAQ driver) is registered by the application, or
   by that repository's own `libmybench::builtin`-like glue.
3. **Ids** are yours: `"<family>:<something stable>"`, at most 63 bytes. `open` returns `LMB_ERR_NOT_FOUND`
   for an id that is not yours; the core asks the next family. The core refuses to open an id twice in one
   context (`LMB_ERR_BUSY`); your transport detects another program holding the port.
4. **Simulators** are devices of the same family with `simulated = 1` and a transport of `"sim"`, listed by
   `enumerate` unless `LMB_ENUM_NO_SIMULATED`. They go through the same facets, so an application and its
   tests run without hardware. The PPK2 simulator uses the real command builder, the real wire format and the
   real decoder; it only replaces the serial port.
5. **No global state.** Everything hangs off `user` and the open device. Two contexts in one process work.
6. **Safe state on close.** `close` stops streams and leaves outputs off.
7. **Timeouts**: never block longer than the caller allowed. Enumerate quickly (no long probes): it is
   called from UI code.
8. A family that cannot list (no permission to read the ports) returns an error from `enumerate`; the core
   skips it and still lists the others.

A runtime-loaded plugin (option C in the architecture note) would be the same `lmb_family` handed over by a
`dlopen`ed `lmb_<family>_register`; nothing here has to change for that, and nothing here builds it.

## The PPK2 family

`drivers/ppk2/src/ppk2_family.c` adapts `ppk2_dev` (hardware) and the in-process simulator to the three
stream and supply facets. Specific behaviour, all documented in `ppk2_family.h`:

- Ids: `ppk2:<usb serial>` (the unit's two serial ports are one device; open tries the lower USB interface
  first and falls back to the other, because which one measures is still an open hardware question), and
  `sim:ppk2` / `sim:ppk2:<n>` for simulators.
- The current stream, the logic input and the supply share one device lock, which is released between
  polls. A read polls the device without blocking, sleeps 1 ms and retries until its timeout.
- One ring of about 1.3 s (131072 samples) feeds every stream facet; each facet has its own read position.
- The supply mode and voltage can't change while a stream runs: the current formula takes the voltage at
  start. The output switch can.
- Starting a stream with nothing chosen selects the external-supply mode at 3.3 V, so "start" measures.

## How the MYBENCH app consumes it

No change to the app is part of this repository; this is the intended shape of the step that follows.

One generic C++ adapter in the app, `bench::Device` implemented once over `lmb_device`, replaces one adapter
per family:

- **Discovery**: a provider that calls `lmb_poll_events` (on a timer) and turns each
  `lmb_device_info` into the app's `DeviceInfo`; `facets` drives which instruments are enabled
  (`InstrumentDescriptor.requires`).
- **Facets**: the adapter exposes each `lmb_*` facet as the matching `bench::` facet object. The mapping is
  one to one, so the C++ side is thin wrappers that translate `lmb_status` into `DeviceError::Code`
  (`LMB_ERR_BUSY` to `ResourceBusy`, `LMB_ERR_ACCESS` to `AccessDenied`, `LMB_ERR_DISCONNECTED` to
  `NotConnected`, `LMB_ERR_OVERFLOW` to `Overflow`, `LMB_ERR_TIMEOUT` to `Timeout`).
- **Streaming**: an instrument's worker thread calls `read` in a loop with a 50 to 100 ms timeout and feeds
  the DSP; a stop request is just the loop ending, then `stop`. Samples carry their timeline index, so the
  Power Profiler plots against `index / rate` and shows gaps from `LMB_SAMPLE_GAP`.
- **Simulation** needs no code in the app for a family that ships its own simulated devices: they show up in
  the list with `simulated = 1`.
- **Build**: one switch for the library; the families inside it are libmybench's own options. The closed
  myDAQ driver registers into the same context.

The myDAQ goes through the same adapter once `mybench-ni` implements AnalogInput, AnalogOutput and
DigitalIO (and Counter and Dmm, to be shaped). Whether any generic part of that driver becomes open is a
separate decision and is not assumed here.

## Open questions

- The AI/AO/DIO shapes are drawn from the app's current interface, not exercised by a driver.
- Hot-plug is polled (`lmb_poll_events`); a callback thread owned by the library is not planned. A family
  that can be told by the operating system (udev, WM_DEVICECHANGE) could make polling cheaper later without
  changing the API.
- Sample structs are per facet and small (16 bytes). If a future device needs millions of samples per
  second, a second read function that takes a struct-of-arrays buffer can be added without breaking anything.
- Changing the supply voltage during a stream (profiling a voltage sweep) is not supported yet: it needs
  the decoder to follow the voltage setting at a known sample.
