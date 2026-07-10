// src/audio/decoders/modplug/modplug.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// libmodplug-plugin Copyright (C) 2006 Hendrik Iben <hiben@tzi.de>
// Enables MOC to play modules via libmodplug (actually just a wrapper around
// libmodplug's C-wrapper... :-)).
// Based on ideas from G"urkan Seng"un's modplugplay. A command line
// interface to the modplugxmms library.
// Structure of this plugin is an adaption of the libsndfile-plugin from
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
#include <climits>
#include <cstring>
#include <memory>
#include <vector>
#include <algorithm>
#include <libmodplug/modplug.h>

#define DEBUG

#include "core/common.h"
#include "io/io.h"
#include "audio/decoder.h"
#include "core/log.h"
#include "library/files.h"
#include "core/options.h"

ModPlug_Settings settings;

struct modplug_data
{
  ModPlugFile *modplugfile;
  int length;
  struct decoder_error error;
};

#if !defined(NDEBUG) && defined(DEBUG)
// this is needed because debugging in plugin_init gets lost
// The alternative is to debug settings when opening a file
// but settings never change so I need a flag to check if it
// has been done...
static int doDebugSettings = 1;

static void debugSettings(void)
{
  debug("\n\
ModPlug-Settings:\n\
Oversampling : %s\n\
NoiseReduction : %s\n\
Reverb : %s\n\
MegaBass : %s\n\
Surround : %s\n\
ResamplingMode : %s\n\
Channels : %d\n\
Bits : %d\n\
Frequency : %d\n\
ReverbDepth : %d\n\
ReverbDelay : %d\n\
BassAmount : %d\n\
BassRange : %d\n\
SurroundDepth : %d\n\
SurroundDelay : %d\n\
LoopCount : %d",
        (settings.mFlags & MODPLUG_ENABLE_OVERSAMPLING) ? "yes" : "no",
        (settings.mFlags & MODPLUG_ENABLE_NOISE_REDUCTION) ? "yes" : "no",
        (settings.mFlags & MODPLUG_ENABLE_REVERB) ? "yes" : "no",
        (settings.mFlags & MODPLUG_ENABLE_MEGABASS) ? "yes" : "no",
        (settings.mFlags & MODPLUG_ENABLE_SURROUND) ? "yes" : "no",
        (settings.mResamplingMode == MODPLUG_RESAMPLE_FIR)       ? "8-tap fir"
        : (settings.mResamplingMode == MODPLUG_RESAMPLE_SPLINE)  ? "spline"
        : (settings.mResamplingMode == MODPLUG_RESAMPLE_LINEAR)  ? "linear"
        : (settings.mResamplingMode == MODPLUG_RESAMPLE_NEAREST) ? "nearest"
                                                                 : "?",
        settings.mChannels, settings.mBits, settings.mFrequency,
        settings.mReverbDepth, settings.mReverbDelay, settings.mBassAmount,
        settings.mBassRange, settings.mSurroundDepth, settings.mSurroundDelay,
        settings.mLoopCount);
}
#endif

static struct modplug_data *make_modplug_data(const char *file)
{
  struct modplug_data *data;

  data = new modplug_data;

  data->modplugfile = nullptr;
  decoder_error_init(&data->error);

  unique_io_stream s(io_open(file, 0));
  if (!io_ok(s.get()))
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't open file: %s", file);
    return data;
  }

  off_t size = io_file_size(s.get());

  if (!in_closed_range(1, size, INT_MAX))
  {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "Module size unsuitable for loading: %s", file);
    return data;
  }

  int max_size = options_get_int("ModPlug_MaxFileSize");
  if (size > static_cast<off_t>(max_size))
  {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "Module file too large (%ldMB). Increase ModPlug_MaxFileSize in config.",
                  static_cast<long>(size) / (1024 * 1024));
    return data;
  }

  std::vector<char> filedata(static_cast<size_t>(size));

  io_read(s.get(), filedata.data(), static_cast<size_t>(size));
  s.reset();

  data->modplugfile = ModPlug_Load(filedata.data(), static_cast<int>(size));

  if (data->modplugfile == nullptr)
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't load module: %s", file);
    return data;
  }

  return data;
}

static void *modplug_open(const char *file)
{
// this is not really needed but without it the calls would still be made
// and thus time gets wasted...
#if !defined(NDEBUG) && defined(DEBUG)
  if (doDebugSettings)
  {
    doDebugSettings = 0;
    debugSettings();
  }
#endif
  struct modplug_data *data = make_modplug_data(file);

  if (data->modplugfile)
  {
    data->length = ModPlug_GetLength(data->modplugfile);
    debug("Opened file %s", file);
  }

  return data;
}

static void modplug_close(void *void_data)
{
  struct modplug_data *data = static_cast<struct modplug_data *>(void_data);

  if (data->modplugfile)
  {
    ModPlug_Unload(data->modplugfile);
  }

  decoder_error_clear(&data->error);
  delete data;
}

static void modplug_info(const char *file_name, struct file_tags *info,
                         const int tags_sel)
{
  struct modplug_data *data = make_modplug_data(file_name);

  if (data->modplugfile == nullptr)
  {
    modplug_close(data);
    return;
  }

  if (tags_sel & TAGS_TIME)
  {
    info->time = ModPlug_GetLength(data->modplugfile) / 1000;
    info->filled |= TAGS_TIME;
  }

  if (tags_sel & TAGS_COMMENTS)
  {
    info->title = ModPlug_GetName(data->modplugfile);
    info->filled |= TAGS_COMMENTS;
  }

  modplug_close(data);
}

static int modplug_seek(void *void_data, int sec)
{
  struct modplug_data *data = static_cast<struct modplug_data *>(void_data);

  assert(sec >= 0);

  int ms = sec * 1000;

  ms = std::min(ms, data->length);

  ModPlug_Seek(data->modplugfile, ms);

  return ms / 1000;
}

static int modplug_decode(void *void_data, char *buf, int buf_len,
                          struct sound_params *sound_params)
{
  struct modplug_data *data = static_cast<struct modplug_data *>(void_data);

  sound_params->channels = settings.mChannels;
  sound_params->rate = settings.mFrequency;
  sound_params->fmt = ((settings.mBits == 16)  ? SFMT_S16
                       : (settings.mBits == 8) ? SFMT_S8
                                               : SFMT_S32) |
                      SFMT_NE;

  return ModPlug_Read(data->modplugfile, buf, buf_len);
}

static int modplug_get_bitrate(void *unused ATTR_UNUSED) { return -1; }

static int modplug_get_duration(void *void_data)
{
  struct modplug_data *data = static_cast<struct modplug_data *>(void_data);
  return data->length / 1000;
}

static int modplug_our_format_ext(const char *ext)
{
  // Do not include non-module formats in this list (even if
  // ModPlug supports them).  Doing so may cause memory exhaustion
  // in make_modplug_data().
  return !strcasecmp(ext, "NONE") || !strcasecmp(ext, "MOD") ||
         !strcasecmp(ext, "S3M") || !strcasecmp(ext, "XM") ||
         !strcasecmp(ext, "MED") || !strcasecmp(ext, "MTM") ||
         !strcasecmp(ext, "IT") || !strcasecmp(ext, "669") ||
         !strcasecmp(ext, "ULT") || !strcasecmp(ext, "STM") ||
         !strcasecmp(ext, "FAR") || !strcasecmp(ext, "AMF") ||
         !strcasecmp(ext, "AMS") || !strcasecmp(ext, "DSM") ||
         !strcasecmp(ext, "MDL") || !strcasecmp(ext, "OKT") ||
         // modplug can do MIDI but not in this form...
         //! strcasecmp (ext, "MID") ||
         !strcasecmp(ext, "DMF") || !strcasecmp(ext, "PTM") ||
         !strcasecmp(ext, "DBM") || !strcasecmp(ext, "MT2") ||
         !strcasecmp(ext, "AMF0") || !strcasecmp(ext, "PSM") ||
         !strcasecmp(ext, "J2B") || !strcasecmp(ext, "UMX");
}

static void modplug_get_error(void *prv_data, struct decoder_error *error)
{
  struct modplug_data *data = static_cast<struct modplug_data *>(prv_data);

  decoder_error_copy(error, &data->error);
}

class ModplugDecoder : public AudioDecoder {
public:
    std::unique_ptr<void, void(*)(void*)> data;
    ModplugDecoder(void *d) : data(d, modplug_close) {}
    ~ModplugDecoder() override = default;

    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return modplug_decode(data.get(), buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return modplug_seek(data.get(), sec);
    }

    int get_bitrate() override {
        return modplug_get_bitrate(data.get());
    }

    int get_duration() override {
        return modplug_get_duration(data.get());
    }

    void get_error(struct decoder_error *error) override {
        modplug_get_error(data.get(), error);
    }
};

class ModplugPlugin : public AudioPlugin {
public:

    void init() override {
        ModPlug_GetSettings(&settings);
        settings.mFlags = 0;
        settings.mFlags |= options_get_bool("ModPlug_Oversampling") ? MODPLUG_ENABLE_OVERSAMPLING : 0;
        settings.mFlags |= options_get_bool("ModPlug_NoiseReduction") ? MODPLUG_ENABLE_NOISE_REDUCTION : 0;
        settings.mFlags |= options_get_bool("ModPlug_Reverb") ? MODPLUG_ENABLE_REVERB : 0;
        settings.mFlags |= options_get_bool("ModPlug_MegaBass") ? MODPLUG_ENABLE_MEGABASS : 0;
        settings.mFlags |= options_get_bool("ModPlug_Surround") ? MODPLUG_ENABLE_SURROUND : 0;
        if (!strcasecmp(options_get_symb("ModPlug_ResamplingMode"), "FIR")) {
            settings.mResamplingMode = MODPLUG_RESAMPLE_FIR;
        }
        if (!strcasecmp(options_get_symb("ModPlug_ResamplingMode"), "SPLINE")) {
            settings.mResamplingMode = MODPLUG_RESAMPLE_SPLINE;
        }
        if (!strcasecmp(options_get_symb("ModPlug_ResamplingMode"), "LINEAR")) {
            settings.mResamplingMode = MODPLUG_RESAMPLE_LINEAR;
        }
        if (!strcasecmp(options_get_symb("ModPlug_ResamplingMode"), "NEAREST")) {
            settings.mResamplingMode = MODPLUG_RESAMPLE_NEAREST;
        }
        settings.mChannels = options_get_int("ModPlug_Channels");
        settings.mBits = options_get_int("ModPlug_Bits");
        settings.mFrequency = options_get_int("ModPlug_Frequency");
        settings.mReverbDepth = options_get_int("ModPlug_ReverbDepth");
        settings.mReverbDelay = options_get_int("ModPlug_ReverbDelay");
        settings.mBassAmount = options_get_int("ModPlug_BassAmount");
        settings.mBassRange = options_get_int("ModPlug_BassRange");
        settings.mSurroundDepth = options_get_int("ModPlug_SurroundDepth");
        settings.mSurroundDelay = options_get_int("ModPlug_SurroundDelay");
        settings.mLoopCount = options_get_int("ModPlug_LoopCount");
        ModPlug_SetSettings(&settings);
    }

    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = modplug_open(file);
        if (!d) return nullptr;
        return std::make_unique<ModplugDecoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        modplug_info(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return modplug_our_format_ext(ext);
    }
};

extern "C" class AudioPlugin *modplug_plugin_init() {
    static ModplugPlugin plugin;
    return &plugin;
}

// EOF
