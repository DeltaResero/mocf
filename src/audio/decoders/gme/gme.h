// src/audio/decoders/gme/gme.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2026 DeltaResero <deltaresero@zoho.com>
//
// Enables mocf to play chiptune formats (NSF/NSFE, GBS, GYM, HES, KSS,
// SAP, SPC, VGM/VGZ, AY) via libgme (Game_Music_Emu), as a lighter
// alternative to FFmpeg for these formats.

#pragma once

#include "audio/decoder.h"

#ifdef __cplusplus

#include <memory>
#include <vector>
#include <cstdint>

#include <gme/gme.h>

// Option keys, following the same naming convention as the sidplayfp
// decoder's OPT_* keys.
#define OPT_GME_FREQUENCY    "GME_Frequency"      // Hz  (int)
#define OPT_GME_SUBTUNES     "GME_PlaySubTunes"   // bool
// Unlike SidPlayFP_PlaySubTunes (default true), this defaults to false:
// chiptune formats like NSF commonly have dozens of subtunes (one per
// game level/area), where auto-playing through all of them back-to-back
// as "one track" is much more surprising than it is for SID files
// (which typically have a handful of subtunes, all variations on one
// piece). See FFMPEG_REPLACEMENT_HANDOFF.md discussion for this task.

struct gme_data
{
  // Custom deleter so this can be a unique_ptr despite Music_Emu being
  // an opaque type deleted via gme_delete() rather than a destructor.
  struct deleter
  {
    void operator()(Music_Emu *e) const noexcept
    {
      if (e) gme_delete(e);
    }
  };
  std::unique_ptr<Music_Emu, deleter> emu;

  int frequency = 0;
  int sample_format = 0; // SFMT_S16 | SFMT_LE/BE

  int track_count = 0;
  int track_start = 0;   // first track to play (always 0 for now - no
                         // start-track concept in libgme like SID's
                         // startSong, so this is just for symmetry
                         // with sidplayfp_data's shape)
  int track_end = 0;     // last track to play (== track_start unless
                         // play_subtunes is on)
  int current_track = 0;

  std::unique_ptr<int[]> sublengths_ms; // per-track lengths in ms
  int length_ms = 0;                    // total playback length in ms

  int track_length_frames = 0;  // length of current track in sample frames
  int track_elapsed_frames = 0; // frames decoded so far in current track

  struct decoder_error error;
};

#endif /* __cplusplus */

void  *gme_decoder_open           (const char *file);
void   gme_decoder_close          (void *void_data);
void   gme_decoder_get_error      (void *prv_data, struct decoder_error *error);
void   gme_decoder_info           (const char *file_name, struct file_tags *info,
                                    const int tags_sel);
int    gme_decoder_seek           (void *void_data, int sec);
int    gme_decoder_decode         (void *void_data, char *buf, int buf_len,
                                    struct sound_params *sound_params);
int    gme_decoder_get_bitrate    (void *void_data);
int    gme_decoder_get_duration   (void *void_data);
int    gme_decoder_our_format_ext (const char *ext);
extern "C" class AudioPlugin *gme_plugin_init (void);

// EOF
