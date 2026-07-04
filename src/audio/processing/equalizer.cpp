// src/audio/processing/equalizer.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Equalizer-extension Copyright (C) 2008 Hendrik Iben <hiben@tzi.de>
// Provides a parametric biquadratic equalizer.
// This code is based on the 'Cookbook formulae for audio EQ biquad filter
// coefficients' by Robert Bristow-Johnson.
// http://www.musicdsp.org/files/Audio-EQ-Cookbook.txt
// TODO:
// - Merge somehow with softmixer code to avoid multiple endianness
// conversions.
// - Implement equalizer routines for integer samples... conversion
// to float (and back) is lazy...
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
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <locale.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <string>
#include <fstream>
#include <sstream>
#include <memory>
#include <new>

#include "core/common.h"
#include "audio/audio.h"
#include "audio/conversion/audio_conversion.h"
#include "core/options.h"
#include "core/log.h"
#include "library/files.h"
#include "audio/processing/equalizer.h"

#define TWOPI (2.0 * M_PI)

/* RAII helper: switches a locale category to "C" for the scope's
 * duration (needed so '.' is always the decimal point when reading/
 * writing EQ config files), restoring the previous locale on exit
 * regardless of how the scope is left. */
class ScopedCLocale
{
public:
  explicit ScopedCLocale(int category) : category_(category)
  {
    const char *cur = setlocale(category_, nullptr);
    saved_ = cur ? cur : "";
    setlocale(category_, "C");
  }

  ~ScopedCLocale()
  {
    if (!saved_.empty())
    {
      setlocale(category_, saved_.c_str());
    }
  }

  ScopedCLocale(const ScopedCLocale &) = delete;
  ScopedCLocale &operator=(const ScopedCLocale &) = delete;

private:
  int category_;
  std::string saved_;
};

#define NEWLINE 0x0A
#define CRETURN 0x0D
#define SPACE 0x20

#define EQSET_HEADER "EQSET"

#define EQUALIZER_CFG_ACTIVE "Active:"
#define EQUALIZER_CFG_PRESET "Preset:"
#define EQUALIZER_CFG_MIXIN "Mixin:"

#define EQUALIZER_SAVE_FILE "equalizer"
#define EQUALIZER_SAVE_OPTION "Equalizer_SaveState"

typedef struct t_biquad t_biquad;

struct t_biquad
{
  float a0;
  float a1;
  float a2;
  float a3;
  float a4;
  float x1;
  float x2;
  float y1;
  float y2;
  float cf;
  float bw;
  float gain;
  float srate;
  int israte;
};

typedef struct t_eq_setup t_eq_setup;

struct t_eq_setup
{
  std::string name;
  float preamp;
  int bcount;
  std::vector<float> cf;
  std::vector<float> bw;
  std::vector<float> dg;
};

typedef struct t_eq_set t_eq_set;

struct t_eq_set
{
  std::string name;
  int channels;
  float preamp;
  int bcount;
  std::vector<t_biquad> b;
};

typedef struct t_eq_set_list t_eq_set_list;

struct t_eq_set_list
{
  t_eq_set *set;
  t_eq_set_list *prev;
  t_eq_set_list *next;
};

typedef struct t_active_set t_active_set;

struct t_active_set
{
  int srate;
  t_eq_set *set;
};

typedef struct t_eq_settings t_eq_settings;

struct t_eq_settings
{
  char *preset_name;
  int bcount;
  float *gain;
  t_eq_settings *next;
};

/* config processing */
static char *skip_line(char *s);
static char *skip_whitespace(char *s);
static int read_float(char *s, float *f, char **endp);
static int read_setup(const char *name, char *desc, std::unique_ptr<t_eq_setup> &s);
static void equalizer_adjust_preamp();
static void equalizer_read_config();
static void equalizer_write_config();

/* biquad application */
static inline void apply_biquads(float *src, float *dst, int channels, int len,
                                 t_biquad *b, int blen);

/* biquad filter creation */
static t_biquad *mk_biquad(float dbgain, float cf, float srate, float bw,
                           t_biquad *b);

/* equalizer list processing */
static t_eq_set_list *append_eq_set(t_eq_set *eqs, t_eq_set_list *l);
static void clear_eq_set(t_eq_set_list *l);

/* sound processing */
static void equ_process_buffer_u8(uint8_t *buf, size_t samples);
static void equ_process_buffer_s8(int8_t *buf, size_t samples);
static void equ_process_buffer_u16(uint16_t *buf, size_t samples);
static void equ_process_buffer_s16(int16_t *buf, size_t samples);
static void equ_process_buffer_u24(uint32_t *buf, size_t samples);
static void equ_process_buffer_s24(int32_t *buf, size_t samples);
static void equ_process_buffer_u32(uint32_t *buf, size_t samples);
static void equ_process_buffer_s32(int32_t *buf, size_t samples);
static void equ_process_buffer_float(float *buf, size_t samples);

/* static global variables */
static t_eq_set_list equ_list;
static t_eq_set_list *current_equ;

static int sample_rate;
static int equ_active;
static int equ_channels;

static float mixin_rate;
static float r_mixin_rate;

static float preamp;
static float preampf;

static std::string eqsetdir;

static std::string config_preset_name;

/* public functions */
int equalizer_is_active() { return equ_active ? 1 : 0; }

int equalizer_set_active(int active) { return equ_active = active ? 1 : 0; }

std::string equalizer_current_eqname()
{
  if (equ_active && current_equ && current_equ->set)
  {
    return current_equ->set->name;
  }

  return std::string("off");
}

void equalizer_next()
{
  if (current_equ)
  {
    if (current_equ->next)
    {
      current_equ = current_equ->next;
    }
    else
    {
      current_equ = &equ_list;
    }

    if (!current_equ->set && !(current_equ == &equ_list && !current_equ->next))
    {
      equalizer_next();
    }
  }

  equalizer_adjust_preamp();
}

void equalizer_prev()
{
  if (current_equ)
  {
    if (current_equ->prev)
    {
      current_equ = current_equ->prev;
    }
    else
    {
      while (current_equ->next)
      {
        current_equ = current_equ->next;
      }
    }

    if (!current_equ->set && !(current_equ == &equ_list && !current_equ->next))
    {
      equalizer_prev();
    }
  }

  equalizer_adjust_preamp();
}

/* biquad functions */

/* Create a Peaking EQ Filter.
 * See 'Audio EQ Cookbook' for more information
 */
static t_biquad *mk_biquad(float dbgain, float cf, float srate, float bw,
                           t_biquad *b)
{
  if (b == nullptr)
  {
    b = new t_biquad;
  }

  float A = powf(10.0f, dbgain / 40.0f);
  float omega = TWOPI * cf / srate;
  float sn = sin(omega);
  float cs = cos(omega);
  float alpha = sn * sinh(M_LN2 / 2.0f * bw * omega / sn);

  float alpha_m_A = alpha * A;
  float alpha_d_A = alpha / A;

  float b0 = 1.0f + alpha_m_A;
  float b1 = -2.0f * cs;
  float b2 = 1.0f - alpha_m_A;
  float a0 = 1.0f + alpha_d_A;
  float a1 = b1;
  float a2 = 1.0f - alpha_d_A;

  b->a0 = b0 / a0;
  b->a1 = b1 / a0;
  b->a2 = b2 / a0;
  b->a3 = a1 / a0;
  b->a4 = a2 / a0;

  b->x1 = 0.0f;
  b->x2 = 0.0f;
  b->y1 = 0.0f;
  b->y2 = 0.0f;

  b->cf = cf;
  b->bw = bw;
  b->srate = srate;
  b->israte = static_cast<int>(srate);
  b->gain = dbgain;

  return b;
}

/* Applies a set of biquadratic filters to a buffer of floating point
 * samples.
 * It is safe to have the same input and output buffer.
 *
 * blen is the sample-count ignoring channels (samples per channel * channels)
 */
static inline void apply_biquads(float *src, float *dst, int channels, int len,
                                 t_biquad *b, int blen)
{
  while (len > 0)
  {
    int boffs = 0;
    for (int ci = 0; ci < channels; ci++)
    {
      float s = *src++;
      float f = s;
      for (int bi = 0; bi < blen; bi++)
      {
        int idx = boffs + bi;
        f = s * b[idx].a0 + b[idx].a1 * b[idx].x1 + b[idx].a2 * b[idx].x2 -
            b[idx].a3 * b[idx].y1 - b[idx].a4 * b[idx].y2;
        b[idx].x2 = b[idx].x1;
        b[idx].x1 = s;
        b[idx].y2 = b[idx].y1;
        b[idx].y1 = f;
        s = f;
      }
      *dst++ = f;
      boffs += blen;
      len--;
    }
  }
}

/*
 preamping
 XMMS / Beep Media Player / Audacious use all the same code but
 do something I do not understand for preamping...

 actually preamping by X dB should be like
 sample * 10^(X/20)

 they do:
 sample * (( 1.0 + 0.0932471 * X + 0.00279033 * X^2 ) / 2)

 what are these constants ?
 the equations are not even close to each other in their results...
 - hiben
*/
static void equalizer_adjust_preamp()
{
  if (current_equ && current_equ->set)
  {
    preamp = current_equ->set->preamp;
    preampf = powf(10.0f, current_equ->set->preamp / 20.0f);
  }
}

static void equalizer_read_config()
{
  ScopedCLocale locale_guard(LC_NUMERIC);

  std::string sfile = create_file_name("equalizer");

  FILE *cf = fopen(sfile.c_str(), "r");

  if (cf == nullptr)
  {
    logit("Unable to read equalizer configuration");
    return;
  }

  while (auto linebuffer = read_line(cf))
  {
    if (strncasecmp(linebuffer->c_str(), EQUALIZER_CFG_ACTIVE, sizeof(EQUALIZER_CFG_ACTIVE) - 1) == 0)
    {
      try {
        int tmp = std::stoi(linebuffer->substr(sizeof(EQUALIZER_CFG_ACTIVE) - 1));
        equ_active = (tmp > 0) ? 1 : 0;
      } catch (const std::exception&) { /* Ignore malformed values */ }
    }
    else if (strncasecmp(linebuffer->c_str(), EQUALIZER_CFG_MIXIN, sizeof(EQUALIZER_CFG_MIXIN) - 1) == 0)
    {
      try {
        float ftmp = std::stof(linebuffer->substr(sizeof(EQUALIZER_CFG_MIXIN) - 1));
        if (in_closed_range(0.0f, ftmp, 1.0f)) mixin_rate = ftmp;
      } catch (const std::exception&) { /* Ignore malformed values */ }
    }
    else if (strncasecmp(linebuffer->c_str(), EQUALIZER_CFG_PRESET, sizeof(EQUALIZER_CFG_PRESET) - 1) == 0)
    {
      std::string val_str = linebuffer->substr(sizeof(EQUALIZER_CFG_PRESET) - 1);
      size_t first = val_str.find_first_not_of(" \t\r\n");
      if (first != std::string::npos) {
          size_t last = val_str.find_last_not_of(" \t\r\n");
          config_preset_name = val_str.substr(first, last - first + 1);
      }
    }
  }

  fclose(cf);
}

static void equalizer_write_config()
{
  ScopedCLocale locale_guard(LC_NUMERIC);

  std::string cfname = create_file_name(EQUALIZER_SAVE_FILE);

  FILE *cf = fopen(cfname.c_str(), "w");

  if (cf == nullptr)
  {
    logit("Unable to write equalizer configuration");
    return;
  }

  fprintf(cf, "%s %i\n", EQUALIZER_CFG_ACTIVE, equ_active);
  if (current_equ && current_equ->set)
  {
    fprintf(cf, "%s %s\n", EQUALIZER_CFG_PRESET, current_equ->set->name.c_str());
  }
  fprintf(cf, "%s %f\n", EQUALIZER_CFG_MIXIN, mixin_rate);

  fclose(cf);

  logit("Equalizer configuration written");
}

void equalizer_init()
{
  equ_active = 1;

  equ_list.set = nullptr;
  equ_list.next = nullptr;
  equ_list.prev = nullptr;

  sample_rate = 44100;

  equ_channels = 2;

  preamp = 0.0f;

  preampf = powf(10.0f, preamp / 20.0f);

  eqsetdir = create_file_name("eqsets");

  config_preset_name.clear();

  mixin_rate = 0.25f;

  equalizer_read_config();

  r_mixin_rate = 1.0f - mixin_rate;

  equalizer_refresh();

  logit("Equalizer initialized");
}

void equalizer_shutdown()
{
  if (options_get_bool(EQUALIZER_SAVE_OPTION))
  {
    equalizer_write_config();
  }

  clear_eq_set(&equ_list);

  logit("Equalizer stopped");
}

void equalizer_refresh()
{
  char buf[1024];

  std::string current_set_name;

  if (current_equ && current_equ->set)
  {
    current_set_name = current_equ->set->name;
  }
  else
  {
    if (!config_preset_name.empty())
    {
      current_set_name = config_preset_name;
    }
  }

  clear_eq_set(&equ_list);

  current_equ = nullptr;

  DIR *d = opendir(eqsetdir.c_str());

  if (!d)
  {
    return;
  }

  struct dirent *de = readdir(d);
  struct stat st;

  t_eq_set_list *last_elem;

  last_elem = &equ_list;

  while (de)
  {
    snprintf(buf, sizeof(buf), "eqsets/%s", de->d_name);

    std::string filename = create_file_name(buf);

    stat(filename.c_str(), &st);

    if (S_ISREG(st.st_mode))
    {
      std::ifstream f(filename);

      if (f)
      {
        std::stringstream buffer;
        buffer << f.rdbuf();
        std::string content = buffer.str();

        std::vector<char> filebuffer(content.begin(), content.end());
        filebuffer.push_back('\0');

        std::unique_ptr<t_eq_setup> eqs;
        int r = read_setup(de->d_name, filebuffer.data(), eqs);

        if (r == 0)
        {
          t_eq_set *eqset = new t_eq_set;
          eqset->b.resize(eqs->bcount * equ_channels);

          eqset->name = eqs->name;
          eqset->preamp = eqs->preamp;
          eqset->bcount = eqs->bcount;
          eqset->channels = equ_channels;

          for (int i = 0; i < eqs->bcount; i++)
          {
            mk_biquad(eqs->dg[i], eqs->cf[i], sample_rate, eqs->bw[i],
                      &eqset->b[i]);

            for (int channel = 1; channel < equ_channels; channel++)
            {
              eqset->b[channel * eqset->bcount + i] = eqset->b[i];
            }
          }

          last_elem = append_eq_set(eqset, last_elem);
        }
        else
        {
          switch (r)
          {
            case -1:
              logit("Not an EQSET (empty file): %s", filename.c_str());
              break;
            case -2:
              logit("Not an EQSET (invalid header): %s", filename.c_str());
              break;
            case -3:
              logit("Error while parsing settings from EQSET: %s", filename.c_str());
              break;
            default:
              logit("Unknown error while parsing EQSET: %s", filename.c_str());
              break;
          }
        }
      }
    }

    de = readdir(d);
  }

  closedir(d);

  current_equ = &equ_list;

  if (!current_set_name.empty())
  {
    current_equ = &equ_list;

    while (current_equ)
    {
      if (current_equ->set)
      {
        if (current_set_name == current_equ->set->name)
        {
          break;
        }
      }
      current_equ = current_equ->next;
    }
  }

  if (!current_equ && !current_set_name.empty())
  {
    logit("EQ %s not found.", current_set_name.c_str());
    /* equalizer not found, pick next equalizer */
    current_equ = &equ_list;
  }
  if (current_equ && !current_equ->set)
  {
    equalizer_next();
  }

  equalizer_adjust_preamp();
}

/* sound processing code */
void equalizer_process_buffer(char *buf, size_t size,
                              const struct sound_params *sound_params)
{
  debug("EQ Processing %zu bytes...", size);

  if (!equ_active || !current_equ || !current_equ->set)
  {
    return;
  }

  if (sound_params->rate != current_equ->set->b[0].israte ||
      sound_params->channels != equ_channels)
  {
    logit("Recreating filters due to sound parameter changes...");
    sample_rate = sound_params->rate;
    equ_channels = sound_params->channels;

    equalizer_refresh();
  }

  long sound_format = sound_params->fmt & SFMT_MASK_FORMAT;
  int samplewidth = sfmt_Bps(sound_format);

  assert(size % (samplewidth * sound_params->channels) == 0);

  switch (sound_format)
  {
    case SFMT_U8:
      equ_process_buffer_u8(reinterpret_cast<uint8_t *>(buf), size);
      break;
    case SFMT_S8:
      equ_process_buffer_s8(reinterpret_cast<int8_t *>(buf), size);
      break;
    case SFMT_U16:
      equ_process_buffer_u16(reinterpret_cast<uint16_t *>(buf), size / sizeof(uint16_t));
      break;
    case SFMT_S16:
      equ_process_buffer_s16(reinterpret_cast<int16_t *>(buf), size / sizeof(int16_t));
      break;
    case SFMT_U24:
      equ_process_buffer_u24(reinterpret_cast<uint32_t *>(buf), size / sizeof(uint32_t));
      break;
    case SFMT_S24:
      equ_process_buffer_s24(reinterpret_cast<int32_t *>(buf), size / sizeof(int32_t));
      break;
    case SFMT_U32:
      equ_process_buffer_u32(reinterpret_cast<uint32_t *>(buf), size / sizeof(uint32_t));
      break;
    case SFMT_S32:
      equ_process_buffer_s32(reinterpret_cast<int32_t *>(buf), size / sizeof(int32_t));
      break;
    case SFMT_FLOAT:
      equ_process_buffer_float(std::launder(reinterpret_cast<float *>(buf)), size / sizeof(float));
      break;
  }
}

static void equ_process_buffer_u8(uint8_t *buf, size_t samples)
{
  size_t i;
  std::vector<float> tmp(samples);

  debug("equalizing");

  for (i = 0; i < samples; i++)
  {
    tmp[i] = preampf * static_cast<float>(buf[i]);
  }

  apply_biquads(tmp.data(), tmp.data(), equ_channels, samples, current_equ->set->b.data(),
                current_equ->set->bcount);

  for (i = 0; i < samples; i++)
  {
    tmp[i] = r_mixin_rate * tmp[i] + mixin_rate * buf[i];
    tmp[i] = std::clamp<float>(tmp[i], 0, UINT8_MAX);
    buf[i] = static_cast<uint8_t>(tmp[i]);
  }

}

static void equ_process_buffer_s8(int8_t *buf, size_t samples)
{
  size_t i;
  std::vector<float> tmp(samples);

  debug("equalizing");

  for (i = 0; i < samples; i++)
  {
    tmp[i] = preampf * static_cast<float>(buf[i]);
  }

  apply_biquads(tmp.data(), tmp.data(), equ_channels, samples, current_equ->set->b.data(),
                current_equ->set->bcount);

  for (i = 0; i < samples; i++)
  {
    tmp[i] = r_mixin_rate * tmp[i] + mixin_rate * buf[i];
    tmp[i] = std::clamp<float>(tmp[i], INT8_MIN, INT8_MAX);
    buf[i] = static_cast<int8_t>(tmp[i]);
  }

}

static void equ_process_buffer_u16(uint16_t *buf, size_t samples)
{
  size_t i;
  std::vector<float> tmp(samples);

  debug("equalizing");

  for (i = 0; i < samples; i++)
  {
    tmp[i] = preampf * static_cast<float>(buf[i]);
  }

  apply_biquads(tmp.data(), tmp.data(), equ_channels, samples, current_equ->set->b.data(),
                current_equ->set->bcount);

  for (i = 0; i < samples; i++)
  {
    tmp[i] = r_mixin_rate * tmp[i] + mixin_rate * buf[i];
    tmp[i] = std::clamp<float>(tmp[i], 0, UINT16_MAX);
    buf[i] = static_cast<uint16_t>(tmp[i]);
  }

}

static void equ_process_buffer_s16(int16_t *buf, size_t samples)
{
  size_t i;
  std::vector<float> tmp(samples);

  debug("equalizing");

  for (i = 0; i < samples; i++)
  {
    tmp[i] = preampf * static_cast<float>(buf[i]);
  }

  apply_biquads(tmp.data(), tmp.data(), equ_channels, samples, current_equ->set->b.data(),
                current_equ->set->bcount);

  for (i = 0; i < samples; i++)
  {
    tmp[i] = r_mixin_rate * tmp[i] + mixin_rate * buf[i];
    tmp[i] = std::clamp<float>(tmp[i], INT16_MIN, INT16_MAX);
    buf[i] = static_cast<int16_t>(tmp[i]);
  }

}

static void equ_process_buffer_u24(uint32_t *buf, size_t samples)
{
  size_t i;
  std::vector<float> tmp(samples);

  debug("equalizing");

  for (i = 0; i < samples; i++)
  {
    tmp[i] = preampf * static_cast<float>(buf[i]);
  }

  apply_biquads(tmp.data(), tmp.data(), equ_channels, samples, current_equ->set->b.data(),
                current_equ->set->bcount);

  for (i = 0; i < samples; i++)
  {
    tmp[i] = r_mixin_rate * tmp[i] + mixin_rate * buf[i];
    tmp[i] = std::clamp<float>(tmp[i], 0, U24_MAX);
    buf[i] = static_cast<uint32_t>(tmp[i]);
  }

}

static void equ_process_buffer_s24(int32_t *buf, size_t samples)
{
  size_t i;
  std::vector<float> tmp(samples);

  debug("equalizing");

  for (i = 0; i < samples; i++)
  {
    tmp[i] = preampf * static_cast<float>(buf[i]);
  }

  apply_biquads(tmp.data(), tmp.data(), equ_channels, samples, current_equ->set->b.data(),
                current_equ->set->bcount);

  for (i = 0; i < samples; i++)
  {
    tmp[i] = r_mixin_rate * tmp[i] + mixin_rate * buf[i];
    tmp[i] = std::clamp<float>(tmp[i], S24_MIN, S24_MAX);
    buf[i] = static_cast<int32_t>(tmp[i]);
  }

}

static void equ_process_buffer_u32(uint32_t *buf, size_t samples)
{
  size_t i;
  std::vector<float> tmp(samples);

  debug("equalizing");

  for (i = 0; i < samples; i++)
  {
    tmp[i] = preampf * static_cast<float>(buf[i]);
  }

  apply_biquads(tmp.data(), tmp.data(), equ_channels, samples, current_equ->set->b.data(),
                current_equ->set->bcount);

  for (i = 0; i < samples; i++)
  {
    tmp[i] = r_mixin_rate * tmp[i] + mixin_rate * buf[i];
    tmp[i] = std::clamp(tmp[i], 0.0f, static_cast<float>(UINT32_MAX));
    buf[i] = static_cast<uint32_t>(tmp[i]);
  }

}

static void equ_process_buffer_s32(int32_t *buf, size_t samples)
{
  size_t i;
  std::vector<float> tmp(samples);

  debug("equalizing");

  for (i = 0; i < samples; i++)
  {
    tmp[i] = preampf * static_cast<float>(buf[i]);
  }

  apply_biquads(tmp.data(), tmp.data(), equ_channels, samples, current_equ->set->b.data(),
                current_equ->set->bcount);

  for (i = 0; i < samples; i++)
  {
    tmp[i] = r_mixin_rate * tmp[i] + mixin_rate * buf[i];
    tmp[i] = std::clamp(tmp[i], static_cast<float>(INT32_MIN), static_cast<float>(INT32_MAX));
    buf[i] = static_cast<int32_t>(tmp[i]);
  }

}

static void equ_process_buffer_float(float *buf, size_t samples)
{
  size_t i;
  std::vector<float> tmp(samples);

  debug("equalizing");

  for (i = 0; i < samples; i++)
  {
    tmp[i] = preampf * buf[i];
  }

  apply_biquads(tmp.data(), tmp.data(), equ_channels, samples, current_equ->set->b.data(),
                current_equ->set->bcount);

  for (i = 0; i < samples; i++)
  {
    tmp[i] = r_mixin_rate * tmp[i] + mixin_rate * buf[i];
    tmp[i] = std::clamp(tmp[i], -1.0f, 1.0f);
    buf[i] = tmp[i];
  }

}

/* equalizer list maintenance */
static t_eq_set_list *append_eq_set(t_eq_set *eqs, t_eq_set_list *l)
{
  if (l->set == nullptr)
  {
    l->set = eqs;
  }
  else
  {
    if (l->next)
    {
      append_eq_set(eqs, l->next);
    }
    else
    {
      l->next = new t_eq_set_list;
      l->next->set  = nullptr;
      l->next->next = nullptr;
      l->next->prev = l;
      l = append_eq_set(eqs, l->next);
    }
  }

  return l;
}

static void clear_eq_set(t_eq_set_list *l)
{
  if (l->set)
  {
    delete l->set;
    l->set = nullptr;
  }
  if (l->next)
  {
    clear_eq_set(l->next);
    delete l->next;
    l->next = nullptr;
  }
}

/* parsing stuff */
static int read_setup(const char *name, char *desc, std::unique_ptr<t_eq_setup> &s)
{
  ScopedCLocale locale_guard(LC_NUMERIC);

  desc = skip_whitespace(desc);

  if (!*desc)
  {
    return -1;
  }

  if (strncasecmp(desc, EQSET_HEADER, sizeof(EQSET_HEADER) - 1))
  {
    return -2;
  }

  desc += 5;

  desc = skip_whitespace(skip_line(desc));

  if (!s)
  {
    s = std::make_unique<t_eq_setup>();
  }

  s->name = name;
  s->bcount = 0;
  s->preamp = 0.0f;
  int max_values = 16;
  s->cf.resize(max_values);
  s->bw.resize(max_values);
  s->dg.resize(max_values);

  while (*desc)
  {
    char *endp;

    float cf_val = 0.0f;

    int r = read_float(desc, &cf_val, &endp);

    if (r != 0)
    {
      return -3;
    }

    desc = skip_whitespace(endp);

    float bw_val = 0.0f;

    r = read_float(desc, &bw_val, &endp);

    if (r != 0)
    {
      return -3;
    }

    desc = skip_whitespace(endp);

    float dg_val = 0.0f;

    /* 0Hz means preamp, only one parameter then */
    if (cf_val != 0.0f)
    {
      r = read_float(desc, &dg_val, &endp);

      if (r != 0)
      {
        return -3;
      }

      desc = skip_whitespace(endp);

      if (s->bcount >= (max_values - 1))
      {
        max_values *= 2;
        s->cf.resize(max_values);
        s->bw.resize(max_values);
        s->dg.resize(max_values);
      }

      s->cf[s->bcount] = cf_val;
      s->bw[s->bcount] = bw_val;
      s->dg[s->bcount] = dg_val;

      s->bcount++;
    }
    else
    {
      s->preamp = bw_val;
    }
  }

  return 0;
}

static char *skip_line(char *s)
{
  int dos_line = 0;
  while (*s && (*s != CRETURN && *s != NEWLINE))
  {
    s++;
  }

  if (*s == CRETURN)
  {
    dos_line = 1;
  }

  if (*s)
  {
    s++;
  }

  if (dos_line && *s == NEWLINE)
  {
    s++;
  }

  return s;
}

static char *skip_whitespace(char *s)
{
  while (*s && (*s <= SPACE))
  {
    s++;
  }

  if (!*s)
  {
    return s;
  }

  if (*s == '#')
  {
    s = skip_line(s);

    s = skip_whitespace(s);
  }

  return s;
}

static int read_float(char *s, float *f, char **endp)
{
  errno = 0;

  float t = strtof(s, endp);

  if (errno == ERANGE)
  {
    return -1;
  }

  if (*endp == s)
  {
    return -2;
  }

  *f = t;

  return 0;
}

// EOF
