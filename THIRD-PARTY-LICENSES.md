# Third-Party Licenses Ledger

- **Document Version:** 1.0.0
- **Last Updated:** 2026-11-07
- **Project:** Desk32
- **Desk32 Own License:** MIT License (Primary / Default)
- **Distribution Model:** Source Code Only (No binary redistribution by Desk32 project)

Desk32 does not vendor or bundle third-party source code into this repository. All external libraries are utilized as dynamic system packages (`.so`) provided by users and respective Linux distributions. The table below documents licensing and copyright details for external libraries evaluated and utilized by Desk32.

---

## 1. Third-Party Dependencies Ledger

| Package Name | Adopted Version (Baseline) | Purpose | SPDX License Identifier | Copyright Holders & Upstream URL | Modifications | Linking / Dependency Mode |
|---|---|---|---|---|---|---|
| **Qt 6 (Core, Quick, Qml, QuickControls2)** | 6.12.0 | GUI framework, Scene Graph, i18n | `LGPL-3.0-only` OR `GPL-3.0-only` | The Qt Company & Qt Project contributors<br>https://www.qt.io/ | None (Unmodified) | Dynamic link (`.so` / System package) |
| **libpipewire-0.3** | 1.6.10 | Standard audio backend, graph routing | `MIT` / `LGPL-2.1-or-later` | Wim Taymans, Red Hat, PipeWire contributors<br>https://pipewire.org/ | None (Unmodified) | Dynamic link (`.so` / System package) |
| **JACK (libjack)** | 1.9.22 | Pro-audio routing, external host bridge | `LGPL-2.1-or-later` | Stephane Letz, Paul Davis, JACK contributors<br>https://jackaudio.org/ | None (Unmodified) | Dynamic link (`.so` / PipeWire-JACK) |
| **alsa-lib (libasound)** | 1.2.16.1 | Hardware direct capability query, ALSA direct | `LGPL-2.1-or-later` | Jaroslav Kysela, ALSA Project contributors<br>https://www.alsa-project.org/ | None (Unmodified) | Dynamic link (`.so` / System package) |
| **SQLite** | 3.54.0 | Operation logs (Log Tab), configs, library DB | `blessing` (Public Domain) | D. Richard Hipp, SQLite contributors<br>https://www.sqlite.org/ | None (Unmodified) | Dynamic link (`.so` / System package) |
| **libFLAC** | 1.5.0 | Hi-Res PCM decoding (FLAC) | `BSD-3-Clause` | Xiph.Org Foundation<br>https://xiph.org/flac/ | None (Unmodified) | Dynamic link (`.so` / System package) |
| **libvorbisfile** | 1.3.7 | Ogg Vorbis compressed audio decoding | `BSD-3-Clause` | Xiph.Org Foundation<br>https://xiph.org/vorbis/ | None (Unmodified) | Dynamic link (`.so` / System package) |
| **libopus** | 1.6.1 | Opus low-latency audio decoding | `BSD-3-Clause` | Xiph.Org Foundation, Skype Ltd., CSIRO<br>https://opus-codec.org/ | None (Unmodified) | Dynamic link (`.so` / System package) |
| **FFmpeg (libavcodec, libavformat)** | 63.1.102 | Comprehensive multi-codec decoding candidate | `LGPL-2.1-or-later` | FFmpeg team<br>https://ffmpeg.org/ | None (Unmodified) | Dynamic link (`.so` / System package) |

---

## 2. Distribution Model Compliance

1. **Source Code Distribution Policy:**
   - The Desk32 project distributes source code only. It does not distribute precompiled binaries or install packages.
   - Users install dependencies via their respective distribution package managers and build locally.
2. **LGPL Compliance via Dynamic Linking:**
   - All LGPL libraries (Qt 6, PipeWire, ALSA, FFmpeg, etc.) are strictly dynamically linked, keeping Desk32's proprietary MIT codebase clearly separated and ensuring user replaceability of libraries.
3. **Strict No-Vendoring Rule:**
   - No third-party code is vendored or bundled into the repository, ensuring complete freedom from redistribution copyright liabilities.
