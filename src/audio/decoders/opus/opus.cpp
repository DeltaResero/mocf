// src/audio/decoders/opus/opus.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2002 - 2005 Damian Pietras <daper@daper.net>
// Copyright (C) 2012 - 2014 Tomasz Golinski <tomaszg@alpha.uwb.edu.pl>
//
// Opus support written by Tomasz Golinski for his MOC fork, using the
// vorbis plugin as its template; portions based on Greg Maxwell's code.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cassert>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <opusfile.h>

#define DEBUG

#include "core/common.h"
#include "core/log.h"
#include "audio/decoder.h"
#include "io/io.h"
#include "audio/audio.h"

struct opus_data
{
  unique_io_stream stream;
  OggOpusFile *of;
  int last_section;
  opus_int32 bitrate;
  opus_int32 avg_bitrate;
  int duration;
  struct decoder_error error;
  int ok;          /* was this stream successfully opened? */
  int tags_change; /* the tags were changed from the last call of
                      opus_current_tags */
  std::unique_ptr<struct file_tags> tags;
};

static void get_comment_tags(OggOpusFile *of, struct file_tags *info)
{
  int i;
  const OpusTags *comments;

  comments = op_tags(of, -1);
  for (i = 0; i < comments->comments; i++)
  {
    if (!strncasecmp(comments->user_comments[i], "title=", strlen("title=")))
    {
      info->title = (comments->user_comments[i] + strlen("title="));
    }
    else if (!strncasecmp(comments->user_comments[i],
                          "artist=", strlen("artist=")))
    {
      info->artist = (comments->user_comments[i] + strlen("artist="));
    }
    else if (!strncasecmp(comments->user_comments[i],
                          "album=", strlen("album=")))
    {
      info->album = (comments->user_comments[i] + strlen("album="));
    }
    else if (!strncasecmp(comments->user_comments[i],
                          "tracknumber=", strlen("tracknumber=")))
    {
      info->track = static_cast<int>(strtol(comments->user_comments[i] + strlen("tracknumber="), nullptr, 10));
    }
    else if (!strncasecmp(comments->user_comments[i],
                          "track=", strlen("track=")))
    {
      info->track = static_cast<int>(strtol(comments->user_comments[i] + strlen("track="), nullptr, 10));
    }
  }
}

/* Return a description of an op_*() error. */
static const char *opus_str_error(const int code)
{
  const char *result;

  switch (code)
  {
    case OP_FALSE:
      result = "Request was not successful";
      break;
    case OP_EOF:
      result = "End of File";
      break;
    case OP_HOLE:
      result = "Hole in stream";
      break;
    case OP_EREAD:
      result = "An underlying read, seek, or tell operation failed.";
      break;
    case OP_EFAULT:
      result = "Internal (Opus) logic fault";
      break;
    case OP_EIMPL:
      result = "Unimplemented feature";
      break;
    case OP_EINVAL:
      result = "Invalid argument";
      break;
    case OP_ENOTFORMAT:
      result = "Not an Opus file";
      break;
    case OP_EBADHEADER:
      result = "Invalid or corrupt header";
      break;
    case OP_EVERSION:
      result = "Opus header version mismatch";
      break;
    case OP_EBADPACKET:
      result = "An audio packet failed to decode properly";
      break;
    case OP_ENOSEEK:
      result = "Requested seeking in unseekable stream";
      break;
    case OP_EBADTIMESTAMP:
      result = "File timestamps fail sanity tests";
      break;
    default:
      result = "Unknown error";
  }

  return result;
}

/* Fill info structure with data from ogg comments */
static void opus_tags(const char *file_name, struct file_tags *info,
                      const int tags_sel)
{
  OggOpusFile *of;
  int err_code;

  // op_test() is faster than op_open(), but we can't read file time with it.
  if (tags_sel & TAGS_TIME)
  {
    of = op_open_file(file_name, &err_code);
  }
  else
  {
    of = op_test_file(file_name, &err_code);
  }

  if (err_code < 0)
  {
    logit("Can't open %s: %s", file_name, opus_str_error(err_code));
    op_free(of);
    return;
  }

  if (tags_sel & TAGS_COMMENTS)
  {
    get_comment_tags(of, info);
  }

  if (tags_sel & TAGS_TIME)
  {
    ogg_int64_t opus_time;

    opus_time = op_pcm_total(of, -1);
    if (opus_time >= 0)
    {
      info->time = opus_time / 48000;
      debug("Duration tags: %d, samples %lld", info->time,
            (long long)opus_time);
    }
  }

  op_free(of);
}

static int read_cb(void *datasource, unsigned char *ptr, int bytes)
{
  ssize_t res;

  res = io_read(static_cast<io_stream *>(datasource), ptr, bytes);
  if (res < 0)
  {
    logit("Read error");
    res = -1;
  }

  return res;
}

static int seek_cb(void *datasource, opus_int64 offset, int whence)
{
  debug("Seek request to %" PRId64 " (%s)", (int64_t)offset,
        whence == SEEK_SET ? "SEEK_SET"
                           : (whence == SEEK_CUR ? "SEEK_CUR" : "SEEK_END"));
  return io_seek(static_cast<io_stream *>(datasource), offset, whence) < 0 ? -1 : 0;
}

static int close_cb(void *datasource ATTR_UNUSED) { return 0; }

static opus_int64 tell_cb(void *datasource)
{
  return static_cast<opus_int64>(io_tell(static_cast<io_stream *>(datasource)));
}

static void opus_open_stream_internal(struct opus_data *data)
{
  int res;

  OpusFileCallbacks callbacks = {read_cb, seek_cb, tell_cb, close_cb};

  data->tags = std::make_unique<file_tags>();

  data->of = op_open_callbacks(data->stream.get(), &callbacks, nullptr, 0, &res);
  if (res < 0)
  {
    const char *opus_err = opus_str_error(res);

    decoder_error(&data->error, ERROR_FATAL, 0, "%s", opus_err);
    debug("op_open error: %s", opus_err);
    op_free(data->of);
    data->of = nullptr;
  }
  else
  {
    ogg_int64_t samples;
    data->last_section = -1;
    data->avg_bitrate = op_bitrate(data->of, -1) / 1000;
    data->bitrate = data->avg_bitrate;
    samples = op_pcm_total(data->of, -1);
    if (samples == OP_EINVAL)
    {
      data->duration = -1;
    }
    else
    {
      data->duration = samples / 48000;
    }
    debug("Duration: %d, samples %lld", data->duration, (long long)samples);
    data->ok = 1;
    get_comment_tags(data->of, data->tags.get());
  }
}

static void *opus_open(const char *file)
{
  struct opus_data *data;
  data = new opus_data;
  data->ok = 0;

  decoder_error_init(&data->error);
  data->tags_change = 0;
  data->tags = nullptr;

  data->stream.reset(io_open(file, 1));
  if (!io_ok(data->stream.get()))
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't load Opus: %s",
                  io_strerror(data->stream.get()));
  }
  else
  {
    opus_open_stream_internal(data);
  }
  return data;
}

static void opus_close(void *prv_data)
{
  struct opus_data *data = static_cast<struct opus_data *>(prv_data);

  if (data->ok)
  {
    op_free(data->of);
  }

  decoder_error_clear(&data->error);
  delete data;
}

static int opus_seek(void *prv_data, int sec)
{
  struct opus_data *data = static_cast<struct opus_data *>(prv_data);

  assert(sec >= 0);

  return op_pcm_seek(data->of, sec * static_cast<ogg_int64_t>(48000)) < 0 ? -1 : sec;
}

static int opus_decodeX(void *prv_data, char *buf, int buf_len,
                        struct sound_params *sound_params)
{
  struct opus_data *data = static_cast<struct opus_data *>(prv_data);
  int ret;
  int current_section;
  int bitrate;

  decoder_error_clear(&data->error);

  while (true)
  {
#if HAVE_OPUSFILE_FLOAT && INTERNAL_FLOAT
    ret = op_read_float(data->of, reinterpret_cast<float *>(buf), buf_len / sizeof(float),
                        &current_section);
    debug("opus float!");
#else
    ret = op_read(data->of, reinterpret_cast<opus_int16 *>(buf), buf_len / sizeof(opus_int16),
                  &current_section);
    debug("opus fixed!");
#endif
    if (ret == 0)
    {
      return 0;
    }
    if (ret < 0)
    {
      decoder_error(&data->error, ERROR_STREAM, 0, "Error in the stream!");
      continue;
    }

    if (current_section != data->last_section)
    {
      logit("section change or first section");
      data->last_section = current_section;
      data->tags_change = 1;
      data->tags = std::make_unique<file_tags>();
      get_comment_tags(data->of, data->tags.get());
    }

    sound_params->channels = op_channel_count(data->of, current_section);
    sound_params->rate = 48000;
#if HAVE_OPUSFILE_FLOAT && INTERNAL_FLOAT
    sound_params->fmt = SFMT_FLOAT;
    ret *= sound_params->channels * sizeof(float);
#else
    sound_params->fmt = SFMT_S16 | SFMT_NE;
    ret *= sound_params->channels * sizeof(opus_int16);
#endif
    /* Update the bitrate information */
    bitrate = op_bitrate_instant(data->of);
    if (bitrate > 0)
    {
      data->bitrate = bitrate / 1000;
    }

    break;
  }
  return ret;
}

static int opus_current_tags(void *prv_data, struct file_tags *tags)
{
  struct opus_data *data = static_cast<struct opus_data *>(prv_data);

  *tags = *data->tags;

  if (data->tags_change)
  {
    data->tags_change = 0;
    return 1;
  }

  return 0;
}

static int opus_get_bitrate(void *prv_data)
{
  struct opus_data *data = static_cast<struct opus_data *>(prv_data);

  return data->bitrate;
}

static int opus_get_avg_bitrate(void *prv_data)
{
  struct opus_data *data = static_cast<struct opus_data *>(prv_data);

  return data->avg_bitrate;
}

static int opus_get_duration(void *prv_data)
{
  struct opus_data *data = static_cast<struct opus_data *>(prv_data);

  return data->duration;
}

static struct io_stream *opus_get_stream(void *prv_data)
{
  struct opus_data *data = static_cast<struct opus_data *>(prv_data);

  return data->stream.get();
}

static std::string opus_get_name(const char *unused ATTR_UNUSED)
{
  return "OPUS";
}

static int opus_our_format_ext(const char *ext)
{
  return !strcasecmp(ext, "opus");
}

static void opus_get_error(void *prv_data, struct decoder_error *error)
{
  struct opus_data *data = static_cast<struct opus_data *>(prv_data);

  decoder_error_copy(error, &data->error);
}

static int opus_our_mime(const char *mime)
{
  return !strcasecmp(mime, "audio/ogg") ||
         !strcasecmp(mime, "audio/ogg; codecs=opus");
}


class OpusDecoder : public AudioDecoder {
public:
    std::unique_ptr<void, void(*)(void*)> data;
    OpusDecoder(void *d) : data(d, opus_close) {}
    ~OpusDecoder() override = default;
    
    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return opus_decodeX(data.get(), buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return opus_seek(data.get(), sec);
    }

    int get_bitrate() override {
        return opus_get_bitrate(data.get());
    }

    int get_duration() override {
        return opus_get_duration(data.get());
    }

    void get_error(struct decoder_error *error) override {
        opus_get_error(data.get(), error);
    }

    int current_tags(struct file_tags *tags) override {
        return opus_current_tags(data.get(), tags);
    }

    struct io_stream *get_stream() override {
        return opus_get_stream(data.get());
    }

    int get_avg_bitrate() override {
        return opus_get_avg_bitrate(data.get());
    }
};

class OpusPlugin : public AudioPlugin {
public:

    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = opus_open(file);
        if (!d) return nullptr;
        return std::make_unique<OpusDecoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        opus_tags(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return opus_our_format_ext(ext);
    }

    const char *our_format_data(const char *buf, size_t len) override {
        /* Ogg capture pattern, then the Opus id header in the first page. */
        if (len >= 4 && !memcmp(buf, "OggS", 4)) {
            for (size_t i = 28; i + 8 <= len; i++) {
                if (!memcmp(buf + i, "OpusHead", 8)) {
                    return "OPUS";
                }
            }
        }
        return nullptr;
    }

    int our_format_mime(const char *mime) override {
        return opus_our_mime(mime);
    }

    std::string get_name(const char *file) override {
        return opus_get_name(file);
    }
};

extern "C" class AudioPlugin *opus_plugin_init() {
    static OpusPlugin plugin;
    return &plugin;
}




// EOF
