#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "FrameImageProvider.h"
#include "TimelineEngine.h"
#include "VideoFrameItem.h"

int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("PrimoReels"));
  QCoreApplication::setApplicationName(QStringLiteral("PrimoReels"));

  qmlRegisterType<VideoFrameItem>("PrimoReels", 1, 0, "VideoFrameItem");

  QQmlApplicationEngine engine;

  TimelineEngine timelineEngine;
  engine.rootContext()->setContextProperty("timelineEngine", &timelineEngine);

  auto* imageProvider = new FrameImageProvider(&timelineEngine);
  engine.addImageProvider(QStringLiteral("preview"), imageProvider);
  engine.addImageProvider(QStringLiteral("thumbs"), new ThumbImageProvider(&timelineEngine));
  engine.addImageProvider(QStringLiteral("source"), new SourceImageProvider(&timelineEngine));

  const QUrl url(QStringLiteral("qrc:/qml/Main.qml"));
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreated, &app,
      [url](QObject* obj, const QUrl& objUrl) {
        if (!obj && url == objUrl)
          QCoreApplication::exit(-1);
      },
      Qt::QueuedConnection);

  engine.load(url);

  return app.exec();
}
