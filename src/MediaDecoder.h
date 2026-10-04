#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <QByteArray>
#include <QImage>
#include <QString>
#include <functional>
#include <memory>

// Plain value type (intentionally NOT a QObject): instances are
// thread-confined and transferred across threads as unique_ptr
// (preload -> playback, see PreloadThread/DecoderThread). Inheriting
// QObject would give each instance thread affinity and make
// moveToThread() from the consuming thread a no-op/warning.
class MediaDecoder {
 public:
  explicit MediaDecoder();
  ~MediaDecoder();

  bool open(const QString& filePath);
  void close();

  QImage getFrameAt(double seconds, const std::function<bool()>& cancelCheck = {});
  // Sequential video iteration for export: seek once, then read forward.
  bool seekVideoTo(double seconds);
  QImage readNextVideoFrame(double* outSeconds = nullptr);
  double duration() const { return m_duration; }
  int width() const { return m_width; }
  int height() const { return m_height; }
  // Display aspect ratio (width/height including sample aspect ratio).
  // Anamorphic/phone footage has non-square pixels; monitors letterbox
  // using this instead of the raw storage aspect.
  double displayAspectRatio() const { return m_displayAspectRatio; }

  static constexpr int audioSampleRate() { return 48000; }
  static constexpr int audioChannelCount() { return 2; }
  static double audioBytesPerSecond() { return 48000.0 * 2 * sizeof(int16_t); }

  bool hasAudio() const { return m_audioStreamIndex != -1; }
  bool hasVideo() const { return m_videoStreamIndex != -1; }
  bool isAudioOnly() const { return m_videoStreamIndex == -1 && m_audioStreamIndex != -1; }
  double audioPosition() const { return m_audioPosition; }
  bool seekAudioTo(double seconds);
  QByteArray readAudioChunk(int maxBytes);

 private:
  struct AVFormatContextDeleter {
    void operator()(AVFormatContext* context) const noexcept {
      if (context) {
        AVFormatContext* c = context;
        avformat_close_input(&c);
      }
    }
  };
  struct AVCodecContextDeleter {
    void operator()(AVCodecContext* context) const noexcept { avcodec_free_context(&context); }
  };
  struct AVFrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
  };
  struct AVPacketDeleter {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
  };
  struct SwsContextDeleter {
    void operator()(SwsContext* context) const noexcept { sws_freeContext(context); }
  };
  struct SwrContextDeleter {
    void operator()(SwrContext* context) const noexcept { swr_free(&context); }
  };

  using FormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
  using CodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
  using FramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;
  using PacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
  using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
  using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

  int findVideoStream() const;
  double durationFromContext() const;
  double audioDurationFromContext() const;
  double frameTimestampSeconds(const AVFrame* frame) const;
  QImage convertFrame(const AVFrame* frame) const;
  bool setupAudio(const QString& filePath);
  QByteArray resampleAudioFrame(const AVFrame* frame);

  FormatContextPtr m_formatContext;
  CodecContextPtr m_codecContext;
  FramePtr m_frame;
  PacketPtr m_packet;
  mutable SwsContextPtr m_swsContext;
  mutable int m_lastSrcW = 0;
  mutable int m_lastSrcH = 0;
  mutable int m_lastSrcFormat = -1;

  int m_videoStreamIndex = -1;
  double m_duration = 0.0;
  int m_width = 0;
  int m_height = 0;
  double m_displayAspectRatio = 16.0 / 9.0;

  FormatContextPtr m_audioFormatContext;
  CodecContextPtr m_audioCodecContext;
  FramePtr m_audioFrame;
  PacketPtr m_audioPacket;
  SwrContextPtr m_swrContext;

  int m_audioStreamIndex = -1;
  double m_audioPosition = 0.0;
};
