// src/audio/processing/softmixer.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Softmixer-extension Copyright (C) 2007-2008 Hendrik Iben <hiben@tzi.de>
// Provides a software-mixer to regulate volume independent from
// hardware.
// Copyright (C) 2004-2008 Damian Pietras <daper@daper.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>

/* #define DEBUG */

#include "core/common.h"
#include "audio/audio.h"
#include "audio/conversion/audio_conversion.h"
#include "audio/processing/softmixer.h"
#include "core/options.h"
#include "library/files.h"
#include "core/log.h"

static int active;
static int mix_mono;
static int mixer_val;
static int mixer_amp;
static int mixer_real;
static float mixer_realf;

static void softmixer_read_config();
static void softmixer_write_config();

/* public code */

std::string softmixer_name()
{
  return active ? SOFTMIXER_NAME : SOFTMIXER_NAME_OFF;
}

void softmixer_init()
{
  active = 0;
  mix_mono = 0;
  mixer_amp = 100;
  softmixer_set_value(100);
  softmixer_read_config();
  logit("Softmixer initialized");
}

void softmixer_shutdown()
{
  if (options_get_bool(SOFTMIXER_SAVE_OPTION))
  {
    softmixer_write_config();
  }
  logit("Softmixer stopped");
}

void softmixer_set_value(const int val)
{
  mixer_val = std::clamp(val, 0, 100);
  mixer_real = exp((mixer_val * mixer_amp) / 100 * 0.06908);
  if (mixer_val < 10)
  {
    mixer_real = static_cast<int>(mixer_real * mixer_val /
                       10.f); // linear roll-off to zero for low values
  }
  mixer_real = std::clamp(mixer_real, SOFTMIXER_MIN, SOFTMIXER_MAX);
  mixer_realf = (static_cast<float>(mixer_real)) / 1000.0f;

  debug("Softmixer value: %d, gain: %d", mixer_val, mixer_real);
}

int softmixer_get_value() { return mixer_val; }

void softmixer_set_active(int act)
{
  if (act)
  {
    active = 1;
  }
  else
  {
    active = 0;
  }
}

int softmixer_is_active() { return active; }

void softmixer_set_mono(int mono)
{
  if (mono)
  {
    mix_mono = 1;
  }
  else
  {
    mix_mono = 0;
  }
}

int softmixer_is_mono() { return mix_mono; }

/* private code */

static void process_buffer_u8(uint8_t *buf, size_t samples);
static void process_buffer_s8(int8_t *buf, size_t samples);
static void process_buffer_u16(uint16_t *buf, size_t samples);
static void process_buffer_s16(int16_t *buf, size_t samples);
static void process_buffer_u24(uint32_t *buf, size_t samples);
static void process_buffer_s24(int32_t *buf, size_t samples);
static void process_buffer_u32(uint32_t *buf, size_t samples);
static void process_buffer_s32(int32_t *buf, size_t samples);
static void process_buffer_float(float *buf, size_t samples);
static void mix_mono_u8(uint8_t *buf, int channels, size_t samples);
static void mix_mono_s8(int8_t *buf, int channels, size_t samples);
static void mix_mono_u16(uint16_t *buf, int channels, size_t samples);
static void mix_mono_s16(int16_t *buf, int channels, size_t samples);
static void mix_mono_u24(uint32_t *buf, int channels, size_t samples);
static void mix_mono_s24(int32_t *buf, int channels, size_t samples);
static void mix_mono_u32(uint32_t *buf, int channels, size_t samples);
static void mix_mono_s32(int32_t *buf, int channels, size_t samples);
static void mix_mono_float(float *buf, int channels, size_t samples);

static void softmixer_read_config()
{
  std::string cfname = create_file_name(SOFTMIXER_SAVE_FILE);

  FILE *cf = fopen(cfname.c_str(), "r");

  if (cf == nullptr)
  {
    logit("Unable to read softmixer configuration");
    return;
  }

  int tmp;

  while (auto linebuffer = read_line(cf))
  {
    if (strncasecmp(linebuffer->c_str(), SOFTMIXER_CFG_ACTIVE,
                    sizeof(SOFTMIXER_CFG_ACTIVE) - 1) == 0)
    {
      if (sscanf(linebuffer->c_str() + sizeof(SOFTMIXER_CFG_ACTIVE) - 1, " %i", &tmp) == 1)
      {
        if (tmp > 0)
        {
          active = 1;
        }
        else
        {
          active = 0;
        }
      }
    }
    if (strncasecmp(linebuffer->c_str(), SOFTMIXER_CFG_AMP, sizeof(SOFTMIXER_CFG_AMP) - 1) ==
        0)
    {
      if (sscanf(linebuffer->c_str() + sizeof(SOFTMIXER_CFG_AMP) - 1, " %i", &tmp) == 1)
      {
        if (in_closed_range(SOFTMIXER_MIN, tmp, SOFTMIXER_MAX))
        {
          mixer_amp = tmp;
        }
        else
        {
          logit("Tried to set softmixer amplification out of range.");
        }
      }
    }
    if (strncasecmp(linebuffer->c_str(), SOFTMIXER_CFG_VALUE,
                    sizeof(SOFTMIXER_CFG_VALUE) - 1) == 0)
    {
      if (sscanf(linebuffer->c_str() + sizeof(SOFTMIXER_CFG_VALUE) - 1, " %i", &tmp) == 1)
      {
        if (in_closed_range(0, tmp, 100))
        {
          softmixer_set_value(tmp);
        }
        else
        {
          logit("Tried to set softmixer value out of range.");
        }
      }
    }
    if (strncasecmp(linebuffer->c_str(), SOFTMIXER_CFG_MONO,
                    sizeof(SOFTMIXER_CFG_MONO) - 1) == 0)
    {
      if (sscanf(linebuffer->c_str() + sizeof(SOFTMIXER_CFG_MONO) - 1, " %i", &tmp) == 1)
      {
        if (tmp > 0)
        {
          mix_mono = 1;
        }
        else
        {
          mix_mono = 0;
        }
      }
    }

  }

  fclose(cf);
}

static void softmixer_write_config()
{
  std::string cfname = create_file_name(SOFTMIXER_SAVE_FILE);

  FILE *cf = fopen(cfname.c_str(), "w");

  if (cf == nullptr)
  {
    logit("Unable to write softmixer configuration");
    return;
  }

  fprintf(cf, "%s %i\n", SOFTMIXER_CFG_ACTIVE, active);
  fprintf(cf, "%s %i\n", SOFTMIXER_CFG_AMP, mixer_amp);
  fprintf(cf, "%s %i\n", SOFTMIXER_CFG_VALUE, mixer_val);
  fprintf(cf, "%s %i\n", SOFTMIXER_CFG_MONO, mix_mono);

  fclose(cf);

  logit("Softmixer configuration written");
}

void softmixer_process_buffer(char *buf, size_t size,
                              const struct sound_params *sound_params)
{
  int do_softmix;
  int do_monomix;

  debug("Processing %zu bytes...", size);

  do_softmix = active && (mixer_real != 100);
  do_monomix = mix_mono && (sound_params->channels > 1);

  if (!do_softmix && !do_monomix)
  {
    return;
  }

  long sound_format = sound_params->fmt & SFMT_MASK_FORMAT;
  int samplewidth = sfmt_Bps(sound_format);

  assert(size % (samplewidth * sound_params->channels) == 0);

  switch (sound_format)
  {
    case SFMT_U8:
      if (do_softmix)
      {
        process_buffer_u8(reinterpret_cast<uint8_t *>(buf), size);
      }
      if (do_monomix)
      {
        mix_mono_u8(reinterpret_cast<uint8_t *>(buf), sound_params->channels, size);
      }
      break;
    case SFMT_S8:
      if (do_softmix)
      {
        process_buffer_s8(reinterpret_cast<int8_t *>(buf), size);
      }
      if (do_monomix)
      {
        mix_mono_s8(reinterpret_cast<int8_t *>(buf), sound_params->channels, size);
      }
      break;
    case SFMT_U16:
      if (do_softmix)
      {
        process_buffer_u16(reinterpret_cast<uint16_t *>(buf), size / sizeof(uint16_t));
      }
      if (do_monomix)
      {
        mix_mono_u16(reinterpret_cast<uint16_t *>(buf), sound_params->channels,
                     size / sizeof(uint16_t));
      }
      break;
    case SFMT_S16:
      if (do_softmix)
      {
        process_buffer_s16(reinterpret_cast<int16_t *>(buf), size / sizeof(int16_t));
      }
      if (do_monomix)
      {
        mix_mono_s16(reinterpret_cast<int16_t *>(buf), sound_params->channels,
                     size / sizeof(int16_t));
      }
      break;
    case SFMT_U24:
      if (do_softmix)
      {
        process_buffer_u24(reinterpret_cast<uint32_t *>(buf), size / sizeof(uint32_t));
      }
      if (mix_mono)
      {
        mix_mono_u24(reinterpret_cast<uint32_t *>(buf), sound_params->channels,
                     size / sizeof(uint32_t));
      }
      break;
    case SFMT_S24:
      if (do_softmix)
      {
        process_buffer_s24(reinterpret_cast<int32_t *>(buf), size / sizeof(int32_t));
      }
      if (mix_mono)
      {
        mix_mono_s24(reinterpret_cast<int32_t *>(buf), sound_params->channels,
                     size / sizeof(int32_t));
      }
      break;
    case SFMT_U32:
      if (do_softmix)
      {
        process_buffer_u32(reinterpret_cast<uint32_t *>(buf), size / sizeof(uint32_t));
      }
      if (do_monomix)
      {
        mix_mono_u32(reinterpret_cast<uint32_t *>(buf), sound_params->channels,
                     size / sizeof(uint32_t));
      }
      break;
    case SFMT_S32:
      if (do_softmix)
      {
        process_buffer_s32(reinterpret_cast<int32_t *>(buf), size / sizeof(int32_t));
      }
      if (do_monomix)
      {
        mix_mono_s32(reinterpret_cast<int32_t *>(buf), sound_params->channels,
                     size / sizeof(int32_t));
      }
      break;
    case SFMT_FLOAT:
      if (do_softmix)
      {
        process_buffer_float(reinterpret_cast<float *>(buf), size / sizeof(float));
      }
      if (do_monomix)
      {
        mix_mono_float(reinterpret_cast<float *>(buf), sound_params->channels,
                       size / sizeof(float));
      }
      break;
    default:
      debug("No softmixer/monomixer for chosen format.");
  }
}

static void process_buffer_u8(uint8_t *buf, size_t samples)
{
  size_t i;

  debug("mixing");

  for (i = 0; i < samples; i++)
  {
    int16_t tmp = buf[i];
    tmp -= (UINT8_MAX >> 1);
    tmp *= mixer_real;
    tmp /= 1000;
    tmp += (UINT8_MAX >> 1);
    tmp = std::clamp<int16_t>(tmp, 0, UINT8_MAX);
    buf[i] = static_cast<uint8_t>(tmp);
  }
}

static void process_buffer_s8(int8_t *buf, size_t samples)
{
  size_t i;

  debug("mixing");

  for (i = 0; i < samples; i++)
  {
    int16_t tmp = buf[i];
    tmp *= mixer_real;
    tmp /= 1000;
    tmp = std::clamp<int16_t>(tmp, INT8_MIN, INT8_MAX);
    buf[i] = static_cast<int8_t>(tmp);
  }
}

static void process_buffer_u16(uint16_t *buf, size_t samples)
{
  size_t i;

  debug("mixing");

  for (i = 0; i < samples; i++)
  {
    int32_t tmp = buf[i];
    tmp -= (UINT16_MAX >> 1);
    tmp *= mixer_real;
    tmp /= 1000;
    tmp += (UINT16_MAX >> 1);
    tmp = std::clamp<int32_t>(tmp, 0, UINT16_MAX);
    buf[i] = static_cast<uint16_t>(tmp);
  }
}

static void process_buffer_s16(int16_t *buf, size_t samples)
{
  size_t i;

  debug("mixing");

  for (i = 0; i < samples; i++)
  {
    int32_t tmp = buf[i];
    tmp *= mixer_real;
    tmp /= 1000;
    tmp = std::clamp<int32_t>(tmp, INT16_MIN, INT16_MAX);
    buf[i] = static_cast<int16_t>(tmp);
  }
}

static void process_buffer_u24(uint32_t *buf, size_t samples)
{
  size_t i;

  debug("mixing");

  for (i = 0; i < samples; i++)
  {
    int64_t tmp = buf[i];
    tmp -= S24_MIN;
    tmp *= mixer_real;
    tmp /= 1000;
    tmp += S24_MIN;
    tmp = std::clamp<int64_t>(tmp, 0, U24_MAX);
    buf[i] = static_cast<uint32_t>(tmp);
  }
}

static void process_buffer_s24(int32_t *buf, size_t samples)
{
  size_t i;

  debug("mixing");

  for (i = 0; i < samples; i++)
  {
    int64_t tmp = buf[i];
    tmp *= mixer_real;
    tmp /= 1000;
    tmp = std::clamp<int64_t>(tmp, S24_MIN, S24_MAX);
    buf[i] = static_cast<int32_t>(tmp);
  }
}

static void process_buffer_u32(uint32_t *buf, size_t samples)
{
  size_t i;

  debug("mixing");

  for (i = 0; i < samples; i++)
  {
    int64_t tmp = buf[i];
    tmp -= (UINT32_MAX >> 1);
    tmp *= mixer_real;
    tmp /= 1000;
    tmp += (UINT32_MAX >> 1);
    tmp = std::clamp<int64_t>(tmp, 0, UINT32_MAX);
    buf[i] = static_cast<uint32_t>(tmp);
  }
}

static void process_buffer_s32(int32_t *buf, size_t samples)
{
  size_t i;

  debug("mixing");

  for (i = 0; i < samples; i++)
  {
    int64_t tmp = buf[i];
    tmp *= mixer_real;
    tmp /= 1000;
    tmp = std::clamp<int64_t>(tmp, INT32_MIN, INT32_MAX);
    buf[i] = static_cast<int32_t>(tmp);
  }
}

static void process_buffer_float(float *buf, size_t samples)
{
  size_t i;

  debug("mixing");

  for (i = 0; i < samples; i++)
  {
    float tmp = buf[i];
    tmp *= mixer_realf;
    tmp = std::clamp(tmp, -1.0f, 1.0f);
    buf[i] = tmp;
  }
}

// Mono-Mixing
static void mix_mono_u8(uint8_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug("making mono");

  assert(channels > 1);

  while (i < samples)
  {
    int16_t mono = 0;

    for (c = 0; c < channels; c++)
    {
      mono += *buf++;
    }

    buf -= channels;

    mono /= channels;
    mono = std::min<int16_t>(mono, UINT8_MAX); // can't be negative

    for (c = 0; c < channels; c++)
    {
      *buf++ = static_cast<uint8_t>(mono);
    }

    i += channels;
  }
}

static void mix_mono_s8(int8_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug("making mono");

  assert(channels > 1);

  while (i < samples)
  {
    int16_t mono = 0;

    for (c = 0; c < channels; c++)
    {
      mono += *buf++;
    }

    buf -= channels;

    mono /= channels;
    mono = std::clamp<int16_t>(mono, INT8_MIN, INT8_MAX);

    for (c = 0; c < channels; c++)
    {
      *buf++ = static_cast<int8_t>(mono);
    }

    i += channels;
  }
}

static void mix_mono_u16(uint16_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug("making mono");

  assert(channels > 1);

  while (i < samples)
  {
    int32_t mono = 0;

    for (c = 0; c < channels; c++)
    {
      mono += *buf++;
    }

    buf -= channels;

    mono /= channels;
    mono = std::min<int32_t>(mono, UINT16_MAX); // can't be negative

    for (c = 0; c < channels; c++)
    {
      *buf++ = static_cast<uint16_t>(mono);
    }

    i += channels;
  }
}

static void mix_mono_s16(int16_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug("making mono");

  assert(channels > 1);

  while (i < samples)
  {
    int32_t mono = 0;

    for (c = 0; c < channels; c++)
    {
      mono += *buf++;
    }

    buf -= channels;

    mono /= channels;
    mono = std::clamp<int32_t>(mono, INT16_MIN, INT16_MAX);

    for (c = 0; c < channels; c++)
    {
      *buf++ = static_cast<int16_t>(mono);
    }

    i += channels;
  }
}

static void mix_mono_u24(uint32_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug("making mono");

  if (channels < 2)
  {
    return;
  }

  while (i < samples)
  {
    int64_t mono = 0;

    for (c = 0; c < channels; c++)
    {
      mono += *buf++;
    }

    buf -= channels;

    mono /= channels;
    mono = std::min<int64_t>(mono, U24_MAX); // can't be negative

    for (c = 0; c < channels; c++)
    {
      *buf++ = static_cast<uint32_t>(mono);
    }

    i += channels;
  }
}

static void mix_mono_s24(int32_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug("making mono");

  if (channels < 2)
  {
    return;
  }

  while (i < samples)
  {
    int64_t mono = 0;

    for (c = 0; c < channels; c++)
    {
      mono += *buf++;
    }

    buf -= channels;

    mono /= channels;
    mono = std::clamp<int64_t>(mono, S24_MIN, S24_MAX);

    for (c = 0; c < channels; c++)
    {
      *buf++ = static_cast<int32_t>(mono);
    }

    i += channels;
  }
}

static void mix_mono_u32(uint32_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug("making mono");

  assert(channels > 1);

  while (i < samples)
  {
    int64_t mono = 0;

    for (c = 0; c < channels; c++)
    {
      mono += *buf++;
    }

    buf -= channels;

    mono /= channels;
    mono = std::min<int64_t>(mono, UINT32_MAX); // can't be negative

    for (c = 0; c < channels; c++)
    {
      *buf++ = static_cast<uint32_t>(mono);
    }

    i += channels;
  }
}

static void mix_mono_s32(int32_t *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug("making mono");

  assert(channels > 1);

  while (i < samples)
  {
    int64_t mono = 0;

    for (c = 0; c < channels; c++)
    {
      mono += *buf++;
    }

    buf -= channels;

    mono /= channels;
    mono = std::clamp<int64_t>(mono, INT32_MIN, INT32_MAX);

    for (c = 0; c < channels; c++)
    {
      *buf++ = static_cast<int32_t>(mono);
    }

    i += channels;
  }
}

static void mix_mono_float(float *buf, int channels, size_t samples)
{
  int c;
  size_t i = 0;

  debug("making mono");

  assert(channels > 1);

  while (i < samples)
  {
    float mono = 0.0f;

    for (c = 0; c < channels; c++)
    {
      mono += *buf++;
    }

    buf -= channels;

    mono /= channels;
    mono = std::clamp(mono, -1.0f, 1.0f);

    for (c = 0; c < channels; c++)
    {
      *buf++ = mono;
    }

    i += channels;
  }
}

// EOF
