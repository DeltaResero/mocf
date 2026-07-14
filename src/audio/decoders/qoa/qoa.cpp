// src/audio/decoders/qoa/qoa.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2026 DeltaResero <deltaresero@zoho.com>
//
// QOA (Quite OK Audio) decoder plugin. Wraps the vendored reference
// decoder (qoa.h, compiled via qoa_impl.c - see both for why the split
// exists) as a lighter alternative to FFmpeg for this format.
//
// Structure of this plugin is an adaption of the ac3 plugin.
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

// Declarations only. The reference implementation is C (its whole-file
// helpers rely on implicit void*->T* conversion from malloc(), legal in
// C but not C++), so it's compiled from qoa_impl.c; here we pull in just
// the extern "C" declarations and macros.
#define QOA_NO_STDIO
#include "qoa.h"

#define DEBUG

#include "core/common.h"
#include "core/log.h"
#include "audio/decoder.h"
#include "io/io.h"
#include "audio/audio.h"

struct qoa_dec_data
{
  unique_io_stream io_stream;

  qoa_desc desc; /* channels/samplerate/samples, filled at open */

  /* One encoded frame is read here off the stream. Sized to the fixed
   * full-frame length once the channel count is known. */
  std::vector<uint8_t> enc;

  /* Decoded PCM for one frame, drained across decode() calls. */
  std::vector<short> pcm;
  size_t pcm_fill = 0; /* valid shorts in pcm */
  size_t pcm_pos = 0;  /* next short to hand out */

  int64_t file_size = -1;
  unsigned int frame_bytes = 0; /* fixed size of a non-last frame */
  int duration = -1;            /* seconds */
  int avg_bitrate = -1;         /* kbps */

  bool eof = false;
  struct decoder_error error;
  bool ok = false;
};

/* Read the next encoded frame off the stream into data->enc and decode
 * it into data->pcm. Returns false at clean EOF or on a decode error
 * (error is set in the latter case). */
static bool qoa_fill_pcm(struct qoa_dec_data *data)
{
  data->pcm_fill = 0;
  data->pcm_pos = 0;

  if (data->eof)
  {
    return false;
  }

  /* A frame begins with an 8-byte header whose low 16 bits give the
   * frame's total byte size. Read that first, then the remainder. */
  ssize_t got = io_read(data->io_stream.get(), data->enc.data(), 8);
  if (got <= 0)
  {
    data->eof = true;
    return false;
  }
  if (got < 8)
  {
    decoder_error(&data->error, ERROR_STREAM, 0, "Truncated QOA frame header");
    data->eof = true;
    return false;
  }

  uint64_t frame_header = 0;
  for (int i = 0; i < 8; i++)
  {
    frame_header = (frame_header << 8) | data->enc[i];
  }
  unsigned int frame_size = frame_header & 0x00ffff;

  if (frame_size < 8 || frame_size > data->enc.size())
  {
    decoder_error(&data->error, ERROR_STREAM, 0, "Invalid QOA frame size");
    data->eof = true;
    return false;
  }

  size_t rest = frame_size - 8;
  size_t off = 8;
  while (rest > 0)
  {
    got = io_read(data->io_stream.get(), data->enc.data() + off, rest);
    if (got <= 0)
    {
      decoder_error(&data->error, ERROR_STREAM, 0, "Truncated QOA frame");
      data->eof = true;
      return false;
    }
    off += static_cast<size_t>(got);
    rest -= static_cast<size_t>(got);
  }

  unsigned int frame_len = 0;
  unsigned int decoded = qoa_decode_frame(data->enc.data(), frame_size,
                                          &data->desc, data->pcm.data(),
                                          &frame_len);

  if (decoded != frame_size || frame_len == 0)
  {
    decoder_error(&data->error, ERROR_STREAM, 0, "QOA frame decode failed");
    data->eof = true;
    return false;
  }

  data->pcm_fill = static_cast<size_t>(frame_len) * data->desc.channels;
  return true;
}

static void *qoa_dec_open(const char *file)
{
  auto *data = new qoa_dec_data;
  decoder_error_init(&data->error);
  memset(&data->desc, 0, sizeof(data->desc));

  data->io_stream.reset(io_open(file, 1));

  if (!io_ok(data->io_stream.get()))
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't open file: %s",
                  io_strerror(data->io_stream.get()));
    return data;
  }

  data->file_size = io_file_size(data->io_stream.get());

  /* The file header plus the first frame's header (16 bytes total)
   * yield channels, samplerate and the total sample count. */
  unsigned char head[16];
  ssize_t got = io_read(data->io_stream.get(), head, sizeof(head));
  if (got < static_cast<ssize_t>(sizeof(head)) ||
      qoa_decode_header(head, static_cast<int>(sizeof(head)), &data->desc) == 0)
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Not a valid QOA file");
    return data;
  }

  /* Every non-last frame is exactly this many bytes; also the upper
   * bound on any frame, so it sizes the read buffer. */
  data->frame_bytes = QOA_FRAME_SIZE(data->desc.channels, QOA_SLICES_PER_FRAME);
  data->enc.resize(data->frame_bytes);
  data->pcm.resize(static_cast<size_t>(QOA_FRAME_LEN) * data->desc.channels);

  /* Total sample count is stored in the header, so duration is exact
   * (QOA is constant-rate) rather than estimated. */
  data->duration = static_cast<int>(data->desc.samples / data->desc.samplerate);
  if (data->duration > 0 && data->file_size > 0)
  {
    data->avg_bitrate =
        static_cast<int>((data->file_size * 8) / data->duration / 1000);
  }

  /* Rewind to the first frame; the peek above only read into a scratch
   * buffer. */
  if (io_seek(data->io_stream.get(), 8, SEEK_SET) == -1)
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "QOA seek to first frame failed");
    return data;
  }

  data->ok = true;

  debug("QOA file opened. Channels %u. Rate %u. Samples %u. Duration %d.",
        data->desc.channels, data->desc.samplerate, data->desc.samples,
        data->duration);

  return data;
}

static void qoa_dec_close(void *prv_data)
{
  auto *data = static_cast<struct qoa_dec_data *>(prv_data);
  decoder_error_clear(&data->error);
  delete data;
  logit("File closed");
}

static int qoa_dec_seek(void *prv_data, int sec)
{
  auto *data = static_cast<struct qoa_dec_data *>(prv_data);

  assert(sec >= 0);

  if (data->duration <= 0 || sec >= data->duration)
  {
    return -1;
  }

  /* Frames are independent and fixed-size, so seeking is exact to the
   * enclosing frame boundary: no proportional guessing or resync. */
  uint64_t target_sample =
      static_cast<uint64_t>(sec) * data->desc.samplerate;
  uint64_t frame_index = target_sample / QOA_FRAME_LEN;
  int64_t offset = 8 + static_cast<int64_t>(frame_index) * data->frame_bytes;

  if (io_seek(data->io_stream.get(), offset, SEEK_SET) == -1)
  {
    logit("seek failed");
    return -1;
  }

  data->pcm_fill = 0;
  data->pcm_pos = 0;
  data->eof = false;

  return static_cast<int>((frame_index * QOA_FRAME_LEN) / data->desc.samplerate);
}

static int qoa_dec_get_bitrate(void *prv_data)
{
  auto *data = static_cast<struct qoa_dec_data *>(prv_data);
  return data->avg_bitrate;
}

static int qoa_dec_get_avg_bitrate(void *prv_data)
{
  auto *data = static_cast<struct qoa_dec_data *>(prv_data);
  return data->avg_bitrate;
}

static int qoa_dec_get_duration(void *prv_data)
{
  auto *data = static_cast<struct qoa_dec_data *>(prv_data);
  return data->duration;
}

static void qoa_dec_get_error(void *prv_data, struct decoder_error *error)
{
  auto *data = static_cast<struct qoa_dec_data *>(prv_data);
  decoder_error_copy(error, &data->error);
}

/* QOA carries no embedded metadata; only the duration is available,
 * read straight out of the header. */
static void qoa_dec_info(const char *file_name, struct file_tags *info,
                         const int tags_sel)
{
  if (!(tags_sel & TAGS_TIME))
  {
    return;
  }

  unique_io_stream io(io_open(file_name, 1));
  if (!io_ok(io.get()))
  {
    return;
  }

  unsigned char head[16];
  qoa_desc desc;
  memset(&desc, 0, sizeof(desc));

  if (io_read(io.get(), head, sizeof(head)) < static_cast<ssize_t>(sizeof(head)))
  {
    return;
  }
  if (qoa_decode_header(head, static_cast<int>(sizeof(head)), &desc) == 0)
  {
    return;
  }

  info->time = static_cast<int>(desc.samples / desc.samplerate);
  info->filled |= TAGS_TIME;
}

static int qoa_dec_decode(void *prv_data, char *buf, int buf_len,
                          struct sound_params *sound_params)
{
  auto *data = static_cast<struct qoa_dec_data *>(prv_data);
  decoder_error_clear(&data->error);

  /* Refill from the next frame when the staged PCM is exhausted. */
  if (data->pcm_pos >= data->pcm_fill)
  {
    if (!qoa_fill_pcm(data))
    {
      return 0; /* clean EOF, or error set by qoa_fill_pcm() */
    }
  }

  size_t avail_shorts = data->pcm_fill - data->pcm_pos;
  size_t want_shorts = static_cast<size_t>(buf_len) / sizeof(short);
  size_t n = avail_shorts < want_shorts ? avail_shorts : want_shorts;

  memcpy(buf, data->pcm.data() + data->pcm_pos, n * sizeof(short));
  data->pcm_pos += n;

  sound_params->channels = static_cast<int>(data->desc.channels);
  sound_params->rate = static_cast<int>(data->desc.samplerate);
  sound_params->fmt = SFMT_S16 | SFMT_NE;

  return static_cast<int>(n * sizeof(short));
}

static int qoa_dec_our_format_ext(const char *ext)
{
  return !strcasecmp(ext, "qoa");
}

static std::string qoa_dec_get_name(const char *unused ATTR_UNUSED)
{
  return "QOA";
}

class QoaDecoder : public AudioDecoder {
public:
    std::unique_ptr<void, void(*)(void*)> data;
    QoaDecoder(void *d) : data(d, qoa_dec_close) {}
    ~QoaDecoder() override = default;

    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return qoa_dec_decode(data.get(), buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return qoa_dec_seek(data.get(), sec);
    }

    int get_bitrate() override {
        return qoa_dec_get_bitrate(data.get());
    }

    int get_duration() override {
        return qoa_dec_get_duration(data.get());
    }

    void get_error(struct decoder_error *error) override {
        qoa_dec_get_error(data.get(), error);
    }

    int get_avg_bitrate() override {
        return qoa_dec_get_avg_bitrate(data.get());
    }
};

class QoaPlugin : public AudioPlugin {
public:

    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = qoa_dec_open(file);
        if (!d) return nullptr;
        return std::make_unique<QoaDecoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        qoa_dec_info(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return qoa_dec_our_format_ext(ext);
    }

    std::string get_name(const char *file) override {
        return qoa_dec_get_name(file);
    }
};

extern "C" class AudioPlugin *qoa_plugin_init() {
    static QoaPlugin plugin;
    return &plugin;
}

// EOF
