#pragma once

#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QIODevice>
#include <QMutex>
#include <QObject>

class AudioFifoDevice : public QIODevice {
  Q_OBJECT
 public:
  explicit AudioFifoDevice(QObject* parent = nullptr);

  void pushData(const QByteArray& data);
  void clear();
  qint64 buffered() const;
  // Non-destructive head copy (up to maxBytes) for tests and future level
  // meters. Locks the same mutex as readData; never blocks the render pull.
  QByteArray peek(qint64 maxBytes) const;
  bool isSequential() const override { return true; }

 protected:
  qint64 readData(char* data, qint64 maxlen) override;
  qint64 writeData(const char* data, qint64 len) override;
  qint64 bytesAvailable() const override;

 private:
  mutable QMutex m_mutex;
  QByteArray m_buffer;
};

class AudioPlayer : public QObject {
  Q_OBJECT
 public:
  static constexpr int sampleRate() { return 48000; }
  static constexpr int channelCount() { return 2; }
  static constexpr double bytesPerSecond() { return 48000.0 * 2 * sizeof(qint16); }
  static QByteArray applyGain(QByteArray samples, double gain);
  // Sum two S16-stereo PCM chunks (shorter padded with silence), clamped.
  static QByteArray mixAudio(const QByteArray& a, const QByteArray& b);
  static void applyGainInPlace(QByteArray& samples, double gain);

  explicit AudioPlayer(QObject* parent = nullptr);
  ~AudioPlayer() override;

  void start();
  void suspend();
  void stop();
  void queueData(const QByteArray& data);
  void setVolume(double volume);
  double volume() const { return m_volume; }
  qint64 bufferedBytes() const;
  qint64 bufferedMsecs() const;
  // Non-destructive copy of queued PCM (tests, future meters).
  QByteArray peekBuffered(qint64 maxBytes = 1048576) const;

 private:
  QAudioSink* m_sink = nullptr;
  AudioFifoDevice* m_device = nullptr;
  double m_volume = 1.0;
};
