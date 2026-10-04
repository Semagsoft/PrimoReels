#include "ProjectFile.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QVariantMap>
#include <algorithm>

#include "ClipUtils.h"
#include "VideoEffects.h"

namespace ProjectFile {

bool save(const QString& filePath, const Data& data, QString* errorMessage) {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  if (path.isEmpty()) {
    *errorMessage = QStringLiteral("Cannot save project: empty file path.");
    return false;
  }
  QJsonObject root;
  root.insert(QStringLiteral("version"), 1);
  root.insert(QStringLiteral("mediaList"), QJsonArray::fromVariantList(data.mediaList));
  root.insert(QStringLiteral("timelineClips"), QJsonArray::fromVariantList(data.timelineClips));
  root.insert(QStringLiteral("volume"), data.volume);
  root.insert(QStringLiteral("clipScaleX"), data.clipScaleX);
  root.insert(QStringLiteral("clipScaleY"), data.clipScaleY);
  root.insert(QStringLiteral("clipRotation"), data.clipRotation);
  root.insert(QStringLiteral("currentSource"), data.currentSource);

  // Atomic save via QSaveFile: writes to a temp sibling and renames over
  // the target on commit, so a crash/power loss or a failed write can never
  // destroy the previous project — the original stays intact on failure.
  const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    *errorMessage = QStringLiteral("Cannot save project to %1: %2").arg(path, file.errorString());
    return false;
  }
  if (file.write(payload) != payload.size()) {
    const QString err = file.errorString();
    file.cancelWriting();
    *errorMessage = QStringLiteral("Cannot save project to %1: %2").arg(path, err);
    return false;
  }
  if (!file.commit()) {
    *errorMessage = QStringLiteral("Cannot save project to %1: %2").arg(path, file.errorString());
    return false;
  }
  return true;
}

bool load(const QString& filePath, Data* out, QString* errorMessage) {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  if (path.isEmpty()) {
    *errorMessage = QStringLiteral("Cannot open project: empty file path.");
    return false;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    *errorMessage = QStringLiteral("Cannot open project %1: %2").arg(path, file.errorString());
    return false;
  }
  if (file.size() > kMaxProjectBytes) {
    *errorMessage = QStringLiteral("Cannot open project %1: file too large (%2 bytes).")
                        .arg(path, QString::number(file.size()));
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    *errorMessage = QStringLiteral("Cannot open project %1: invalid project file (%2).")
                        .arg(path, parseError.errorString());
    return false;
  }
  const QJsonObject root = doc.object();
  if (!root.value(QStringLiteral("mediaList")).isArray() ||
      !root.value(QStringLiteral("timelineClips")).isArray()) {
    *errorMessage =
        QStringLiteral("Cannot open project %1: missing media or timeline data.").arg(path);
    return false;
  }
  const QJsonArray mediaArray = root.value(QStringLiteral("mediaList")).toArray();
  const QJsonArray clipsArray = root.value(QStringLiteral("timelineClips")).toArray();
  if (mediaArray.size() > kMaxProjectMediaEntries || clipsArray.size() > kMaxProjectTimelineClips) {
    *errorMessage =
        QStringLiteral("Cannot open project %1: too many entries (%2 media, %3 clips).")
            .arg(path, QString::number(mediaArray.size()), QString::number(clipsArray.size()));
    return false;
  }
  if (root.value(QStringLiteral("version")).toInt(-1) != 1) {
    *errorMessage =
        QStringLiteral("Cannot open project %1: unsupported version %2.")
            .arg(path, root.value(QStringLiteral("version")).toVariant().toString().left(64));
    return false;
  }

  // Validate media rows like timeline clips below: crafted types must not
  // reach models/threads/QML as 0-second placeholder ghosts.
  Data data = out ? *out : Data{};
  data.mediaList.reserve(mediaArray.size());
  for (const auto& v : mediaArray) {
    if (!v.isObject()) {
      continue;
    }
    QVariantMap entry = v.toObject().toVariantMap();
    const QString entryPath = entry.value(QStringLiteral("path")).toString();
    if (entryPath.isEmpty() || entryPath.size() > ClipUtils::kMaxPathChars) {
      continue;
    }
    const QVariant durVar = entry.value(QStringLiteral("duration"), 0.0);
    double entryDur = 0.0;
    if (durVar.typeId() == QMetaType::Double || durVar.typeId() == QMetaType::Float ||
        durVar.typeId() == QMetaType::Int || durVar.typeId() == QMetaType::UInt ||
        durVar.typeId() == QMetaType::LongLong || durVar.typeId() == QMetaType::ULongLong) {
      entryDur = durVar.toDouble();
    }
    if (!ClipUtils::isFiniteDouble(entryDur) || entryDur < 0.0) {
      continue;
    }
    entry.insert(QStringLiteral("path"), entryPath);
    entry.insert(QStringLiteral("duration"), std::min(entryDur, kMaxMediaSeconds));
    if (!entry.contains(QStringLiteral("name")) ||
        entry.value(QStringLiteral("name")).toString().isEmpty()) {
      entry.insert(QStringLiteral("name"), QFileInfo(entryPath).fileName());
    } else {
      entry.insert(QStringLiteral("name"),
                   entry.value(QStringLiteral("name")).toString().left(256));
    }
    data.mediaList.append(entry);
  }
  // Normalize + validate clips (older files predate keys; hand-edited files
  // may carry garbage). Clips with an empty path are dropped.
  data.timelineClips.reserve(clipsArray.size());
  for (const auto& v : clipsArray) {
    QVariantMap clip = v.isObject() ? v.toObject().toVariantMap() : QVariantMap();
    const QString clipPath = clip.value(QStringLiteral("path")).toString();
    if (clipPath.isEmpty() || clipPath.size() > ClipUtils::kMaxPathChars) {
      continue;
    }
    double sourceDur = clip.value(QStringLiteral("sourceDuration"), -1.0).toDouble();
    double dur = clip.value(QStringLiteral("duration"), 0.0).toDouble();
    if (!ClipUtils::isFiniteDouble(sourceDur) || !ClipUtils::isFiniteDouble(dur)) {
      continue;
    }
    if (!(sourceDur > 0.0)) {
      sourceDur = (dur > 0.0) ? dur : 0.0;
    }
    sourceDur = std::clamp(sourceDur, 0.0, kMaxMediaSeconds);
    double trimStart = std::clamp(clip.value(QStringLiteral("trimStart"), 0.0).toDouble(), 0.0,
                                  std::max(0.0, sourceDur - 0.1));
    double trimEnd = std::clamp(clip.value(QStringLiteral("trimEnd"), 0.0).toDouble(), 0.0,
                                std::max(0.0, sourceDur - trimStart - 0.1));
    dur = std::max(0.1, sourceDur - trimStart - trimEnd);
    clip.insert(QStringLiteral("sourceDuration"), sourceDur);
    clip.insert(QStringLiteral("trimStart"), trimStart);
    clip.insert(QStringLiteral("trimEnd"), trimEnd);
    clip.insert(QStringLiteral("duration"), dur);
    const QString effect = clip.value(QStringLiteral("effect")).toString();
    clip.insert(QStringLiteral("effect"), VideoEffects::isKnownEffect(effect) ? effect : QString());
    const QString transition = clip.value(QStringLiteral("transition")).toString();
    if (!ClipUtils::isKnownTransition(transition)) {
      clip.insert(QStringLiteral("transition"), QString());
      clip.insert(QStringLiteral("transitionDuration"), 0.5);
    } else {
      clip.insert(QStringLiteral("transitionDuration"),
                  std::clamp(clip.value(QStringLiteral("transitionDuration"), 0.5).toDouble(), 0.1,
                             std::max(0.1, std::min(2.0, dur))));
    }
    clip.insert(QStringLiteral("track"),
                std::clamp(clip.value(QStringLiteral("track"), 0).toInt(), 0, 2));
    clip.insert(QStringLiteral("gain"),
                std::clamp(clip.value(QStringLiteral("gain"), 1.0).toDouble(), 0.0, 2.0));
    clip.insert(QStringLiteral("muted"), clip.value(QStringLiteral("muted"), false).toBool());
    clip.insert(QStringLiteral("fadeIn"),
                std::clamp(clip.value(QStringLiteral("fadeIn"), 0.0).toDouble(), 0.0, 30.0));
    clip.insert(QStringLiteral("fadeOut"),
                std::clamp(clip.value(QStringLiteral("fadeOut"), 0.0).toDouble(), 0.0, 30.0));
    clip.insert(QStringLiteral("title"), clip.value(QStringLiteral("title")).toString().left(200));
    if (!clip.contains(QStringLiteral("name")) ||
        clip.value(QStringLiteral("name")).toString().isEmpty()) {
      clip.insert(QStringLiteral("name"), QFileInfo(clipPath).fileName());
    } else {
      clip.insert(QStringLiteral("name"), clip.value(QStringLiteral("name")).toString().left(256));
    }
    if (clip.value(QStringLiteral("track"), 0).toInt() != 1 &&
        clip.value(QStringLiteral("track"), 0).toInt() != 2) {
      clip.insert(QStringLiteral("startTime"), 0.0);  // recomputed below
    } else {
      const double overlayStart = clip.value(QStringLiteral("startTime"), 0.0).toDouble();
      clip.insert(QStringLiteral("startTime"), ClipUtils::isFiniteDouble(overlayStart)
                                                   ? std::clamp(overlayStart, 0.0, kMaxMediaSeconds)
                                                   : 0.0);
    }
    data.timelineClips.append(clip);
  }
  data.volume = root.value(QStringLiteral("volume")).toDouble(data.volume);
  data.clipScaleX = root.value(QStringLiteral("clipScaleX")).toDouble(data.clipScaleX);
  data.clipScaleY = root.value(QStringLiteral("clipScaleY")).toDouble(data.clipScaleY);
  data.clipRotation = root.value(QStringLiteral("clipRotation")).toDouble(data.clipRotation);
  data.currentSource = root.value(QStringLiteral("currentSource")).toString();
  *out = data;
  return true;
}

}  // namespace ProjectFile
