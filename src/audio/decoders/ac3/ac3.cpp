// src/audio/decoders/ac3/ac3.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2026 DeltaResero <deltaresero@zoho.com>
//
// Enables mocf to play raw AC-3 (ATSC A/52) elementary streams via
// liba52 (a52dec), as a lighter alternative to FFmpeg for this format.
// liba52 decodes classic AC-3 only, not E-AC-3 (Dolby Digital Plus) -
// files with the eac3 extension are left to FFmpeg.
//
// Structure of this plugin is an adaption of the wavpack plugin.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <a52dec/a52.h>
}

#define DEBUG

#include "core/common.h"
#include "core/log.h"
#include "audio/decoder.h"
#include "io/io.h"
#include "audio/audio.h"

/* A52 frames are at most 3840 bytes (per a52_syncinfo() docs) and at
 * least 7 bytes are needed to probe for a sync word. Keep enough
 * headroom in the ring buffer to always hold one full frame plus a
 * bit for resync scanning. */
static constexpr size_t AC3_BUF_SIZE = 4096 * 2;
static constexpr int AC3_BLOCKS_PER_FRAME = 6;
static constexpr int AC3_SAMPLES_PER_BLOCK = 256;

struct a52_state_deleter
{
  void operator()(a52_state_t *s) const noexcept
  {
    if (s)
    {
      a52_free(s);
    }
  }
};
using unique_a52_state = std::unique_ptr<a52_state_t, a52_state_deleter>;

struct ac3_data
{
  unique_io_stream io_stream;
  unique_a52_state state;

  std::vector<uint8_t> buf;
  size_t buf_fill = 0; /* valid bytes at the front of buf */

  int sample_rate = 0;
  int bit_rate = 0;    /* bits per second, from the stream itself */
  int avg_bitrate = 0; /* kbps, as reported to the UI */
  int64_t file_size = -1;
  int duration = -1; /* seconds; estimated assuming CBR (see comment
                        in ac3_count_time_internal()) */
  bool eof = false;
  struct decoder_error error;
  bool ok = false; /* was this stream successfully opened? */
};

/* Ensure at least 'needed' bytes are available at the front of
 * data->buf, refilling from the stream as necessary. Returns false
 * only if EOF is hit and fewer than 'needed' bytes could be
 * gathered. */
static bool ac3_fill_buf(struct ac3_data *data, size_t needed)
{
  while (data->buf_fill < needed && !data->eof)
  {
    if (data->buf_fill + 4096 > data->buf.size())
    {
      data->buf.resize(data->buf.size() + 4096);
    }

    ssize_t got = io_read(data->io_stream.get(), data->buf.data() + data->buf_fill,
                          data->buf.size() - data->buf_fill);

    if (got <= 0)
    {
      data->eof = true;
      break;
    }

    data->buf_fill += got;
  }

  return data->buf_fill >= needed;
}

static void ac3_consume(struct ac3_data *data, size_t n)
{
  assert(n <= data->buf_fill);
  memmove(data->buf.data(), data->buf.data() + n, data->buf_fill - n);
  data->buf_fill -= n;
}

/* Estimate duration assuming constant bitrate, matching the CBR
 * fallback in mp3.cpp - AC-3's bitstream syntax technically allows
 * per-frame bitrate changes, but real-world raw .ac3 elementary
 * streams (already demuxed from a DVD/broadcast source) are, in
 * practice, always CBR. A full frame-count pre-scan would be more
 * exact but costs CPU/IO at open time we'd rather not pay on the
 * project's low-power target hardware. */
static int ac3_estimate_duration(struct ac3_data *data)
{
  if (data->file_size <= 0 || data->bit_rate <= 0)
  {
    return -1;
  }

  return static_cast<int>((data->file_size * 8) / data->bit_rate);
}

/* Scan for the next valid A/52 sync word starting at the front of
 * data->buf. On success, returns the frame size in bytes (as
 * reported by a52_syncinfo()) with the frame's bytes available at
 * the front of data->buf, and sample_rate/bit_rate updated. On
 * failure (EOF reached before a sync word or a full frame could be
 * found), returns 0. */
static int ac3_next_frame(struct ac3_data *data)
{
  for (;;)
  {
    if (!ac3_fill_buf(data, 7))
    {
      return 0; /* not enough data left to even probe for sync */
    }

    /* Scan byte-by-byte within the currently buffered data for a
     * sync word, refilling as we approach the end of what's
     * buffered. */
    while (data->buf_fill >= 7)
    {
      int flags = 0;
      int frame_size = a52_syncinfo(data->buf.data(), &flags,
                                    &data->sample_rate, &data->bit_rate);

      if (frame_size == 0)
      {
        ac3_consume(data, 1);
        if (!ac3_fill_buf(data, 7))
        {
          return 0;
        }
        continue;
      }

      if (!ac3_fill_buf(data, static_cast<size_t>(frame_size)))
      {
        return 0; /* truncated final frame */
      }

      return frame_size;
    }
  }
}

static void *ac3_open(const char *file)
{
  auto *data = new ac3_data;
  decoder_error_init(&data->error);

  data->io_stream.reset(io_open(file, 1));

  if (!io_ok(data->io_stream.get()))
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't open file: %s",
                  io_strerror(data->io_stream.get()));
    return data;
  }

  data->file_size = io_file_size(data->io_stream.get());
  data->buf.resize(AC3_BUF_SIZE);

  data->state.reset(a52_init(0));
  if (!data->state)
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "a52_init() failed");
    return data;
  }

  int frame_size = ac3_next_frame(data);
  if (frame_size == 0)
  {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "No valid AC-3 sync word found in file");
    return data;
  }

  data->duration = ac3_estimate_duration(data);
  data->avg_bitrate = data->bit_rate / 1000;
  data->ok = true;

  debug("AC3 file opened. Sample rate %d. Bit rate %d. Duration %d.",
        data->sample_rate, data->bit_rate, data->duration);

  return data;
}

static void ac3_close(void *prv_data)
{
  auto *data = static_cast<struct ac3_data *>(prv_data);
  decoder_error_clear(&data->error);
  delete data;
  logit("File closed");
}

static int ac3_seek(void *prv_data, int sec)
{
  auto *data = static_cast<struct ac3_data *>(prv_data);

  assert(sec >= 0);

  if (data->duration <= 0 || sec >= data->duration)
  {
    return -1;
  }

  /* Approximate: proportional byte offset, then resync on the next
   * frame boundary once we start decoding again. Exact only for
   * genuinely CBR files, same caveat mp3.cpp's CBR seek path has. */
  int64_t new_position =
      (static_cast<double>(sec) / static_cast<double>(data->duration)) *
      data->file_size;

  if (io_seek(data->io_stream.get(), new_position, SEEK_SET) == -1)
  {
    logit("seek failed");
    return -1;
  }

  data->buf_fill = 0;
  data->eof = false;

  return sec;
}

static int ac3_get_bitrate(void *prv_data)
{
  auto *data = static_cast<struct ac3_data *>(prv_data);
  return data->bit_rate / 1000;
}

static int ac3_get_avg_bitrate(void *prv_data)
{
  auto *data = static_cast<struct ac3_data *>(prv_data);
  return data->avg_bitrate;
}

static int ac3_get_duration(void *prv_data)
{
  auto *data = static_cast<struct ac3_data *>(prv_data);
  return data->duration;
}

static void ac3_get_error(void *prv_data, struct decoder_error *error)
{
  auto *data = static_cast<struct ac3_data *>(prv_data);
  decoder_error_copy(error, &data->error);
}

/* Raw elementary AC-3 streams carry no embedded metadata (no title,
 * artist, etc.) - just report the duration, computed the same way
 * ac3_open() does. */
static void ac3_info(const char *file_name, struct file_tags *info,
                     const int tags_sel)
{
  if (!(tags_sel & TAGS_TIME))
  {
    return;
  }

  struct ac3_data data;
  decoder_error_init(&data.error);
  data.io_stream.reset(io_open(file_name, 1));

  if (!io_ok(data.io_stream.get()))
  {
    return;
  }

  data.file_size = io_file_size(data.io_stream.get());
  data.buf.resize(AC3_BUF_SIZE);
  data.state.reset(a52_init(0));

  if (!data.state || ac3_next_frame(&data) == 0)
  {
    return;
  }

  info->time = ac3_estimate_duration(&data);
  info->filled |= TAGS_TIME;
}

static int ac3_decode(void *prv_data, char *buf, int buf_len,
                      struct sound_params *sound_params)
{
  auto *data = static_cast<struct ac3_data *>(prv_data);
  decoder_error_clear(&data->error);

  int frame_size = ac3_next_frame(data);
  if (frame_size == 0)
  {
    return 0; /* EOF or unrecoverable resync failure */
  }

  int req_flags = A52_STEREO;
  sample_t level = 1.0f;
  sample_t bias = 0.0f;

  if (a52_frame(data->state.get(), data->buf.data(), &req_flags, &level,
               bias) != 0)
  {
    decoder_error(&data->error, ERROR_STREAM, 0, "a52_frame() failed");
    ac3_consume(data, frame_size);
    return 0;
  }

  int needed = AC3_BLOCKS_PER_FRAME * AC3_SAMPLES_PER_BLOCK * 2 *
              static_cast<int>(sizeof(float));

  if (buf_len < needed)
  {
    /* The engine's buffer should always be large enough for one
     * frame's worth of output; if it somehow isn't, decoding a
     * partial frame would desync channel interleaving, so bail
     * out instead of producing corrupt audio. */
    decoder_error(&data->error, ERROR_STREAM, 0,
                  "Output buffer too small for one AC-3 frame");
    ac3_consume(data, frame_size);
    return 0;
  }

  auto *out = reinterpret_cast<float *>(buf);

  for (int b = 0; b < AC3_BLOCKS_PER_FRAME; b++)
  {
    if (a52_block(data->state.get()) != 0)
    {
      decoder_error(&data->error, ERROR_STREAM, 0, "a52_block() failed");
      ac3_consume(data, frame_size);
      return 0;
    }

    sample_t *samples = a52_samples(data->state.get());

    /* liba52 returns samples in planar form (256 for channel 0,
     * then 256 for channel 1); mocf expects interleaved. */
    for (int s = 0; s < AC3_SAMPLES_PER_BLOCK; s++)
    {
      out[2 * s] = samples[s];
      out[2 * s + 1] = samples[AC3_SAMPLES_PER_BLOCK + s];
    }

    out += AC3_SAMPLES_PER_BLOCK * 2;
  }

  ac3_consume(data, frame_size);

  sound_params->channels = 2;
  sound_params->rate = data->sample_rate;
  sound_params->fmt = SFMT_FLOAT;

  return needed;
}

static int ac3_our_format_ext(const char *ext)
{
  return !strcasecmp(ext, "ac3");
}

static std::string ac3_get_name(const char *unused ATTR_UNUSED)
{
  return "AC3";
}

class AC3Decoder : public AudioDecoder {
public:
    std::unique_ptr<void, void(*)(void*)> data;
    AC3Decoder(void *d) : data(d, ac3_close) {}
    ~AC3Decoder() override = default;

    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return ac3_decode(data.get(), buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return ac3_seek(data.get(), sec);
    }

    int get_bitrate() override {
        return ac3_get_bitrate(data.get());
    }

    int get_duration() override {
        return ac3_get_duration(data.get());
    }

    void get_error(struct decoder_error *error) override {
        ac3_get_error(data.get(), error);
    }

    int get_avg_bitrate() override {
        return ac3_get_avg_bitrate(data.get());
    }
};

class AC3Plugin : public AudioPlugin {
public:

    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = ac3_open(file);
        if (!d) return nullptr;
        return std::make_unique<AC3Decoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        ac3_info(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return ac3_our_format_ext(ext);
    }

    std::string get_name(const char *file) override {
        return ac3_get_name(file);
    }
};

extern "C" class AudioPlugin *ac3_plugin_init() {
    static AC3Plugin plugin;
    return &plugin;
}

// EOF
