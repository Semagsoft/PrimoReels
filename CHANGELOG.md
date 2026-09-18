# Changelog

## Unreleased

### Added

- Audio support: audio-only import (MP3/WAV/M4A/OGG play as black video),
  A1 audio bed lane (absolute start, drag-drop attach, Library Audio tab
  attaches bin files), per-clip gain/mute/fades + detach-audio-to-A1,
  Inspector clip-audio controls, V2/A1 bed mixing in export, global volume
  + clip gain burned into export (stays `version: 1`, additive defaults).
- Live preview mix: dedicated bed decoder thread (`m_bedDecoderThread`,
  audio-only requests) mixes the topmost audible V2/A1 bed with program
  audio in `tryFlushAudioStages`; per-chunk gain × fades in preview (WYSIWYG
  with export), bed-only preview when the program clip is silent, transient
  warnings (not sticky errors) for bed failures.
- A1 lane parity: bed clips drag to move (V2 pattern), click/Enter previews,
  keyboard + remove button, split-at-playhead via output-time mapping,
  Library attach lands under the playhead (`attachAudioAtPlayhead`); split
  shifts the right half's absolute start on V2/A1 tracks.
- Export audio integration tests: A1 bed mix level, gain/volume attenuation,
  mute silence, fade shaping, and audio-only playable video (volumedetect).
- Preview-mix PCM tests: per-chunk gain × fade, live 440 + 880 Hz bed mix,
  bed-only flow, transient bed-failure warning (Goertzel over queued PCM);
  non-destructive `peekPreviewAudio` / `peekBuffered` introspection (also the
  future meters primitive).
- Source-audio policy: monitor stays silent by design (documented in README /
  ARCHITECTURE); concurrent-playback invariant locked by
  `sourcePlayback_leavesProgramAudioUntouched` (source tone absent from the
  program FIFO while both transports run).

- MIT `LICENSE`; AppStream developer info and OARS content rating.
- `QAbstractListModel` views (`mediaModel`, `timelineModel`,
  `mediaFilterModel` with C++ search filtering) backing the Bin and timeline.
- Source Monitor bound to the engine (bin selection preview, transport,
  Insert to Timeline).
- Shared `qml/TimeUtils.js` timecode helpers (`mm:ss:cc`) for both monitors
  and the Bin.
- View menu toggles for all panels (incl. Source/Program monitors) with full
  layout persistence (visibility, split sizes, zoom, tab, window size) and
  `View → Reset Layout`.
- Sticky error status; unified `mm:ss:cc` timecode.
- CI lint job (`qmllint`, `clang-format` check, packaging validation) and
  sanitizer job (ASan/UBSan); `.clang-format` style.
- `ARCHITECTURE.md`, `CHANGELOG.md`, troubleshooting docs.

### Fixed

- QML bindings: reactive Source entry, `curClip`/`canSplit` timeline props
  (no stale toolbar/trim state), no `visible`/`source` self-reference loops
  in either monitor, async per-frame images, drag-safe overlay `x` via
  `Binding{when:!active}`.
- Accessibility: keyboard operation for Bin rows (Enter/Insert/Delete),
  timeline clips (Enter/Menu/Delete), ruler and playhead (arrows/Home/End);
  `Accessible.name` on all transports, trims, tabs, sliders and status;
  ≥32px control targets; secondary text brightened for contrast.
- All user-visible strings translatable (`qsTr`), incl. dialogs, Inspector,
  monitor headers and empty-state hints.
- Startup crash from invalid `Keys.onInsertPressed`/`onEndPressed`/
  `onHomePressed` handlers (no such convenience signals) fixed via generic
  `Keys.onPressed`; `currentTitle` moved above its consumer to avoid an
  `undefined` first evaluation. New `QmlLoadTests` smoke test loads the full
  `Main.qml` tree offscreen so this crash class fails in CI, not at launch.

- `MediaDecoder` is no longer a `QObject`: preload → playback handoff is a
  pure ownership transfer (removes `moveToThread` wrong-thread warning and
  the preloader teardown race).
- Dropped seeks now emit `DecoderThread::seekDropped`, which clears the
  engine's seeking gate (`onSeekDropped`): a seek preempted by an open or
  audio restart no longer sticks the seek/load coalescing.
- `PreloadThread::requestPreload` retargets the same path at a new position
  (e.g. trim change) instead of promoting a stale warmed decoder; the warmed
  position is tracked and only identical targets are skipped.
- Export silence padding is sized from the encoder's real `frame_size`
  instead of a `2048`-sample magic bound.
- `MediaDecoder::seekAudioTo` reports failure when catch-up never reaches
  the target (truncated/corrupt tail) instead of claiming the position, so
  callers fall back to silent video rather than drifting A/V.
- Decoder teardown is sequenced: both decoder threads are joined
  (`DecoderThread::stopAndWait`) before the `PreloadThread` they reference
  is destroyed, closing a use-after-free where a racing `RequestOpen` could
  deref the preloader mid-destruction.
- Audio resampler setup deep-copies the codec channel layout
  (`av_channel_layout_copy`, always released) and adopts the `SwrContext`
  into the RAII holder immediately, so no layout aliasing or context leak
  on any failure path.
- Undo/redo no longer probes files on the GUI thread: `applySnapshot`
  re-queues metadata probes on the preload thread, with `onProbeReady` /
  `onProbeFailed` producing the same outcomes as a fresh import.
- Export warnings (e.g. skipped undecodable clips) emit a dedicated
  `warning()` signal shown transiently instead of masquerading as sticky
  errors via `failed()`.
- `newProject` also resets the playback base position and sequence window;
  `requestSeek` no longer writes a dead `m_pendingSeek` store on the fresh
  path; list-model reentrancy guards arm in the constructor.
- Documented the pixel-format contracts (decode-side RGB32 is
  endian-portable, export aliases RGB32 bytes as BGRA with a little-endian
  compile-time pin) and the waveform short-bucket guarantee.
- Whole-repo `clang-format` pass so the lint job is green.

### Hardening

- Project loads are bounded and validated: files over 32 MiB and lists over
  20k media / 10k clips are rejected; media rows require a usable path and a
  numeric finite duration (bad rows are dropped, overlong names truncated);
  clip durations and overlay start times are clamped finite (24 h max); the
  auto-loaded source must belong to the media bin (foreign paths warn
  instead of opening); history is snapshotted only after validation.
- Paths with embedded NUL or over 1024 chars are rejected in
  `normalizedMediaPath`; the preload probe queue is capped at 64; artwork
  caches are cleared on `newProject`.
- Media open rejects absurd dimensions/durations; export uses 64-bit frame
  and sample counters with a 12 h timeline cap; corrupt zero sample rates
  can no longer divide by zero; export checks layout copy and scale results
  and treats a failed audio rewind as silence instead of skewed audio.
- Export preserves the first error on cancel, resets progress on failure,
  and explicitly discards partial temp files; a failed project-source open
  warns transiently instead of erroring; `newProject` cancels in-flight
  exports.
- Project saves are atomic (temp + rename, byte-count checked); loads
  validate `version` and sanitize every clip field.
- Video decode failure stops the playback clock (program + source); audio
  seek errors no longer masquerade as EOF (`audioError` vs `audioFinished`).
- Bin thumbnails are cancellable and yield to playback/seek/audio work.
- Export reports skipped undecodable clips via `exportWarning` instead of
  silently dropping them.
- Preview provider reads frame + effect under one lock snapshot.
- Undo restores volume/transform/source/effect; `removeMedia` cascades to
  timeline clips; `newProject` clears the stale decoder frame.
- `S` split shortcut no longer fires while typing in search/title fields.
- Program title resolves duplicate bin paths via the selected clip.
- Timeline vertical scroll (`contentHeight`), larger clip controls,
  accessible names, translated labels.
- Export audio-stream rollback no longer pokes `AVFormatContext` internals.

### Changed

- Shared clip accessors extracted to `src/ClipUtils.h`.
- `ExportThread` sample-and-hold loops unified behind `SampleHoldPump`;
  `sendVideoFrame`/`sendAudioFrame` merged into `sendFrame`.
- `TimelineEngine::onLoaded` split into staged helpers.
- Removed dead `MediaDecoder` signal/slot shims; cached effect catalog.
- DEB dependencies resolved via shlibdeps instead of pinned FFmpeg SONAMEs.

## 0.1.0

- Initial editor: bin/library, V1+V2 timeline, monitor, effects, transitions,
  MP4 export, `*.reels.json` projects.
