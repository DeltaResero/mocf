// src/audio/decoders/mlp/mlp.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2026 DeltaResero <deltaresero@zoho.com>
//
// MLP (Meridian Lossless Packing) and TrueHD playback. The decode itself lives
// in mlpdec.cpp, ported from FFmpeg (LGPLv2.1+); this file is the container
// side: it walks the access unit chain, indexes the random-access points and
// feeds the decoder.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "config.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define DEBUG

#include "core/common.h"
#include "core/log.h"
#include "audio/decoder.h"
#include "io/io.h"
#include "audio/audio.h"
#include "library/files.h"

#include "mlpdec.h"

namespace
{

/// Raw MLP/TrueHD carries no timestamps and no index; both are recovered by
/// walking the access unit chain at open. Storing every access unit would be
/// hopeless -- a 48kHz stream has 1200 of them per second, so an hour would
/// cost tens of megabytes of offsets -- so only one seek point per this many
/// seconds is kept. Seeking then decodes forward from the preceding point,
/// which is bounded by this interval.
constexpr int SEEK_INDEX_INTERVAL_SEC = 1;

/// Upper bound on a single access unit: the length field is 12 bits counting
/// 16-bit words.
constexpr int MAX_ACCESS_UNIT_BYTES = 0xfff * 2;

struct SeekPoint
{
  int64_t sample; ///< first sample of the access unit at @ref offset
  int64_t offset; ///< byte offset of an access unit carrying a major sync
};

struct mlp_data
{
  struct decoder_error error;
  std::unique_ptr<struct io_stream, io_stream_deleter> io_stream;

  unique_mlp_core core;

  bool ok = false;
  bool eof = false;
  int is_truehd = 0;

  int channels = 0;
  int rate = 0;
  long fmt = 0;
  int bytes_per_frame = 0; ///< bytes for one sample across all channels

  int duration = 0;
  int64_t total_samples = 0;
  int64_t file_size = 0;
  int64_t first_au_pos = 0;
  int avg_bitrate = 0;
  int bitrate = 0;

  std::vector<SeekPoint> seek_points;

  /// Access unit staging buffer, reused for the life of the stream.
  std::vector<uint8_t> au;

  /// Samples decoded from the last access unit, and how many have been handed
  /// out so far.
  const void *pcm = nullptr;
  int pcm_samples = 0;
  int pcm_pos = 0;
  /// Samples still to be dropped after a seek landed before the target.
  int64_t seek_skip = 0;
};

uint16_t rb16(const uint8_t *b)
{
  return static_cast<uint16_t>((b[0] << 8) | b[1]);
}

/// Reads the next access unit into @p data->au.
/// @return its length in bytes, 0 at clean EOF, -1 on a short/invalid read.
int read_access_unit(struct mlp_data *data)
{
  uint8_t hdr[4];
  const ssize_t n = io_read(data->io_stream.get(), hdr, sizeof(hdr));
  if (n == 0) return 0;
  if (n != static_cast<ssize_t>(sizeof(hdr))) return -1;

  const int len = (rb16(hdr) & 0xfff) * 2;
  if (len < 4 || len > MAX_ACCESS_UNIT_BYTES) return -1;

  data->au.resize(static_cast<size_t>(len));
  std::memcpy(data->au.data(), hdr, sizeof(hdr));

  const int rest = len - static_cast<int>(sizeof(hdr));
  if (rest > 0)
  {
    if (io_read(data->io_stream.get(), data->au.data() + sizeof(hdr),
                static_cast<size_t>(rest)) != rest)
    {
      return -1;
    }
  }
  return len;
}

/// Walks the whole chain once to recover what the container does not carry:
/// the duration, and the offsets of the major syncs that decoding can start
/// from. Every access unit holds exactly access_unit_size samples, so the
/// sample count is exact rather than estimated from the bitrate -- which
/// matters here because the format is losslessly variable-bitrate.
bool build_index(struct mlp_data *data, int access_unit_size)
{
  data->seek_points.clear();

  if (io_seek(data->io_stream.get(), data->first_au_pos, SEEK_SET) == -1) return false;

  int64_t sample = 0;
  int64_t offset = data->first_au_pos;
  int64_t next_index_sample = 0;
  const int64_t index_stride =
      static_cast<int64_t>(SEEK_INDEX_INTERVAL_SEC) * data->rate;

  // Read straight through rather than seeking from one access unit to the
  // next. Every access unit has to be looked at either way, so a sequential
  // pass costs the same reads without asking the buffered reader thread to
  // reposition thousands of times.
  for (;;)
  {
    const int len = read_access_unit(data);
    if (len <= 0) break;
    if (offset + len > data->file_size) break;

    // Only major sync units reset all decoder state, so they are the only
    // points a seek may land on.
    const uint32_t sync = (static_cast<uint32_t>(data->au[4]) << 24) |
                          (static_cast<uint32_t>(data->au[5]) << 16) |
                          (static_cast<uint32_t>(data->au[6]) << 8) |
                          static_cast<uint32_t>(data->au[7]);
    const bool major_sync = (sync == 0xf8726fba || sync == 0xf8726fbb);

    if (major_sync && sample >= next_index_sample)
    {
      data->seek_points.push_back({sample, offset});
      next_index_sample = sample + index_stride;
    }

    sample += access_unit_size;
    offset += len;
  }

  data->total_samples = sample;
  data->duration = data->rate > 0
                       ? static_cast<int>(sample / data->rate)
                       : 0;

  return !data->seek_points.empty();
}

/// Opens the stream far enough to learn its parameters: probe the first major
/// sync for the format, then decode access units until the first one produces
/// output, which is when the restart header has been read and the channel
/// count, rate and sample width are known.
bool mlp_read_header(struct mlp_data *data)
{
  data->file_size = io_file_size(data->io_stream.get());

  uint8_t probe[8];
  if (io_read(data->io_stream.get(), probe, sizeof(probe)) !=
      static_cast<ssize_t>(sizeof(probe)))
  {
    return false;
  }

  const int is_thd = mlp_probe_is_truehd(probe, sizeof(probe));
  if (is_thd < 0) return false;
  data->is_truehd = is_thd;
  data->first_au_pos = 0;

  if (io_seek(data->io_stream.get(), data->first_au_pos, SEEK_SET) == -1) return false;

  // Ask for a stereo downmix. TrueHD carries its 2-channel presentation in
  // substream 0 and layers 5.1 and 7.1 on top in later substreams, so this
  // both yields the mix the stream was mastered with and skips decoding the
  // surround substreams entirely. Streams whose first substream is already
  // multi-channel are unaffected and decode as they are.
  data->core = mlp_core_create(data->is_truehd, MLP_LAYOUT_STEREO);
  if (!data->core) return false;

  for (int tries = 0; tries < 64; tries++)
  {
    const int len = read_access_unit(data);
    if (len <= 0) return false;

    const void *out = nullptr;
    int nb = 0;
    mlp_core_decode(data->core.get(), data->au.data(), len, &out, &nb);

    if (mlp_core_params_valid(data->core.get()) && out && nb > 0)
    {
      data->channels = mlp_core_channels(data->core.get());
      data->rate = mlp_core_sample_rate(data->core.get());
      break;
    }
  }

  if (data->channels <= 0 || data->rate <= 0) return false;

  const bool is32 = mlp_core_is32(data->core.get()) != 0;
  data->fmt = (is32 ? SFMT_S32 : SFMT_S16) | SFMT_NE;
  data->bytes_per_frame = (is32 ? 4 : 2) * data->channels;

  const int au_size = mlp_core_access_unit_size(data->core.get());
  if (au_size <= 0) return false;

  if (!build_index(data, au_size)) return false;

  // The index scan left the stream at EOF and the decoder holding state from
  // it; rewind both before playback starts.
  mlp_core_flush(data->core.get());
  if (io_seek(data->io_stream.get(), data->first_au_pos, SEEK_SET) == -1) return false;
  data->pcm = nullptr;
  data->pcm_samples = 0;
  data->pcm_pos = 0;

  return true;
}

/// Decodes access units until one yields samples.
/// @return false at clean EOF or on an unrecoverable read error.
bool mlp_fill_buffer(struct mlp_data *data)
{
  while (!data->eof)
  {
    const int len = read_access_unit(data);
    if (len == 0)
    {
      data->eof = true;
      return false;
    }
    if (len < 0)
    {
      // A malformed tail is common in real files; treat it as the end rather
      // than failing the whole track.
      debug("mlp: short or invalid access unit, ending stream");
      data->eof = true;
      return false;
    }

    const void *out = nullptr;
    int nb = 0;
    const int ret =
        mlp_core_decode(data->core.get(), data->au.data(), len, &out, &nb);

    if (ret < 0)
    {
      // Corrupt access unit. The decoder recovers at the next major sync, so
      // keep going rather than dropping the rest of the track.
      continue;
    }

    if (out && nb > 0)
    {
      data->pcm = out;
      data->pcm_samples = nb;
      data->pcm_pos = 0;

      if (data->file_size > 0 && data->duration > 0)
      {
        data->bitrate = data->avg_bitrate;
      }
      return true;
    }
  }
  return false;
}

void *mlp_open(const char *file)
{
  auto *data = new mlp_data;
  decoder_error_init(&data->error);

  data->io_stream.reset(io_open(file, 1));
  if (!io_ok(data->io_stream.get()))
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't open file: %s",
                  io_strerror(data->io_stream.get()));
    return data;
  }

  if (!mlp_read_header(data))
  {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "Not a valid or supported MLP/TrueHD file: %s", file);
    return data;
  }

  if (data->duration > 0 && data->file_size > 0)
  {
    data->avg_bitrate =
        static_cast<int>((data->file_size * 8) / data->duration / 1000);
    data->bitrate = data->avg_bitrate;
  }

  data->ok = true;

  debug("%s file opened. Channels %d. Rate %d. Bits %d. Samples %" PRId64 ". "
        "Seek points %zu. Duration %d.",
        data->is_truehd ? "TrueHD" : "MLP", data->channels, data->rate,
        mlp_core_bits_per_sample(data->core.get()), data->total_samples,
        data->seek_points.size(), data->duration);

  return data;
}

void mlp_close(void *prv_data)
{
  auto *data = static_cast<struct mlp_data *>(prv_data);
  decoder_error_clear(&data->error);
  delete data;
  logit("File closed");
}

void mlp_get_error(void *prv_data, struct decoder_error *error)
{
  auto *data = static_cast<struct mlp_data *>(prv_data);
  decoder_error_copy(error, &data->error);
}

int mlp_get_duration(void *prv_data)
{
  auto *data = static_cast<struct mlp_data *>(prv_data);
  return data->ok ? data->duration : -1;
}

int mlp_get_avg_bitrate(void *prv_data)
{
  auto *data = static_cast<struct mlp_data *>(prv_data);
  return data->ok ? data->avg_bitrate : -1;
}

int mlp_get_bitrate(void *prv_data)
{
  auto *data = static_cast<struct mlp_data *>(prv_data);
  return data->ok ? data->bitrate : -1;
}

int mlp_seek(void *prv_data, int sec)
{
  auto *data = static_cast<struct mlp_data *>(prv_data);

  if (!data->ok || sec < 0 || data->duration <= 0 || sec >= data->duration ||
      data->rate <= 0 || data->seek_points.empty())
  {
    return -1;
  }

  const int64_t target = static_cast<int64_t>(sec) * data->rate;

  // Land on the last indexed major sync at or before the target; decoding
  // cannot start anywhere else, since only a major sync re-establishes the
  // stream parameters and forces every substream to re-read its restart
  // header.
  auto it = std::upper_bound(
      data->seek_points.begin(), data->seek_points.end(), target,
      [](int64_t s, const SeekPoint &p) { return s < p.sample; });
  if (it == data->seek_points.begin()) return -1;
  --it;

  if (io_seek(data->io_stream.get(), it->offset, SEEK_SET) == -1)
  {
    logit("mlp: seek to offset %" PRId64 " failed", it->offset);
    return -1;
  }

  mlp_core_flush(data->core.get());
  data->pcm = nullptr;
  data->pcm_samples = 0;
  data->pcm_pos = 0;
  data->eof = false;
  data->seek_skip = target - it->sample;

  return sec;
}

int mlp_decode(void *prv_data, char *buf, int buf_len,
               struct sound_params *sound_params)
{
  auto *data = static_cast<struct mlp_data *>(prv_data);
  decoder_error_clear(&data->error);

  if (!data->ok) return 0;

  for (;;)
  {
    if (data->pcm_pos >= data->pcm_samples)
    {
      if (!mlp_fill_buffer(data)) return 0;
    }

    if (data->seek_skip > 0)
    {
      const int avail = data->pcm_samples - data->pcm_pos;
      const int drop = static_cast<int>(std::min<int64_t>(data->seek_skip, avail));
      data->pcm_pos += drop;
      data->seek_skip -= drop;
      if (data->pcm_pos >= data->pcm_samples) continue;
    }

    const int want = buf_len / data->bytes_per_frame;
    if (want <= 0) return 0;

    const int avail = data->pcm_samples - data->pcm_pos;
    const int n = std::min(avail, want);

    const size_t off =
        static_cast<size_t>(data->pcm_pos) * data->bytes_per_frame;
    std::memcpy(buf, static_cast<const uint8_t *>(data->pcm) + off,
                static_cast<size_t>(n) * data->bytes_per_frame);
    data->pcm_pos += n;

    sound_params->channels = data->channels;
    sound_params->rate = data->rate;
    sound_params->fmt = data->fmt;

    return n * data->bytes_per_frame;
  }
}

/// Raw MLP/TrueHD elementary streams carry no metadata, so only the duration
/// is available -- and only by walking the chain, which is what open does.
void mlp_info(const char *file_name, struct file_tags *info, const int tags_sel)
{
  if (!(tags_sel & TAGS_TIME)) return;

  auto data = std::make_unique<struct mlp_data>();
  decoder_error_init(&data->error);
  data->io_stream.reset(io_open(file_name, 1));

  if (!io_ok(data->io_stream.get())) return;
  if (!mlp_read_header(data.get())) return;

  info->time = data->duration;
  info->filled |= TAGS_TIME;
}

int mlp_our_format_ext(const char *ext)
{
  return !strcasecmp(ext, "mlp") || !strcasecmp(ext, "thd");
}

std::string mlp_get_name(const char *file)
{
  const char *ext = ext_pos(file);
  if (ext && !strcasecmp(ext, "thd")) return "THD";
  return "MLP";
}

class MlpDecoder : public AudioDecoder
{
public:
  std::unique_ptr<void, void (*)(void *)> data;
  MlpDecoder(void *d) : data(d, mlp_close) {}
  ~MlpDecoder() override = default;

  int decode(char *buf, int buf_len, struct sound_params *sound_params) override
  {
    return mlp_decode(data.get(), buf, buf_len, sound_params);
  }

  int seek(int sec) override { return mlp_seek(data.get(), sec); }

  int get_bitrate() override { return mlp_get_bitrate(data.get()); }

  int get_duration() override { return mlp_get_duration(data.get()); }

  void get_error(struct decoder_error *error) override
  {
    mlp_get_error(data.get(), error);
  }

  int get_avg_bitrate() override { return mlp_get_avg_bitrate(data.get()); }
};

class MlpPlugin : public AudioPlugin
{
public:
  std::unique_ptr<AudioDecoder> open(const char *file) override
  {
    void *d = mlp_open(file);
    if (!d) return nullptr;
    return std::make_unique<MlpDecoder>(d);
  }

  void info(const char *file_name, struct file_tags *info,
            const int tags_sel) override
  {
    mlp_info(file_name, info, tags_sel);
  }

  int our_format_ext(const char *ext) override { return mlp_our_format_ext(ext); }

  std::string get_name(const char *file) override { return mlp_get_name(file); }
};

} // namespace

extern "C" class AudioPlugin *mlp_plugin_init()
{
  static MlpPlugin plugin;
  return &plugin;
}
