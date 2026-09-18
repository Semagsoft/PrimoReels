#include "AudioPlayer.h"

#include <QAudioDevice>
#include <QDebug>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <cstdint>

AudioFifoDevice::AudioFifoDevice(QObject* parent) : QIODevice(parent) {}

void AudioFifoDevice::pushData(const QByteArray& data) {
  if (data.isEmpty()) {
    return;
  }
  QMutexLocker locker(&m_mutex);
  m_buffer.append(data);
}

void AudioFifoDevice::clear() {
  QMutexLocker locker(&m_mutex);
  m_buffer.clear();
}

qint64 AudioFifoDevice::buffered() const {
  QMutexLocker locker(&m_mutex);
  return m_buffer.size();
}

QByteArray AudioFifoDevice::peek(qint64 maxBytes) const {
  QMutexLocker locker(&m_mutex);
  if (m_buffer.isEmpty() || maxBytes <= 0) {
    return {};
  }
  return m_buffer.left(
      static_cast<qsizetype>(std::min(maxBytes, static_cast<qint64>(m_buffer.size()))));
}

qint64 AudioFifoDevice::readData(char* data, qint64 maxlen) {
  QMutexLocker locker(&m_mutex);
  if (m_buffer.isEmpty() || maxlen <= 0) {
    return 0;
  }
  const qint64 n = std::min(maxlen, static_cast<qint64>(m_buffer.size()));
  memcpy(data, m_buffer.constData(), static_cast<size_t>(n));
  m_buffer.remove(0, static_cast<qsizetype>(n));
  return n;
}

qint64 AudioFifoDevice::writeData(const char* data, qint64 len) {
  Q_UNUSED(data)
  Q_UNUSED(len)
  return -1;
}

qint64 AudioFifoDevice::bytesAvailable() const {
  QMutexLocker locker(&m_mutex);
  return m_buffer.size() + QIODevice::bytesAvailable();
}

AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent) {
  m_device = new AudioFifoDevice(this);
  m_device->open(QIODevice::ReadOnly);
}

AudioPlayer::~AudioPlayer() {
  stop();
}

QByteArray AudioPlayer::applyGain(QByteArray samples, double gain) {
  applyGainInPlace(samples, gain);
  return samples;
}

void AudioPlayer::applyGainInPlace(QByteArray& samples, double gain) {
  if (samples.isEmpty()) {
    return;
  }
  const qsizetype count = samples.size() / static_cast<qsizetype>(sizeof(qint16));
  auto* data = reinterpret_cast<qint16*>(samples.data());
  for (qsizetype i = 0; i < count; ++i) {
    const double scaled = static_cast<double>(data[i]) * gain;
    const double clamped = std::clamp(scaled, -32768.0, 32767.0);
    data[i] = static_cast<qint16>(std::llround(clamped));
  }
}

QByteArray AudioPlayer::mixAudio(const QByteArray& a, const QByteArray& b) {
  if (a.isEmpty()) {
    return b;
  }
  if (b.isEmpty()) {
    return a;
  }
  const qsizetype n = std::max(a.size(), b.size());
  // Round down to whole samples so reinterpret_cast stays aligned.
  const qsizetype count = n / static_cast<qsizetype>(sizeof(qint16));
  QByteArray out(static_cast<qsizetype>(count * sizeof(qint16)), 0);
  auto* dst = reinterpret_cast<qint16*>(out.data());
  const auto* pa = reinterpret_cast<const qint16*>(a.constData());
  const auto* pb = reinterpret_cast<const qint16*>(b.constData());
  const qsizetype ca = a.size() / static_cast<qsizetype>(sizeof(qint16));
  const qsizetype cb = b.size() / static_cast<qsizetype>(sizeof(qint16));
  for (qsizetype i = 0; i < count; ++i) {
    const int va = i < ca ? static_cast<int>(pa[i]) : 0;
    const int vb = i < cb ? static_cast<int>(pb[i]) : 0;
    dst[i] = static_cast<qint16>(std::clamp(va + vb, -32768, 32767));
  }
  return out;
}

void AudioPlayer::start() {
  if (!m_sink) {
    QAudioFormat format;
    format.setSampleRate(sampleRate());
    format.setChannelCount(channelCount());
    format.setSampleFormat(QAudioFormat::Int16);

    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
      qWarning() << "No default audio output device available";
      return;
    }
    if (!device.isFormatSupported(format)) {
      qWarning() << "Audio format not supported by output device";
      return;
    }

    m_sink = new QAudioSink(device, format, this);
    m_sink->setVolume(1.0);
  }
  if (!m_sink || !m_device) {
    qWarning() << "AudioPlayer::start without a valid sink/device";
    return;
  }
  m_sink->start(m_device);
}

void AudioPlayer::suspend() {
  if (m_sink) {
    m_sink->suspend();
  }
}

void AudioPlayer::stop() {
  if (m_sink) {
    m_sink->stop();
  }
  if (m_device) {
    m_device->clear();
  }
}

void AudioPlayer::queueData(const QByteArray& data) {
  if (data.isEmpty() || !m_device) {
    return;
  }
  constexpr qint64 maxBuffered = static_cast<qint64>(8 * bytesPerSecond());
  if (m_device->buffered() >= maxBuffered) {
    return;
  }
  m_device->pushData(applyGain(QByteArray(data), m_volume));
}

void AudioPlayer::setVolume(double volume) {
  m_volume = std::clamp(volume, 0.0, 2.0);
}

qint64 AudioPlayer::bufferedBytes() const {
  return m_device ? m_device->buffered() : 0;
}

qint64 AudioPlayer::bufferedMsecs() const {
  return static_cast<qint64>(static_cast<double>(bufferedBytes()) / bytesPerSecond() * 1000.0);
}

QByteArray AudioPlayer::peekBuffered(qint64 maxBytes) const {
  return m_device ? m_device->peek(maxBytes) : QByteArray();
}
