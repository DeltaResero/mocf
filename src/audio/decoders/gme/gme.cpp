// src/audio/decoders/gme/gme.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Enables mocf to play chiptune formats via libgme (Game_Music_Emu),
// as a lighter alternative to FFmpeg for these formats.
//
// Structure of this plugin is an adaption of the sidplayfp plugin,
// which solves a structurally similar problem (a chip-music emulator
// library with multi-track/subtune files).

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <cstring>
#include <strings.h>
#include <algorithm>
#include <memory>

#include "core/common.h"
#include "audio/decoders/gme/gme.h"
#include "core/log.h"
#include "core/options.h"

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static int  frequency;
static bool play_subtunes;

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------

void *gme_decoder_open(const char *file)
{
  auto *s = new gme_data();
  decoder_error_init(&s->error);

  s->frequency = frequency;

#ifdef WORDS_BIGENDIAN
  s->sample_format = SFMT_S16 | SFMT_BE;
#else
  s->sample_format = SFMT_S16 | SFMT_LE;
#endif

  // ---- Load file --------------------------------------------------------

  Music_Emu *emu = nullptr;
  gme_err_t err = gme_open_file(file, &emu, s->frequency);
  if (err)
  {
    decoder_error(&s->error, ERROR_FATAL, 0, "gme: cannot load \"%s\": %s",
                  file, err);
    return s;
  }
  s->emu.reset(emu);

  s->track_count = gme_track_count(s->emu.get());
  s->track_start = 0;
  s->track_end = play_subtunes ? (s->track_count - 1) : s->track_start;

  // ---- Accumulate per-track lengths --------------------------------------

  s->sublengths_ms = std::make_unique<int[]>(s->track_count);
  s->length_ms = 0;

  for (int track = s->track_start; track <= s->track_end; ++track)
  {
    gme_info_t *info = nullptr;
    err = gme_track_info(s->emu.get(), &info, track);
    if (err)
    {
      decoder_error(&s->error, ERROR_FATAL, 0,
                    "gme: cannot query track %d in \"%s\": %s", track, file,
                    err);
      return s;
    }

    // play_length is libgme's own best-effort estimate, already
    // falling back to a sensible default (150000ms) when the file
    // itself carries no duration hint - no need for mocf-side
    // default/minimum-length options the way sidplayfp needs, since
    // there's no equivalent HVSC-style external duration database
    // being consulted here either.
    int ms = info->play_length;
    s->sublengths_ms[track] = ms;
    s->length_ms += ms;

    gme_free_info(info);
  }

  if (s->length_ms == 0)
  {
    s->length_ms = 150000;
  }

  // ---- Start first track --------------------------------------------------

  s->current_track = s->track_start;
  err = gme_start_track(s->emu.get(), s->current_track);
  if (err)
  {
    decoder_error(&s->error, ERROR_FATAL, 0, "gme: start_track failed: %s",
                  err);
    return s;
  }

  s->track_length_frames = static_cast<int>(
      (static_cast<int64_t>(s->sublengths_ms[s->current_track]) *
       s->frequency) /
      1000);
  s->track_elapsed_frames = 0;

  return s;
}

void gme_decoder_close(void *void_data)
{
  auto *s = static_cast<struct gme_data *>(void_data);
  decoder_error_clear(&s->error);
  delete s;
}

// ---------------------------------------------------------------------------
// Error / metadata
// ---------------------------------------------------------------------------

void gme_decoder_get_error(void *prv_data, struct decoder_error *error)
{
  auto *s = static_cast<struct gme_data *>(prv_data);
  decoder_error_copy(error, &s->error);
}

void gme_decoder_info(const char *file_name, struct file_tags *info,
                      const int tags_sel)
{
  Music_Emu *emu = nullptr;
  gme_err_t err = gme_open_file(file_name, &emu, frequency);
  if (err)
  {
    return;
  }
  std::unique_ptr<Music_Emu, gme_data::deleter> guard(emu);

  int track_count = gme_track_count(emu);
  int track_start = 0;
  int track_end = play_subtunes ? (track_count - 1) : track_start;

  if (tags_sel & TAGS_COMMENTS)
  {
    gme_info_t *ti = nullptr;
    if (!gme_track_info(emu, &ti, track_start))
    {
      if (ti->song[0])
      {
        info->title = ti->song;
        info->filled |= TAGS_COMMENTS;
      }
      if (ti->author[0])
      {
        info->artist = ti->author;
        info->filled |= TAGS_COMMENTS;
      }
      if (ti->game[0])
      {
        info->album = ti->game;
        info->filled |= TAGS_COMMENTS;
      }
      gme_free_info(ti);
    }
  }

  if (tags_sel & TAGS_TIME)
  {
    info->time = 0;
    for (int track = track_start; track <= track_end; ++track)
    {
      gme_info_t *ti = nullptr;
      if (!gme_track_info(emu, &ti, track))
      {
        info->time += ti->play_length / 1000;
        gme_free_info(ti);
      }
    }
    info->filled |= TAGS_TIME;
  }
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

int gme_decoder_seek(void *void_data, int sec)
{
  auto *s = static_cast<struct gme_data *>(void_data);

  if (s->length_ms <= 0 || sec < 0 || sec * 1000 >= s->length_ms)
  {
    return -1;
  }

  // Figure out which virtual track the target time falls into, in
  // case play_subtunes has us treating several tracks as one
  // continuous stream (same accumulated-length model as sidplayfp's
  // decode loop, just applied to seek directly - libgme itself has
  // no notion of our cross-track virtual timeline).
  int target_ms = sec * 1000;
  int track = s->track_start;
  int elapsed_ms = 0;

  while (track < s->track_end &&
         elapsed_ms + s->sublengths_ms[track] <= target_ms)
  {
    elapsed_ms += s->sublengths_ms[track];
    ++track;
  }

  int offset_ms = target_ms - elapsed_ms;

  if (track != s->current_track)
  {
    gme_err_t err = gme_start_track(s->emu.get(), track);
    if (err)
    {
      logit("gme: start_track failed during seek: %s", err);
      return -1;
    }
    s->current_track = track;
    s->track_length_frames = static_cast<int>(
        (static_cast<int64_t>(s->sublengths_ms[track]) * s->frequency) /
        1000);
  }

  gme_err_t err = gme_seek(s->emu.get(), offset_ms);
  if (err)
  {
    logit("gme: seek failed: %s", err);
    return -1;
  }

  s->track_elapsed_frames =
      static_cast<int>((static_cast<int64_t>(offset_ms) * s->frequency) / 1000);

  return sec;
}

int gme_decoder_decode(void *void_data, char *buf, int buf_len,
                       struct sound_params *sound_params)
{
  auto *s = static_cast<struct gme_data *>(void_data);

  if (!s->emu)
  {
    return 0;
  }

  for (;;)
  {
    int frames_remaining = s->track_length_frames - s->track_elapsed_frames;

    if (frames_remaining <= 0 || gme_track_ended(s->emu.get()))
    {
      if (s->current_track >= s->track_end)
      {
        return 0; // all tracks consumed
      }

      ++s->current_track;
      gme_err_t err = gme_start_track(s->emu.get(), s->current_track);
      if (err)
      {
        logit("gme: start_track failed mid-decode: %s", err);
        return 0;
      }

      s->track_length_frames = static_cast<int>(
          (static_cast<int64_t>(s->sublengths_ms[s->current_track]) *
           s->frequency) /
          1000);
      s->track_elapsed_frames = 0;
      continue;
    }

    // Stereo 16-bit output: 4 bytes per frame (2 samples).
    int frames_available = buf_len / 4;
    int frames_to_render = std::min(frames_available, frames_remaining);

    if (frames_to_render <= 0)
    {
      return 0;
    }

    // gme_play()'s count parameter is the total number of 16-bit
    // values to fill, i.e. frames * channels, not a frame count -
    // confirmed empirically against a real file before writing this
    // (see task notes); this is NOT what the doc comment's phrasing
    // alone would tell you unambiguously.
    int count = frames_to_render * 2;

    gme_err_t err =
        gme_play(s->emu.get(), count, reinterpret_cast<int16_t *>(buf));
    if (err)
    {
      decoder_error(&s->error, ERROR_STREAM, 0, "gme: play failed: %s", err);
      return 0;
    }

    s->track_elapsed_frames += frames_to_render;

    sound_params->channels = 2;
    sound_params->rate = s->frequency;
    sound_params->fmt = s->sample_format;

    return count * static_cast<int>(sizeof(int16_t));
  }
}

// ---------------------------------------------------------------------------
// Bitrate / duration / format
// ---------------------------------------------------------------------------

int gme_decoder_get_bitrate(void *)
{
  return -1; // synthesized - no meaningful bitrate
}

int gme_decoder_get_duration(void *void_data)
{
  auto *s = static_cast<struct gme_data *>(void_data);
  return s->length_ms / 1000;
}

int gme_decoder_our_format_ext(const char *ext)
{
  return !strcasecmp(ext, "ay") || !strcasecmp(ext, "gbs") ||
         !strcasecmp(ext, "gym") || !strcasecmp(ext, "hes") ||
         !strcasecmp(ext, "kss") || !strcasecmp(ext, "nsf") ||
         !strcasecmp(ext, "nsfe") || !strcasecmp(ext, "sap") ||
         !strcasecmp(ext, "spc") || !strcasecmp(ext, "vgm") ||
         !strcasecmp(ext, "vgz");
}

// ---------------------------------------------------------------------------
// Plugin lifecycle
// ---------------------------------------------------------------------------

static void init()
{
  frequency = options_get_int(OPT_GME_FREQUENCY);
  play_subtunes = options_get_bool(OPT_GME_SUBTUNES);
}

static void destroy()
{
}

class GmeDecoder : public AudioDecoder {
public:
    std::unique_ptr<void, void(*)(void*)> data;
    GmeDecoder(void *d) : data(d, gme_decoder_close) {}
    ~GmeDecoder() override = default;

    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return gme_decoder_decode(data.get(), buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return gme_decoder_seek(data.get(), sec);
    }

    int get_bitrate() override {
        return gme_decoder_get_bitrate(data.get());
    }

    int get_duration() override {
        return gme_decoder_get_duration(data.get());
    }

    void get_error(struct decoder_error *error) override {
        gme_decoder_get_error(data.get(), error);
    }
};

class GmePlugin : public AudioPlugin {
public:
    void init() override {
        ::init();
    }

    void destroy() override {
        ::destroy();
    }

    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = gme_decoder_open(file);
        if (!d) return nullptr;
        return std::make_unique<GmeDecoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        gme_decoder_info(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return gme_decoder_our_format_ext(ext);
    }
};

extern "C" class AudioPlugin *gme_plugin_init() {
    static GmePlugin plugin;
    return &plugin;
}

// EOF
