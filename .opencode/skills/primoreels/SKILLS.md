# PrimoReels Developer & Agent Skills

This document serves as the core reference guide for developers and AI agents working on **PrimoReels**, a modern desktop video editor built with **C++17**, **Qt 6 (QML)**, and **FFmpeg**.

---

## 1. Project Overview & Technology Stack

- **Language:** C++17
- **UI Framework:** Qt 6 (QML, Quick, Multimedia, QuickDialogs2)
- **Media Processing:** FFmpeg (`libavcodec`, `libavformat`, `libavutil`, `libswscale`, `libswresample`)
- **Build System:** CMake (≥ 3.16)
- **Architecture:** 
  - `PrimoReelsLib` (Static Library): Contains core engine logic, decoders, threads, models, and effects.
  - `PrimoReels` (Executable): QML frontend and application launcher.
  - Test Binaries: `PrimoReelsTests`, `EffectTests`, `ExportTests`, `QmlLoadTests`.

---

## 2. Architecture & Threading Model

PrimoReels uses a strict multi-threaded architecture to maintain responsive 60fps UI performance during playback, scrubbing, and timeline editing.

| Thread | Owner / Class | Role & Responsibilities |
| --- | --- | --- |
| **GUI** | `TimelineEngine` | Orchestrator & QML bridge. Owns all state (`mediaList`, `timelineClips`, caches, undo stack). Drives 33ms playback timer. |
| **Decode** | `DecoderThread` | Central playback worker owning one `MediaDecoder`. Coalesces requests: Close > Open > newest Seek/AudioRestart > top-up > thumbnails. |
| **Preload** | `PreloadThread` | Warms up the next sequence clip (`takeReadyDecoder()` pure `unique_ptr` ownership transfer). Renders audio waveforms off-thread. |
| **Export** | `ExportThread` | Offline MP4 muxer. Single request slot, cooperative cancel, 30fps sample-and-hold video resampling, transition/V2 overlay mixing. |
| **Audio Render** | Qt-owned (`AudioFifoDevice`) | Pulls PCM from mutex FIFO into `QAudioSink`. |
| **Image Providers** | Qt-owned (`FrameImageProvider`) | Pull `currentFrameWithEffect()` atomic snapshot under `m_frameMutex` + `m_dataMutex` (fixed lock order); `thumbImage()` under `m_dataMutex`. |

### Golden Rules of Threading
1. **GUI Mutates State:** Only the GUI thread mutates engine state.
2. **Asynchronous Communication:** Workers communicate back exclusively via queued signals (`loaded`, `frameReady`, `audioDataReady`, `audioError`, `thumbnailReady`, `waveformReady`, `progressChanged`, `exportWarning`).
3. **Thread Confinement:** `MediaDecoder` is a plain value type (no `QObject`, no thread affinity). Instances are thread-confined and never shared across threads. Preload handoff is a pure `std::unique_ptr` transfer (no `moveToThread()`).
4. **Lock Order:** Always acquire locks in the established order (`m_frameMutex` then `m_dataMutex`) to prevent deadlocks.
5. **Teardown Order:** `TimelineEngine::~TimelineEngine` must disconnect worker signals first, clear the decoder → preloader back-pointer, then stop/cancel workers.

---

## 3. Build, Test, & Verification Commands

### Building the Project
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### Running Tests
```sh
ctest --test-dir build --output-on-failure
```
Individual test targets:
- `./build/PrimoReelsTests`
- `./build/EffectTests`
- `./build/ExportTests`
- `./build/QmlLoadTests`

### Running the Application
```sh
./build/PrimoReels
```

---

## 4. Coding Standards & Conventions

- **RAII & Resource Safety:** Use smart pointers (`std::unique_ptr`, `std::shared_ptr`) everywhere. FFmpeg resources (`AVCodecContext`, `AVFormatContext`, `AVFrame`, `AVPacket`) must be managed via RAII wrappers/custom deleters (e.g., `PacketGuard`, `FrameGuard`, `MediaDecoder`).
- **List Models:** Edits to media or timeline lists must push undo snapshots (200-deep stack) and call `syncModels()` so that `QAbstractListModel` views (`mediaModel`, `timelineModel`, `mediaFilterModel`) stay synchronized.
- **Shared Accessors:** Use `src/ClipUtils.h` as the single source of truth for clip property accessors (path, duration, trims, effect, transition, track, title, startTime) across both the engine and exporter.
- **Project Files (`*.reels.json`):** Saves must be atomic (write to temp file, then rename). Loads must sanitize and clamp all fields and drop empty-path clips. Version validation (`version == 1`) is mandatory.

---

## 5. Troubleshooting & Environment Tips

- **Headless / CI Testing:** Set `QT_MEDIA_BACKEND=dummy` when running tests or headless instances without an active audio output device.
- **Video Decoders / Fallback:** Ensure FFmpeg runtime codecs (`ffmpeg`) are installed. Export automatically falls back from H.264 (`libx264`) to MPEG-4 if `libx264` is unavailable.
- **Layout Reset:** If UI panels or window geometry become corrupted, use `View → Reset Layout` or clear the platform `QSettings` store (`~/.config/PrimoReels/PrimoReels.conf` on Linux).
