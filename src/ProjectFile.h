#pragma once

// Project file (*.reels.json) serialization: schema version, on-disk
// guards, and per-field sanitization of loaded data. Extracted from
// TimelineEngine so load/save rules stay in one translation unit; the
// engine keeps the side effects (signals, state swap, source auto-load).

#include <QString>
#include <QVariantList>

namespace ProjectFile {

struct Data {
  QVariantList mediaList;
  QVariantList timelineClips;
  double volume = 1.0;
  double clipScaleX = 1.0;
  double clipScaleY = 1.0;
  double clipRotation = 0.0;
  QString currentSource;
};

// Project-file load guards (robustness): legit projects are kilobytes;
// these bound GUI-thread parse work and downstream list math.
inline constexpr qint64 kMaxProjectBytes = 32 * 1024 * 1024;
inline constexpr int kMaxProjectMediaEntries = 20000;
inline constexpr int kMaxProjectTimelineClips = 10000;
// Longest single media duration accepted anywhere (also bounds timestamp
// rescaling, waveform buckets, and timeline cursor math).
inline constexpr double kMaxMediaSeconds = 86400.0;

// Serialize + atomically write to filePath. On failure returns false and
// leaves any previous file intact; errorMessage carries a user-safe reason.
bool save(const QString& filePath, const Data& data, QString* errorMessage);

// Read + validate + sanitize. On failure returns false, leaves `out`
// untouched, and sets errorMessage.
bool load(const QString& filePath, Data* out, QString* errorMessage);

}  // namespace ProjectFile
