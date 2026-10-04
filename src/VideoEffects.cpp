#include "VideoEffects.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

inline int clamp8(int v) {
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

QImage boxBlurOnce(const QImage& src) {
  const int w = src.width();
  const int h = src.height();
  QImage dst(w, h, QImage::Format_RGB32);
  for (int y = 0; y < h; ++y) {
    const int y0 = std::max(0, y - 1);
    const int y1 = std::min(h - 1, y + 1);
    const QRgb* r0 = reinterpret_cast<const QRgb*>(src.constScanLine(y0));
    const QRgb* r1 = reinterpret_cast<const QRgb*>(src.constScanLine(y));
    const QRgb* r2 = reinterpret_cast<const QRgb*>(src.constScanLine(y1));
    QRgb* out = reinterpret_cast<QRgb*>(dst.scanLine(y));
    for (int x = 0; x < w; ++x) {
      const int x0 = std::max(0, x - 1);
      const int x1 = std::min(w - 1, x + 1);
      int r = 0, g = 0, b = 0;
      r += qRed(r0[x0]) + qRed(r0[x]) + qRed(r0[x1]);
      r += qRed(r1[x0]) + qRed(r1[x]) + qRed(r1[x1]);
      r += qRed(r2[x0]) + qRed(r2[x]) + qRed(r2[x1]);
      g += qGreen(r0[x0]) + qGreen(r0[x]) + qGreen(r0[x1]);
      g += qGreen(r1[x0]) + qGreen(r1[x]) + qGreen(r1[x1]);
      g += qGreen(r2[x0]) + qGreen(r2[x]) + qGreen(r2[x1]);
      b += qBlue(r0[x0]) + qBlue(r0[x]) + qBlue(r0[x1]);
      b += qBlue(r1[x0]) + qBlue(r1[x]) + qBlue(r1[x1]);
      b += qBlue(r2[x0]) + qBlue(r2[x]) + qBlue(r2[x1]);
      out[x] = qRgb(r / 9, g / 9, b / 9);
    }
  }
  return dst;
}

}  // namespace

QStringList VideoEffects::availableEffects() {
  // Static cache: previously rebuilt (with QString allocations) on every
  // isKnownEffect() call in the per-frame monitor/export paths.
  static const QStringList effects = {
      QStringLiteral("Blur"),      QStringLiteral("Color Correction"),
      QStringLiteral("Sharpen"),   QStringLiteral("Vignette"),
      QStringLiteral("Glitch"),    QStringLiteral("Sepia"),
      QStringLiteral("Grayscale"), QStringLiteral("Invert")};
  return effects;
}

bool VideoEffects::isKnownEffect(const QString& name) {
  return availableEffects().contains(name);
}

QImage VideoEffects::toRgb32(const QImage& frame) {
  if (frame.format() == QImage::Format_RGB32 || frame.format() == QImage::Format_ARGB32) {
    return frame;
  }
  return frame.convertToFormat(QImage::Format_RGB32);
}

QImage VideoEffects::apply(const QImage& frame, const QString& effectName) {
  if (frame.isNull() || effectName.isEmpty()) {
    return frame;
  }
  const QImage src = toRgb32(frame);
  if (effectName == QLatin1String("Blur")) {
    return blur(src);
  }
  if (effectName == QLatin1String("Color Correction")) {
    return colorCorrect(src);
  }
  if (effectName == QLatin1String("Sharpen")) {
    return sharpen(src);
  }
  if (effectName == QLatin1String("Vignette")) {
    return vignette(src);
  }
  if (effectName == QLatin1String("Glitch")) {
    return glitch(src);
  }
  if (effectName == QLatin1String("Sepia")) {
    return sepia(src);
  }
  if (effectName == QLatin1String("Grayscale")) {
    return grayscale(src);
  }
  if (effectName == QLatin1String("Invert")) {
    return invert(src);
  }
  return frame;
}

QImage VideoEffects::blur(const QImage& frame) {
  if (frame.width() < 3 || frame.height() < 3) {
    return frame;
  }
  // Fast path for large frames (4K preview / 1080p export): blur at half
  // resolution then upscale. ~4x fewer pixels through the 3x3 kernel;
  // visually indistinguishable for a blur.
  const qint64 pixels = static_cast<qint64>(frame.width()) * frame.height();
  if (pixels > 2000000) {
    const QImage small = frame.scaled(frame.width() / 2, frame.height() / 2, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation);
    const QImage blurred = boxBlurOnce(boxBlurOnce(small));
    return blurred.scaled(frame.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  }
  return boxBlurOnce(boxBlurOnce(frame));
}

QImage VideoEffects::colorCorrect(const QImage& frame) {
  // Contrast 1.2 around mid-gray + 1.3x saturation via luma mixing.
  QImage dst(frame.size(), QImage::Format_RGB32);
  for (int y = 0; y < frame.height(); ++y) {
    const QRgb* src = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
    QRgb* out = reinterpret_cast<QRgb*>(dst.scanLine(y));
    for (int x = 0; x < frame.width(); ++x) {
      const int r0 = qRed(src[x]);
      const int g0 = qGreen(src[x]);
      const int b0 = qBlue(src[x]);
      const int luma = (r0 * 30 + g0 * 59 + b0 * 11) / 100;
      const int r = clamp8(static_cast<int>(((r0 - 128) * 1.2) + 128 + (r0 - luma) * 0.3));
      const int g = clamp8(static_cast<int>(((g0 - 128) * 1.2) + 128 + (g0 - luma) * 0.3));
      const int b = clamp8(static_cast<int>(((b0 - 128) * 1.2) + 128 + (b0 - luma) * 0.3));
      out[x] = qRgb(r, g, b);
    }
  }
  return dst;
}

QImage VideoEffects::sharpen(const QImage& frame) {
  if (frame.width() < 3 || frame.height() < 3) {
    return frame;
  }
  QImage dst(frame.size(), QImage::Format_RGB32);
  for (int y = 0; y < frame.height(); ++y) {
    const int y0 = std::max(0, y - 1);
    const int y1 = std::min(frame.height() - 1, y + 1);
    const QRgb* r0 = reinterpret_cast<const QRgb*>(frame.constScanLine(y0));
    const QRgb* r1 = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
    const QRgb* r2 = reinterpret_cast<const QRgb*>(frame.constScanLine(y1));
    QRgb* out = reinterpret_cast<QRgb*>(dst.scanLine(y));
    for (int x = 0; x < frame.width(); ++x) {
      const int x0 = std::max(0, x - 1);
      const int x1 = std::min(frame.width() - 1, x + 1);
      const int r =
          clamp8(5 * qRed(r1[x]) - qRed(r0[x]) - qRed(r2[x]) - qRed(r1[x0]) - qRed(r1[x1]));
      const int g = clamp8(5 * qGreen(r1[x]) - qGreen(r0[x]) - qGreen(r2[x]) - qGreen(r1[x0]) -
                           qGreen(r1[x1]));
      const int b =
          clamp8(5 * qBlue(r1[x]) - qBlue(r0[x]) - qBlue(r2[x]) - qBlue(r1[x0]) - qBlue(r1[x1]));
      out[x] = qRgb(r, g, b);
    }
  }
  return dst;
}

QImage VideoEffects::vignette(const QImage& frame) {
  const int w = frame.width();
  const int h = frame.height();
  if (w <= 0 || h <= 0) {
    return frame;
  }
  const double cx = w / 2.0;
  const double cy = h / 2.0;
  const double invMax2 = 1.0 / std::max(1.0, cx * cx + cy * cy);
  // Precompute per-column dx^2 term; per-pixel work is then a multiply-add
  // (no sqrt: factor uses d^2 directly, identical to 1-0.75*d*d).
  std::vector<double> colDx2(static_cast<size_t>(w));
  for (int x = 0; x < w; ++x) {
    const double dx = x - cx;
    colDx2[static_cast<size_t>(x)] = dx * dx * invMax2;
  }
  // Precompute integer-scaled factors per row band? Factors vary per pixel,
  // so quantize to 256-entry LUT over d2 in [0,1] for cache-friendliness.
  static const int kLutN = 256;
  int lut[kLutN + 1];
  for (int i = 0; i <= kLutN; ++i) {
    const double d2 = i / static_cast<double>(kLutN);
    lut[i] = static_cast<int>(std::lround(std::max(0.45, 1.0 - 0.75 * d2) * 256.0));
  }
  QImage dst(frame.size(), QImage::Format_RGB32);
  for (int y = 0; y < h; ++y) {
    const double dy = y - cy;
    const double dy2 = dy * dy * invMax2;
    const QRgb* src = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
    QRgb* out = reinterpret_cast<QRgb*>(dst.scanLine(y));
    for (int x = 0; x < w; ++x) {
      double d2 = colDx2[static_cast<size_t>(x)] + dy2;
      if (d2 > 1.0) {
        d2 = 1.0;
      }
      const int f = lut[static_cast<int>(std::lround(d2 * kLutN))];
      out[x] = qRgb((qRed(src[x]) * f) >> 8, (qGreen(src[x]) * f) >> 8, (qBlue(src[x]) * f) >> 8);
    }
  }
  return dst;
}

QImage VideoEffects::glitch(const QImage& frame) {
  // Deterministic digital artifacting: banded horizontal shifts plus
  // red/blue channel displacement, seeded by row index (stable output).
  const int w = frame.width();
  const int h = frame.height();
  QImage dst(frame.size(), QImage::Format_RGB32);
  for (int y = 0; y < h; ++y) {
    const QRgb* src = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
    QRgb* out = reinterpret_cast<QRgb*>(dst.scanLine(y));
    const int band = (y / 7) % 5;
    const int shift = (band == 2) ? 6 : (band == 4 ? -6 : 0);
    const bool tear = (y % 37) < 2;
    for (int x = 0; x < w; ++x) {
      const int xs = std::clamp(x + shift, 0, w - 1);
      const int xr = std::clamp(x + 3, 0, w - 1);
      const int xb = std::clamp(x - 3, 0, w - 1);
      int g = qGreen(src[xs]);
      if (tear) {
        g = 255 - g;
      }
      out[x] = qRgb(qRed(src[xr]), g, qBlue(src[xb]));
    }
  }
  return dst;
}

QImage VideoEffects::sepia(const QImage& frame) {
  const int w = frame.width();
  const int h = frame.height();
  QImage dst(frame.size(), QImage::Format_RGB32);
  for (int y = 0; y < h; ++y) {
    const QRgb* src = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
    QRgb* out = reinterpret_cast<QRgb*>(dst.scanLine(y));
    for (int x = 0; x < w; ++x) {
      const int r0 = qRed(src[x]);
      const int g0 = qGreen(src[x]);
      const int b0 = qBlue(src[x]);
      const int r = clamp8(static_cast<int>(r0 * 0.393 + g0 * 0.769 + b0 * 0.189));
      const int g = clamp8(static_cast<int>(r0 * 0.349 + g0 * 0.686 + b0 * 0.168));
      const int b = clamp8(static_cast<int>(r0 * 0.272 + g0 * 0.534 + b0 * 0.131));
      out[x] = qRgb(r, g, b);
    }
  }
  return dst;
}

QImage VideoEffects::grayscale(const QImage& frame) {
  const int w = frame.width();
  const int h = frame.height();
  QImage dst(frame.size(), QImage::Format_RGB32);
  for (int y = 0; y < h; ++y) {
    const QRgb* src = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
    QRgb* out = reinterpret_cast<QRgb*>(dst.scanLine(y));
    for (int x = 0; x < w; ++x) {
      const int r0 = qRed(src[x]);
      const int g0 = qGreen(src[x]);
      const int b0 = qBlue(src[x]);
      const int luma = (r0 * 30 + g0 * 59 + b0 * 11) / 100;
      out[x] = qRgb(luma, luma, luma);
    }
  }
  return dst;
}

QImage VideoEffects::invert(const QImage& frame) {
  const int w = frame.width();
  const int h = frame.height();
  QImage dst(frame.size(), QImage::Format_RGB32);
  for (int y = 0; y < h; ++y) {
    const QRgb* src = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
    QRgb* out = reinterpret_cast<QRgb*>(dst.scanLine(y));
    for (int x = 0; x < w; ++x) {
      out[x] = qRgb(255 - qRed(src[x]), 255 - qGreen(src[x]), 255 - qBlue(src[x]));
    }
  }
  return dst;
}
