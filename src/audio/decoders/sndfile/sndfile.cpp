// src/audio/decoders/sndfile/sndfile.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
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
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sndfile.h>

#define DEBUG

#include "core/common.h"
#include "audio/decoder.h"
#include "core/server.h"
#include "core/log.h"
#include "library/files.h"

#include <algorithm>
#include <string>
#include <vector>

/* TODO:
 * - sndfile is not thread-safe: use a mutex?
 * - some tags can be read.
 */

struct sndfile_data
{
  SNDFILE *sndfile;
  SF_INFO snd_info;
  struct decoder_error error;
  bool timing_broken;
  int bitrate;
};

static std::vector<std::string> supported_extns;

static bool extn_exists(const std::string &ext)
{
  return std::find(supported_extns.begin(), supported_extns.end(), ext) !=
         supported_extns.end();
}

static void load_extn_list()
{
  const int counts[] = {SFC_GET_SIMPLE_FORMAT_COUNT,
                        SFC_GET_FORMAT_MAJOR_COUNT};
  const int formats[] = {SFC_GET_SIMPLE_FORMAT, SFC_GET_FORMAT_MAJOR};

  supported_extns.clear();
  supported_extns.reserve(16);

  for (size_t ix = 0; ix < ARRAY_SIZE(counts); ix += 1)
  {
    int limit;
    SF_FORMAT_INFO format_info;

    sf_command(nullptr, counts[ix], &limit, sizeof(limit));
    for (int iy = 0; iy < limit; iy += 1)
    {
      format_info.format = iy;
      sf_command(nullptr, formats[ix], &format_info, sizeof(format_info));
      if (!extn_exists(format_info.extension))
      {
        supported_extns.push_back(format_info.extension);
      }
    }
  }

  /* These are synonyms of supported extensions. */
  if (extn_exists("aiff"))
  {
    supported_extns.push_back("aif");
  }
  if (extn_exists("au"))
  {
    supported_extns.push_back("snd");
  }
  if (extn_exists("wav"))
  {
    supported_extns.push_back("nist");
    supported_extns.push_back("sph");
  }
  if (extn_exists("iff"))
  {
    supported_extns.push_back("svx");
  }
  if (extn_exists("oga"))
  {
    supported_extns.push_back("ogg");
  }
  if (extn_exists("sf"))
  {
    supported_extns.push_back("ircam");
  }
  if (extn_exists("mat"))
  {
    supported_extns.push_back("mat4");
    supported_extns.push_back("mat5");
  }
}

static void sndfile_init() { load_extn_list(); }

static void sndfile_destroy() { supported_extns.clear(); }

/* Return true iff libsndfile's frame count is unknown or miscalculated. */
static bool is_timing_broken(int fd, struct sndfile_data *data)
{
  int rc;
  struct stat buf;
  SF_INFO *info = &data->snd_info;

  if (info->frames == SF_COUNT_MAX)
  {
    return true;
  }

  if (info->frames / info->samplerate > INT32_MAX)
  {
    return true;
  }

  /* The libsndfile code warns of miscalculation for huge files of
   * specific formats, but it's unclear if others are known to work
   * or the test is just omitted for them.  We'll assume they work
   * until it's shown otherwise. */
  switch (info->format & SF_FORMAT_TYPEMASK)
  {
    case SF_FORMAT_AIFF:
    case SF_FORMAT_AU:
    case SF_FORMAT_SVX:
    case SF_FORMAT_WAV:
      rc = fstat(fd, &buf);
      if (rc == -1)
      {
        log_errno("Can't stat file", errno);
        /* We really need to return "unknown" here. */
        return false;
      }

      if (buf.st_size > UINT32_MAX)
      {
        return true;
      }
  }

  return false;
}

static void *sndfile_open(const char *file)
{
  int fd;
  struct sndfile_data *data;

  data = new sndfile_data;

  decoder_error_init(&data->error);
  memset(&data->snd_info, 0, sizeof(data->snd_info));
  data->sndfile = nullptr;
  data->timing_broken = false;
  data->bitrate = -1;

  fd = open(file, O_RDONLY);
  if (fd == -1)
  {
    std::string err = xstrerror(errno);
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't open file: %s", err.c_str());
    return data;
  }

  /* sf_open_fd() close()s 'fd' on error and in sf_close(). */
  data->sndfile = sf_open_fd(fd, SFM_READ, &data->snd_info, SF_TRUE);
  if (!data->sndfile)
  {
    /* FIXME: sf_strerror is not thread safe with NULL argument */
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't open file: %s",
                  sf_strerror(nullptr));
    return data;
  }

  /* If the timing is broken, sndfile only decodes up to the broken value. */
  data->timing_broken = is_timing_broken(fd, data);
  if (data->timing_broken)
  {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "File too large for audio format!");
    return data;
  }

#ifdef HAVE_SNDFILE_BYTERATE
  data->bitrate = sf_current_byterate(data->sndfile);
  if (data->bitrate > 0)
  {
    data->bitrate = data->bitrate * 8 / 1000;
  }
#endif

  debug("Opened file %s", file);
  debug("Channels: %d", data->snd_info.channels);
  debug("Format: %08X", data->snd_info.format);
  debug("Sample rate: %d", data->snd_info.samplerate);
  debug("Bitrate: %d", data->bitrate);

#ifndef INTERNAL_FLOAT
  if ((data->snd_info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_FLOAT ||
      (data->snd_info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_DOUBLE)
  {
    sf_command(data->sndfile, SFC_SET_SCALE_FLOAT_INT_READ, NULL, SF_TRUE);
  }
#endif
  return data;
}

static void sndfile_close(void *void_data)
{
  struct sndfile_data *data = static_cast<struct sndfile_data *>(void_data);

  if (data->sndfile)
  {
    sf_close(data->sndfile);
  }

  decoder_error_clear(&data->error);
  delete data;
}

static void sndfile_info(const char *file_name, struct file_tags *info,
                         const int tags_sel)
{
  struct sndfile_data *data;
  data = static_cast<struct sndfile_data *>(sndfile_open(file_name));
  if (!data->sndfile)
  {
    sndfile_close(data);
    return;
  }

  if ((tags_sel & TAGS_TIME) && !data->timing_broken)
  {
    info->time = data->snd_info.frames / data->snd_info.samplerate;
  }

  if (tags_sel & TAGS_COMMENTS)
  {
    const char *res;
    if ((res = sf_get_string(data->sndfile, SF_STR_TITLE)))
    {
      info->title = res;
    }
    if ((res = sf_get_string(data->sndfile, SF_STR_ARTIST)))
    {
      info->artist = res;
    }
    if ((res = sf_get_string(data->sndfile, SF_STR_ALBUM)))
    {
      info->album = res;
    }
    if ((res = sf_get_string(data->sndfile, SF_STR_TRACKNUMBER)))
    {
      info->track = static_cast<int>(strtol(res, nullptr, 10));
    }
  }

  sndfile_close(data);
}

static int sndfile_seek(void *void_data, int sec)
{
  struct sndfile_data *data = static_cast<struct sndfile_data *>(void_data);
  int res;

  assert(sec >= 0);

  res = sf_seek(data->sndfile, data->snd_info.samplerate * sec, SEEK_SET);

  if (res < 0)
  {
    return -1;
  }

  return res / data->snd_info.samplerate;
}

static int sndfile_decode(void *void_data, char *buf, int buf_len,
                          struct sound_params *sound_params)
{
  struct sndfile_data *data = static_cast<struct sndfile_data *>(void_data);
  int use_float = 0;
  int res;

  sound_params->channels = data->snd_info.channels;
  sound_params->rate = data->snd_info.samplerate;

#ifdef INTERNAL_FLOAT
  switch (data->snd_info.format & SF_FORMAT_SUBMASK)
  {
    case SF_FORMAT_FLOAT:
    case SF_FORMAT_DOUBLE:
    case SF_FORMAT_VORBIS:
      sound_params->fmt = SFMT_FLOAT;
      use_float = 1;
  }
#endif

  if (!use_float)
  {
    switch (sizeof(int))
    {
      case 4:
        sound_params->fmt = SFMT_S32 | SFMT_NE;
        break;
      case 2:
        sound_params->fmt = SFMT_S16 | SFMT_NE;
        break;
      default:
        logit("sizeof(int)=%d is not supported. Please report this error.\
						Falling back to float decoding.",
              (int)sizeof(int));
        sound_params->fmt = SFMT_FLOAT;
        use_float = 1;
    }
  }

  if (use_float)
  {
    res = sf_readf_float(data->sndfile, reinterpret_cast<float *>(buf),
                         buf_len / sizeof(float) / data->snd_info.channels) *
          sizeof(float) * data->snd_info.channels;
  }
  else
  {
    res = sf_readf_int(data->sndfile, reinterpret_cast<int *>(buf),
                       buf_len / sizeof(int) / data->snd_info.channels) *
          sizeof(int) * data->snd_info.channels;
  }

#ifdef HAVE_SNDFILE_BYTERATE
  data->bitrate = sf_current_byterate(data->sndfile);
  if (data->bitrate > 0)
  {
    data->bitrate = data->bitrate * 8 / 1000;
  }
#endif

  return res;
}

static int sndfile_get_bitrate(void *void_data)
{
  struct sndfile_data *data = static_cast<struct sndfile_data *>(void_data);

  return data->bitrate;
}

static int sndfile_get_duration(void *void_data)
{
  int result;
  struct sndfile_data *data = static_cast<struct sndfile_data *>(void_data);

  result = -1;
  if (!data->timing_broken)
  {
    result = data->snd_info.frames / data->snd_info.samplerate;
  }

  return result;
}

static void sndfile_get_name(const char *file, char buf[4])
{
  char *ext;

  ext = ext_pos(file);
  if (ext)
  {
    if (!strcasecmp(ext, "snd"))
    {
      strcpy(buf, "AU");
    }
    else if (!strcasecmp(ext, "8svx"))
    {
      strcpy(buf, "SVX");
    }
    else if (!strcasecmp(ext, "oga"))
    {
      strcpy(buf, "OGG");
    }
    else if (!strcasecmp(ext, "sf") || !strcasecmp(ext, "icram"))
    {
      strcpy(buf, "IRC");
    }
    else if (!strcasecmp(ext, "mat4") || !strcasecmp(ext, "mat5"))
    {
      strcpy(buf, "MAT");
    }
  }
}

static int sndfile_our_format_ext(const char *ext)
{
  return extn_exists(ext);
}

static void sndfile_get_error(void *prv_data, struct decoder_error *error)
{
  struct sndfile_data *data = static_cast<struct sndfile_data *>(prv_data);

  decoder_error_copy(error, &data->error);
}


class SndfileDecoder : public AudioDecoder {
public:
    void *data;
    SndfileDecoder(void *d) : data(d) {}
    ~SndfileDecoder() override { sndfile_close(data); }
    
    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return sndfile_decode(data, buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return sndfile_seek(data, sec);
    }

    int get_bitrate() override {
        return sndfile_get_bitrate(data);
    }

    int get_duration() override {
        return sndfile_get_duration(data);
    }

    void get_error(struct decoder_error *error) override {
        sndfile_get_error(data, error);
    }
};

class SndfilePlugin : public AudioPlugin {
public:

    void init() override {
        sndfile_init();
    }

    void destroy() override {
        sndfile_destroy();
    }

    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = sndfile_open(file);
        if (!d) return nullptr;
        return std::make_unique<SndfileDecoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        sndfile_info(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return sndfile_our_format_ext(ext);
    }

    void get_name(const char *file, char buf[4]) override {
        sndfile_get_name(file, buf);
    }
};

extern "C" class AudioPlugin *sndfile_plugin_init() {
    static SndfilePlugin plugin;
    return &plugin;
}




// EOF
