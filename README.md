# Desk32

[English] | [日本語](README.ja.md)

**Desk32** is an audiophile studio-console-style music player designed to reproduce the highest-quality monitoring environment locally on Linux.

> **Note:** Desk32 is an authentic monitoring console for audiophiles and audio engineers, strictly excluding DJ mixing tools, beat sync, and crossfaders.

---

## Key Features

- **Reference Deck System:** 4-slot always-visible source comparison system (Decks A/B/C/D) with independent Transport, Seek position, and 2-mode switching (Normal & Comparative with sample/ms precision seek).
- **Folder Session:** Continuous folder auditioning from the first track in natural sort order, without relying on bloated playlists or standalone player engines.
- **3-Path Output Routing:**
  1. *Listening Path*: Dedicated acoustic correction (headphone, speaker, room EQ).
  2. *Render Monitor Path*: Audition mastering results without listening correction leakage.
  3. *Render / File Output Path*: Offline file rendering chain (Phase 4).
- **64-bit Float Precision:** Internal summing and DSP pipeline strictly adhere to IEEE 754 64-bit double-precision floating-point arithmetic.
- **Studio Console UI:** Standard layout optimized for WQHD (2560 × 1440) fullscreen, featuring mandatory track waveforms, 32-band GEQ, and 24-band (12+12) PEQ with non-destructive switching.
- **Complete Local Privacy:** Zero internet communication, telemetry, cloud sync, or online AI features. Zero artwork extraction or caching.
- **Asynchronous Operation Logging:** Structured logging into SQLite with JSONL export capabilities, fully isolated from real-time audio threads.

---

## Technical Stack

- **Language:** C++20
- **GUI Framework:** Qt 6 (Qt Quick / QML / Scene Graph)
- **Audio Backends:** PipeWire (Standard / Top Priority) / JACK (Early Priority) / ALSA direct (Subsequent)
- **Build System:** CMake (≥ 3.22) & Ninja
- **Target OS:** Linux (X11 / Wayland) — Baseline: CachyOS + COSMIC Desktop Environment

---

## Building from Source

Desk32 is distributed strictly as source code. Dependencies must be installed via your Linux distribution's package manager.

### Prerequisites (Arch Linux / CachyOS)

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-declarative qt6-tools \
    pipewire jack2 alsa-lib sqlite flac libvorbis opus ffmpeg
```

### Build & Test

```bash
# Configure via CMake Preset (Debug with GCC & Ninja)
cmake --preset default

# Build
cmake --build --preset default

# Run Automated Test Suite
ctest --preset default
```

Alternatively, to build with Clang:

```bash
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

---

## Licensing

- Desk32 custom code is licensed under the [MIT License](LICENSE).
- For third-party library licenses and compliance information, refer to [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md).
