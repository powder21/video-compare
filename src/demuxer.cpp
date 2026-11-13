#include "demuxer.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include "ffmpeg.h"
#include "string_utils.h"

static bool is_http_url(const std::string& file_name) {
  std::string lower = file_name.substr(0, 8);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

  return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

// Servers drop connections the player stops reading from (e.g. while paused), so
// resume such reads from the last byte instead of feeding a truncated packet to
// the decoder. Options given by the user take precedence.
static void set_default_http_options(AVDictionary** options) {
  av_dict_set(options, "reconnect", "1", AV_DICT_DONT_OVERWRITE);
  av_dict_set(options, "reconnect_on_network_error", "1", AV_DICT_DONT_OVERWRITE);
  av_dict_set(options, "reconnect_delay_max", "10", AV_DICT_DONT_OVERWRITE);
  av_dict_set(options, "rw_timeout", "15000000", AV_DICT_DONT_OVERWRITE);
}

Demuxer::Demuxer(const Side& side, const std::string& demuxer_name, const std::string& file_name, AVDictionary* demuxer_options, const AVDictionary* decoder_options) : SideAware(side) {
  ScopedLogSide scoped_log_side(side);

  const AVInputFormat* input_format = nullptr;

  if (!demuxer_name.empty()) {
    input_format = av_find_input_format(demuxer_name.c_str());

    if (input_format == nullptr) {
      throw std::runtime_error(file_name + ": Demuxer '" + demuxer_name + "' not found");
    }
  }

  AVDictionary* open_options = nullptr;
  av_dict_copy(&open_options, demuxer_options, 0);

  if (is_http_url(file_name)) {
    set_default_http_options(&open_options);
  }

  const int open_result = avformat_open_input(&format_context_, file_name.c_str(), const_cast<AVInputFormat*>(input_format), &open_options);

  if (open_result < 0) {
    av_dict_free(&open_options);
    ffmpeg::check(file_name, open_result);
  }

  try {
    ffmpeg::check_dict_is_empty(open_options, string_sprintf("Demuxer %s", format_name().c_str()));
  } catch (...) {
    av_dict_free(&open_options);
    avformat_close_input(&format_context_);
    throw;
  }
  av_dict_free(&open_options);

  // Try to find best stream first
  video_stream_index_ = av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

  const unsigned int nb_streams_before = format_context_->nb_streams;

  AVDictionary** opts_for_streams = (AVDictionary**)av_calloc(nb_streams_before, sizeof(AVDictionary*));

  // User-specified options are only used if we have a valid video stream index
  if (video_stream_index_ >= 0) {
    av_dict_copy(&opts_for_streams[video_stream_index_], decoder_options, 0);
  }

  // Achtung: avformat_find_stream_info() may modify the copied options
  ffmpeg::check(file_name, avformat_find_stream_info(format_context_, opts_for_streams));

  if (format_context_->nb_streams == 0) {
    throw std::runtime_error(file_name + ": No streams found in container");
  }

  if (video_stream_index_ < 0) {
    // Try manual search for video stream
    for (unsigned int i = 0; i < format_context_->nb_streams; i++) {
      const AVStream* stream = format_context_->streams[i];

      if (stream != nullptr && stream->codecpar != nullptr && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        video_stream_index_ = i;
        break;
      }
    }

    if (video_stream_index_ < 0) {
      throw std::runtime_error(file_name + ": No video stream found");
    }
  }

  for (unsigned int i = 0; i < nb_streams_before; i++) {
    av_dict_free(&opts_for_streams[i]);
  }

  av_freep(&opts_for_streams);
}

Demuxer::~Demuxer() {
  avformat_close_input(&format_context_);
}

AVCodecParameters* Demuxer::video_codec_parameters() {
  return format_context_->streams[video_stream_index_]->codecpar;
}

int Demuxer::video_stream_index() const {
  return video_stream_index_;
}

AVRational Demuxer::time_base() const {
  return format_context_->streams[video_stream_index_]->time_base;
}

int64_t Demuxer::duration() const {
  // use stream duration if available, otherwise use container duration if available, else 0
  const int64_t stream_duration = format_context_->streams[video_stream_index_]->duration;

  return stream_duration != AV_NOPTS_VALUE ? av_rescale_q(stream_duration, time_base(), AV_R_MICROSECONDS) : (format_context_->duration != AV_NOPTS_VALUE ? format_context_->duration : 0);
}

int64_t Demuxer::container_start_time() const {
  return format_context_->start_time != AV_NOPTS_VALUE ? format_context_->start_time : 0;
}

int64_t Demuxer::video_start_time() const {
  const AVStream* video_stream = format_context_->streams[video_stream_index_];

  if (video_stream->start_time != AV_NOPTS_VALUE) {
    return av_rescale_q(video_stream->start_time, video_stream->time_base, AV_R_MICROSECONDS);
  }

  return container_start_time();
}

int Demuxer::rotation() const {
  double theta = 0;
  AVStream* stream = format_context_->streams[video_stream_index_];

#if LIBAVFORMAT_VERSION_MAJOR >= 62
  const AVPacketSideData* side_data = av_packet_side_data_get(stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);

  // require 9 integers (3x3 matrix) + allow hypothetical padding
  if (side_data != nullptr && side_data->data != nullptr && side_data->size >= int(9 * sizeof(int32_t))) {
    theta = -av_display_rotation_get(reinterpret_cast<const int32_t*>(side_data->data));
  }
#else
  uint8_t* displaymatrix = av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, nullptr);

  if (displaymatrix != nullptr) {
    theta = -av_display_rotation_get(reinterpret_cast<int32_t*>(displaymatrix));
  }
#endif

  theta -= 360 * floor(theta / 360 + 0.9 / 360);

  return theta;
}

AVRational Demuxer::guess_frame_rate(AVFrame* frame) const {
  return av_guess_frame_rate(format_context_, format_context_->streams[video_stream_index_], frame);
}

AVRational Demuxer::sample_aspect_ratio(AVFrame* frame) const {
  AVStream* stream = format_context_->streams[video_stream_index_];

  if (stream == nullptr) {
    return AVRational{0, 1};
  }

  return av_guess_sample_aspect_ratio(format_context_, stream, frame);
}

bool Demuxer::operator()(AVPacket& packet) {
  const int ret = av_read_frame(format_context_, &packet);

  if (ret >= 0) {
    if (packet.stream_index == video_stream_index_) {
      const int64_t ts = packet.pts != AV_NOPTS_VALUE ? packet.pts : packet.dts;

      if (ts != AV_NOPTS_VALUE) {
        last_read_position_ = av_rescale_q(ts, time_base(), AV_R_MICROSECONDS);
      }
    }
    return true;
  }

  // Demuxers report a failed read (e.g. a dropped connection) as EOF, but leave the error on the I/O context
  const int io_error = format_context_->pb != nullptr ? format_context_->pb->error : 0;
  const int error = ret != AVERROR_EOF ? ret : (io_error < 0 && io_error != AVERROR_EOF ? io_error : 0);

  if (error < 0) {
    const std::string position = last_read_position_ != AV_NOPTS_VALUE ? format_position(last_read_position_ * AV_TIME_TO_SEC, false) : "the start";
    log_warning(string_sprintf("Reading stopped after %s (%s); the rest of the video is unavailable", position.c_str(), ffmpeg::error_string(error).c_str()));
  }

  return false;
}

bool Demuxer::seek(const float position, const bool backward) {
  ScopedLogSide scoped_log_side(get_side());

  int64_t seek_target = static_cast<int64_t>(position * AV_TIME_BASE);

  return av_seek_frame(format_context_, -1, seek_target, backward ? AVSEEK_FLAG_BACKWARD : 0) >= 0;
}

std::string Demuxer::format_name() {
  return format_context_->iformat->name;
}

int64_t Demuxer::file_size() {
  return avio_size(format_context_->pb);
}

int64_t Demuxer::bit_rate() {
  // use stream bit rate if available, otherwise use container bit rate
  const int64_t stream_bit_rate = format_context_->streams[video_stream_index_]->codecpar->bit_rate;

  return stream_bit_rate > 0 ? stream_bit_rate : format_context_->bit_rate;
}