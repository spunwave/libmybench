# PPK2 driver: clean-room record

## Allowed sources

Only these, kept outside this repository in the project's private reference folder:

| Source | Identity |
|---|---|
| `PPK2-technical-report.md`, an interoperability report written by someone else from desktop app 4.4.1 (commit 39bba39), firmware 1.2.4-db16a94 and board rev 1.0.1 | sha256 `68106d75…b553` |
| Nordic's published PCA63100 1.0.1 board files (schematic, BOM, gerbers) | sha256 `8dc51b74…0725` |

Not read, not copied, not to be read while working on this driver: Nordic's pc-nrfconnect-ppk source,
the PPK2 firmware image, the third-party ppk2-api-python and Rust ppk2 crates (derived from Nordic's
app). Behaviour the report leaves open is settled with USB captures and measurements on a real
PPK2, or comes back from the report's author as a written addendum.

## What the code takes from the report

| Report section | Code |
|---|---|
| Measurement connection: command bytes, one write each, power order | `src/ppk2_ctl.c` |
| Calibration and current units: metadata lines, coefficients, fallbacks, formula | `src/ppk2_meta.c` |
| Sample framing and loss: word layout, carry, sequence, invalid range | `src/ppk2_stream.c` |
| Board connections: gate after the shunts, disconnect before changing supply | `src/ppk2_ctl.c`, `src/ppk2_sim.c` |
| Capture interchange: `session.raw` records, `metadata.json`, the ZIP container | `src/ppk2_stream.c` (records), `src/ppk2_file.c` |

Golden vectors used in `tests/test_ppk2.c`: `23 41 54 A5` → ADC 291, range 1, aux 0, sequence 21,
logic A5; `00 00 80 3F 55 56` = 1.0 µA with D0 high; the 3.3 V example sequence
`0C 00, 11 02, 0D 0C E4, 0C 01, 06 … 07, 0C 00`.

## Deliberate choices

- User gain (`25 …`) is not sent: the report suspects a firmware payload-length mismatch.
  Calibration is applied on the host. The simulator accepts and ignores it.
- The measurement port is chosen by USB interface number, not "first listed" (transport, not yet written).
- `ppk2_ctl` refuses a metadata request until the stream is known to be stopped, so a unit left
  streaming by another program can't corrupt the reply.
- Numbers in the metadata are parsed without the C locale (a French or Dutch desktop would
  otherwise read `1.5` wrongly).

## Open questions (hardware or report addendum)

1. Interface numbers of the two CDC ports on Linux, macOS and Windows, and whether their order is stable.
2. Value formats of `Calibrated`, `HW` and `mode` (the parser accepts true/false/1/0/yes/no and keeps the other two as text).
3. **Fallback GS = 1.** With the report's fallbacks the x² term is as large as x itself near 1 A
   (range 3–4), so a unit without `GS` keys would read far too high there. Either real units always send
   GS, or the fallback is used differently. Ask the report's author, and check on a real unit.
4. Bit 17 (auxiliary USB sense), bytes arriving after `07`, and where gaps appear when the host stops reading.
5. `minimap.raw` layout, and whether nRF Connect opens a `.ppk2` without it.
6. Whether `VDD` in the metadata equals the current voltage setting (the tool uses it only when `--mv` is not given).
