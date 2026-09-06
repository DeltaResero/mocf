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
#include <atomic>
#include <mutex>

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
static void mk_biquad(float dbgain, float cf, float srate, float bw,
                      t_biquad &b);

/* sound processing */
template <typename T>
static void equ_process_buffer(T *buf, size_t samples, float min_val, float max_val,
                               t_eq_set *set);

/* static global variables */

/* Guards equ_sets, current_equ_idx, sample_rate, equ_channels, the
 * preamp pair and equ_scratch. */
static std::mutex equ_mtx;

static std::vector<t_eq_set> equ_sets;
static int current_equ_idx = -1; /* -1: no preset loaded */

/* Currently-selected preset, or nullptr if none is loaded. Hold equ_mtx,
 * and drop the pointer with it: a refresh frees what it points at. */
static t_eq_set *current_set()
{
  return (current_equ_idx >= 0 && current_equ_idx < static_cast<int>(equ_sets.size()))
         ? &equ_sets[current_equ_idx] : nullptr;
}

static int sample_rate;
static int equ_channels;

/* Read by the audio thread outside equ_mtx. */
static std::atomic<int> equ_active;

static float mixin_rate;
static float r_mixin_rate;

static float preamp;
static float preampf;

/* Scratch for equ_process_buffer, kept between buffers. */
static std::vector<float> equ_scratch;

static std::string eqsetdir;

static std::string config_preset_name;

/* public functions */
int equalizer_is_active() { return equ_active ? 1 : 0; }

int equalizer_set_active(int active) { return equ_active = active ? 1 : 0; }

std::string equalizer_current_eqname()
{
  if (equ_active)
  {
    std::lock_guard<std::mutex> lock(equ_mtx);

    if (t_eq_set *cur = current_set())
      return cur->name;
  }

  return std::string("off");
}

void equalizer_next()
{
  std::lock_guard<std::mutex> lock(equ_mtx);

  if (!equ_sets.empty())
  {
    current_equ_idx = (current_equ_idx + 1) % static_cast<int>(equ_sets.size());
  }

  equalizer_adjust_preamp();
}

void equalizer_prev()
{
  std::lock_guard<std::mutex> lock(equ_mtx);

  if (!equ_sets.empty())
  {
    int count = static_cast<int>(equ_sets.size());
    current_equ_idx = (current_equ_idx - 1 + count) % count;
  }

  equalizer_adjust_preamp();
}

/* biquad functions */

/* Create a Peaking EQ Filter.
 * See 'Audio EQ Cookbook' for more information
 */
static void mk_biquad(float dbgain, float cf, float srate, float bw,
                      t_biquad &b)
{
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

  b.a0 = b0 / a0;
  b.a1 = b1 / a0;
  b.a2 = b2 / a0;
  b.a3 = a1 / a0;
  b.a4 = a2 / a0;

  b.x1 = 0.0f;
  b.x2 = 0.0f;
  b.y1 = 0.0f;
  b.y2 = 0.0f;

  b.cf = cf;
  b.bw = bw;
  b.srate = srate;
  b.israte = static_cast<int>(srate);
  b.gain = dbgain;
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
/* Caller must hold equ_mtx. */
static void equalizer_adjust_preamp()
{
  if (t_eq_set *cur = current_set())
  {
    preamp = cur->preamp;
    preampf = powf(10.0f, cur->preamp / 20.0f);
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

  std::string preset_name;

  {
    std::lock_guard<std::mutex> lock(equ_mtx);

    if (t_eq_set *cur = current_set())
    {
      preset_name = cur->name;
    }
  }

  fprintf(cf, "%s %i\n", EQUALIZER_CFG_ACTIVE, equ_active.load());
  if (!preset_name.empty())
  {
    fprintf(cf, "%s %s\n", EQUALIZER_CFG_PRESET, preset_name.c_str());
  }
  fprintf(cf, "%s %f\n", EQUALIZER_CFG_MIXIN, mixin_rate);

  fclose(cf);

  logit("Equalizer configuration written");
}

void equalizer_init()
{
  equ_active = 1;

  {
    std::lock_guard<std::mutex> lock(equ_mtx);

    equ_sets.clear();
    current_equ_idx = -1;

    sample_rate = 44100;

    equ_channels = 2;

    preamp = 0.0f;

    preampf = powf(10.0f, preamp / 20.0f);
  }

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

  {
    std::lock_guard<std::mutex> lock(equ_mtx);

    equ_sets.clear();
    current_equ_idx = -1;
    equ_scratch.clear();
    equ_scratch.shrink_to_fit();
  }

  logit("Equalizer stopped");
}

void equalizer_refresh()
{
  char buf[1024];

  /* Note what to build against, then build off the lock so the audio
   * thread keeps playing on the old sets while the disk is read. */
  std::string current_set_name;
  int build_rate;
  int build_channels;

  {
    std::lock_guard<std::mutex> lock(equ_mtx);

    if (t_eq_set *cur = current_set())
    {
      current_set_name = cur->name;
    }
    else if (!config_preset_name.empty())
    {
      current_set_name = config_preset_name;
    }

    build_rate = sample_rate;
    build_channels = equ_channels;
  }

  std::vector<t_eq_set> new_sets;

  DIR *d = opendir(eqsetdir.c_str());

  struct dirent *de = d ? readdir(d) : nullptr;
  struct stat st;

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
          t_eq_set eqset;
          eqset.b.resize(eqs->bcount * build_channels);

          eqset.name = eqs->name;
          eqset.preamp = eqs->preamp;
          eqset.bcount = eqs->bcount;
          eqset.channels = build_channels;

          for (int i = 0; i < eqs->bcount; i++)
          {
            mk_biquad(eqs->dg[i], eqs->cf[i], build_rate, eqs->bw[i],
                      eqset.b[i]);

            for (int channel = 1; channel < build_channels; channel++)
            {
              eqset.b[channel * eqset.bcount + i] = eqset.b[i];
            }
          }

          new_sets.push_back(std::move(eqset));
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
            case -4:
              logit("Not an EQSET (no filter bands): %s", filename.c_str());
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

  if (d)
  {
    closedir(d);
  }

  int new_idx = new_sets.empty() ? -1 : 0;

  if (!current_set_name.empty())
  {
    new_idx = -1;

    for (size_t i = 0; i < new_sets.size(); i++)
    {
      if (current_set_name == new_sets[i].name)
      {
        new_idx = static_cast<int>(i);
        break;
      }
    }

    if (new_idx == -1)
    {
      logit("EQ %s not found.", current_set_name.c_str());
      /* equalizer not found, pick the first one (if any) */
      new_idx = new_sets.empty() ? -1 : 0;
    }
  }

  /* The only part of a rebuild the audio thread can wait on. */
  {
    std::lock_guard<std::mutex> lock(equ_mtx);

    equ_sets = std::move(new_sets);
    current_equ_idx = new_idx;

    equalizer_adjust_preamp();
  }
}

/* sound processing code */
void equalizer_process_buffer(char *buf, size_t size,
                              const struct sound_params *sound_params)
{
  debug("EQ Processing %zu bytes...", size);

  if (!equ_active)
  {
    return;
  }

  /* Checked before the buffer lock: equalizer_refresh() takes equ_mtx
   * and would deadlock this thread against itself. */
  bool stale;

  {
    std::lock_guard<std::mutex> lock(equ_mtx);

    t_eq_set *cur = current_set();

    if (!cur)
    {
      return;
    }

    stale = (sound_params->rate != cur->b[0].israte ||
             sound_params->channels != equ_channels);

    if (stale)
    {
      sample_rate = sound_params->rate;
      equ_channels = sound_params->channels;
    }
  }

  if (stale)
  {
    logit("Recreating filters due to sound parameter changes...");
    equalizer_refresh();
  }

  long sound_format = sound_params->fmt & SFMT_MASK_FORMAT;
  int samplewidth = sfmt_Bps(sound_format);

  assert(size % (samplewidth * sound_params->channels) == 0);

  /* Held for the whole buffer; the rebuild above may have left none. */
  std::lock_guard<std::mutex> lock(equ_mtx);

  t_eq_set *cur = current_set();

  if (!cur)
  {
    return;
  }

  switch (sound_format)
  {
    case SFMT_U8:
      equ_process_buffer(reinterpret_cast<uint8_t *>(buf), size, 0.0f, static_cast<float>(UINT8_MAX), cur);
      break;
    case SFMT_S8:
      equ_process_buffer(reinterpret_cast<int8_t *>(buf), size, static_cast<float>(INT8_MIN), static_cast<float>(INT8_MAX), cur);
      break;
    case SFMT_U16:
      equ_process_buffer(reinterpret_cast<uint16_t *>(buf), size / sizeof(uint16_t), 0.0f, static_cast<float>(UINT16_MAX), cur);
      break;
    case SFMT_S16:
      equ_process_buffer(reinterpret_cast<int16_t *>(buf), size / sizeof(int16_t), static_cast<float>(INT16_MIN), static_cast<float>(INT16_MAX), cur);
      break;
    case SFMT_U24:
      equ_process_buffer(reinterpret_cast<uint32_t *>(buf), size / sizeof(uint32_t), 0.0f, static_cast<float>(U24_MAX), cur);
      break;
    case SFMT_S24:
      equ_process_buffer(reinterpret_cast<int32_t *>(buf), size / sizeof(int32_t), static_cast<float>(S24_MIN), static_cast<float>(S24_MAX), cur);
      break;
    case SFMT_U32:
      equ_process_buffer(reinterpret_cast<uint32_t *>(buf), size / sizeof(uint32_t), 0.0f, static_cast<float>(UINT32_MAX), cur);
      break;
    case SFMT_S32:
      equ_process_buffer(reinterpret_cast<int32_t *>(buf), size / sizeof(int32_t), static_cast<float>(INT32_MIN), static_cast<float>(INT32_MAX), cur);
      break;
    case SFMT_FLOAT:
      equ_process_buffer(std::launder(reinterpret_cast<float *>(buf)), size / sizeof(float), -1.0f, 1.0f, cur);
      break;
  }
}

/* Hold equ_mtx: writes filter state through set->b. */
template <typename T>
static void equ_process_buffer(T *buf, size_t samples, float min_val, float max_val,
                               t_eq_set *set)
{
  size_t i;

  if (equ_scratch.size() < samples)
  {
    equ_scratch.resize(samples);
  }

  float *tmp = equ_scratch.data();

  debug("equalizing");

  for (i = 0; i < samples; i++)
  {
    tmp[i] = preampf * static_cast<float>(buf[i]);
  }

  apply_biquads(tmp, tmp, equ_channels, samples, set->b.data(),
                set->bcount);

  for (i = 0; i < samples; i++)
  {
    tmp[i] = r_mixin_rate * tmp[i] + mixin_rate * static_cast<float>(buf[i]);
    tmp[i] = std::clamp(tmp[i], min_val, max_val);
    buf[i] = static_cast<T>(tmp[i]);
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

  /* A file holding only a header, or only a preamp line, parses cleanly
   * but describes no filters. The processing code reads b[0] to find the
   * rate the set was built for, so an empty set is indexed out of
   * bounds. Refuse it here instead. */
  if (s->bcount == 0)
  {
    return -4;
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
