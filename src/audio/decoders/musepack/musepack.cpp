// src/audio/decoders/musepack/musepack.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2005 Damian Pietras <daper@daper.net>
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
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

#ifdef MPC_IS_OLD_API
#include <mpcdec/mpcdec.h>
#else
#include <mpc/mpcdec.h>
#endif

#include <tag_c.h>

#define DEBUG

#include "core/common.h"
#include "core/log.h"
#include "audio/decoder.h"
#include "io/io.h"
#include "audio/audio.h"

struct musepack_data
{
  struct io_stream *stream;
#ifdef MPC_IS_OLD_API
  mpc_decoder decoder;
#else
  mpc_demux *demux;
#endif
  mpc_reader reader;
  mpc_streaminfo info;
  int avg_bitrate;
  int bitrate;
  struct decoder_error error;
  int ok; /* was this stream successfully opened? */
  std::vector<MPC_SAMPLE_FORMAT> remain_buf;
};

#ifdef MPC_IS_OLD_API
static mpc_int32_t read_cb(void *t, void *buf, mpc_int32_t size)
#else
static mpc_int32_t read_cb(mpc_reader *t, void *buf, mpc_int32_t size)
#endif
{
#ifdef MPC_IS_OLD_API
  struct musepack_data *data = (struct musepack_data *)t;
#else
  struct musepack_data *data = static_cast<struct musepack_data *>(t->data);
#endif
  ssize_t res;

  res = io_read(data->stream, buf, size);
  if (res < 0)
  {
    logit("Read error");
    res = 0;
  }

  return res;
}

#ifdef MPC_IS_OLD_API
static mpc_bool_t seek_cb(void *t, mpc_int32_t offset)
#else
static mpc_bool_t seek_cb(mpc_reader *t, mpc_int32_t offset)
#endif
{
#ifdef MPC_IS_OLD_API
  struct musepack_data *data = (struct musepack_data *)t;
#else
  struct musepack_data *data = static_cast<struct musepack_data *>(t->data);
#endif

  debug("Seek request to %" PRId32, offset);

  return io_seek(data->stream, offset, SEEK_SET) >= 0 ? 1 : 0;
}

#ifdef MPC_IS_OLD_API
static mpc_int32_t tell_cb(void *t)
#else
static mpc_int32_t tell_cb(mpc_reader *t)
#endif
{
#ifdef MPC_IS_OLD_API
  struct musepack_data *data = (struct musepack_data *)t;
#else
  struct musepack_data *data = static_cast<struct musepack_data *>(t->data);
#endif

  debug("tell callback");

  return static_cast<mpc_int32_t>(io_tell(data->stream));
}

#ifdef MPC_IS_OLD_API
static mpc_int32_t get_size_cb(void *t)
#else
static mpc_int32_t get_size_cb(mpc_reader *t)
#endif
{
#ifdef MPC_IS_OLD_API
  struct musepack_data *data = (struct musepack_data *)t;
#else
  struct musepack_data *data = static_cast<struct musepack_data *>(t->data);
#endif

  debug("size callback");

  return static_cast<mpc_int32_t>(io_file_size(data->stream));
}

#ifdef MPC_IS_OLD_API
static mpc_bool_t canseek_cb(void *t)
#else
static mpc_bool_t canseek_cb(mpc_reader *t)
#endif
{
#ifdef MPC_IS_OLD_API
  struct musepack_data *data = (struct musepack_data *)t;
#else
  struct musepack_data *data = static_cast<struct musepack_data *>(t->data);
#endif

  return io_seekable(data->stream);
}

static void musepack_open_stream_internal(struct musepack_data *data)
{
  data->reader.read = read_cb;
  data->reader.seek = seek_cb;
  data->reader.tell = tell_cb;
  data->reader.get_size = get_size_cb;
  data->reader.canseek = canseek_cb;
  data->reader.data = data;

#ifdef MPC_IS_OLD_API
  mpc_streaminfo_init(&data->info);

  if (mpc_streaminfo_read(&data->info, &data->reader) != ERROR_CODE_OK)
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Not a valid MPC file.");
    return;
  }

  mpc_decoder_setup(&data->decoder, &data->reader);

  if (!mpc_decoder_initialize(&data->decoder, &data->info))
  {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "Can't initialize mpc decoder.");
    return;
  }
#else
  data->demux = mpc_demux_init(&data->reader);
  if (!data->demux)
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Not a valid MPC file.");
    return;
  }

  mpc_demux_get_info(data->demux, &data->info);
#endif

  data->avg_bitrate = static_cast<int>(data->info.average_bitrate / 1000);
  debug("Avg bitrate: %d", data->avg_bitrate);

  data->bitrate = 0;
  data->ok = 1;
}

static void *musepack_open(const char *file)
{
  struct musepack_data *data;

  data = new musepack_data;
  data->ok = 0;
  decoder_error_init(&data->error);

  data->stream = io_open(file, 1);
  if (!io_ok(data->stream))
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't open file: %s",
                  io_strerror(data->stream));
    return data;
  }

  /* This a restriction placed on us by the Musepack API. */
  if (io_file_size(data->stream) > INT32_MAX)
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "File too large!");
    return data;
  }

  musepack_open_stream_internal(data);

  return data;
}

static void musepack_close(void *prv_data)
{
  struct musepack_data *data = static_cast<struct musepack_data *>(prv_data);

  if (data->ok)
  {
#ifndef MPC_IS_OLD_API
    mpc_demux_exit(data->demux);
#endif
  }

  io_close(data->stream);
  decoder_error_clear(&data->error);
  delete data;
}

static std::string tag_str(const char *str)
{
  return str ? str : "";
}

/* Fill info structure with data from musepack comments */
static void musepack_info(const char *file_name, struct file_tags *info,
                          const int tags_sel)
{
  if (tags_sel & TAGS_COMMENTS)
  {
    TagLib_File *tf;

    tf = taglib_file_new_type(file_name, TagLib_File_MPC);
    if (tf)
    {
      TagLib_Tag *tt;

      tt = taglib_file_tag(tf);

      if (tt)
      {
        info->title = tag_str(taglib_tag_title(tt));
        info->artist = tag_str(taglib_tag_artist(tt));
        info->album = tag_str(taglib_tag_album(tt));
        info->track = taglib_tag_track(tt);

        if (info->track == 0)
        {
          info->track = -1;
        }
      }

      taglib_file_free(tf);
      taglib_tag_free_strings();
    }
    else
    {
      logit("taglib_file_new_type() failed.");
    }
  }

  if (tags_sel & TAGS_TIME)
  {
    struct musepack_data *data = static_cast<struct musepack_data *>(musepack_open(file_name));

    if (data->error.type == ERROR_OK)
    {
      info->time = mpc_streaminfo_get_length(&data->info);
    }

    musepack_close(data);
  }
}

static int musepack_seek(void *prv_data, int sec)
{
  struct musepack_data *data = static_cast<struct musepack_data *>(prv_data);
  int res;

  assert(sec >= 0);

#ifdef MPC_IS_OLD_API
  res = mpc_decoder_seek_seconds(&data->decoder, sec) ? sec : -1;
#else
  mpc_status status;
  status = mpc_demux_seek_second(data->demux, sec);
  if (status == MPC_STATUS_OK)
  {
    res = sec;
  }
  else
  {
    res = -1;
  }
#endif

  if (res != -1 && !data->remain_buf.empty())
  {
    data->remain_buf.clear();
  }

  return res;
}

static int musepack_decode(void *prv_data, char *buf, int buf_len,
                           struct sound_params *sound_params)
{
  struct musepack_data *data = static_cast<struct musepack_data *>(prv_data);
  int decoded;
  int bytes_from_decoder;
#ifndef MPC_IS_OLD_API
  mpc_frame_info frame;
  mpc_status err;
#else
  int ret;
  mpc_uint32_t vbrAcc = 0;
  mpc_uint32_t vbrUpd = 0;
#endif
  MPC_SAMPLE_FORMAT decode_buf[MPC_DECODER_BUFFER_LENGTH];
  if (!data->remain_buf.empty())
  {
    size_t to_copy =
        std::min(static_cast<size_t>(buf_len), data->remain_buf.size() * sizeof(MPC_SAMPLE_FORMAT));

    debug("Copying %zu bytes from the remain buf", to_copy);

    memcpy(buf, data->remain_buf.data(), to_copy);
    size_t consumed = to_copy / sizeof(MPC_SAMPLE_FORMAT);
    if (consumed < data->remain_buf.size())
    {
      data->remain_buf.erase(data->remain_buf.begin(),
                             data->remain_buf.begin() + consumed);
    }
    else
    {
      debug("Remain buf is now empty");
      data->remain_buf.clear();
    }

    return to_copy;
  }

#ifdef MPC_IS_OLD_API
  ret = mpc_decoder_decode(&data->decoder, decode_buf, &vbrAcc, &vbrUpd);
  if (ret == 0)
  {
    debug("EOF");
    return 0;
  }

  if (ret < 0)
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Error in the stream!");
    return 0;
  }

  bytes_from_decoder = ret * sizeof(MPC_SAMPLE_FORMAT) * 2; /* stereo */
  data->bitrate = vbrUpd * sound_params->rate / 1152 / 1000;
#else
  do
  {
    frame.buffer = decode_buf;
    err = mpc_demux_decode(data->demux, &frame);

    if (err == MPC_STATUS_OK && frame.bits == -1)
    {
      debug("EOF");
      return 0;
    }

    if (err == MPC_STATUS_OK)
    {
      continue;
    }

    if (frame.bits == -1)
    {
      decoder_error(&data->error, ERROR_FATAL, 0, "Error in the stream!");
      return 0;
    }

    decoder_error(&data->error, ERROR_STREAM, 0, "Broken frame.");
  } while (err != MPC_STATUS_OK || frame.samples == 0);

  mpc_demux_get_info(data->demux, &data->info);
  bytes_from_decoder =
      frame.samples * sizeof(MPC_SAMPLE_FORMAT) * data->info.channels;
  data->bitrate = data->info.bitrate;
#endif

  decoder_error_clear(&data->error);
  sound_params->channels = data->info.channels;
  sound_params->rate = data->info.sample_freq;
#ifdef MPC_FIXED_POINT
  sound_params->fmt = SFMT_S32 | SFMT_NE;
#else
  sound_params->fmt = SFMT_FLOAT;
#endif

  if (bytes_from_decoder >= buf_len)
  {
    size_t to_copy = std::min(buf_len, bytes_from_decoder);

    debug("Copying %zu bytes", to_copy);

    memcpy(buf, decode_buf, to_copy);
    size_t remain_len = (bytes_from_decoder - to_copy) / sizeof(MPC_SAMPLE_FORMAT);
    data->remain_buf.assign(decode_buf + to_copy / sizeof(MPC_SAMPLE_FORMAT),
                            decode_buf + to_copy / sizeof(MPC_SAMPLE_FORMAT) + remain_len);
    decoded = to_copy;
  }
  else
  {
    debug("Copying whole decoded sound (%d bytes)", bytes_from_decoder);
    memcpy(buf, decode_buf, bytes_from_decoder);
    decoded = bytes_from_decoder;
  }

  return decoded;
}

static int musepack_get_bitrate(void *prv_data)
{
  struct musepack_data *data = static_cast<struct musepack_data *>(prv_data);

  return data->bitrate;
}

static int musepack_get_avg_bitrate(void *prv_data)
{
  struct musepack_data *data = static_cast<struct musepack_data *>(prv_data);

  return data->avg_bitrate;
}

static int musepack_get_duration(void *prv_data)
{
  struct musepack_data *data = static_cast<struct musepack_data *>(prv_data);

  return mpc_streaminfo_get_length(&data->info);
}

static struct io_stream *musepack_get_stream(void *prv_data)
{
  struct musepack_data *data = static_cast<struct musepack_data *>(prv_data);

  return data->stream;
}

static void musepack_get_name(const char *unused ATTR_UNUSED, char buf[4])
{
  std::memcpy(buf, "MPC", sizeof("MPC"));
}

static int musepack_our_format_ext(const char *ext)
{
  return !strcasecmp(ext, "mpc");
}

static void musepack_get_error(void *prv_data, struct decoder_error *error)
{
  struct musepack_data *data = static_cast<struct musepack_data *>(prv_data);

  decoder_error_copy(error, &data->error);
}


class MusepackDecoder : public AudioDecoder {
public:
    std::unique_ptr<void, void(*)(void*)> data;
    MusepackDecoder(void *d) : data(d, musepack_close) {}
    ~MusepackDecoder() override = default;

    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return musepack_decode(data.get(), buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return musepack_seek(data.get(), sec);
    }

    int get_bitrate() override {
        return musepack_get_bitrate(data.get());
    }

    int get_duration() override {
        return musepack_get_duration(data.get());
    }

    void get_error(struct decoder_error *error) override {
        musepack_get_error(data.get(), error);
    }

    struct io_stream *get_stream() override {
        return musepack_get_stream(data.get());
    }

    int get_avg_bitrate() override {
        return musepack_get_avg_bitrate(data.get());
    }
};

class MusepackPlugin : public AudioPlugin {
public:

    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = musepack_open(file);
        if (!d) return nullptr;
        return std::make_unique<MusepackDecoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        musepack_info(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return musepack_our_format_ext(ext);
    }

    void get_name(const char *file, char buf[4]) override {
        musepack_get_name(file, buf);
    }
};

extern "C" class AudioPlugin *musepack_plugin_init() {
    static MusepackPlugin plugin;
    return &plugin;
}

// EOF
