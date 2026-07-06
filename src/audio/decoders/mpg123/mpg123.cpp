// src/audio/decoders/mpg123/mpg123.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2002 - 2005 Damian Pietras <daper@daper.net>
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
#include <string>
#include <mpg123.h>

#define DEBUG

#include "core/common.h"
#include "core/log.h"
#include "audio/decoder.h"
#include "io/io.h"
#include "audio/audio.h"

struct mpg123_data
{
  struct io_stream *stream;
  mpg123_handle *mf;
  int bitrate;
  int avg_bitrate;
  int duration;
  int sample_rate;
  int channels;
  int encoding;
  struct decoder_error error;
  int ok;          /* was this stream successfully opened? */
  int tags_change; /* the tags were changed from the last call decode function
                    */
  std::unique_ptr<struct file_tags> tags;
};

// ID3v1 tag values may not be null-terminated. Truncate trailing spaces and
// zeros.
std::string safe_string(char text[30])
{
  int n = 0;
  while (n < 29 && text[n] != 0)
  {
    n++;
  }
  while (n > 0 && text[n] == ' ')
  {
    n--;
  }
  return std::string(text, n + 1);
}

static void get_tags(mpg123_handle *mf, struct file_tags *info)
{
  mpg123_id3v1 *v1;
  mpg123_id3v2 *v2;
  int meta;

  meta = mpg123_meta_check(mf);
  if (meta & MPG123_ID3 && mpg123_id3(mf, &v1, &v2) == MPG123_OK)
  {
    if (v2)
    {
      debug("TG: v2 tags present");
      if (v2->title && v2->title->p)
      {
        info->title = v2->title->p;
        debug("TG: title v2 %s.", info->title.c_str());
      }
      if (v2->artist && v2->artist->p)
      {
        info->artist = v2->artist->p;
        debug("TG: artist v2 %s.", info->artist.c_str());
      }
      if (v2->album && v2->album->p)
      {
        info->album = v2->album->p;
        debug("TG: album v2 %s.", info->album.c_str());
      }

      size_t i, j;
      for (i = 0; i < v2->texts; ++i)
      {
        char tag_id[5];
        memcpy(tag_id, v2->text[i].id, 4);
        tag_id[4] = 0;

        debug("TG: field id: %s, value v2: %s.", tag_id, v2->text[i].text.p);
        if (strcmp(tag_id, "TRCK") == 0)
        {
          debug("TG: track number found.");

          for (j = 0; j < v2->text[i].text.fill; ++j)
          {
            if (v2->text[i].text.p[j] == '/')
            {
              break;
            }
          }
          if (j > 0)
          {
            std::string num(v2->text[i].text.p, j);
            long track_num = strtol(num.c_str(), nullptr, 10);
            if (track_num > 0)
            {
              info->track = static_cast<int>(track_num);
            }
            debug("TG: track v2 %d.", info->track);
          }
        }
      }
    }

    if (v1)
    {
      debug("TG: v1 tags present");

      if (info->title.empty())
      {
        info->title = safe_string(v1->title);
        debug("TG: title v1 %s.", info->title.c_str());
      }
      if (info->artist.empty())
      {
        info->artist = safe_string(v1->artist);
        debug("TG: artist v1 %s.", info->artist.c_str());
      }
      if (info->album.empty())
      {
        info->album = safe_string(v1->album);
        debug("TG: album v1 %s.", info->album.c_str());
      }
      if (info->track == -1 && v1->comment[28] == 0 && v1->comment[29] > 0)
      {
        info->track = static_cast<int>(v1->comment[29]);
        debug("TG: track v1 %d.", info->track);
      }
    }
    mpg123_meta_free(mf);
  }
}

static void mpg123_tags(const char *file_name, struct file_tags *info,
                        const int tags_sel)
{
  mpg123_handle *mf;
  int res;
  int ch, enc;
  long rate;
  off_t samples;
#if MPG123_API_VERSION < 46
  mpg123_init();
#endif
  mf = mpg123_new(nullptr, &res);
  if (mf == nullptr || mpg123_open(mf, file_name) != MPG123_OK ||
      mpg123_getformat(mf, &rate, &ch, &enc) != MPG123_OK)
  {
    logit("Can't open file %s:", file_name);
    mpg123_delete(mf);
    return;
  }

  if (tags_sel & TAGS_COMMENTS)
  {
    get_tags(mf, info);
  }

  if (tags_sel & TAGS_TIME)
  {
    samples = mpg123_length(mf);
    if (samples > 0)
    {
      info->time = samples / rate;
    }
    debug("Duration tags: %d, samples %lld", info->time, (long long)samples);
  }

  mpg123_delete(mf);
}

static ssize_t read_cb(void *datasource, void *ptr, size_t bytes)
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

static off_t seek_cb(void *datasource, off_t offset, int whence)
{
  debug("Seek request to %" PRId64 " (%s)", (int64_t)offset,
        whence == SEEK_SET ? "SEEK_SET"
                           : (whence == SEEK_CUR ? "SEEK_CUR" : "SEEK_END"));
  off_t res = io_seek(static_cast<io_stream *>(datasource), offset, whence);
  return res;
}

static void mpg123_open_stream_internal(struct mpg123_data *data)
{
  int res;
  int ch, enc;
  long rate;
  struct mpg123_frameinfo info;
  off_t file_size, samples;

  data->tags = std::make_unique<file_tags>();

#if MPG123_API_VERSION < 46
  mpg123_init();
#endif

  data->mf = mpg123_new(nullptr, &res);

  if (data->mf == nullptr)
  {
    goto err;
  }

  res = mpg123_replace_reader_handle(data->mf, read_cb, seek_cb, nullptr);
  if (res != MPG123_OK)
  {
    goto err;
  }

  const long *rates;
  size_t rate_count;
  size_t i;
  res = mpg123_format_none(data->mf);
  if (res != MPG123_OK)
  {
    goto err;
  }
  mpg123_rates(&rates, &rate_count);

#ifdef INTERNAL_FLOAT
  data->encoding = SFMT_FLOAT | SFMT_NE;
  debug("TG: selected FLOAT");
  for (i = 0; i < rate_count; ++i)
  {
    switch (sizeof(float))
    {
      case 4:
        res = mpg123_format(data->mf, rates[i], MPG123_MONO | MPG123_STEREO,
                            MPG123_ENC_FLOAT_32);
        break;
      case 8:
        res = mpg123_format(data->mf, rates[i], MPG123_MONO | MPG123_STEREO,
                            MPG123_ENC_FLOAT_64);
        break;
      default:
        res = mpg123_format(data->mf, rates[i], MPG123_MONO | MPG123_STEREO,
                            MPG123_ENC_SIGNED_32);
        data->encoding = SFMT_S32 | SFMT_NE;
        debug("TG: unsupported sizeof(float): %zu, falling back to S32",
              sizeof(float));
        break;
    }
    if (res != MPG123_OK)
    {
      goto err;
    }
  }
#else
  for (i = 0; i < rate_count; ++i)
  {
    res = mpg123_format(data->mf, rates[i], MPG123_MONO | MPG123_STEREO,
                        MPG123_ENC_SIGNED_32);
    if (res != MPG123_OK)
    {
      goto err;
    }
  }
  data->encoding = SFMT_S32 | SFMT_NE;
  debug("TG: selected S32");
#endif

  res = mpg123_open_handle(data->mf, data->stream);
  if (res != MPG123_OK)
  {
    goto err;
  }
  res = mpg123_getformat(data->mf, &rate, &ch, &enc);
  if (res != MPG123_OK)
  {
    goto err;
  }
  debug("Encoding: %i, sample rate: %li, channels: %i", enc, rate, ch);
  data->sample_rate = rate;
  data->channels = ch;

  res = mpg123_info(data->mf, &info);
  if (res != MPG123_OK)
  {
    goto err;
  }
  debug("Bitrate %i", info.bitrate);
  data->bitrate = info.bitrate;

  res = mpg123_scan(data->mf);
  if (res != MPG123_OK)
  {
    goto err;
  }
  samples = mpg123_length(data->mf);
  if (samples == MPG123_ERR)
  {
    data->duration = -1;
  }
  else
  {
    data->duration = samples / rate;
  }
  debug("Duration: %d, samples %lld", data->duration, (long long)samples);
  file_size = io_file_size(data->stream);
  if (data->duration > 0 && file_size != -1)
  {
    data->avg_bitrate = file_size / data->duration * 8;
  }
  get_tags(data->mf, data->tags.get());

  debug("TG: active mpg123 decoder %s", mpg123_current_decoder(data->mf));

  data->ok = 1;
  return;

err:
  {
    const char *mpg123_err = mpg123_strerror(data->mf);
    decoder_error(&data->error, ERROR_FATAL, 0, "%s", mpg123_err);
    debug("mpg123 error: %s", mpg123_err);
  }
  mpg123_delete(data->mf);
  data->mf = nullptr;
  io_close(data->stream);
}

static void *mpg123_openX(const char *file)
{
  struct mpg123_data *data;
  data = new mpg123_data;
  data->ok = 0;

  decoder_error_init(&data->error);
  data->tags_change = 0;
  data->tags = nullptr;

  data->stream = io_open(file, 1);
  if (!io_ok(data->stream))
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't open mpg123 file: %s",
                  io_strerror(data->stream));
    io_close(data->stream);
  }
  else
  {
    mpg123_open_stream_internal(data);
  }
  return data;
}

static void mpg123_closeX(void *prv_data)
{
  struct mpg123_data *data = static_cast<struct mpg123_data *>(prv_data);

  if (data->ok)
  {
    mpg123_delete(data->mf);
    io_close(data->stream);
  }

  decoder_error_clear(&data->error);
  delete data;
}

static int mpg123_seekX(void *prv_data, int sec)
{
  struct mpg123_data *data = static_cast<struct mpg123_data *>(prv_data);

  assert(sec >= 0);

  return mpg123_seek(data->mf, sec * data->sample_rate, SEEK_SET) < 0 ? -1
                                                                      : sec;
}

static int mpg123_decodeX(void *prv_data, char *buf, int buf_len,
                          struct sound_params *sound_params)
{
  struct mpg123_data *data = static_cast<struct mpg123_data *>(prv_data);
  int ret;
  size_t decoded_bytes;
  struct mpg123_frameinfo info;

  int ch, enc;
  long rate;

  decoder_error_clear(&data->error);

  while (true)
  {
    ret = mpg123_read(data->mf, reinterpret_cast<unsigned char *>(buf), buf_len, &decoded_bytes);

    if (ret != MPG123_OK && ret != MPG123_DONE && ret != MPG123_NEW_FORMAT)
    {
      decoder_error(&data->error, ERROR_STREAM, 0, "Error in the stream: %s",
                    mpg123_plain_strerror(ret));
      debug("mpg123 decoder error: %s", mpg123_plain_strerror(ret));
      if (decoded_bytes == 0)
      {
        continue; // try to play more
      }
      else
      {
        break; // play what you decoded
      }
    }

    if (ret == MPG123_DONE)
    {
      return 0;
    }
    else if (decoded_bytes == 0)
    {
      continue;
    }

    if (ret == MPG123_NEW_FORMAT)
    {
      mpg123_getformat(data->mf, &rate, &ch, &enc);
      debug("Encoding change: %i, sample rate: %li, channels: %i", enc, rate,
            ch);
      data->sample_rate = rate;
      data->channels = ch;
    }

    if (mpg123_meta_check(data->mf) & MPG123_NEW_ID3)
    {
      logit("Tags change");
      data->tags_change = 1;
      data->tags = std::make_unique<file_tags>();
      get_tags(data->mf, data->tags.get());
    }

    sound_params->channels = data->channels;
    sound_params->rate = data->sample_rate;
    sound_params->fmt = data->encoding;

    /* Update the bitrate information */
    mpg123_info(data->mf, &info);
    if (info.bitrate > 0)
    {
      data->bitrate = info.bitrate;
    }

    break;
  }
  return static_cast<int>(decoded_bytes);
}

static int mpg123_current_tags(void *prv_data, struct file_tags *tags)
{
  struct mpg123_data *data = static_cast<struct mpg123_data *>(prv_data);

  *tags = *data->tags;

  if (data->tags_change)
  {
    data->tags_change = 0;
    return 1;
  }

  return 0;
}

static int mpg123_get_bitrate(void *prv_data)
{
  struct mpg123_data *data = static_cast<struct mpg123_data *>(prv_data);

  return data->bitrate;
}

static int mpg123_get_avg_bitrate(void *prv_data)
{
  struct mpg123_data *data = static_cast<struct mpg123_data *>(prv_data);

  return data->avg_bitrate;
}

static int mpg123_get_duration(void *prv_data)
{
  struct mpg123_data *data = static_cast<struct mpg123_data *>(prv_data);

  return data->duration;
}

static struct io_stream *mpg123_get_stream(void *prv_data)
{
  struct mpg123_data *data = static_cast<struct mpg123_data *>(prv_data);

  return data->stream;
}

static void mpg123_get_name(const char *file ATTR_UNUSED, char buf[4])
{
  std::memcpy(buf, "123", sizeof("123"));
}

static int mpg123_our_format_ext(const char *ext)
{
  return !strcasecmp(ext, "mp3");
}

static void mpg123_get_error(void *prv_data, struct decoder_error *error)
{
  struct mpg123_data *data = static_cast<struct mpg123_data *>(prv_data);

  decoder_error_copy(error, &data->error);
}

static int mpg123_our_mime(const char *mime)
{
  return !strcasecmp(mime, "audio/mpeg") ||
         !strncasecmp(mime, "audio/mpeg;", 11);
}


class Mpg123Decoder : public AudioDecoder {
public:
    std::unique_ptr<void, void(*)(void*)> data;
    Mpg123Decoder(void *d) : data(d, mpg123_closeX) {}
    ~Mpg123Decoder() override = default;
    
    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return mpg123_decodeX(data.get(), buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return mpg123_seekX(data.get(), sec);
    }

    int get_bitrate() override {
        return mpg123_get_bitrate(data.get());
    }

    int get_duration() override {
        return mpg123_get_duration(data.get());
    }

    void get_error(struct decoder_error *error) override {
        mpg123_get_error(data.get(), error);
    }

    int current_tags(struct file_tags *tags) override {
        return mpg123_current_tags(data.get(), tags);
    }

    struct io_stream *get_stream() override {
        return mpg123_get_stream(data.get());
    }

    int get_avg_bitrate() override {
        return mpg123_get_avg_bitrate(data.get());
    }
};

class Mpg123Plugin : public AudioPlugin {
public:

    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = mpg123_openX(file);
        if (!d) return nullptr;
        return std::make_unique<Mpg123Decoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        mpg123_tags(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return mpg123_our_format_ext(ext);
    }

    int our_format_mime(const char *mime) override {
        return mpg123_our_mime(mime);
    }

    void get_name(const char *file, char buf[4]) override {
        mpg123_get_name(file, buf);
    }
};

extern "C" class AudioPlugin *mpg123_plugin_init() {
    static Mpg123Plugin plugin;
    return &plugin;
}




// EOF
