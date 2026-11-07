#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

// Parameter spec for a single effect parameter, used by the Inspector UI
// to build sliders/spin boxes generically.
struct EffectParamSpec {
  QString name;   // e.g. "radius", "amount", "contrast", "saturation", "intensity"
  QString label;  // user-visible label (translatable at the QML call site)
  double min;     // minimum value
  double max;     // maximum value
  double def;     // default value
  double step;    // slider / spin box step
};

// CPU video effects applied per-frame in the monitor path and the exporter.
// Effect names match the Library panel catalog; unknown/empty names are
// passed through unchanged. Effects accept an optional named-parameter map;
// missing parameters fall back to their defaults.
class VideoEffects {
 public:
  static QStringList availableEffects();
  static bool isKnownEffect(const QString& name);
  static QImage apply(const QImage& frame,
                      const QString& effectName,
                      const QVariantMap& params = {});
  // Parameter specifications for an effect (empty list = no parameters).
  static QVector<EffectParamSpec> effectParamSpecs(const QString& effectName);

 private:
  static QImage toRgb32(const QImage& frame);
  static QImage blur(const QImage& frame, const QVariantMap& params);
  static QImage colorCorrect(const QImage& frame, const QVariantMap& params);
  static QImage sharpen(const QImage& frame, const QVariantMap& params);
  static QImage vignette(const QImage& frame, const QVariantMap& params);
  static QImage glitch(const QImage& frame, const QVariantMap& params);
  static QImage sepia(const QImage& frame, const QVariantMap& params);
  static QImage grayscale(const QImage& frame, const QVariantMap& params);
  static QImage invert(const QImage& frame, const QVariantMap& params);
  static double param(const QVariantMap& params, const QString& key, double def);
};
