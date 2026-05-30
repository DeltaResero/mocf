// src/audio/decoders/sidplayfp/sidplayfp.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// This code is based on the original MOC sidplay2 plugin
// Copyright (C) 2004 Damian Pietras <daper@daper.net>
// Copyright (C) 2007 Hendrik Iben <hiben@tzi.de>
// Copyright (C) 2026 DeltaResero <deltaresero@zoho.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "audio/decoder.h"

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

// libsidplayfp defines debug/error macros that collide with ours.
#undef debug
#undef error

#include <sidplayfp/sidplayfp.h>
#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidConfig.h>
#include <sidplayfp/SidInfo.h>
#include <sidplayfp/SidTuneInfo.h>
#include <sidplayfp/builders/resid.h>
#include <sidplayfp/SidDatabase.h>

// Option keys — updated from old sidplay2 keys
// New key added for chip model selection.
#define OPT_DEFLEN    "SidPlayFP_DefaultSongLength"   // seconds (int)
#define OPT_MINLEN    "SidPlayFP_MinimumSongLength"   // seconds (int)
#define OPT_DATABASE  "SidPlayFP_Database"            // path (str)
#define OPT_FREQ      "SidPlayFP_Frequency"           // Hz    (int)
#define OPT_START     "SidPlayFP_StartAtStart"        // bool
#define OPT_SUBTUNES  "SidPlayFP_PlaySubTunes"        // bool
// 0 = honour tune header (default), 1 = force 6581, 2 = force 8580
#define OPT_SID_MODEL "SidPlayFP_SIDModel"           // int

struct sidplayfp_data
{
    SidTune        *tune;
    sidplayfp      *engine;
    ReSIDBuilder   *builder;

    int   length_ms;        // total playback length in milliseconds
    int  *sublengths_ms;    // per-song lengths in milliseconds
    int   songs;
    int   startSong;
    int   currentSong;
    int   timeStart;
    int   timeEnd;

    struct decoder_error error;

    int   frequency;
    int   sample_format;    // SFMT_S16 | SFMT_LE/BE

    int   song_length_frames;   // length of current song in sample frames
    int   song_elapsed_frames;  // frames decoded so far in current song
};

#endif /* __cplusplus */

#ifdef __cplusplus
extern "C" {
#endif

void  *sidplayfp_open           (const char *file);
void   sidplayfp_close          (void *void_data);
void   sidplayfp_get_error      (void *prv_data, struct decoder_error *error);
void   sidplayfp_info           (const char *file_name, struct file_tags *info,
                                  const int tags_sel);
int    sidplayfp_seek           (void *void_data, int sec);
int    sidplayfp_decode         (void *void_data, char *buf, int buf_len,
                                  struct sound_params *sound_params);
int    sidplayfp_get_bitrate    (void *void_data);
int    sidplayfp_get_duration   (void *void_data);
int    sidplayfp_our_format_ext (const char *ext);
struct decoder *sidplayfp_plugin_init (void);

#ifdef __cplusplus
}
#endif

// EOF
