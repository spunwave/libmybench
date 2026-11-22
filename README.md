# libmybench

The open device library of [MYBENCH](https://mybench.spunwave.com): plain C11, no Qt, a C ABI, and
MPL-2.0 licensed (see [LICENSING.md](LICENSING.md)). It sits between the app (or the CLI, a headless
MCP server, a web build, Python) and the device drivers. Each device family is one module under
`drivers/`, built as its own library with its own tool.

> Working name `libmybench-open` until the closed myDAQ driver, today's private `spunwave/libmybench`,
> has been renamed `mybench-ni`. This repository then takes the `libmybench` name. Until then, don't
> install both into the same prefix: both export a CMake package called `libmybench`.

## Modules

| Module | Device | Status |
|---|---|---|
| `drivers/ppk2` (`libmybench::ppk2`) | Nordic Power Profiler Kit II | protocol core, simulator, `ppk2-tool`; serial transport not yet written; not tested on hardware |

Planned: the device model and capability facets (`AnalogInput`, `PowerSupply`, `CurrentStream`,
`LogicInput`, …), discovery and hot-plug, serial transport (desktop and Web Serial), the RP2
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
| `LIBMYBENCH_BUILD_TESTS` / `_TOOLS` | ON | unit tests, `ppk2-tool` |
| `LIBMYBENCH_FUZZ` | OFF | libFuzzer targets (clang): `ppk2_fuzz_meta`, `ppk2_fuzz_stream` |
| `BUILD_SHARED_LIBS` | OFF | shared libraries export only the public `ppk2_*` API |

Use from CMake: `find_package(libmybench)` and link `libmybench::ppk2`.

## ppk2-tool

```sh
ppk2-tool sim --seconds 1 --amps 2e-5 > capture.bin   # simulated capture (metadata + samples)
ppk2-tool meta capture.bin                            # metadata, coefficients, fallbacks
ppk2-tool decode capture.bin > capture.csv            # index,time_s,current_a,range,adc,logic,flags
```

`decode` reads any raw byte capture of the measurement port, so a first hardware session can use a
plain serial terminal to record bytes and decode them here.
