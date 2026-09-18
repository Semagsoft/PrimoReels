# PrimoReels

A modern desktop video editor built with **C++17**, **Qt 6 (QML)**, and **FFmpeg**.

## Features

- **Project Bin** — import (file dialog, multi-select, or drag-and-drop from the
  file manager), search, thumbnails, remove/clear, undo/redo
- **Library** — video effects (Blur, Color Correction, Sharpen, Vignette, Glitch;
  double-click to apply to the timeline selection), audio/transition catalogs
- **Timeline** — drag-and-drop insert, reorder, trim in/out points, split at
  playhead (`S`), per-clip titles, V1 sequence + V2 overlay (PiP) lanes,
  A1 audio bed (drag to move, split at playhead, attach at playhead,
  per-clip gain/mute/fades, detach from V1),
  transitions (Cross Dissolve, Dip to Black), audio waveforms
- **Monitor** — playback with transforms (scale/rotation), volume, sequence
  auto-advance with preloaded decoder, effect/title preview, clip indicator;
  audio-only files play as black video with sound
- **Export** — timeline to MP4 (H.264 + AAC, 30 fps) with progress and cancel;
  honors trims, effects, titles, transitions, V1/V2 overlays, A1 bed mix,
  per-clip gain/mute/fades and global volume (audio-only timelines render
  720p black video)
- **Project files** (`*.reels.json`) — save/open, recent projects, dirty-dot
  title, unsaved-changes prompts

## Requirements

- CMake ≥ 3.16, a C++17 compiler, pkg-config
- Qt 6: Core, Gui, Quick, Multimedia, QuickDialogs2, Test
- FFmpeg dev libraries: libavcodec, libavformat, libavutil, libswscale,
  libswresample (libx264 used when available, otherwise MPEG-4)
- `ffmpeg` / `ffprobe` CLIs (only needed to generate fixtures for some tests)

On Debian/Ubuntu:

```sh
sudo apt-get install libavcodec-dev libavformat-dev libavutil-dev \
  libswscale-dev libswresample-dev ffmpeg
```

## Build & Test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/PrimoReels
```

Install (Linux):

```sh
cmake --install build --prefix /usr/local
```

## Shortcuts

| Keys | Action |
| ---- | ------ |
| `Space` | Play / pause (sequence-aware) |
| `S` | Split selected clip at playhead (timeline focused) |
| `Home` / `End` | Go to start / end |
| `Ctrl+O` / `Ctrl+S` / `Ctrl+N` | Import media / save project / new project |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / redo bin & timeline edits |

## Project file format

`*.reels.json`: `{ version, mediaList, timelineClips, volume, clipScaleX/Y,
clipRotation, currentSource }`. Timeline clips carry `path, name, duration,
sourceDuration, trimStart, trimEnd, effect, transition, transitionDuration,
track (0=V1, 1=V2, 2=A1), title, startTime, gain, muted, fadeIn, fadeOut`.
`version` stays 1: audio fields are additive with defaults.

## Troubleshooting

- **No video / black frame:** install the FFmpeg runtime codecs
  (`ffmpeg` package) and confirm the file plays in `ffplay`. Export falls
  back from H.264 (`libx264`) to MPEG-4 when the encoder is missing.
- **No audio in preview:** tests and headless runs use
  `QT_MEDIA_BACKEND=dummy`. On desktop, check the system output device and
  the Inspector volume (0–2.0). Waveforms render only for clips with an
  audio stream.
- **Source Monitor has no sound:** by design — it is a silent visual
  audition tool so browsing the bin never interrupts program audio. To hear
  a bin clip, attach it to the timeline (double-click attaches to A1 at the
  playhead) and play the program monitor.
- **Export fails immediately:** the timeline must be non-empty; the
  destination must be writable (failed exports remove the partial file).
- **Layout looks wrong after an update:** use `View → Reset Layout` to clear
  persisted view settings (panel visibility, splits, zoom, window size).
- **Settings location:** view settings and recents live in the platform
  `QSettings` store for org `PrimoReels`, app `PrimoReels`
  (e.g. `~/.config/PrimoReels/PrimoReels.conf` on Linux).
