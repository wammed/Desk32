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
    pipewire jack2 alsa-lib sqlite flac libvorbis opus ffmpeg vulkan-headers vulkan-icd-loader
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

## Known Issues

### Memory usage keeps growing while playing with the window visible

On Wayland, if you keep playing with the Desk32 window visible, the process memory usage (RSS) grows almost linearly over time. In the development environment, 12 hours of continuous playback measured **+54.8 MB/h** (335.9 MB → 974.3 MB). The process itself remains stable, and no impact on playback or audio (XRUN) was observed under the same conditions. Minimizing the window stops the growth.

#### Cause

This is a defect on the rendering platform side, not in Desk32's own code. Investigation found that the main component (about +49.3 MB/h) is response events of `wl_display.sync`, issued by the Qt Wayland EGL integration on every frame, accumulating inside libwayland-client at 216 bytes each without being released (approx. 63.4 frames/sec × 216 bytes). This event queue cannot be operated from the application side, so it cannot be fundamentally fixed in Desk32's code — the handle of the queue in question is not exposed to the application.

#### How it was investigated (summary)

1. Reproduced the linear growth of about +54–55 MB/h in 9.8-hour and 12-hour playback tests. The growth was uncorrelated with track changes, and thread and file descriptor counts were stable.
2. Short controlled runs narrowed the cause to rendering with the window displayed: no growth in a minimized idle state, and no change with the entire DSP chain bypassed.
3. Heap profiling with heaptrack identified that 94% of the growth came from a single allocation site inside libwayland-client's `wl_display_read_events` (216 bytes, allocated at frame frequency and not freed until exit). Stack traces also confirmed that Desk32's own code does not appear on this path.
4. Protocol logging with `WAYLAND_DEBUG=client` counted the per-frame events and confirmed that the sync callbacks arrive at 63.42/sec, matching the observed memory growth.

#### Impact and workarounds

- The impact is proportional to the time spent playing with the window visible (about +49 MB/h for this issue in the measured environment). It is not a problem for shorter sessions, or when listening with the window minimized.
- It does not occur when started via XWayland (`QT_QPA_PLATFORM=xcb`) (measured 2026-11-14: almost no memory growth over 30 minutes of playback; the Wayland path is a necessary condition for this issue).
- It also does not occur with the Vulkan rendering backend (measured 2026-11-14: almost zero under the same conditions — supporting evidence that the problem lies in the EGL rendering path).
- **Current configuration (decided 2026-11-16, implemented 2026-11-17)**: Desk32's default backend is Vulkan (this issue does not occur). On systems where Vulkan is unavailable, Desk32 falls back to OpenGL; in that case, particularly with the NVIDIA + Wayland combination, this issue may occur, so please be careful during long playback sessions. The xcb startup above is also a workaround when using OpenGL.
- It may be resolved by fixes on the Qt or graphics driver (EGL) side; we will re-check when dependencies are updated.

#### Verified environment

- OS: CachyOS, Desktop: COSMIC (Wayland), GPU: NVIDIA (EGL)
- Desk32 build: commit `94960aa` (2026-11-12)
- Other environments (different compositor, GPU, or Qt version) may differ in whether this occurs and how fast memory grows.

---

## Licensing

- Desk32 custom code is licensed under the [MIT License](LICENSE).
- For third-party library licenses and compliance information, refer to [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md).
