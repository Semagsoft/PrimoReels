# PrimoReels Architecture

## Overview

PrimoReels is a desktop video editor: C++17 + Qt 6 (QML) + FFmpeg.
`PrimoReelsLib` (static) holds all logic; it is shared by the `PrimoReels`
executable and the test binaries.

## Threading model

| Thread | Owner | Role |
| --- | --- | --- |
| GUI | `TimelineEngine` | Orchestrator + QML bridge. Owns all state (`mediaList`, `timelineClips`, caches). 33 ms `QTimer` playback clock. |
| Decode | `DecoderThread` | Central playback worker. Owns one `MediaDecoder`. Coalesced requests: Close > Open > newest Seek/AudioRestart > top-up > thumbnails. |
| Bed | `DecoderThread` (`m_bedDecoderThread`) | Live A1/V2 preview mixer source. Audio restarts + top-ups only (no preloader, no video/thumbnail requests) so bed work never starves program seeks. |
| Preload | `PreloadThread` | Warms the next sequence clip (`takeReadyDecoder()` pure `unique_ptr` ownership transfer); renders audio waveforms off-thread. |
| Export | `ExportThread` | Offline MP4 muxer. Single request slot, cooperative cancel. |
| Audio render | Qt-owned | Pulls PCM from `AudioFifoDevice` (mutex FIFO into `QAudioSink`). |
| QQuick image providers | Qt-owned | Pull `currentFrameWithEffect()` atomic snapshot under `m_frameMutex` + `m_dataMutex` (fixed order); `thumbImage()` under `m_dataMutex`. |

Rules:

- Only the GUI thread mutates engine state. Workers communicate back via
  queued signals (`loaded`, `frameReady`, `audioDataReady`, `audioError`,
  `thumbnailReady`, `waveformReady`, `progressChanged`, `exportWarning`).
- `MediaDecoder` is a plain value type (no `QObject`, no thread affinity).
  Instances are thread-confined (never shared across threads). The preload
  handoff is a pure `unique_ptr` ownership transfer — no `moveToThread()`.
- `TimelineEngine::~TimelineEngine` disconnects worker signals first, clears
  the decoder → preloader back-pointer, then stops/cancels workers —
  teardown order matters.
- FFmpeg objects use `unique_ptr` custom deleters (`MediaDecoder`,
  `PacketGuard`/`FrameGuard` in export).

## Data flow

- **Preview:** QML seek/load → `DecoderThread::requestOpen/Seek` →
  `MediaDecoder::open/getFrameAt` → `frameReady/loaded` →
  `m_currentFrame` → `currentFrameChanged` → `image://preview/frame`.
  Audio-only files open video-less (`hasVideo()==false`): the engine shows a
  black placeholder and drives the cursor from the wall clock while PCM
  flows through the normal audio path with per-clip effective gain.
- **Source preview (silent by design):** `loadSource` opens on
  `m_sourceDecoderThread` (own decoder, own 33 ms clock) but never requests
  audio — no `requestAudioRestart/Topup`, `hasAudio` ignored in
  `onSourceLoaded`. The source monitor is a visual audition + insert tool:
  bin selection auto-plays video while program audio keeps playing
  undisturbed (the single audible path is the program FIFO). Rationale:
  audible source would need transport exclusion (breaking the undisturbed
  invariant) plus a second sink and volume UI, while timeline audition
  (attach → play) already gives full-fidelity preview with mix. Locked by
  `sourcePlayback_leavesProgramAudioUntouched` (PCM-level: the source tone
  is absent from the program FIFO during concurrent playback).
- **A/V playback:** `play()`/`seek()` → program audio restart + bed
  (re)select by output time (`outputTimeFor` → `bedClipAt`, topmost audible
  V2/A1 row) → per-chunk effective gain (`global × clip`, 0 when muted) ×
  fade (`clipFadeGain` on a played-frames clock) staged per source, mixed via
  `mixAudio` in `tryFlushAudioStages` (80 ms solo-flush guard so a slow bed
  open never stalls program audio) → single FIFO into `QAudioSink` at unity.
  33 ms tick advances position; drift ≥ 150 ms re-issues seeks; mixed pending
  (FIFO + stages) < 400 ms requests top-ups on both threads; per-tick
  `updateBedForOutput` switches beds mid-clip. Audio seek errors
  emit `audioError` (video keeps playing silent); bed failures stop the bed
  with a transient `warning()` and the program drains solo; video decode failure stops
  the clock. Thumbnails are cancellable and yield to playback requests.
- **Sequence:** `playSequenceFrom(i)` loads clip `i`, arms `m_seqStart/End`,
  preloads clip `i+1`; `onTick` auto-advances at `seqEnd`.
- **Export:** `exportSequence()` snapshots `m_timelineClips` + volume into
  `ExportThread`, which probes, resamples video at 30 fps (sample-and-hold
  via `SampleHoldPump`; black frames for audio-only V1), mixes transitions, overlays V2 PiP, mixes the V2/A1 audio bed with per-clip gain/mute/fades,
  burns titles,
  encodes H.264 (fallback MPEG-4) + AAC. Undecodable clips are skipped
  (excluded from total/progress) and reported via `exportWarning`.
- **Edits:** list mutations push undo snapshots (`mediaList` +
  `timelineClips` + volume/transform/source/effect, 200 deep) and call `syncModels()` so the
  undo/redo stack mechanics live in `src/ProjectHistory.h` (`ProjectHistory<T>`),
  and project `*.reels.json` load/save with all validation/sanitization lives
  in `src/ProjectFile.{h,cpp}`.
  `QAbstractListModel` views (`mediaModel`, `timelineModel`,
  `mediaFilterModel`) stay in sync. See `src/ListModels.*`.
- **Shared clip accessors:** `src/ClipUtils.h` (path/duration/trims/effect/
  transition/track/title/startTime) — the single source of truth used by
  both the engine and the exporter.

## QML frontend

`timelineEngine` context property + `image://preview` / `image://thumbs`
providers. C++ owns media/timeline/playback/undo/export state; QML mirrors
selection, zoom, dialogs, and view layout (persisted via `QtCore.Settings`).
Bin/timeline views bind to the C++ list models (role-based delegates that
recycle); counts and window logic use the compat `QVariantList` props.

## Project files

`*.reels.json`: `{ version (=1, validated on load), mediaList, timelineClips, volume, clipScaleX/Y,
clipRotation, currentSource }`. Timeline clips carry `path, name, duration,
sourceDuration, trimStart, trimEnd, effect, transition, transitionDuration,
track (0=V1,1=V2,2=A1), title, startTime, gain, muted, fadeIn, fadeOut`. Saves are atomic (temp file + rename); loads
sanitize/clamp every field and drop empty-path clips.
