// src/audio/decoders/wavpack/wavpack.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// libwavpack-plugin Copyright (C) 2006 Alexandrov Sergey <splav@unsorted.ru>
// Enables MOC to play wavpack files (actually just a wrapper around
// wavpack library).
// Structure of this plugin is an adaption of the libvorbis-plugin from
// moc.
// Copyright (C) 2004 Damian Pietras <daper@daper.net>
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
#include <wavpack/wavpack.h>

#define DEBUG

#include "core/common.h"
#include "core/log.h"
#include "audio/decoder.h"
#include "io/io.h"
#include "audio/audio.h"

struct wavpack_data
{
  WavpackContext *wpc;
  int sample_num;
  int sample_rate;
  int avg_bitrate;
  int channels;
  int duration;
  int mode;
  struct decoder_error error;
  int ok; /* was this stream successfully opened? */
};

static void wav_data_init(struct wavpack_data *data)
{
  data->sample_num = WavpackGetNumSamples(data->wpc);
  data->sample_rate = WavpackGetSampleRate(data->wpc);
  data->channels = WavpackGetNumChannels(data->wpc);
  data->duration = data->sample_num / data->sample_rate;
  data->mode = WavpackGetMode(data->wpc);
  data->avg_bitrate = WavpackGetAverageBitrate(data->wpc, 1) / 1000;

  data->ok = 1;
  debug(
      "File opened. S_num %d. S_rate %d. Time %d. Avg_Bitrate %d. Channels %d",
      data->sample_num, data->sample_rate, data->duration, data->avg_bitrate,
      data->channels);
}

static void *wav_open(const char *file)
{
  struct wavpack_data *data;
  data = new wavpack_data;
  data->ok = 0;
  decoder_error_init(&data->error);

  int o_flags = OPEN_WVC;

  char wv_error[100];

  if ((data->wpc = WavpackOpenFileInput(file, wv_error, o_flags, 0)) == nullptr)
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "%s", wv_error);
    logit("wv_open error: %s", wv_error);
  }
  else
  {
    wav_data_init(data);
  }

  return data;
}

static void wav_close(void *prv_data)
{
  struct wavpack_data *data = static_cast<struct wavpack_data *>(prv_data);

  if (data->ok)
  {
    WavpackCloseFile(data->wpc);
  }

  decoder_error_clear(&data->error);
  delete data;
  logit("File closed");
}

static int wav_seek(void *prv_data, int sec)
{
  struct wavpack_data *data = static_cast<struct wavpack_data *>(prv_data);

  assert(sec >= 0);

  if (WavpackSeekSample(data->wpc, sec * data->sample_rate))
  {
    return sec;
  }

  decoder_error(&data->error, ERROR_FATAL, 0, "Fatal seeking error!");
  return -1;
}

static int wav_get_bitrate(void *prv_data)
{
  struct wavpack_data *data = static_cast<struct wavpack_data *>(prv_data);

  int bitrate;
  bitrate = WavpackGetInstantBitrate(data->wpc) / 1000;

  return (bitrate == 0) ? data->avg_bitrate : bitrate;
}

static int wav_get_avg_bitrate(void *prv_data)
{
  struct wavpack_data *data = static_cast<struct wavpack_data *>(prv_data);

  return data->avg_bitrate;
}

static int wav_get_duration(void *prv_data)
{
  struct wavpack_data *data = static_cast<struct wavpack_data *>(prv_data);
  return data->duration;
}

static void wav_get_error(void *prv_data, struct decoder_error *error)
{
  struct wavpack_data *data = static_cast<struct wavpack_data *>(prv_data);
  decoder_error_copy(error, &data->error);
}

static void wav_info(const char *file_name, struct file_tags *info,
                     const int tags_sel)
{
  char wv_error[100];
  char *tag;
  int tag_len;

  WavpackContext *wpc;

  wpc = WavpackOpenFileInput(file_name, wv_error, OPEN_TAGS, 0);

  if (wpc == nullptr)
  {
    logit("wv_open error: %s", wv_error);
    return;
  }

  int duration = WavpackGetNumSamples(wpc) / WavpackGetSampleRate(wpc);

  if (tags_sel & TAGS_TIME)
  {
    info->time = duration;
    info->filled |= TAGS_TIME;
  }

  if (tags_sel & TAGS_COMMENTS)
  {
    if ((tag_len = WavpackGetTagItem(wpc, "title", nullptr, 0)) > 0)
    {
      std::vector<char> buf(++tag_len);
      WavpackGetTagItem(wpc, "title", buf.data(), tag_len);
      info->title = buf.data();
    }

    if ((tag_len = WavpackGetTagItem(wpc, "artist", nullptr, 0)) > 0)
    {
      std::vector<char> buf(++tag_len);
      WavpackGetTagItem(wpc, "artist", buf.data(), tag_len);
      info->artist = buf.data();
    }

    if ((tag_len = WavpackGetTagItem(wpc, "album", nullptr, 0)) > 0)
    {
      std::vector<char> buf(++tag_len);
      WavpackGetTagItem(wpc, "album", buf.data(), tag_len);
      info->album = buf.data();
    }

    if ((tag_len = WavpackGetTagItem(wpc, "track", nullptr, 0)) > 0)
    {
      std::vector<char> buf(++tag_len);
      WavpackGetTagItem(wpc, "track", buf.data(), tag_len);
      info->track = static_cast<int>(strtol(buf.data(), nullptr, 10));
    }

    info->filled |= TAGS_COMMENTS;
  }

  WavpackCloseFile(wpc);
}

static int wav_decode(void *prv_data, char *buf, int buf_len,
                      struct sound_params *sound_params)
{
  struct wavpack_data *data = static_cast<struct wavpack_data *>(prv_data);
  int ret, i, s_num, Bps, iBps, oBps;

  int8_t *buf8 = reinterpret_cast<int8_t *>(buf);
  int16_t *buf16 = reinterpret_cast<int16_t *>(buf);
  int32_t *buf32 = reinterpret_cast<int32_t *>(buf);

  Bps = WavpackGetBytesPerSample(data->wpc);
  iBps = data->channels * Bps;
  oBps = (Bps == 3) ? 4 * data->channels : iBps;
  s_num = buf_len / oBps;

  decoder_error_clear(&data->error);

  std::vector<int32_t> dbuf(s_num * data->channels);

  ret = WavpackUnpackSamples(data->wpc, dbuf.data(), s_num);

  if (ret == 0)
  {
    return 0;
  }

  if (data->mode & MODE_FLOAT)
  {
    sound_params->fmt = SFMT_FLOAT;
    memcpy(buf, dbuf.data(), ret * oBps);
  }
  else
  {
    debug("iBps %d", iBps);
    switch (Bps)
    {
      case 4:
        for (i = 0; i < ret * data->channels; i++)
        {
          buf32[i] = dbuf[i];
        }
        sound_params->fmt = SFMT_S32 | SFMT_NE;
        break;
      case 3:
        for (i = 0; i < ret * data->channels; i++)
        {
          buf32[i] = dbuf[i] * 256;
        }
        sound_params->fmt = SFMT_S32 | SFMT_NE;
        break;
      case 2:
        for (i = 0; i < ret * data->channels; i++)
        {
          buf16[i] = dbuf[i];
        }
        sound_params->fmt = SFMT_S16 | SFMT_NE;
        break;
      case 1:
        for (i = 0; i < ret * data->channels; i++)
        {
          buf8[i] = dbuf[i];
        }
        sound_params->fmt = SFMT_S8 | SFMT_NE;
    }
  }

  sound_params->channels = data->channels;
  sound_params->rate = data->sample_rate;

  return ret * oBps;
}

static int wav_our_mime(const char *mime ATTR_UNUSED)
{
  /* We don't support internet streams for now. */
#if 0
	return !strcasecmp (mime, "audio/x-wavpack")
		|| !strncasecmp (mime, "audio/x-wavpack;", 16)
#endif

  return 0;
}

static std::string wav_get_name(const char *unused ATTR_UNUSED)
{
  return "WV";
}

static int wav_our_format_ext(const char *ext)
{
  return !strcasecmp(ext, "WV");
}


class WavpackDecoder : public AudioDecoder {
public:
    std::unique_ptr<void, void(*)(void*)> data;
    WavpackDecoder(void *d) : data(d, wav_close) {}
    ~WavpackDecoder() override = default;
    
    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return wav_decode(data.get(), buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return wav_seek(data.get(), sec);
    }

    int get_bitrate() override {
        return wav_get_bitrate(data.get());
    }

    int get_duration() override {
        return wav_get_duration(data.get());
    }

    void get_error(struct decoder_error *error) override {
        wav_get_error(data.get(), error);
    }

    int get_avg_bitrate() override {
        return wav_get_avg_bitrate(data.get());
    }
};

class WavpackPlugin : public AudioPlugin {
public:

    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = wav_open(file);
        if (!d) return nullptr;
        return std::make_unique<WavpackDecoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        wav_info(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return wav_our_format_ext(ext);
    }

    int our_format_mime(const char *mime) override {
        return wav_our_mime(mime);
    }

    std::string get_name(const char *file) override {
        return wav_get_name(file);
    }
};

extern "C" class AudioPlugin *wavpack_plugin_init() {
    static WavpackPlugin plugin;
    return &plugin;
}




// EOF
