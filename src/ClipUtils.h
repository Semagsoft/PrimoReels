#pragma once

// Shared clip/path accessors for TimelineEngine and ExportThread.
// Single source of truth: previously duplicated in both translation units.

#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <utility>

namespace ClipUtils {

// Longest accepted filesystem path (also bounds engine cache keys,
// validation, and user-visible messages).
inline constexpr int kMaxPathChars = 1024;

inline QString normalizedMediaPath(const QString& filePath) {
  QString path = filePath.trimmed();
  if (path.isEmpty()) {
    return path;
  }
  // Embedded NUL would truncate the path at the native/FFmpeg boundary
  // ("/tmp/a\0/etc/passwd" opens "/tmp/a"); overlong paths pollute caches
  // and messages. Reject both up front — every caller treats empty as
  // failure.
  if (path.contains(QChar(0)) || path.size() > kMaxPathChars) {
    return QString();
  }
  // Handle file:// URLs (including file://localhost/ and %-encoding).
  if (path.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive)) {
    QString local = QUrl(path).toLocalFile();
    if (!local.isEmpty()) {
      return local;
    }
    return path;
  }
  // Generic URL with explicit file scheme.
  const QUrl url(path);
  if (url.isValid() && !url.scheme().isEmpty()) {
    if (url.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0) {
      QString local = url.toLocalFile();
      if (!local.isEmpty()) {
        return local;
      }
    }
    // Non-file schemes (qrc:/, content://, http://, ...) are not local
    // media files: return as-is so open() fails with a clear message
    // instead of silently mangling the path.
    return path;
  }
  return path;
}

inline bool isFiniteDouble(double v) {
  return std::isfinite(v) != 0;
}

inline double clampFinite(double v, double lo, double hi) {
  if (!isFiniteDouble(v)) {
    return lo;
  }
  return std::clamp(v, lo, hi);
}

inline QString clipPath(const QVariantMap& clip) {
  return clip.value(QStringLiteral("path")).toString();
}

inline QString clipName(const QVariantMap& clip, const QString& fallback = QString()) {
  return clip.value(QStringLiteral("name"), fallback).toString();
}

inline double clipDuration(const QVariantMap& clip) {
  return std::max(0.0, clip.value(QStringLiteral("duration"), 0.0).toDouble());
}

inline double clipSourceDuration(const QVariantMap& clip) {
  const double source = clip.value(QStringLiteral("sourceDuration"), -1.0).toDouble();
  if (source > 0.0) {
    return source;
  }
  return clipDuration(clip);
}

inline double clipTrimStart(const QVariantMap& clip) {
  return std::max(0.0, clip.value(QStringLiteral("trimStart"), 0.0).toDouble());
}

inline double clipTrimEnd(const QVariantMap& clip) {
  return std::max(0.0, clip.value(QStringLiteral("trimEnd"), 0.0).toDouble());
}

inline QString clipEffect(const QVariantMap& clip) {
  return clip.value(QStringLiteral("effect")).toString();
}

inline QString clipTransition(const QVariantMap& clip) {
  return clip.value(QStringLiteral("transition")).toString();
}

inline double clipTransitionDuration(const QVariantMap& clip) {
  return clip.value(QStringLiteral("transitionDuration"), 0.5).toDouble();
}

inline int clipTrack(const QVariantMap& clip) {
  return clip.value(QStringLiteral("track"), 0).toInt();
}

inline double clipStartTime(const QVariantMap& clip) {
  return clip.value(QStringLiteral("startTime"), 0.0).toDouble();
}

inline QString clipTitle(const QVariantMap& clip) {
  return clip.value(QStringLiteral("title")).toString();
}

// Per-clip audio controls (v1 format, additive with defaults so old files
// load unchanged): gain 0..2 (default 1), muted bool, fades in seconds.
inline double clipGain(const QVariantMap& clip) {
  return clampFinite(clip.value(QStringLiteral("gain"), 1.0).toDouble(), 0.0, 2.0);
}

inline bool clipMuted(const QVariantMap& clip) {
  return clip.value(QStringLiteral("muted"), false).toBool();
}

inline double clipFadeIn(const QVariantMap& clip) {
  return clampFinite(clip.value(QStringLiteral("fadeIn"), 0.0).toDouble(), 0.0, 30.0);
}

inline double clipFadeOut(const QVariantMap& clip) {
  return clampFinite(clip.value(QStringLiteral("fadeOut"), 0.0).toDouble(), 0.0, 30.0);
}

inline double clipEffectiveGain(const QVariantMap& clip, double globalVolume) {
  if (clipMuted(clip)) {
    return 0.0;
  }
  return clampFinite(globalVolume, 0.0, 2.0) * clipGain(clip);
}

// Fade multiplier 0..1 at intra-clip time t (duration dur).
inline double clipFadeGain(const QVariantMap& clip, double t, double dur) {
  double g = 1.0;
  const double fi = clipFadeIn(clip);
  const double fo = clipFadeOut(clip);
  if (fi > 0.0 && t < fi) {
    g *= std::clamp(t / fi, 0.0, 1.0);
  }
  if (fo > 0.0 && dur > 0.0 && t > dur - fo) {
    g *= std::clamp((dur - t) / fo, 0.0, 1.0);
  }
  return g;
}

}  // namespace ClipUtils
