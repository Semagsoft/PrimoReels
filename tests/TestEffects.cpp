#include <QTest>
#include "VideoEffects.h"

namespace {

QImage testCard() {
  QImage img(64, 48, QImage::Format_RGB32);
  for (int y = 0; y < img.height(); ++y) {
    QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
    for (int x = 0; x < img.width(); ++x) {
      row[x] = qRgb((x * 4) % 256, (y * 5) % 256, ((x + y) * 3) % 256);
    }
  }
  return img;
}

bool imagesEqual(const QImage& a, const QImage& b) {
  if (a.size() != b.size() || a.format() != b.format()) {
    return false;
  }
  for (int y = 0; y < a.height(); ++y) {
    if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                    static_cast<size_t>(a.bytesPerLine())) != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

class TestEffects : public QObject {
  Q_OBJECT
 private slots:
  void catalogMatchesLibrary() {
    const QStringList effects = VideoEffects::availableEffects();
    QVERIFY(effects.contains("Blur"));
    QVERIFY(effects.contains("Color Correction"));
    QVERIFY(effects.contains("Sharpen"));
    QVERIFY(effects.contains("Vignette"));
    QVERIFY(effects.contains("Glitch"));
    QVERIFY(effects.contains("Sepia"));
    QVERIFY(effects.contains("Grayscale"));
    QVERIFY(effects.contains("Invert"));
    QVERIFY(VideoEffects::isKnownEffect("Blur"));
    QVERIFY(!VideoEffects::isKnownEffect("Nonexistent"));
    QVERIFY(!VideoEffects::isKnownEffect(""));
  }

  void passthroughCases() {
    const QImage src = testCard();
    QVERIFY(imagesEqual(VideoEffects::apply(src, ""), src));
    QVERIFY(imagesEqual(VideoEffects::apply(src, "Nonexistent"), src));
    QVERIFY(VideoEffects::apply(QImage(), "Blur").isNull());
  }

  void eachEffectKeepsSizeAndChangesPixels() {
    const QImage src = testCard();
    for (const QString& name : VideoEffects::availableEffects()) {
      const QImage out = VideoEffects::apply(src, name);
      QCOMPARE(out.size(), src.size());
      QCOMPARE(out.format(), QImage::Format_RGB32);
      QVERIFY2(!imagesEqual(out, src), qPrintable(name));
    }
  }

  void tinyImagesDoNotCrash() {
    const QImage tiny(2, 2, QImage::Format_RGB32);
    for (const QString& name : VideoEffects::availableEffects()) {
      const QImage out = VideoEffects::apply(tiny, name);
      QCOMPARE(out.size(), tiny.size());
    }
  }

  void vignetteDarkensCorners() {
    QImage white(40, 40, QImage::Format_RGB32);
    white.fill(Qt::white);
    const QImage out = VideoEffects::apply(white, "Vignette");
    const int center = qRed(reinterpret_cast<const QRgb*>(out.constScanLine(20))[20]);
    const int corner = qRed(reinterpret_cast<const QRgb*>(out.constScanLine(0))[0]);
    QVERIFY(center > 200);
    QVERIFY(corner < center);
  }
};

QTEST_MAIN(TestEffects)
#include "TestEffects.moc"
