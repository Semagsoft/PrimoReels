#include "MediaDecoder.h"

#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <QDebug>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

MediaDecoder::MediaDecoder() {}

MediaDecoder::~MediaDecoder() {
  close();
}

bool MediaDecoder::open(const QString& filePath) {
  close();

  const QByteArray pathBytes = filePath.toUtf8();
  AVFormatContext* formatContext = nullptr;
  if (avformat_open_input(&formatContext, pathBytes.constData(), nullptr, nullptr) < 0) {
    qWarning() << "Could not open file:" << filePath;
    return false;
  }
  m_formatContext.reset(formatContext);

  if (avformat_find_stream_info(m_formatContext.get(), nullptr) < 0) {
    qWarning() << "Could not find stream info";
    close();
    return false;
  }

  m_videoStreamIndex = findVideoStream();
  if (m_videoStreamIndex == -1) {
    // Audio-only file (mp3/wav/m4a/ogg): no video stream, but usable as an
    // audio clip. Set duration from the audio/container and continue.
    if (!setupAudio(filePath)) {
      qWarning() << "No video or audio stream found";
      close();
      return false;
    }
    m_duration = audioDurationFromContext();
    if (!std::isfinite(m_duration) || m_duration <= 0.0 || m_duration > 86400.0) {
      // Fall back to container duration when the audio stream reports none.
      m_duration = durationFromContext();
    }
    if (!std::isfinite(m_duration) || m_duration <= 0.0 || m_duration > 86400.0) {
      qWarning() << "Unsupported audio-only duration:" << m_duration;
      close();
      return false;
    }
    m_width = 0;
    m_height = 0;
    m_frame.reset(av_frame_alloc());
    m_packet.reset(av_packet_alloc());
    if (!m_frame || !m_packet) {
      qWarning() << "Could not allocate frame or packet buffers";
      close();
      return false;
    }
    return true;
  }

  AVCodecParameters* codecParameters = m_formatContext->streams[m_videoStreamIndex]->codecpar;
  const AVCodec* codec = avcodec_find_decoder(codecParameters->codec_id);
  if (!codec) {
    qWarning() << "Unsupported codec";
    close();
    return false;
  }

  m_codecContext = CodecContextPtr(avcodec_alloc_context3(codec));
  if (!m_codecContext) {
    qWarning() << "Could not allocate codec context";
    close();
    return false;
  }

  if (avcodec_parameters_to_context(m_codecContext.get(), codecParameters) < 0) {
    qWarning() << "Could not copy codec parameters";
    close();
    return false;
  }

  AVBufferRef* hwDeviceCtx = nullptr;
#ifdef PRIMOREELS_TSAN
  // Skip VAAPI under TSan: creating the VAAPI context spawns
  // radeonsi_drv_video threads that corrupt this platform's TSan thread
  // registry (CHECK failed in sanitizer_thread_registry). Software decode
  // gives the same coverage in tests.
  if (false) {
#else
  if (av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) >= 0) {
#endif
    m_codecContext->hw_device_ctx = hwDeviceCtx;
    m_codecContext->get_format =
        [](AVCodecContext * ctx, const enum AVPixelFormat* pix_fmts)->enum AVPixelFormat {
      for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VAAPI) {
          return *p;
        }
      }
      return avcodec_default_get_format(ctx, pix_fmts);
    };
  }

  if (avcodec_open2(m_codecContext.get(), codec, nullptr) < 0) {
    qWarning() << "Could not open codec";
    close();
    return false;
  }

  m_width = m_codecContext->width;
  m_height = m_codecContext->height;
  m_duration = durationFromContext();
  if (m_width > 0 && m_height > 0) {
    AVRational sar = m_codecContext->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) {
      sar = m_formatContext->streams[m_videoStreamIndex]->codecpar->sample_aspect_ratio;
    }
    m_displayAspectRatio = (sar.num > 0 && sar.den > 0)
                               ? static_cast<double>(m_width) * sar.num / (m_height * sar.den)
                               : static_cast<double>(m_width) / m_height;
  } else {
    m_displayAspectRatio = 16.0 / 9.0;  // audio-only placeholder
  }
  if (m_videoStreamIndex >= 0) {
    const AVStream* vstream = m_formatContext->streams[m_videoStreamIndex];
    if (vstream && vstream->start_time != AV_NOPTS_VALUE) {
      m_videoStartSeconds = av_q2d(vstream->time_base) * static_cast<double>(vstream->start_time);
    }
  }
  // Bound what the rest of the pipeline may assume: absurd dimensions
  // overflow bytesPerLine casts and frame buffers, and absurd durations
  // overflow timestamp rescaling (seconds * AV_TIME_BASE) and waveform
  // bucket math. Real media never approaches these; crafted files might.
  if (m_width <= 0 || m_height <= 0 || m_width > 8192 || m_height > 8192) {
    qWarning() << "Unsupported video dimensions:" << m_width << "x" << m_height;
    close();
    return false;
  }
  if (!std::isfinite(m_duration) || m_duration > 86400.0) {
    qWarning() << "Unsupported media duration:" << m_duration;
    close();
    return false;
  }

  m_frame = FramePtr(av_frame_alloc());
  m_packet = PacketPtr(av_packet_alloc());
  if (!m_frame || !m_packet) {
    qWarning() << "Could not allocate frame or packet buffers";
    close();
    return false;
  }

  if (!setupAudio(filePath)) {
    qWarning() << "No usable audio stream in file:" << filePath;
    m_audioStreamIndex = -1;
    m_audioPosition = 0.0;
  }

  return true;
}

void MediaDecoder::close() {
  // Unique_ptr deleters now own all FFmpeg teardown (close_input for
  // format contexts), so reset() is sufficient and exception-safe.
  m_swsContext.reset();
  m_frame.reset();
  m_packet.reset();
  m_codecContext.reset();
  m_formatContext.reset();
  m_swrContext.reset();
  m_audioFrame.reset();
  m_audioPacket.reset();
  m_audioCodecContext.reset();
  m_audioFormatContext.reset();

  m_videoStreamIndex = -1;
  m_duration = 0.0;
  m_width = 0;
  m_height = 0;
  m_displayAspectRatio = 16.0 / 9.0;
  m_videoStartSeconds = 0.0;
  m_audioStartSeconds = 0.0;
  m_lastSrcW = 0;
  m_lastSrcH = 0;
  m_lastSrcFormat = -1;
  m_audioStreamIndex = -1;
  m_audioPosition = 0.0;
}

QImage MediaDecoder::getFrameAt(double seconds, const std::function<bool()>& cancelCheck) {
  if (!m_formatContext || m_videoStreamIndex == -1 || !m_frame || !m_packet) {
    return {};
  }

  if (!std::isfinite(seconds)) {
    return {};
  }

  const double clampedSeconds = std::clamp(seconds, 0.0, m_duration > 0.0 ? m_duration : 0.0);
  const AVStream* stream = m_formatContext->streams[m_videoStreamIndex];
  if (!stream) {
    return {};
  }

  const int64_t timestamp = av_rescale_q(
      static_cast<int64_t>(std::llround((clampedSeconds + m_videoStartSeconds) * AV_TIME_BASE)),
      AVRational{1, AV_TIME_BASE}, stream->time_base);

  const int seekResult =
      av_seek_frame(m_formatContext.get(), m_videoStreamIndex, timestamp, AVSEEK_FLAG_BACKWARD);
  if (seekResult < 0) {
    return {};
  }

  avcodec_flush_buffers(m_codecContext.get());

  // Bounded forward scan: corrupt timestamps must not spin through the
  // whole file on the playback thread.
  constexpr int kMaxScanPackets = 1200;
  for (int scanned = 0; scanned < kMaxScanPackets; ++scanned) {
    if (cancelCheck && cancelCheck()) {
      return {};
    }
    const int readResult = av_read_frame(m_formatContext.get(), m_packet.get());
    if (readResult < 0) {
      break;
    }

    if (m_packet->stream_index != m_videoStreamIndex) {
      av_packet_unref(m_packet.get());
      continue;
    }

    int ret = avcodec_send_packet(m_codecContext.get(), m_packet.get());
    av_packet_unref(m_packet.get());

    if (ret < 0 && ret != AVERROR(EAGAIN)) {
      break;
    }

    for (;;) {
      ret = avcodec_receive_frame(m_codecContext.get(), m_frame.get());
      if (ret == 0) {
        const double frameTime = frameTimestampSeconds(m_frame.get());
        // NaN = unorderable (stills/NOPTS): accept the first decodable
        // frame after the seek rather than spinning to the scan cap.
        if (std::isnan(frameTime) || frameTime >= clampedSeconds - 0.001) {
          const QImage image = convertFrame(m_frame.get());
          av_frame_unref(m_frame.get());
          return image;
        }
        av_frame_unref(m_frame.get());
        continue;
      }
      break;
    }
  }

  return {};
}

bool MediaDecoder::seekVideoTo(double seconds) {
  if (!m_formatContext || m_videoStreamIndex == -1 || !m_frame || !m_packet) {
    return false;
  }
  if (!std::isfinite(seconds)) {
    return false;
  }
  const double clampedSeconds = std::clamp(seconds, 0.0, m_duration > 0.0 ? m_duration : 0.0);
  const AVStream* stream = m_formatContext->streams[m_videoStreamIndex];
  if (!stream) {
    return false;
  }
  const int64_t timestamp = av_rescale_q(
      static_cast<int64_t>(std::llround((clampedSeconds + m_videoStartSeconds) * AV_TIME_BASE)),
      AVRational{1, AV_TIME_BASE}, stream->time_base);
  if (av_seek_frame(m_formatContext.get(), m_videoStreamIndex, timestamp, AVSEEK_FLAG_BACKWARD) <
      0) {
    return false;
  }
  avcodec_flush_buffers(m_codecContext.get());
  return true;
}

QImage MediaDecoder::readNextVideoFrame(double* outSeconds) {
  if (!m_formatContext || m_videoStreamIndex == -1 || !m_frame || !m_packet) {
    return {};
  }
  // Bounded like getFrameAt: corrupt streams must not spin the export or
  // preload thread through the whole file.
  constexpr int kMaxScanPackets = 1200;
  for (int scanned = 0; scanned < kMaxScanPackets; ++scanned) {
    const int readResult = av_read_frame(m_formatContext.get(), m_packet.get());
    if (readResult < 0) {
      return {};  // EOF or read error: end of forward iteration.
    }
    if (m_packet->stream_index != m_videoStreamIndex) {
      av_packet_unref(m_packet.get());
      continue;
    }
    int ret = avcodec_send_packet(m_codecContext.get(), m_packet.get());
    av_packet_unref(m_packet.get());
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
      return {};
    }
    for (;;) {
      ret = avcodec_receive_frame(m_codecContext.get(), m_frame.get());
      if (ret == 0) {
        const double frameTime = frameTimestampSeconds(m_frame.get());
        const QImage image = convertFrame(m_frame.get());
        av_frame_unref(m_frame.get());
        if (outSeconds) {
          *outSeconds = frameTime;
        }
        if (image.isNull()) {
          continue;
        }
        return image;
      }
      break;
    }
  }
  return {};
}

int MediaDecoder::findVideoStream() const {
  if (!m_formatContext) {
    return -1;
  }

  for (unsigned int i = 0; i < m_formatContext->nb_streams; ++i) {
    const AVStream* stream = m_formatContext->streams[i];
    if (stream && stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      return static_cast<int>(i);
    }
  }

  return -1;
}

double MediaDecoder::durationFromContext() const {
  if (!m_formatContext) {
    return 0.0;
  }

  if (m_videoStreamIndex >= 0) {
    const AVStream* stream = m_formatContext->streams[m_videoStreamIndex];
    if (stream && stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
      return av_q2d(stream->time_base) * stream->duration;
    }
  }

  if (m_formatContext->duration != AV_NOPTS_VALUE && m_formatContext->duration > 0) {
    return av_q2d(AVRational{1, AV_TIME_BASE}) * m_formatContext->duration;
  }

  return 0.0;
}

double MediaDecoder::audioDurationFromContext() const {
  if (m_audioFormatContext) {
    if (m_audioStreamIndex >= 0) {
      const AVStream* stream = m_audioFormatContext->streams[m_audioStreamIndex];
      if (stream && stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        const double d = av_q2d(stream->time_base) * stream->duration;
        if (std::isfinite(d) && d > 0.0) {
          return d;
        }
      }
    }
    if (m_audioFormatContext->duration != AV_NOPTS_VALUE && m_audioFormatContext->duration > 0) {
      return av_q2d(AVRational{1, AV_TIME_BASE}) * m_audioFormatContext->duration;
    }
  }
  return 0.0;
}

double MediaDecoder::frameTimestampSeconds(const AVFrame* frame) const {
  if (!frame) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  int64_t timestamp = frame->best_effort_timestamp;
  if (timestamp == AV_NOPTS_VALUE) {
    timestamp = frame->pts;
  }

  // Never synthesize 0.0 for unknown timestamps: that mis-orders stills and
  // B-frames to the file start. Callers treat NaN as "unorderable".
  if (timestamp == AV_NOPTS_VALUE) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  const AVStream* stream = m_formatContext ? m_formatContext->streams[m_videoStreamIndex] : nullptr;
  if (!stream) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  return av_q2d(stream->time_base) * timestamp - m_videoStartSeconds;
}

QImage MediaDecoder::convertFrame(const AVFrame* frame) const {
  if (!frame || frame->width <= 0 || frame->height <= 0) {
    return {};
  }

  const AVFrame* srcFrame = frame;
  FramePtr swFrame;
  if (frame->format == AV_PIX_FMT_VAAPI) {
    swFrame = FramePtr(av_frame_alloc());
    if (!swFrame || av_hwframe_transfer_data(swFrame.get(), frame, 0) < 0) {
      return {};
    }
    srcFrame = swFrame.get();
  }

  const int srcW = srcFrame->width;
  const int srcH = srcFrame->height;
  const int srcFormat = srcFrame->format;

  if (!m_swsContext || m_lastSrcW != srcW || m_lastSrcH != srcH || m_lastSrcFormat != srcFormat) {
    m_swsContext.reset();

    // AV_PIX_FMT_RGB32 is host-endian 0xAARRGGBB, which matches QRgb /
    // QImage::Format_RGB32 on every platform: both sides interpret the word
    // in CPU order, so this conversion is endian-portable (unlike the
    // export path, which aliases the bytes as BGRA — see ExportThread).
    m_swsContext =
        SwsContextPtr(sws_getContext(srcW, srcH, static_cast<AVPixelFormat>(srcFormat), srcW, srcH,
                                     AV_PIX_FMT_RGB32, SWS_BILINEAR, nullptr, nullptr, nullptr));

    if (m_swsContext) {
      m_lastSrcW = srcW;
      m_lastSrcH = srcH;
      m_lastSrcFormat = srcFormat;
    }

    if (!m_swsContext) {
      return {};
    }
  }

  QImage image(srcW, srcH, QImage::Format_RGB32);
  if (image.isNull()) {
    return {};
  }

  uint8_t* destData[4] = {image.bits(), nullptr, nullptr, nullptr};
  int destLinesize[4] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};

  const int converted = sws_scale(m_swsContext.get(), srcFrame->data, srcFrame->linesize, 0, srcH,
                                  destData, destLinesize);

  if (converted <= 0) {
    return {};
  }

  return image;
}

bool MediaDecoder::setupAudio(const QString& filePath) {
  const QByteArray pathBytes = filePath.toUtf8();
  AVFormatContext* formatContext = nullptr;
  if (avformat_open_input(&formatContext, pathBytes.constData(), nullptr, nullptr) < 0) {
    return false;
  }
  FormatContextPtr audioFormat(formatContext);

  if (avformat_find_stream_info(audioFormat.get(), nullptr) < 0) {
    return false;
  }

  const int streamIndex =
      av_find_best_stream(audioFormat.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (streamIndex < 0) {
    return false;
  }

  AVCodecParameters* codecParameters = audioFormat->streams[streamIndex]->codecpar;
  const AVCodec* codec = avcodec_find_decoder(codecParameters->codec_id);
  if (!codec) {
    return false;
  }

  CodecContextPtr codecContext(avcodec_alloc_context3(codec));
  if (!codecContext) {
    return false;
  }

  if (avcodec_parameters_to_context(codecContext.get(), codecParameters) < 0) {
    return false;
  }

  if (avcodec_open2(codecContext.get(), codec, nullptr) < 0) {
    return false;
  }

  FramePtr audioFrame(av_frame_alloc());
  PacketPtr audioPacket(av_packet_alloc());
  if (!audioFrame || !audioPacket) {
    return false;
  }

  SwrContext* swrContext = nullptr;
  AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
  // Deep-copy the codec layout: a struct copy would share `opaque` with the
  // codec context for custom layouts, making later uninit version-dependent
  // (freeing shared state vs. leaking). The copy is fully owned here, so it
  // can always be replaced/uninit-ed safely.
  AVChannelLayout inLayout{};
  if (av_channel_layout_copy(&inLayout, &codecContext->ch_layout) < 0) {
    return false;
  }
  // Some decoders leave ch_layout unset; fall back to a default layout so
  // valid audio is not silently dropped.
  if (inLayout.nb_channels == 0) {
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_default(&inLayout, 2);
  }
  const int swrRet =
      swr_alloc_set_opts2(&swrContext, &outLayout, AV_SAMPLE_FMT_S16, audioSampleRate(), &inLayout,
                          codecContext->sample_fmt, codecContext->sample_rate, 0, nullptr);
  // swr_alloc_set_opts2 copies the layouts; release ours on all paths.
  av_channel_layout_uninit(&inLayout);
  // Adopt immediately: on error the context is freed and *ps nulled by
  // swr_alloc_set_opts2, and SwrContextPtr is null-safe — either way the
  // context cannot leak regardless of FFmpeg version behavior.
  SwrContextPtr swr(swrContext);
  if (swrRet < 0) {
    return false;
  }

  if (swr_init(swr.get()) < 0) {
    return false;
  }

  m_audioFormatContext = std::move(audioFormat);
  m_audioCodecContext = std::move(codecContext);
  m_audioFrame = std::move(audioFrame);
  m_audioPacket = std::move(audioPacket);
  m_swrContext = std::move(swr);
  m_audioStreamIndex = streamIndex;
  m_audioPosition = 0.0;
  if (streamIndex >= 0 && m_audioFormatContext) {
    const AVStream* astream = m_audioFormatContext->streams[streamIndex];
    if (astream && astream->start_time != AV_NOPTS_VALUE) {
      m_audioStartSeconds = av_q2d(astream->time_base) * static_cast<double>(astream->start_time);
    }
  }
  return true;
}

bool MediaDecoder::seekAudioTo(double seconds) {
  if (!hasAudio() || !std::isfinite(seconds)) {
    return false;
  }

  const double target = std::clamp(seconds, 0.0, m_duration > 0.0 ? m_duration : 0.0);
  const AVStream* stream = m_audioFormatContext->streams[m_audioStreamIndex];
  if (!stream) {
    return false;
  }

  const int64_t timestamp = av_rescale_q(
      static_cast<int64_t>(std::llround((target + m_audioStartSeconds) * AV_TIME_BASE)),
      AVRational{1, AV_TIME_BASE}, stream->time_base);

  if (av_seek_frame(m_audioFormatContext.get(), m_audioStreamIndex, timestamp,
                    AVSEEK_FLAG_BACKWARD) < 0) {
    return false;
  }
  avcodec_flush_buffers(m_audioCodecContext.get());

  const double timeBase = av_q2d(stream->time_base);
  const double sampleRate = m_audioCodecContext->sample_rate > 0
                                ? static_cast<double>(m_audioCodecContext->sample_rate)
                                : static_cast<double>(audioSampleRate());

  // Track catch-up across the whole skip loop: breaking out early (EOF,
  // corrupt packets) without reaching the target must report failure.
  // Claiming m_audioPosition = target in that case desyncs A/V after
  // corrupt seeks (callers would mix audio from the wrong offset).
  bool caughtUp = (target <= 0.0);
  for (int skipped = 0; skipped < 6000 && !caughtUp; ++skipped) {
    if (av_read_frame(m_audioFormatContext.get(), m_audioPacket.get()) < 0) {
      break;
    }
    if (m_audioPacket->stream_index != m_audioStreamIndex) {
      av_packet_unref(m_audioPacket.get());
      continue;
    }
    if (avcodec_send_packet(m_audioCodecContext.get(), m_audioPacket.get()) < 0) {
      av_packet_unref(m_audioPacket.get());
      break;
    }
    av_packet_unref(m_audioPacket.get());

    for (;;) {
      if (avcodec_receive_frame(m_audioCodecContext.get(), m_audioFrame.get()) < 0) {
        break;
      }
      int64_t pts = m_audioFrame->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE) {
        pts = m_audioFrame->pts;
      }
      const double frameEnd = timeBase * static_cast<double>(pts) +
                              static_cast<double>(m_audioFrame->nb_samples) / sampleRate -
                              m_audioStartSeconds;
      av_frame_unref(m_audioFrame.get());
      if (frameEnd > target) {
        caughtUp = true;
        break;
      }
    }
  }

  if (!caughtUp) {
    return false;
  }
  m_audioPosition = target;
  return true;
}

QByteArray MediaDecoder::readAudioChunk(int maxBytes) {
  QByteArray out;
  if (!hasAudio() || maxBytes <= 0) {
    return out;
  }
  out.reserve(maxBytes);

  for (int iterations = 0; iterations < 20000 && out.size() < maxBytes; ++iterations) {
    if (av_read_frame(m_audioFormatContext.get(), m_audioPacket.get()) < 0) {
      break;
    }
    if (m_audioPacket->stream_index != m_audioStreamIndex) {
      av_packet_unref(m_audioPacket.get());
      continue;
    }

    int ret = avcodec_send_packet(m_audioCodecContext.get(), m_audioPacket.get());
    if (ret == AVERROR(EAGAIN)) {
      for (;;) {
        if (avcodec_receive_frame(m_audioCodecContext.get(), m_audioFrame.get()) < 0) {
          break;
        }
        out.append(resampleAudioFrame(m_audioFrame.get()));
        av_frame_unref(m_audioFrame.get());
        if (out.size() >= maxBytes) {
          break;
        }
      }
      ret = avcodec_send_packet(m_audioCodecContext.get(), m_audioPacket.get());
      if (ret == AVERROR(EAGAIN)) {
        av_packet_unref(m_audioPacket.get());
        break;
      }
    }
    av_packet_unref(m_audioPacket.get());
    if (ret < 0) {
      break;
    }

    for (;;) {
      if (avcodec_receive_frame(m_audioCodecContext.get(), m_audioFrame.get()) < 0) {
        break;
      }
      out.append(resampleAudioFrame(m_audioFrame.get()));
      av_frame_unref(m_audioFrame.get());
      if (out.size() >= maxBytes) {
        break;
      }
    }
  }

  m_audioPosition += static_cast<double>(out.size()) / audioBytesPerSecond();
  return out;
}

QByteArray MediaDecoder::resampleAudioFrame(const AVFrame* frame) {
  if (!frame || !m_swrContext || frame->nb_samples <= 0) {
    return {};
  }
  // Corrupt headers can report sample_rate 0, which would divide by zero
  // inside av_rescale_rnd (the seek path already falls back; this one must).
  if (m_audioCodecContext->sample_rate <= 0) {
    return {};
  }

  const int64_t delay = swr_get_delay(m_swrContext.get(), m_audioCodecContext->sample_rate);
  const int outSamples = static_cast<int>(av_rescale_rnd(
      delay + frame->nb_samples, audioSampleRate(), m_audioCodecContext->sample_rate, AV_ROUND_UP));

  uint8_t** dstData = nullptr;
  int dstLinesize = 0;
  if (av_samples_alloc_array_and_samples(&dstData, &dstLinesize, audioChannelCount(), outSamples,
                                         AV_SAMPLE_FMT_S16, 0) < 0) {
    return {};
  }

  QByteArray out;
  const int converted =
      swr_convert(m_swrContext.get(), dstData, outSamples, frame->data, frame->nb_samples);
  if (converted > 0) {
    out = QByteArray(reinterpret_cast<const char*>(dstData[0]),
                     static_cast<qsizetype>(converted) * audioChannelCount() *
                         static_cast<int>(sizeof(int16_t)));
  }

  if (dstData) {
    av_freep(reinterpret_cast<void*>(&dstData[0]));
    av_freep(reinterpret_cast<void*>(&dstData));
  }
  return out;
}