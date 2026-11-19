# Firmware and desktop app provenance

Source repository [Yassine511/Hydroponics-Monitoring-System-Firmware](https://github.com/Yassine511/Hydroponics-Monitoring-System-Firmware), `main` at commit [`5a22bd9e6cf1508db271a46908d43b04974da595`](https://github.com/Yassine511/Hydroponics-Monitoring-System-Firmware/commit/5a22bd9e6cf1508db271a46908d43b04974da595). Upstream history credits Yassine (Yassine511) and records the co-author trailers.

| Source | Destination |
| --- | --- |
| `src/`, `include/`, `test/`, `tools/`, `platformio.ini` | `Firmware/`, with the same internal paths |
| `README.md`, architecture PDFs, and `docs/` | `Firmware/`, with the original documentation layout |
| Current `app/` modules, assets, guides, tests, tools, and build scripts | `BluetoothControlApp/`, with the same internal paths |
| `.gitignore` and `.gitattributes` | Root Git rules, adapted to the new locations |

The upstream snapshot retains the prebuilt `app/dist/` binaries and the v1 files in `app/legacy-v1/`, `app/ESP-Demo/`, and `app/FirmwareContract-v1.md`. The current app builds from its source and uses its bundled v2 simulator.

The v2 app uses the same Linux launcher identity and history database location as the destination prototype. The earlier icon, pairing code, and ESP demo are available in the [destination's v1 snapshot](https://github.com/Zer0-Dayy/Hydroponics-Monitoring-System/tree/4c0b0895dd9323b862e5e9cd1ae6bcd2e323142f/BluetoothControlApp).

The upstream snapshot contains no explicit license grant. The destination's existing [MIT notice](LICENSE) applies to its original material. The license and redistribution terms for the imported material remain unresolved.
