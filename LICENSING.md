# Licensing

libmybench is licensed under the **Mozilla Public License 2.0** ([LICENSE](LICENSE)), unless a
file says otherwise.

Every source file starts with:

```
SPDX-License-Identifier: MPL-2.0
SPDX-FileCopyrightText: 2026 Martin Dolez
```

and the MPL notice (Exhibit A). A file without that header must not be added.

## What MPL-2.0 means here

- **The library stays open.** Anyone who distributes a changed version of one of these files has
  to publish that file's source under MPL-2.0.
- **Copyleft is per file.** These files may be combined with code under any other licence
  (MPL section 3.3, "Larger Work"), as long as the source of the MPL files stays available. The
  app (GPL-3.0-or-later), the closed myDAQ driver `mybench-ni`, a WebAssembly build that links
  both statically, other programs and firmware may all use the library.
- **GPL compatible.** None of these files carries Exhibit B ("Incompatible With Secondary
  Licenses"), so they can be combined into GPL-2.0-or-later, LGPL and GPL-3.0 works, such as the
  mybench app and the sigrok helper.

## How the pieces fit

| Component | Licence | Relation to libmybench |
|---|---|---|
| libmybench (this repository) | MPL-2.0 | the open device library: device model, open drivers, simulators |
| MYBENCH app | GPL-3.0-or-later with a section 7 exception for `mybench-ni` and NI-DAQmx | links libmybench (compatible, no exception needed) |
| `mybench-ni` | proprietary, binary SDK | the closed myDAQ driver; registers into libmybench, never part of this repository |
| `mybench-sigrok` helper | GPL-3.0-or-later | may link libmybench, never `mybench-ni` |

## Rules for this repository

1. **No code from:** GPL-only or LGPL-only third-party projects (libsigrok, libsigrokdecode,
   PulseView, libslogic, sigrok-pico), Nordic's pc-nrfconnect-ppk or PPK2 firmware, the
   third-party ppk2-api-python or Rust ppk2 crates (derived from Nordic's app), NI software, or
   anything under CC BY-NC / CC BY-NC-SA (for example CircuitKit). Using their **file formats**
   and **published protocol specifications** is fine; code is not.
2. **Permissive third-party code** (MIT, BSD-2/3, Apache-2.0, zlib, ISC) may be vendored under
   `third_party/<name>/` with its licence file kept and its own SPDX headers left unchanged. For
   example, the RP2 component tester's host parser (`tester_line`, `tester_client`, MIT, © 2026 Alex U)
   keeps its MIT licence.
3. **Clean-room drivers:** a driver written from an interoperability report has a `CLEANROOM.md`
   naming the only allowed sources. Gaps go back to the report's author as addenda, not to the
   vendor's code.
4. **Contributions** are accepted under MPL-2.0 (inbound = outbound). Contributors keep their
   copyright and add their own `SPDX-FileCopyrightText` line to files they change substantially.

This is the usual reading of these licences, not legal advice.
