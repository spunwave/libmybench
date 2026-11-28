# libmybench

The open device library of [MYBENCH](https://mybench.spunwave.com): plain C11, no Qt, a C ABI, and
MPL-2.0 licensed (see [LICENSING.md](LICENSING.md)). It sits between the app (or the CLI, a headless
MCP server, a web build, Python) and the device drivers. Each device family is one module under
`drivers/`, built as its own library with its own tool.

> Working name `libmybench-open`: the closed myDAQ driver has moved to `mybench-ni`, and this repository
> takes the `libmybench` name once MYBENCH fetches the driver from its new place. Until the driver's
> CMake package is renamed too, don't install both into the same prefix: both export a package called
> `libmybench`.

## Modules

| Module | Device | Status |
|---|---|---|
| `drivers/ppk2` (`libmybench::ppk2`) | Nordic Power Profiler Kit II | protocol core, simulator, device layer on the serial transport, `.ppk2` files (read and write), `ppk2-tool`; tested end to end against the simulator on a pseudo-terminal, not yet on hardware |
| `transports/serial` (`libmybench::serial`) | USB CDC and other serial ports | list ports with their USB ids and interface (Linux sysfs, macOS IOKit, Windows SetupAPI), open raw with DTR, timed reads, exclusive use (busy detection) |

Planned: the device model and capability facets (`AnalogInput`, `PowerSupply`, `CurrentStream`,
`LogicInput`, …), discovery and hot-plug, Web Serial, the RP2
component tester, the DSP moved out of the app, and file formats. The design and order of work are in
MYBENCH's `docs/design/driver-architecture-options.md`.

## How drivers are written

Each driver has a **protocol core that does no I/O**: commands go in and come out as the bytes to
write, and bytes read go in and come out as parsed data. Nothing in the core blocks, reads a clock or
allocates outside `*_new()`. Every path can be tested with fixed byte vectors (golden tests,
splits at every byte boundary, fuzzing), and the same code runs on the desktop, in WebAssembly and on a
microcontroller. Transports and threads live in a thin layer on top, and the simulator speaks the same
bytes as the hardware, so the simulator path tests the real parser.

## Build

```sh
cmake -S . -B build -G Ninja -DLIBMYBENCH_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

| Option | Default | |
|---|---|---|
| `LIBMYBENCH_DRIVER_PPK2` | ON | the PPK2 module |
| `LIBMYBENCH_TRANSPORT_SERIAL` | ON | the serial transport; without it the PPK2 module has no device layer (core, simulator and files only) |
| `LIBMYBENCH_BUILD_TESTS` / `_TOOLS` | ON | unit tests, `ppk2-tool` |
| `LIBMYBENCH_FUZZ` | OFF | libFuzzer targets (clang): `ppk2_fuzz_meta`, `ppk2_fuzz_stream`, `ppk2_fuzz_file`; the library itself is instrumented too |
| `BUILD_SHARED_LIBS` | OFF | shared libraries export only the public API (`ppk2_*`, `lmb_serial_*`) |

Use from CMake: `find_package(libmybench)` and link `libmybench::ppk2` (it brings `libmybench::serial`).

`.ppk2` files are ZIP archives; the reader and writer use miniz 3.1.2 (MIT), vendored in `third_party/miniz/`
and built into the library with hidden symbols.

## ppk2-tool

```sh
ppk2-tool sim --seconds 1 --amps 2e-5 > capture.bin   # simulated capture (metadata + samples)
ppk2-tool meta capture.bin                            # metadata, coefficients, fallbacks
ppk2-tool decode capture.bin > capture.csv            # index,time_s,current_a,range,adc,logic,flags
ppk2-tool decode --ppk2 capture.ppk2 capture.bin      # the same capture as a .ppk2 file
ppk2-tool info capture.ppk2                           # rate, start time, records, minimap
ppk2-tool export capture.ppk2 > capture.csv           # .ppk2 -> CSV

ppk2-tool list                                        # PPK2 serial ports (USB interface, serial number)
ppk2-tool capture --port /dev/ttyACM0 --mv 3300 --power --seconds 5 --ppk2 run.ppk2
ppk2-tool sim-port --amps 2.5e-5                      # simulated PPK2 on a pseudo-terminal (not Windows):
                                                      # prints its path, use it as --port
```

`capture` opens the port (exclusively), stops a stream left running by another program, reads the
unit's calibration, sets the supply mode and voltage, powers the device under test if asked, and
records at 100 kS/s; samples lost on the way are counted as gaps. Closing powers the DUT off.

`decode` reads any raw byte capture of the measurement port, so a first hardware session can use a
plain serial terminal to record bytes and decode them here.
