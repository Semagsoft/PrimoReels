#include "ExportThread.h"

#include <QFont>
#include <QMutexLocker>
#include <QPainter>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#include "ClipUtils.h"
#include "MediaDecoder.h"
#include "VideoEffects.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr int kFps = 30;
constexpr int kAudioRate = 48000;
constexpr int kAudioChannels = 2;
constexpr int64_t kAudioBitRate = 128000;

struct PacketGuard {
  AVPacket* pkt = nullptr;
  explicit PacketGuard(AVPacket* p) : pkt(p) {}
  ~PacketGuard() { av_packet_free(&pkt); }
  AVPacket* get() const { return pkt; }
};

struct FrameGuard {
  AVFrame* frame = nullptr;
  explicit FrameGuard(AVFrame* f) : frame(f) {}
  ~FrameGuard() { av_frame_free(&frame); }
  AVFrame* get() const { return frame; }
};

// Overlap duration for the transition out of clip a into clip b (0 = none).
// Centered on the cut; total sequence length is unchanged.
double transitionOverlap(const QVariantMap& a, const QVariantMap& b, int fps) {
  const QString type = ClipUtils::clipTransition(a);
  if (type != QLatin1String("Cross Dissolve") && type != QLatin1String("Dip to Black")) {
    return 0.0;
  }
  const double requested = std::max(0.0, ClipUtils::clipTransitionDuration(a));
  const double overlap =
      std::min({requested, ClipUtils::clipDuration(a), ClipUtils::clipDuration(b), 2.0});
  return (overlap >= 1.0 / fps) ? overlap : 0.0;
}

QImage toOutputSize(const QImage& img, int outW, int outH) {
  if (img.size() == QSize(outW, outH)) {
    return img;
  }
  return img.scaled(outW, outH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

inline int clamp8(int v) {
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// General two-weight mix: out = a*aa + b*ba (remainder, if any, is black).
// Size-mismatched inputs are normalized by the caller via toOutputSize;
// defensively return the first frame rather than reading out of bounds.
QImage mixWeighted(const QImage& a, double aa, const QImage& b, double ba) {
  if (a.isNull()) {
    return b;
  }
  if (b.isNull() || a.size() != b.size()) {
    return a;
  }
  const int w = a.width();
  const int h = a.height();
  QImage out(w, h, QImage::Format_RGB32);
  for (int y = 0; y < h; ++y) {
    const QRgb* ra = reinterpret_cast<const QRgb*>(a.constScanLine(y));
    const QRgb* rb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
    QRgb* ro = reinterpret_cast<QRgb*>(out.scanLine(y));
    for (int x = 0; x < w; ++x) {
      ro[x] = qRgb(clamp8(static_cast<int>(qRed(ra[x]) * aa + qRed(rb[x]) * ba)),
                   clamp8(static_cast<int>(qGreen(ra[x]) * aa + qGreen(rb[x]) * ba)),
                   clamp8(static_cast<int>(qBlue(ra[x]) * aa + qBlue(rb[x]) * ba)));
    }
  }
  return out;
}

QImage mixFrames(const QImage& a, const QImage& b, double alpha) {
  return mixWeighted(a, 1.0 - alpha, b, alpha);
}

// Lower-third title burned into an output-sized RGB32 frame (no-op when empty).
QImage drawTitle(QImage base, const QString& title, int outW, int outH) {
  if (title.isEmpty()) {
    return base;
  }
  if (base.size() != QSize(outW, outH)) {
    base = base.scaled(outW, outH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  }
  base.detach();
  QFont font;
  font.setBold(true);
  font.setPixelSize(std::max(12, outH / 16));
  QPainter painter(&base);
  painter.setFont(font);
  const QRect rect(20, outH * 2 / 3, outW - 40, outH / 4);
  painter.setPen(Qt::black);
  for (int dx = -2; dx <= 2; dx += 2) {
    for (int dy = -2; dy <= 2; dy += 2) {
      if (dx != 0 || dy != 0) {
        painter.drawText(rect.translated(dx, dy),
                         Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextWordWrap, title);
      }
    }
  }
  painter.setPen(Qt::white);
  painter.drawText(rect, Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextWordWrap, title);
  return base;
}

// Drain available packets from an encoder into the output.
bool drainEncoder(AVCodecContext* enc, AVStream* stream, AVFormatContext* oc, AVPacket* pkt) {
  for (;;) {
    const int ret = avcodec_receive_packet(enc, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return true;
    }
    if (ret < 0) {
      return false;
    }
    av_packet_rescale_ts(pkt, enc->time_base, stream->time_base);
    pkt->stream_index = stream->index;
    if (av_interleaved_write_frame(oc, pkt) < 0) {
      av_packet_unref(pkt);
      return false;
    }
    av_packet_unref(pkt);
  }
}

// Send one frame (or flush with nullptr) to an encoder and drain output.
bool sendFrame(AVCodecContext* enc,
               AVStream* stream,
               AVFormatContext* oc,
               AVPacket* pkt,
               AVFrame* frame) {
  if (avcodec_send_frame(enc, frame) < 0) {
    return false;
  }
  return drainEncoder(enc, stream, oc, pkt);
}

// Sample-and-hold resampler over a forward-reading decoder: the caller seeks
// once, then asks for the frame covering each output timestamp. Frames before
// rangeStart are skipped, rangeEnd stops the stream (EOF), and NaN timestamps
// (unordered stills) are held forever once acquired.
class SampleHoldPump {
 public:
  SampleHoldPump(MediaDecoder* decoder,
                 double rangeStart,
                 double rangeEnd = std::numeric_limits<double>::infinity())
      : m_decoder(decoder),
        m_rangeStart(rangeStart),
        m_rangeEnd(rangeEnd),
        m_heldTs(rangeStart - 1.0) {}

  QImage sampleAt(double target) {
    while (!m_eof && !std::isnan(m_heldTs) && m_heldTs < target) {
      double ts = 0.0;
      const QImage img = m_decoder->readNextVideoFrame(&ts);
      if (img.isNull()) {
        m_eof = true;
        break;
      }
      if (!std::isnan(ts) && ts >= m_rangeEnd) {
        m_eof = true;
        break;
      }
      if (!std::isnan(ts) && ts < m_rangeStart) {
        continue;
      }
      m_held = img;
      m_heldTs = ts;
      if (std::isnan(ts)) {
        break;
      }
    }
    return m_held;
  }

 private:
  MediaDecoder* m_decoder = nullptr;
  double m_rangeStart = 0.0;
  double m_rangeEnd = std::numeric_limits<double>::infinity();
  QImage m_held;
  double m_heldTs = -1.0;
  bool m_eof = false;
};

}  // namespace

ExportThread::ExportThread() = default;

ExportThread::~ExportThread() {
  {
    QMutexLocker locker(&m_mutex);
    m_abort = true;
    m_condition.wakeOne();
  }
  wait();
}

void ExportThread::requestExport(const QVariantList& clips,
                                 const QString& outputPath,
                                 double volume) {
  QMutexLocker locker(&m_mutex);
  m_pendingClips = clips;
  m_pendingOutput = outputPath;
  m_pendingVolume = std::clamp(volume, 0.0, 2.0);
  m_cancelRequested = false;
  m_hasRequest = true;
  m_condition.wakeOne();
}

void ExportThread::requestCancel() {
  QMutexLocker locker(&m_mutex);
  m_cancelRequested = true;
  m_condition.wakeOne();
}

bool ExportThread::isExporting() const {
  QMutexLocker locker(&m_mutex);
  return m_exporting;
}

bool ExportThread::isCancelRequested() const {
  QMutexLocker locker(&m_mutex);
  // Destruction (m_abort) must also interrupt an in-flight runExport;
  // otherwise deleting the thread mid-export would join only after the
  // full encode instead of cancelling promptly.
  return m_cancelRequested || m_abort;
}

void ExportThread::run() {
  for (;;) {
    QVariantList clips;
    QString output;
    double volume = 1.0;
    {
      QMutexLocker locker(&m_mutex);
      while (!m_hasRequest && !m_abort) {
        m_condition.wait(&m_mutex);
      }
      if (m_abort) {
        break;
      }
      clips = m_pendingClips;
      output = m_pendingOutput;
      volume = m_pendingVolume;
      m_hasRequest = false;
      m_exporting = true;
    }

    QString error;
    QString warning;
    const bool ok = runExport(clips, output, &error, &warning, volume);

    {
      QMutexLocker locker(&m_mutex);
      m_exporting = false;
      // Preserve the first (root-cause) error: a concurrent cancel must not
      // overwrite it, only annotate it.
      if (!ok && m_cancelRequested) {
        if (error.isEmpty()) {
          error = QStringLiteral("Export cancelled.");
        } else {
          error += QStringLiteral(" (cancel requested)");
        }
      }
    }
    if (ok && error.isEmpty()) {
      emit exportFinished(output);
      if (!warning.isEmpty()) {
        emit exportWarning(warning);
      }
    } else {
      emit exportFailed(error.isEmpty() ? QStringLiteral("Export failed.") : error);
    }
  }
}

bool ExportThread::runExport(const QVariantList& clips,
                             const QString& outputPath,
                             QString* error,
                             QString* warning,
                             double volume) {
  const double globalVolume = std::clamp(volume, 0.0, 2.0);
  auto fail = [&](const QString& msg) {
    if (error && error->isEmpty()) {
      *error = msg;
    }
    return false;
  };
  const QString outPath = ClipUtils::normalizedMediaPath(outputPath);
  if (clips.isEmpty() || outPath.isEmpty()) {
    return fail(QStringLiteral("Export failed: nothing to export."));
  }

  // Probe once per distinct path: fix output dims from the first decodable
  // clip, detect audio, and record decodability so total/progress/audio stay
  // authoritative (skipped clips must not count toward duration).
  int outW = 0;
  int outH = 0;
  bool anyAudio = false;
  bool anyVideo = false;
  QSet<QString> decodablePaths;
  QSet<QString> skippedPaths;
  for (const QVariant& v : clips) {
    if (isCancelRequested()) {
      return fail(QStringLiteral("Export failed: encoding interrupted."));
    }
    const QString path = ClipUtils::clipPath(v.toMap());
    if (decodablePaths.contains(path) || skippedPaths.contains(path)) {
      continue;
    }
    MediaDecoder dec;
    if (!dec.open(path)) {
      skippedPaths.insert(path);
      continue;
    }
    decodablePaths.insert(path);
    if (outW <= 0 && dec.hasVideo()) {
      outW = dec.width() & ~1;
      outH = dec.height() & ~1;
    }
    if (dec.hasVideo()) {
      anyVideo = true;
    }
    if (dec.hasAudio()) {
      anyAudio = true;
    }
    if ((outW > 0 || !anyVideo) && anyAudio && anyVideo) {
      // Dims fixed and audio confirmed; remaining probes only check
      // decodability of unseen paths.
      bool allSeen = true;
      for (const QVariant& w : clips) {
        if (!decodablePaths.contains(ClipUtils::clipPath(w.toMap()))) {
          // Unseen path: still need to know if it decodes. Keep going
          // unless every remaining unseen path was already probed.
          // Fast path: if all V1 paths seen, stop.
          allSeen = false;
          break;
        }
      }
      if (allSeen) {
        break;
      }
    }
  }
  if ((outW < 16 || outH < 16)) {
    if (anyAudio && !anyVideo) {
      // Audio-only timeline: render 720p black video so the MP4 carries sound.
      outW = 1280;
      outH = 720;
    } else {
      return fail(QStringLiteral("Export failed: no decodable video clip to size the output."));
    }
  }

  // V1 is the edited sequence; V2 clips are overlays composited on top.
  // Transition overlaps consume shared time: total = V1 clips minus overlaps.
  // Only decodable V1 clips count; undecodable ones are skipped like in
  // sequence playback, with overlaps recomputed between survivors.
  std::vector<int> v1;
  for (int i = 0; i < static_cast<int>(clips.size()); ++i) {
    const QVariantMap c = clips.at(i).toMap();
    if (c.value(QStringLiteral("track"), 0).toInt() != 0) {
      continue;
    }
    if (!decodablePaths.contains(ClipUtils::clipPath(c))) {
      skippedPaths.insert(ClipUtils::clipPath(c));
      continue;
    }
    if (!(ClipUtils::clipDuration(c) > 0.0)) {
      skippedPaths.insert(ClipUtils::clipPath(c));
      continue;
    }
    v1.push_back(i);
  }
  std::vector<double> blendOut(clips.size(), 0.0);
  for (size_t k = 0; k + 1 < v1.size(); ++k) {
    blendOut[v1[k]] = transitionOverlap(clips.at(v1[k]).toMap(), clips.at(v1[k + 1]).toMap(), kFps);
  }
  double totalSeconds = 0.0;
  for (int i : v1) {
    totalSeconds += ClipUtils::clipDuration(clips.at(i).toMap()) - blendOut[i];
  }
  if (!(totalSeconds > 0.0)) {
    return fail(QStringLiteral("Export failed: timeline has no playable duration."));
  }
  // Bound 32-bit frame/sample counters below (outFrames, neededSamples):
  // beyond this the export is rejected instead of silently overflowing.
  static constexpr double kMaxExportSeconds = 12.0 * 3600.0;
  if (!std::isfinite(totalSeconds) || totalSeconds > kMaxExportSeconds) {
    return fail(QStringLiteral("Export failed: timeline too long (%1 seconds).")
                    .arg(QString::number(totalSeconds)));
  }

  AVFormatContext* oc = nullptr;
  if (avformat_alloc_output_context2(&oc, nullptr, "mp4", nullptr) < 0 || !oc) {
    return fail(QStringLiteral("Export failed: could not allocate MP4 muxer."));
  }

  // --- Video stream ---
  const AVCodec* videoCodec = avcodec_find_encoder_by_name("libx264");
  const bool useX264 = videoCodec != nullptr;
  if (!videoCodec) {
    videoCodec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  }
  if (!videoCodec) {
    avformat_free_context(oc);
    return fail(QStringLiteral("Export failed: no H.264/MPEG-4 video encoder found."));
  }
  AVStream* videoStream = avformat_new_stream(oc, nullptr);
  AVCodecContext* videoEnc = avcodec_alloc_context3(videoCodec);
  if (!videoStream || !videoEnc) {
    avcodec_free_context(&videoEnc);
    avformat_free_context(oc);
    return fail(QStringLiteral("Export failed: could not allocate video stream."));
  }
  videoEnc->width = outW;
  videoEnc->height = outH;
  videoEnc->time_base = AVRational{1, kFps};
  videoEnc->framerate = AVRational{kFps, 1};
  videoEnc->pix_fmt = AV_PIX_FMT_YUV420P;
  videoEnc->gop_size = 2 * kFps;
  if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
    videoEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  if (useX264) {
    av_opt_set(videoEnc->priv_data, "preset", "veryfast", 0);
    av_opt_set(videoEnc->priv_data, "crf", "23", 0);
  } else {
    videoEnc->bit_rate = 5000000;
  }
  bool streamsOk = avcodec_open2(videoEnc, videoCodec, nullptr) >= 0 &&
                   avcodec_parameters_from_context(videoStream->codecpar, videoEnc) >= 0;
  videoStream->time_base = videoEnc->time_base;

  // --- Audio stream (optional): allocate + open first, create the muxer
  // stream only on success so a failed encoder leaves no empty stream.
  AVStream* audioStream = nullptr;
  AVCodecContext* audioEnc = nullptr;
  if (anyAudio && streamsOk) {
    const AVCodec* audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    AVCodecContext* enc = audioCodec ? avcodec_alloc_context3(audioCodec) : nullptr;
    bool audioOk = false;
    if (enc) {
      enc->sample_rate = kAudioRate;
      av_channel_layout_default(&enc->ch_layout, kAudioChannels);
      enc->sample_fmt = AV_SAMPLE_FMT_FLTP;  // native AAC sample format
      enc->time_base = AVRational{1, kAudioRate};
      enc->bit_rate = kAudioBitRate;
      if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
      }
      audioOk = avcodec_open2(enc, audioCodec, nullptr) >= 0;
    }
    if (audioOk) {
      // Stage parameters into a temp first and only create the muxer
      // stream once they are known-good, so a failure leaves no empty
      // stream behind (no nb_streams surgery on the format context).
      AVCodecParameters* tmp = avcodec_parameters_alloc();
      if (tmp && avcodec_parameters_from_context(tmp, enc) >= 0) {
        audioStream = avformat_new_stream(oc, nullptr);
        if (audioStream && avcodec_parameters_copy(audioStream->codecpar, tmp) >= 0) {
          audioStream->time_base = enc->time_base;
          audioEnc = enc;
        } else {
          audioStream = nullptr;
        }
      }
      avcodec_parameters_free(&tmp);
      if (!audioEnc) {
        avcodec_free_context(&enc);
      }
    } else {
      avcodec_free_context(&enc);
    }
  }

  QSaveFile outputFile(outPath);
  outputFile.setDirectWriteFallback(false);
  auto cleanup = [&] {
    if (videoEnc) {
      avcodec_free_context(&videoEnc);
    }
    if (audioEnc) {
      avcodec_free_context(&audioEnc);
    }
    if (oc) {
      if (oc->pb) {
        av_freep(reinterpret_cast<void*>(&oc->pb->buffer));
        avio_context_free(&oc->pb);
      }
      avformat_free_context(oc);
    }
  };

  if (!streamsOk) {
    cleanup();
    return fail(QStringLiteral("Export failed: could not open video encoder."));
  }
  if (!outputFile.open(QIODevice::WriteOnly)) {
    cleanup();
    return fail(QStringLiteral("Export failed: cannot write to %1.").arg(outPath));
  }
  constexpr int ioBufferSize = 32768;
  auto* ioBuffer = static_cast<unsigned char*>(av_malloc(ioBufferSize));
  if (!ioBuffer) {
    cleanup();
    return fail(QStringLiteral("Export failed: out of memory."));
  }
#if LIBAVFORMAT_VERSION_MAJOR >= 61
  auto writePacket = [](void* opaque, const uint8_t* data, int size) -> int {
#else
  auto writePacket = [](void* opaque, uint8_t* data, int size) -> int {
#endif
    auto* file = static_cast<QSaveFile*>(opaque);
    const qint64 written = file->write(reinterpret_cast<const char*>(data), size);
    return written == size ? size : AVERROR(EIO);
  };
  auto seekOutput = [](void* opaque, int64_t offset, int whence) -> int64_t {
    auto* file = static_cast<QSaveFile*>(opaque);
    if (whence == AVSEEK_SIZE) {
      return file->size();
    }
    whence &= ~AVSEEK_FORCE;
    qint64 base = 0;
    if (whence == SEEK_CUR) {
      base = file->pos();
    } else if (whence == SEEK_END) {
      base = file->size();
    } else if (whence != SEEK_SET) {
      return AVERROR(EINVAL);
    }
    if (base < 0 || offset < -base || offset > std::numeric_limits<qint64>::max() - base) {
      return AVERROR(EINVAL);
    }
    return file->seek(base + offset) ? file->pos() : AVERROR(EIO);
  };
  oc->pb =
      avio_alloc_context(ioBuffer, ioBufferSize, 1, &outputFile, nullptr, writePacket, seekOutput);
  if (!oc->pb) {
    av_free(ioBuffer);
    cleanup();
    outputFile.cancelWriting();
    return fail(QStringLiteral("Export failed: out of memory."));
  }
  oc->flags |= AVFMT_FLAG_CUSTOM_IO;
  if (avformat_write_header(oc, nullptr) < 0) {
    cleanup();
    outputFile.cancelWriting();
    return fail(QStringLiteral("Export failed: could not write MP4 header to %1.").arg(outPath));
  }

  PacketGuard pkt(av_packet_alloc());
  if (!pkt.get()) {
    cleanup();
    outputFile.cancelWriting();
    return fail(QStringLiteral("Export failed: out of memory."));
  }

  // Reusable video conversion + frame buffers.
  // QImage::Format_RGB32 bytes are fed to sws as AV_PIX_FMT_BGRA: on
  // little-endian hosts RGB32 is stored B,G,R,x in memory, which is exactly
  // BGRA. Pin the assumption so a big-endian port fails loudly instead of
  // exporting channel-swapped video.
#if Q_BYTE_ORDER != Q_LITTLE_ENDIAN
#error "RGB32->BGRA export path assumes little-endian byte order"
#endif
  SwsContext* sws = sws_getContext(outW, outH, AV_PIX_FMT_BGRA, outW, outH, AV_PIX_FMT_YUV420P,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
  FrameGuard yuv(av_frame_alloc());
  bool success = sws && yuv.get();
  if (success) {
    yuv.get()->format = AV_PIX_FMT_YUV420P;
    yuv.get()->width = outW;
    yuv.get()->height = outH;
    success = av_frame_get_buffer(yuv.get(), 32) >= 0;
  }

  // Audio resampler s16-stereo -> encoder format.
  SwrContext* swr = nullptr;
  AVChannelLayout inLayout;
  av_channel_layout_default(&inLayout, kAudioChannels);
  if (success && audioEnc) {
    swr_alloc_set_opts2(&swr, &audioEnc->ch_layout, audioEnc->sample_fmt, kAudioRate, &inLayout,
                        AV_SAMPLE_FMT_S16, kAudioRate, 0, nullptr);
    success = swr && swr_init(swr) >= 0;
  }
  av_channel_layout_uninit(&inLayout);

  int64_t videoFrameIndex = 0;
  int64_t audioSampleIndex = 0;
  double exportedSeconds = 0.0;
  double exportedBase = 0.0;
  int progressCounter = 0;

  // Persistent V2 overlay decoder (reopened only when coverage changes).
  // All overlay state is local to this runExport call, so a transient
  // decode failure cannot poison later exports.
  MediaDecoder overlayDec;
  QString overlayKey;
  std::optional<SampleHoldPump> overlayPump;
  QSet<QString> overlayFailed;

  auto overlayV2 = [&](QImage base, double outSeconds) -> QImage {
    base = toOutputSize(base.convertToFormat(QImage::Format_RGB32), outW, outH);
    base.detach();
    if (isCancelRequested()) {
      return base;
    }
    // Topmost = last V2 clip in list order covering this output time.
    QVariantMap cover;
    bool covered = false;
    for (const QVariant& v : clips) {
      const QVariantMap c = v.toMap();
      if (c.value(QStringLiteral("track"), 0).toInt() != 1) {
        continue;
      }
      const double start = c.value(QStringLiteral("startTime"), 0.0).toDouble();
      if (start <= outSeconds && outSeconds < start + ClipUtils::clipDuration(c)) {
        cover = c;
        covered = true;
      }
    }
    if (!covered) {
      return base;
    }
    const QString path = ClipUtils::clipPath(cover);
    const double vTrim = ClipUtils::clipTrimStart(cover);
    const double vStart = cover.value(QStringLiteral("startTime"), 0.0).toDouble();
    const QString key = path + QChar('|') + QString::number(vStart, 'f', 6);
    if (key != overlayKey) {
      overlayPump.reset();
      if (!overlayFailed.contains(path) && overlayDec.open(path)) {
        overlayDec.seekVideoTo(vTrim);
        overlayPump.emplace(&overlayDec, vTrim);
        overlayKey = key;
      } else {
        overlayKey.clear();
        overlayFailed.insert(path);
        return base;
      }
    }
    const double target = vTrim + (outSeconds - vStart);
    const QImage overlayHeld = overlayPump->sampleAt(target);
    if (overlayHeld.isNull()) {
      return base;
    }
    QImage pip =
        toOutputSize(VideoEffects::apply(overlayHeld, ClipUtils::clipEffect(cover)), outW, outH)
            .scaledToWidth(std::max(16, outW * 35 / 100), Qt::SmoothTransformation);
    QPainter painter(&base);
    painter.drawImage(outW - pip.width() - 12, outH - pip.height() - 12, pip);
    return base;
  };

  auto encodeRgbFrame = [&](const QImage& rgb) -> bool {
    const double outSeconds = videoFrameIndex / static_cast<double>(kFps);
    const QImage composited = overlayV2(rgb.convertToFormat(QImage::Format_RGB32), outSeconds);
    if (av_frame_make_writable(yuv.get()) < 0) {
      return false;
    }
    const uint8_t* srcData[4] = {composited.constBits(), nullptr, nullptr, nullptr};
    int srcLinesize[4] = {static_cast<int>(composited.bytesPerLine()), 0, 0, 0};
    // A short scale means corrupt input: encoding the stale YUV buffer as a
    // valid frame would silently corrupt the output.
    if (sws_scale(sws, srcData, srcLinesize, 0, outH, yuv.get()->data, yuv.get()->linesize) !=
        outH) {
      return false;
    }
    yuv.get()->pts = videoFrameIndex;
    if (!sendFrame(videoEnc, videoStream, oc, pkt.get(), yuv.get())) {
      return false;
    }
    ++videoFrameIndex;
    if (++progressCounter % 15 == 0) {
      emit progressChanged(exportedSeconds + (videoFrameIndex * 1.0 / kFps - exportedBase),
                           totalSeconds);
    }
    return true;
  };

  if (success) {
    for (size_t vi = 0; vi < v1.size(); ++vi) {
      const int ci = v1[vi];
      if (isCancelRequested()) {
        success = false;
        break;
      }
      const QVariantMap clip = clips.at(ci).toMap();
      const double dur = ClipUtils::clipDuration(clip);
      if (!(dur > 0.0)) {
        continue;
      }
      const double trimStart = ClipUtils::clipTrimStart(clip);
      const double inT = (vi > 0) ? blendOut[v1[vi - 1]] : 0.0;
      const double outT = blendOut[ci];
      const double soloStart = trimStart + inT;
      const double soloLen = std::max(0.0, dur - inT - outT);
      const double soloEnd = soloStart + soloLen;
      const QString effect = ClipUtils::clipEffect(clip);
      exportedBase = exportedSeconds;
      MediaDecoder dec;
      if (!dec.open(ClipUtils::clipPath(clip))) {
        skippedPaths.insert(ClipUtils::clipPath(clip));
        continue;  // skip undecodable clips, like sequence playback
      }
      QImage blackFrame(outW, outH, QImage::Format_RGB32);
      blackFrame.fill(Qt::black);

      // --- Video solo part: sample-and-hold resampling (black for audio-only) ---
      const int64_t outFrames = static_cast<int64_t>(std::ceil(soloLen * kFps));
      int64_t clipFrames = 0;
      if (dec.hasVideo()) {
        dec.seekVideoTo(soloStart);
        SampleHoldPump pump(&dec, soloStart, soloEnd);
        for (int64_t n = 0; n < outFrames; ++n) {
          if (isCancelRequested()) {
            break;
          }
          const double target = soloStart + n / static_cast<double>(kFps);
          const QImage held = pump.sampleAt(target);
          if (held.isNull()) {
            break;
          }
          const QImage frame =
              drawTitle(VideoEffects::apply(held, effect).convertToFormat(QImage::Format_RGB32),
                        clip.value(QStringLiteral("title")).toString(), outW, outH);
          if (!encodeRgbFrame(frame)) {
            success = false;
            break;
          }
          ++clipFrames;
        }
      } else {
        // Audio-only V1: hold black for the clip's duration.
        const QImage titled =
            drawTitle(blackFrame, clip.value(QStringLiteral("title")).toString(), outW, outH);
        for (int64_t n = 0; n < outFrames; ++n) {
          if (isCancelRequested()) {
            break;
          }
          if (!encodeRgbFrame(titled)) {
            success = false;
            break;
          }
          ++clipFrames;
        }
      }
      if (!success) {
        break;
      }

      // --- Transition blend into the next V1 clip ---
      int blendFrames = 0;
      if (outT > 0.0 && vi + 1 < v1.size()) {
        const QVariantMap next = clips.at(v1[vi + 1]).toMap();
        MediaDecoder decB;
        if (decB.open(ClipUtils::clipPath(next))) {
          const double trimB = ClipUtils::clipTrimStart(next);
          const QString effectB = ClipUtils::clipEffect(next);
          const QString transType = clip.value(QStringLiteral("transition")).toString();
          const double blendAStart = trimStart + dur - outT;
          const bool hasA = dec.hasVideo();
          const bool hasB = decB.hasVideo();
          if (hasA) {
            dec.seekVideoTo(blendAStart);
          }
          if (hasB) {
            decB.seekVideoTo(trimB);
          }
          const int blendCount = std::max(1, static_cast<int>(std::round(outT * kFps)));
          SampleHoldPump pumpA(&dec, blendAStart);
          SampleHoldPump pumpB(&decB, trimB);
          for (int k = 0; k < blendCount; ++k) {
            if (isCancelRequested()) {
              break;
            }
            const double tA = blendAStart + k / static_cast<double>(kFps);
            const double tB = trimB + k / static_cast<double>(kFps);
            QImage heldA = hasA ? pumpA.sampleAt(tA) : blackFrame;
            QImage heldB = hasB ? pumpB.sampleAt(tB) : blackFrame;
            if (heldA.isNull() || heldB.isNull()) {
              break;
            }
            const QImage frameA =
                drawTitle(toOutputSize(VideoEffects::apply(heldA, effect), outW, outH),
                          clip.value(QStringLiteral("title")).toString(), outW, outH);
            const QImage frameB =
                drawTitle(toOutputSize(VideoEffects::apply(heldB, effectB), outW, outH),
                          next.value(QStringLiteral("title")).toString(), outW, outH);
            const double t = (blendCount <= 1) ? 1.0 : k / (blendCount - 1.0);
            QImage blended;
            if (transType == QLatin1String("Dip to Black")) {
              // Centered dip: A fades to black over the first
              // half, B rises from black over the second half.
              const double aAmt = std::clamp(1.0 - 2.0 * t, 0.0, 1.0);
              const double bAmt = std::clamp(2.0 * t - 1.0, 0.0, 1.0);
              blended = mixWeighted(frameA, aAmt, frameB, bAmt);
            } else {
              blended = mixFrames(frameA, frameB, t);
            }
            if (!encodeRgbFrame(blended.convertToFormat(QImage::Format_RGB32))) {
              success = false;
              break;
            }
            ++blendFrames;
          }
        }
      }
      if (!success) {
        break;
      }
      exportedSeconds += (clipFrames + blendFrames) / static_cast<double>(kFps);
      emit progressChanged(exportedSeconds, totalSeconds);

      // --- Audio: streaming PCM from solo + blend range (hard cut), silence-padded ---
      // Bounded memory: at most one decoder chunk + one AAC frame of s16
      // staged at a time (no whole-clip QByteArray buffering).
      if (audioEnc) {
        // Audio covers the solo part plus this clip's outgoing blend,
        // whose pictures come from both sides but whose sound stays on A.
        const double audioOffset = soloStart;
        const double audioLen = soloLen + outT;
        const int bytesPerSampleFrame = 2 * kAudioChannels;
        const int64_t neededSamples = std::llround(audioLen * kAudioRate);
        bool clipHasAudio = dec.hasAudio();
        if (clipHasAudio) {
          if (!dec.seekAudioTo(0.0)) {
            clipHasAudio = false;
          } else {
            // Discard everything before the audio offset.
            int64_t toSkip =
                static_cast<int64_t>(audioOffset * MediaDecoder::audioBytesPerSecond());
            while (toSkip > 0) {
              if (isCancelRequested()) {
                break;
              }
              const QByteArray skipped =
                  dec.readAudioChunk(static_cast<int>(std::min(toSkip, int64_t{65536})));
              if (skipped.isEmpty()) {
                break;
              }
              toSkip -= skipped.size();
            }
          }
        }
        const int frameSize = audioEnc->frame_size > 0 ? audioEnc->frame_size : 1024;
        const double v1Gain = ClipUtils::clipEffectiveGain(clip, globalVolume);
        const double v1Dur = dur;
        const double v1InT = inT;
        // Pre-mix bed audio (V2 overlays + A1 clips) overlapping this
        // segment's output range [exportedBase, exportedBase+audioLen).
        std::vector<int16_t> bedMix;
        if (neededSamples > 0) {
          bedMix.assign(static_cast<size_t>(neededSamples) * kAudioChannels, 0);
          const double segOutStart = exportedBase;
          for (const QVariant& bv : clips) {
            if (isCancelRequested()) {
              break;
            }
            const QVariantMap bed = bv.toMap();
            const int bTrack = bed.value(QStringLiteral("track"), 0).toInt();
            if (bTrack != 1 && bTrack != 2) {
              continue;
            }
            const double bStart = bed.value(QStringLiteral("startTime"), 0.0).toDouble();
            const double bDur = ClipUtils::clipDuration(bed);
            if (!(bDur > 0.0)) {
              continue;
            }
            const double ovStart = std::max(segOutStart, bStart);
            const double ovEnd = std::min(segOutStart + audioLen, bStart + bDur);
            if (!(ovEnd > ovStart)) {
              continue;
            }
            const double bGain = ClipUtils::clipEffectiveGain(bed, globalVolume);
            if (!(bGain > 0.0)) {
              continue;
            }
            MediaDecoder bdec;
            if (!bdec.open(ClipUtils::clipPath(bed)) || !bdec.hasAudio()) {
              continue;
            }
            const double bTrim = ClipUtils::clipTrimStart(bed);
            const double srcOff = bTrim + (ovStart - bStart);
            if (!bdec.seekAudioTo(0.0)) {
              continue;
            }
            int64_t toSkip = static_cast<int64_t>(srcOff * MediaDecoder::audioBytesPerSecond());
            bool skipFail = false;
            while (toSkip > 0) {
              const QByteArray sk =
                  bdec.readAudioChunk(static_cast<int>(std::min(toSkip, int64_t{65536})));
              if (sk.isEmpty()) {
                skipFail = true;
                break;
              }
              toSkip -= sk.size();
            }
            if (skipFail) {
              continue;
            }
            int64_t wantSamples = std::llround((ovEnd - ovStart) * kAudioRate);
            wantSamples = std::min<int64_t>(wantSamples, neededSamples);
            const int64_t bedOffsetSamples = std::llround((ovStart - segOutStart) * kAudioRate);
            int64_t gotSamples = 0;
            while (gotSamples < wantSamples) {
              if (isCancelRequested()) {
                break;
              }
              const QByteArray chunk = bdec.readAudioChunk(65536);
              if (chunk.isEmpty()) {
                break;
              }
              const auto* sp = reinterpret_cast<const int16_t*>(chunk.constData());
              const int64_t chunkFrames = chunk.size() / (2LL * kAudioChannels);
              for (int64_t f = 0; f < chunkFrames && gotSamples < wantSamples; ++f) {
                const double intraT =
                    (ovStart - bStart) + static_cast<double>(gotSamples) / kAudioRate;
                const double fg = ClipUtils::clipFadeGain(bed, intraT, bDur);
                const double g = bGain * fg;
                const int64_t dstF = bedOffsetSamples + gotSamples;
                if (dstF < 0 || dstF >= neededSamples) {
                  ++gotSamples;
                  continue;
                }
                for (int ch = 0; ch < kAudioChannels; ++ch) {
                  const int idx = static_cast<int>(dstF * kAudioChannels + ch);
                  const int sidx = static_cast<int>(f * kAudioChannels + ch);
                  const int mixed = static_cast<int>(bedMix[static_cast<size_t>(idx)]) +
                                    static_cast<int>(std::llround(sp[sidx] * g));
                  bedMix[static_cast<size_t>(idx)] =
                      static_cast<int16_t>(std::clamp(mixed, -32768, 32767));
                }
                ++gotSamples;
              }
            }
          }
        }
        FrameGuard aframe(av_frame_alloc());
        if (!aframe.get()) {
          success = false;
          break;
        }
        aframe.get()->format = audioEnc->sample_fmt;
        if (av_channel_layout_copy(&aframe.get()->ch_layout, &audioEnc->ch_layout) < 0) {
          success = false;
          break;
        }
        aframe.get()->sample_rate = kAudioRate;
        aframe.get()->nb_samples = frameSize;
        if (av_frame_get_buffer(aframe.get(), 0) < 0) {
          success = false;
          break;
        }
        auto encodeS16Frame = [&](const int16_t* samples, int n) -> bool {
          if (av_frame_make_writable(aframe.get()) < 0) {
            return false;
          }
          const uint8_t* in[1] = {reinterpret_cast<const uint8_t*>(samples)};
          // Convert exactly n samples; pad remainder of the frame with silence.
          if (swr_convert(swr, aframe.get()->data, frameSize, in, n) < 0) {
            return false;
          }
          if (n < frameSize) {
            // Zero the unwritten tail (planar float).
            const int bytesPerPlane = (frameSize - n) * static_cast<int>(sizeof(float));
            for (int ch = 0; ch < kAudioChannels; ++ch) {
              std::memset(static_cast<uint8_t*>(aframe.get()->data[ch]) + n * sizeof(float), 0,
                          static_cast<size_t>(bytesPerPlane));
            }
          }
          aframe.get()->nb_samples = frameSize;
          aframe.get()->pts = audioSampleIndex;
          if (!sendFrame(audioEnc, audioStream, oc, pkt.get(), aframe.get())) {
            return false;
          }
          audioSampleIndex += frameSize;
          return true;
        };
        // Stage s16 samples across decoder-chunk boundaries so every AAC
        // frame is full except the final one (silence-padded).
        std::vector<int16_t> staging;
        staging.reserve(static_cast<size_t>(frameSize) * kAudioChannels * 2);
        int encodedSamples = 0;
        bool audioEof = !clipHasAudio;
        QByteArray decoderChunk;
        int chunkPos = 0;
        const int16_t* silenceFrame = nullptr;
        std::vector<int16_t> silenceStorage;
        if (frameSize > 0) {
          // Sized from the encoder's real frame_size (AAC: 1024) instead of
          // a magic constant, so a future encoder with larger frames cannot
          // over-read the silence pad in encodeS16Frame()/staging below.
          silenceStorage.assign(static_cast<size_t>(frameSize) * kAudioChannels, 0);
          silenceFrame = silenceStorage.data();
        }
        while (encodedSamples < neededSamples) {
          if (isCancelRequested()) {
            break;
          }
          // Fill staging up to one full frame.
          while (static_cast<int>(staging.size()) < frameSize * kAudioChannels &&
                 encodedSamples + static_cast<int>(staging.size() / kAudioChannels) <
                     neededSamples) {
            if (chunkPos >= decoderChunk.size()) {
              if (audioEof) {
                break;
              }
              // Bounded by frameSize (small): int64 min, then narrow.
              const int wantSamples = static_cast<int>(std::min<int64_t>(
                  static_cast<int64_t>(frameSize) * 4, neededSamples - encodedSamples -
                                     static_cast<int>(staging.size() / kAudioChannels)));
              const int wantBytes = std::max(4096, wantSamples * bytesPerSampleFrame);
              decoderChunk = dec.readAudioChunk(std::min(wantBytes, 65536));
              chunkPos = 0;
              if (decoderChunk.isEmpty()) {
                audioEof = true;
                break;
              }
            }
            const int availSamples = (decoderChunk.size() - chunkPos) / bytesPerSampleFrame;
            const int roomSamples = frameSize * kAudioChannels - static_cast<int>(staging.size());
            const int roomFrames = roomSamples / kAudioChannels;
            const int remainingFrames =
                neededSamples - encodedSamples - static_cast<int>(staging.size() / kAudioChannels);
            const int takeFrames = std::min({availSamples, roomFrames, remainingFrames});
            if (takeFrames <= 0) {
              break;
            }
            const auto* src = reinterpret_cast<const int16_t*>(decoderChunk.constData() + chunkPos);
            const int stageBaseFrames =
                encodedSamples + static_cast<int>(staging.size() / kAudioChannels);
            for (int f = 0; f < takeFrames; ++f) {
              const double intraT = v1InT + static_cast<double>(stageBaseFrames + f) / kAudioRate;
              const double fg = ClipUtils::clipFadeGain(clip, intraT, v1Dur);
              const double g = v1Gain * fg;
              const int64_t absF = static_cast<int64_t>(stageBaseFrames + f);
              for (int ch = 0; ch < kAudioChannels; ++ch) {
                int v = static_cast<int>(std::llround(src[f * kAudioChannels + ch] * g));
                if (absF >= 0 && absF < neededSamples && !bedMix.empty()) {
                  v += bedMix[static_cast<size_t>(absF * kAudioChannels + ch)];
                }
                staging.push_back(static_cast<int16_t>(std::clamp(v, -32768, 32767)));
              }
            }
            chunkPos += takeFrames * bytesPerSampleFrame;
          }
          const int stagedFrames = static_cast<int>(staging.size() / kAudioChannels);
          if (stagedFrames >= frameSize ||
              (audioEof && encodedSamples + stagedFrames < neededSamples) ||
              encodedSamples + stagedFrames >= neededSamples) {
            const int n = std::min(frameSize, stagedFrames);
            if (n > 0) {
              if (!encodeS16Frame(staging.data(), n)) {
                success = false;
                break;
              }
              staging.erase(staging.begin(), staging.begin() + static_cast<QList<char>::difference_type>(n) * kAudioChannels);
              encodedSamples += n;
              continue;
            }
            // Silence pad to the next frame boundary / needed length.
            // Bed audio still mixes in when V1 is silent/muted.
            const int pad =
                static_cast<int>(std::min<int64_t>(frameSize, neededSamples - encodedSamples));
            if (pad <= 0) {
              break;
            }
            if (!bedMix.empty()) {
              std::vector<int16_t> bedFrame(static_cast<size_t>(pad) * kAudioChannels);
              for (int f = 0; f < pad; ++f) {
                const int64_t absF = static_cast<int64_t>(encodedSamples + f);
                for (int ch = 0; ch < kAudioChannels; ++ch) {
                  int v = 0;
                  if (absF >= 0 && absF < neededSamples) {
                    v = bedMix[static_cast<size_t>(absF * kAudioChannels + ch)];
                  }
                  bedFrame[static_cast<size_t>(f * kAudioChannels + ch)] = static_cast<int16_t>(v);
                }
              }
              if (!encodeS16Frame(bedFrame.data(), pad)) {
                success = false;
                break;
              }
            } else if (!encodeS16Frame(silenceFrame, pad)) {
              success = false;
              break;
            }
            encodedSamples += pad;
          } else if (audioEof) {
            // Decoder exhausted mid-frame: pad with silence (+ bed mix).
            const int pad = static_cast<int>(std::min<int64_t>(
                frameSize - stagedFrames, neededSamples - encodedSamples - stagedFrames));
            const int base = encodedSamples + stagedFrames;
            for (int f = 0; f < pad; ++f) {
              const int64_t absF = static_cast<int64_t>(base + f);
              for (int ch = 0; ch < kAudioChannels; ++ch) {
                int v = 0;
                if (absF >= 0 && absF < neededSamples && !bedMix.empty()) {
                  v = bedMix[static_cast<size_t>(absF * kAudioChannels + ch)];
                }
                staging.push_back(static_cast<int16_t>(v));
              }
            }
          }
        }
        if (!success || isCancelRequested()) {
          break;
        }
      }
    }
  }

  if (success && !isCancelRequested()) {
    success = sendFrame(videoEnc, videoStream, oc, pkt.get(), nullptr)  // flush video
              && (!audioEnc || sendFrame(audioEnc, audioStream, oc, pkt.get(), nullptr));
    if (!success) {
      fail(QStringLiteral("Export failed: encoder flush failed."));
    }
  }

  if (success && !isCancelRequested()) {
    success = av_write_trailer(oc) >= 0;
    if (!success) {
      fail(QStringLiteral("Export failed: could not finalize %1.").arg(outPath));
    }
  } else {
    success = false;
    fail(QStringLiteral("Export failed: encoding interrupted."));
  }

  if (success) {
    avio_flush(oc->pb);
    if (oc->pb->error < 0) {
      success = false;
      fail(QStringLiteral("Export failed: could not flush %1.").arg(outPath));
    }
  }
  sws_freeContext(sws);
  swr_free(&swr);
  cleanup();

  if (success) {
    QMutexLocker locker(&m_mutex);
    if (m_cancelRequested || m_abort) {
      success = false;
      fail(QStringLiteral("Export failed: encoding interrupted."));
    } else if (!outputFile.commit()) {
      success = false;
      fail(QStringLiteral("Export failed: could not replace %1: %2.")
               .arg(outPath, outputFile.errorString()));
    }
  }
  if (success) {
    emit progressChanged(totalSeconds, totalSeconds);
    if (warning && !skippedPaths.isEmpty()) {
      const QStringList skipped = skippedPaths.values();
      *warning = QStringLiteral("Export finished with %1 skipped clip(s): %2")
                     .arg(skipped.size())
                     .arg(skipped.join(QStringLiteral(", ")));
    }
  } else {
    // Discard the partial temp file explicitly instead of relying on the
    // QSaveFile destructor, so an early return added later cannot litter.
    outputFile.cancelWriting();
  }
  return success;
}
