// src/audio/decoders/wma/wma.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2026 DeltaResero <deltaresero@zoho.com>
// Structure of this plugin is an adaption of the tta plugin.
//
// Windows Media Audio v1/v2 playback: asf.cpp reads the container, wmadec.cpp
// decodes the stream. WMA Pro, Lossless and Voice are separate codecs and are
// not handled; files carrying them are rejected at open with a clear reason.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include "config.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/common.h"
#include "core/log.h"
#include "audio/decoder.h"
#include "io/io.h"
#include "audio/audio.h"

#include "asf.h"
#include "wmadec.h"

namespace {

struct wma_data
{
  unique_io_stream io_stream;
  struct decoder_error error;
  bool ok = false;

  AsfReader asf;
  unique_wma_core core;

  int channels = 0;
  int sample_rate = 0;
  int duration = 0;
  int avg_bitrate = -1;
  int bitrate = -1;

  /* Samples decoded but not yet handed to the player. The pointers are owned
   * by the decoder core and stay valid until the next decode call. */
  float *const *planes = nullptr;
  int frame_samples = 0;
  int buffer_pos = 0;

  /* The MDCT needs a frame of overlap before its output means anything, so the
   * first 2 * frame_len samples are priming and are dropped. FFmpeg reports the
   * same figure as its decoder delay. Reused after a seek to drop the samples
   * between the packet actually landed on and the position asked for. */
  int64_t skip_samples = 0;

  bool eof = false;
  bool drained = false;

  std::vector<uint8_t> packet;
};

/// Pulls one superframe and decodes it. False means end of stream.
bool wma_fill_buffer(struct wma_data *data)
{
  for (;;)
  {
    if (data->eof)
    {
      if (data->drained)
      {
        return false;
      }

      /* Flush the trailing overlap the last superframe left behind. */
      data->drained = true;
      int n = 0;
      if (wma_core_decode(data->core.get(), nullptr, 0, &data->planes, &n) < 0 ||
          n == 0)
      {
        return false;
      }
      data->frame_samples = n;
      data->buffer_pos = 0;
      return true;
    }

    const int size = data->asf.next_packet(data->packet.data(),
                                           static_cast<int>(data->packet.size()));
    if (size == 0)
    {
      data->eof = true;
      continue;
    }
    if (size < 0)
    {
      decoder_error(&data->error, ERROR_STREAM, 0, "Broken ASF packet");
      data->eof = true;
      continue;
    }

    int n = 0;
    if (wma_core_decode(data->core.get(), data->packet.data(), size,
                        &data->planes, &n) < 0)
    {
      /* One bad superframe costs a frame of audio, not the rest of the
       * track: the decoder resynchronises on the next one. */
      continue;
    }
    if (n == 0)
    {
      continue; /* superframe buffered into the bit reservoir */
    }

    data->frame_samples = n;
    data->buffer_pos = 0;
    return true;
  }
}

/// Interleaves @p count sample-frames of planar float into 16-bit output.
void wma_pack_output(struct wma_data *data, int start, int count, char *buf)
{
  auto *out = reinterpret_cast<int16_t *>(buf);
  const int channels = data->channels;

  for (int i = 0; i < count; i++)
  {
    for (int c = 0; c < channels; c++)
    {
      const float v = data->planes[c][start + i];
      /* The decoder emits normalised floats; anything outside the range is a
       * clipped stream rather than a decode fault, so clamp rather than wrap. */
      const int s = static_cast<int>(lrintf(v * 32768.0f));
      *out++ = static_cast<int16_t>(std::clamp(s, -32768, 32767));
    }
  }
}

} // namespace

// ---------------------------------------------------------------------
// mocf decoder/plugin interface
// ---------------------------------------------------------------------

static void *wma_open(const char *file)
{
  auto *data = new wma_data;
  decoder_error_init(&data->error);

  data->io_stream.reset(io_open(file, 1));
  if (!io_ok(data->io_stream.get()))
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't open file: %s",
                  io_strerror(data->io_stream.get()));
    return data;
  }

  if (!data->asf.open(data->io_stream.get()))
  {
    /* Prefer the reader's own reason when it has one: "this is WMA Pro" is a
     * far more useful thing to read than "not a valid WMA file". */
    if (!data->asf.failure_reason().empty())
    {
      decoder_error(&data->error, ERROR_FATAL, 0, "%s: %s",
                    data->asf.failure_reason().c_str(), file);
    }
    else
    {
      decoder_error(&data->error, ERROR_FATAL, 0,
                    "Not a valid or supported WMA file: %s", file);
    }
    return data;
  }

  const AsfAudioInfo &info = data->asf.info();

  WmaStreamParams params;
  params.version        = (info.codec_tag == 0x0160) ? 1 : 2;
  params.channels       = info.channels;
  params.sample_rate    = info.sample_rate;
  params.block_align    = info.block_align;
  params.bit_rate       = info.byte_rate * 8;
  params.extradata      = info.extradata.data();
  params.extradata_size = static_cast<int>(info.extradata.size());

  data->core = wma_core_create(params);
  if (!data->core)
  {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "Can't initialise the WMA decoder for: %s", file);
    return data;
  }

  data->channels = info.channels;
  data->sample_rate = info.sample_rate;
  data->duration = static_cast<int>(data->asf.duration_sec());
  data->avg_bitrate = info.byte_rate * 8 / 1000;
  data->bitrate = data->avg_bitrate;
  data->skip_samples = 2LL * wma_core_frame_len(data->core.get());

  /* One superframe is block_align bytes; the ASF reader never hands out more. */
  data->packet.resize(static_cast<size_t>(info.block_align));

  data->ok = true;

  debug("WMA file opened. Version %d. Channels %d. Rate %d. Block align %d. "
        "Duration %d.",
        params.version, data->channels, data->sample_rate, info.block_align,
        data->duration);

  return data;
}

static void wma_close(void *prv_data)
{
  auto *data = static_cast<struct wma_data *>(prv_data);
  decoder_error_clear(&data->error);
  delete data;
  logit("File closed");
}

static void wma_get_error(void *prv_data, struct decoder_error *error)
{
  auto *data = static_cast<struct wma_data *>(prv_data);
  decoder_error_copy(error, &data->error);
}

static int wma_get_duration(void *prv_data)
{
  auto *data = static_cast<struct wma_data *>(prv_data);
  return data->duration;
}

static int wma_get_avg_bitrate(void *prv_data)
{
  auto *data = static_cast<struct wma_data *>(prv_data);
  return data->avg_bitrate;
}

/* WMA is constant bitrate within a stream, so the header figure is the
 * instantaneous one too. */
static int wma_get_bitrate(void *prv_data)
{
  auto *data = static_cast<struct wma_data *>(prv_data);
  return data->bitrate;
}

static int wma_seek(void *prv_data, int sec)
{
  auto *data = static_cast<struct wma_data *>(prv_data);

  if (!data->ok || sec < 0 || data->duration <= 0 || sec >= data->duration)
  {
    return -1;
  }

  if (!data->asf.seek_ms(static_cast<int64_t>(sec) * 1000))
  {
    return -1;
  }

  wma_core_flush(data->core.get());
  data->frame_samples = 0;
  data->buffer_pos = 0;
  data->eof = false;
  data->drained = false;

  /* Landing on a packet boundary is enough: the overlap that the following
   * superframes need is rebuilt as they decode, and the first frame after a
   * seek is priming just as it is at start of stream. */
  data->skip_samples = 2LL * wma_core_frame_len(data->core.get());

  return sec;
}

static int wma_decode(void *prv_data, char *buf, int buf_len,
                      struct sound_params *sound_params)
{
  auto *data = static_cast<struct wma_data *>(prv_data);
  decoder_error_clear(&data->error);

  if (!data->ok)
  {
    return 0;
  }

  for (;;)
  {
    if (data->buffer_pos >= data->frame_samples)
    {
      if (!wma_fill_buffer(data))
      {
        return 0; /* clean EOF, or error already set */
      }
    }

    if (data->skip_samples > 0)
    {
      const int avail = data->frame_samples - data->buffer_pos;
      const int drop = static_cast<int>(
          std::min<int64_t>(data->skip_samples, avail));
      data->buffer_pos += drop;
      data->skip_samples -= drop;
      if (data->buffer_pos >= data->frame_samples)
      {
        continue;
      }
    }

    const int bytes_per_sample_frame = 2 * data->channels;
    const int want = buf_len / bytes_per_sample_frame;
    if (want <= 0)
    {
      return 0;
    }

    const int avail = data->frame_samples - data->buffer_pos;
    const int n = std::min(avail, want);

    wma_pack_output(data, data->buffer_pos, n, buf);
    data->buffer_pos += n;

    sound_params->channels = data->channels;
    sound_params->rate = data->sample_rate;
    sound_params->fmt = SFMT_S16 | SFMT_NE;

    return n * bytes_per_sample_frame;
  }
}

/* Metadata lives in the ASF header, so this only reads far enough to parse it
 * and never touches the data packets. */
static void wma_info(const char *file_name, struct file_tags *info,
                     const int tags_sel)
{
  if (!(tags_sel & (TAGS_COMMENTS | TAGS_TIME)))
  {
    return;
  }

  unique_io_stream stream(io_open(file_name, 0));
  if (!io_ok(stream.get()))
  {
    return;
  }

  AsfReader asf;
  if (!asf.open(stream.get()))
  {
    return;
  }

  const AsfAudioInfo &ai = asf.info();

  if (tags_sel & TAGS_TIME)
  {
    info->time = static_cast<int>(asf.duration_sec());
    info->filled |= TAGS_TIME;
  }

  if (tags_sel & TAGS_COMMENTS)
  {
    /* Any one of these may be absent; mark the tags filled if we found
     * something, so the cache does not keep asking. */
    if (!ai.title.empty())
    {
      info->title = ai.title;
    }
    if (!ai.artist.empty())
    {
      info->artist = ai.artist;
    }
    if (!ai.album.empty())
    {
      info->album = ai.album;
    }
    if (ai.track >= 0)
    {
      info->track = ai.track;
    }

    if (!ai.title.empty() || !ai.artist.empty() || !ai.album.empty() ||
        ai.track >= 0)
    {
      info->filled |= TAGS_COMMENTS;
    }
  }
}

static int wma_our_format_ext(const char *ext)
{
  return !strcasecmp(ext, "wma");
}

static std::string wma_get_name(const char *)
{
  return "WMA";
}

class WmaDecoder : public AudioDecoder {
public:
    std::unique_ptr<void, void(*)(void*)> data;
    WmaDecoder(void *d) : data(d, wma_close) {}
    ~WmaDecoder() override = default;

    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return wma_decode(data.get(), buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return wma_seek(data.get(), sec);
    }

    int get_bitrate() override {
        return wma_get_bitrate(data.get());
    }

    int get_duration() override {
        return wma_get_duration(data.get());
    }

    void get_error(struct decoder_error *error) override {
        wma_get_error(data.get(), error);
    }

    int get_avg_bitrate() override {
        return wma_get_avg_bitrate(data.get());
    }
};

class WmaPlugin : public AudioPlugin {
public:
    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = wma_open(file);
        if (!d) return nullptr;
        return std::make_unique<WmaDecoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        wma_info(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return wma_our_format_ext(ext);
    }

    const char *our_format_data(const char *buf, size_t len) override {
        static const unsigned char asf_guid[16] = {
            0x30,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,
            0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C};
        if (len >= 16 && !memcmp(buf, asf_guid, 16)) {
            return "WMA";
        }
        return nullptr;
    }

    std::string get_name(const char *file) override {
        return wma_get_name(file);
    }
};

extern "C" class AudioPlugin *wma_plugin_init() {
    static WmaPlugin plugin;
    return &plugin;
}

// EOF
