// src/audio/decoders/xmp/xmp.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Tracker module playback via libxmp. Replaces the earlier libmodplug
// plugin, whose lineage is preserved here for attribution:
// libmodplug-plugin Copyright (C) 2006 Hendrik Iben <hiben@tzi.de>
// Structure of this plugin is an adaption of the libsndfile-plugin from moc.
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
#include <cstring>
#include <memory>
#include <string>
#include <strings.h>
#include <xmp.h>

#define DEBUG

#include "core/common.h"
#include "io/io.h"
#include "audio/decoder.h"
#include "core/log.h"
#include "library/files.h"
#include "core/options.h"

namespace
{

/* Player settings, read once from the config at init(). libxmp keeps its
 * state per context, so these are only the values handed to each new
 * context rather than shared mutable state. */
struct xmp_settings
{
  int frequency = 44100;
  int format = 0;    /* XMP_FORMAT_* flags */
  int interp = XMP_INTERP_LINEAR;
  int dsp = 0;       /* XMP_DSP_* flags */
  int loop = 0;      /* passed to xmp_play_buffer() */
  int channels = 2;
  int bits = 16;
};

xmp_settings g_settings;

struct xmp_data
{
  xmp_context ctx = nullptr;
  bool playing = false;   /* xmp_start_player() succeeded */
  bool loaded = false;    /* a module is held by the context */
  int duration = 0;       /* seconds */
  struct decoder_error error;
};

/* --- libxmp I/O callbacks, backed by mocf's own io_stream ---------------
 * Letting libxmp pull through these means the module is parsed straight
 * off the stream: no copy of the whole file is held in memory. */

unsigned long io_read_cb(void *dest, unsigned long len, unsigned long nmemb,
                         void *priv)
{
  auto *s = static_cast<struct io_stream *>(priv);
  if (len == 0 || nmemb == 0)
  {
    return 0;
  }

  ssize_t got = io_read(s, dest, static_cast<size_t>(len) * nmemb);
  if (got <= 0)
  {
    return 0;
  }

  /* libxmp expects a count of whole items, as fread() reports. */
  return static_cast<unsigned long>(got) / len;
}

int io_seek_cb(void *priv, long offset, int whence)
{
  auto *s = static_cast<struct io_stream *>(priv);
  return io_seek(s, offset, whence) < 0 ? -1 : 0;
}

long io_tell_cb(void *priv)
{
  auto *s = static_cast<struct io_stream *>(priv);
  return static_cast<long>(io_tell(s));
}

int io_close_cb(void *priv ATTR_UNUSED)
{
  /* The stream outlives the load call and is closed by its owner. */
  return 0;
}

struct xmp_callbacks make_callbacks()
{
  struct xmp_callbacks cb;
  cb.read_func = io_read_cb;
  cb.seek_func = io_seek_cb;
  cb.tell_func = io_tell_cb;
  cb.close_func = io_close_cb;
  return cb;
}

const char *load_error_str(int err)
{
  switch (err)
  {
    case -XMP_ERROR_FORMAT: return "unsupported module format";
    case -XMP_ERROR_LOAD:   return "corrupt or truncated module";
    case -XMP_ERROR_DEPACK: return "cannot unpack module";
    case -XMP_ERROR_SYSTEM: return "system error reading module";
    case -XMP_ERROR_INVALID: return "invalid parameter";
    default: return "cannot load module";
  }
}

/* Load a module into a fresh context. Returns nullptr-filled data with
 * error set on failure; the caller always gets a valid xmp_data. */
struct xmp_data *make_xmp_data(const char *file)
{
  auto *data = new xmp_data;
  decoder_error_init(&data->error);

  unique_io_stream s(io_open(file, 0));
  if (!io_ok(s.get()))
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't open file: %s", file);
    return data;
  }

  data->ctx = xmp_create_context();
  if (data->ctx == nullptr)
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't create libxmp context");
    return data;
  }

  int res = xmp_load_module_from_callbacks(data->ctx, s.get(),
                                           make_callbacks());
  if (res != 0)
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't load module: %s (%s)",
                  file, load_error_str(res));
    return data;
  }

  data->loaded = true;

  /* Duration needs the player running: total_time is filled from the
   * module scan that xmp_start_player() performs. */
  if (xmp_start_player(data->ctx, g_settings.frequency, g_settings.format) != 0)
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't start player: %s", file);
    return data;
  }
  data->playing = true;

  xmp_set_player(data->ctx, XMP_PLAYER_INTERP, g_settings.interp);
  xmp_set_player(data->ctx, XMP_PLAYER_DSP, g_settings.dsp);

  struct xmp_frame_info fi;
  xmp_get_frame_info(data->ctx, &fi);
  data->duration = fi.total_time / 1000;

  return data;
}

void xmp_dec_close(void *void_data)
{
  auto *data = static_cast<struct xmp_data *>(void_data);

  if (data->ctx != nullptr)
  {
    if (data->playing)
    {
      xmp_end_player(data->ctx);
    }
    if (data->loaded)
    {
      xmp_release_module(data->ctx);
    }
    xmp_free_context(data->ctx);
  }

  decoder_error_clear(&data->error);
  delete data;
}

void *xmp_dec_open(const char *file)
{
  struct xmp_data *data = make_xmp_data(file);

  if (data->playing)
  {
    debug("Opened file %s", file);
  }

  return data;
}

void xmp_dec_info(const char *file_name, struct file_tags *info,
                  const int tags_sel)
{
  /* Title alone can be had from a probe, which does not build the
   * player state; the duration needs a full load. */
  if (!(tags_sel & TAGS_TIME))
  {
    if (tags_sel & TAGS_COMMENTS)
    {
      unique_io_stream s(io_open(file_name, 0));
      struct xmp_test_info ti;
      if (io_ok(s.get()) &&
          xmp_test_module_from_callbacks(s.get(), make_callbacks(), &ti) == 0)
      {
        info->title = ti.name;
        info->filled |= TAGS_COMMENTS;
      }
    }
    return;
  }

  struct xmp_data *data = make_xmp_data(file_name);

  if (!data->playing)
  {
    xmp_dec_close(data);
    return;
  }

  if (tags_sel & TAGS_TIME)
  {
    info->time = data->duration;
    info->filled |= TAGS_TIME;
  }

  if (tags_sel & TAGS_COMMENTS)
  {
    struct xmp_module_info mi;
    xmp_get_module_info(data->ctx, &mi);
    if (mi.mod != nullptr)
    {
      info->title = mi.mod->name;
    }
    info->filled |= TAGS_COMMENTS;
  }

  xmp_dec_close(data);
}

int xmp_dec_seek(void *void_data, int sec)
{
  auto *data = static_cast<struct xmp_data *>(void_data);

  assert(sec >= 0);

  if (!data->playing)
  {
    return -1;
  }

  if (xmp_seek_time(data->ctx, sec * 1000) < 0)
  {
    return -1;
  }

  return sec;
}

int xmp_dec_decode(void *void_data, char *buf, int buf_len,
                   struct sound_params *sound_params)
{
  auto *data = static_cast<struct xmp_data *>(void_data);

  if (!data->playing)
  {
    return 0;
  }

  sound_params->channels = g_settings.channels;
  sound_params->rate = g_settings.frequency;
  sound_params->fmt =
      ((g_settings.bits == 8) ? SFMT_S8 : SFMT_S16) | SFMT_NE;

  if (xmp_play_buffer(data->ctx, buf, buf_len, g_settings.loop) != 0)
  {
    return 0; /* end of module, or the player stopped */
  }

  return buf_len;
}

int xmp_dec_get_bitrate(void *unused ATTR_UNUSED) { return -1; }

int xmp_dec_get_duration(void *void_data)
{
  auto *data = static_cast<struct xmp_data *>(void_data);
  return data->duration;
}

int xmp_dec_our_format_ext(const char *ext)
{
  /* Formats libxmp loads that have a conventional extension. Kept to the
   * set the previous libmodplug plugin claimed, minus AMS, DSM, DMF and
   * MT2, which libxmp does not implement. */
  return !strcasecmp(ext, "MOD") || !strcasecmp(ext, "S3M") ||
         !strcasecmp(ext, "XM") || !strcasecmp(ext, "MED") ||
         !strcasecmp(ext, "MTM") || !strcasecmp(ext, "IT") ||
         !strcasecmp(ext, "669") || !strcasecmp(ext, "ULT") ||
         !strcasecmp(ext, "STM") || !strcasecmp(ext, "FAR") ||
         !strcasecmp(ext, "AMF") || !strcasecmp(ext, "MDL") ||
         !strcasecmp(ext, "OKT") || !strcasecmp(ext, "PTM") ||
         !strcasecmp(ext, "DBM") || !strcasecmp(ext, "AMF0") ||
         !strcasecmp(ext, "PSM") || !strcasecmp(ext, "J2B") ||
         !strcasecmp(ext, "UMX");
}

void xmp_dec_get_error(void *prv_data, struct decoder_error *error)
{
  auto *data = static_cast<struct xmp_data *>(prv_data);

  decoder_error_copy(error, &data->error);
}

}  // namespace

class XmpDecoder : public AudioDecoder {
public:
    std::unique_ptr<void, void(*)(void*)> data;
    explicit XmpDecoder(void *d) : data(d, xmp_dec_close) {}
    ~XmpDecoder() override = default;

    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return xmp_dec_decode(data.get(), buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return xmp_dec_seek(data.get(), sec);
    }

    int get_bitrate() override {
        return xmp_dec_get_bitrate(data.get());
    }

    int get_duration() override {
        return xmp_dec_get_duration(data.get());
    }

    void get_error(struct decoder_error *error) override {
        xmp_dec_get_error(data.get(), error);
    }
};

class XmpPlugin : public AudioPlugin {
public:

    void init() override {
        int freq = options_get_int("XMP_Frequency");
        if (freq > XMP_MAX_SRATE) {
            logit("XMP_Frequency %d above libxmp's limit, using %d",
                  freq, XMP_MAX_SRATE);
            freq = XMP_MAX_SRATE;
        }
        g_settings.frequency = freq;

        g_settings.channels = options_get_int("XMP_Channels");
        g_settings.bits = options_get_int("XMP_Bits");

        g_settings.format = 0;
        if (g_settings.channels == 1) {
            g_settings.format |= XMP_FORMAT_MONO;
        }
        if (g_settings.bits == 8) {
            g_settings.format |= XMP_FORMAT_8BIT;
        }

        const char *interp = options_get_symb("XMP_ResamplingMode");
        if (!strcasecmp(interp, "NEAREST")) {
            g_settings.interp = XMP_INTERP_NEAREST;
        } else if (!strcasecmp(interp, "SPLINE")) {
            g_settings.interp = XMP_INTERP_SPLINE;
        } else {
            g_settings.interp = XMP_INTERP_LINEAR;
        }

        g_settings.dsp = options_get_bool("XMP_LowPassFilter") ? XMP_DSP_ALL : 0;
        g_settings.loop = options_get_int("XMP_LoopCount") != 0 ? 1 : 0;

        debug("libxmp %s: %dHz %dch %dbit interp=%d dsp=%d loop=%d",
              xmp_version, g_settings.frequency, g_settings.channels,
              g_settings.bits, g_settings.interp, g_settings.dsp,
              g_settings.loop);
    }

    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = xmp_dec_open(file);
        if (!d) return nullptr;
        return std::make_unique<XmpDecoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        xmp_dec_info(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return xmp_dec_our_format_ext(ext);
    }

    const char *our_format_data(const char *buf, size_t len) override {
        if (len >= 4 && !memcmp(buf, "IMPM", 4)) {
            return "IT";
        }
        if (len >= 17 && !memcmp(buf, "Extended Module: ", 17)) {
            return "XM";
        }
        if (len >= 48 && !memcmp(buf + 44, "SCRM", 4)) {
            return "S3M";
        }
        return nullptr;
    }

    std::string get_name(const char *file) override {
        const char *ext = ext_pos(file);
        /* .amf0 is just an older AMF variant. */
        if (ext && !strcasecmp(ext, "amf0")) {
            return "AMF";
        }
        return "";  /* uppercased extension via file_type_name() */
    }
};

extern "C" class AudioPlugin *xmp_plugin_init() {
    static XmpPlugin plugin;
    return &plugin;
}

// EOF
