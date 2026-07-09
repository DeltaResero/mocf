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

#include "audio/decoder.h"

#ifdef __cplusplus

#include <memory>
#include <vector>
#include <cstdint>

// libsidplayfp defines debug/error macros that collide with ours.
#undef debug
#undef error

#include <sidplayfp/sidplayfp.h>
#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidConfig.h>
#include <sidplayfp/SidInfo.h>
#include <sidplayfp/SidTuneInfo.h>
#include <sidplayfp/builders/sidlite.h>
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
    // Declaration order matters here: members are destroyed in reverse
    // declaration order, and the engine holds a raw (non-owning) pointer
    // into both tune and builder, so it must be torn down first, while
    // they're still alive. Declaring tune and builder before engine gives
    // the destruction order engine -> builder -> tune. Do not reorder
    // these without re-checking that constraint.
    std::unique_ptr<SidTune>        tune;
    std::unique_ptr<SIDLiteBuilder> builder;
    std::unique_ptr<sidplayfp>      engine;

    // Interleaved stereo samples produced by the engine but not yet
    // delivered to the caller. The engine is driven in fixed cycle-sized
    // steps (see SIDPLAYFP_CYCLES in the .cpp) rather than by requested
    // sample count, so a decode() call rarely lines up exactly with what
    // the engine just produced. The remainder is queued here for the
    // next call. Cleared on every song change, since leftover audio was
    // rendered under the previous song's (now-reset) engine state.
    std::vector<int16_t> pcm_queue;

    // Fixed-size scratch buffer passed to sidplayfp::mix() each step.
    // Sized via engine->getBufSize(SIDPLAYFP_CYCLES), which accounts for
    // the worst-case *interleaved* (stereo-doubled) output for that
    // cycle count. The mix()'s "samples" parameter is a per-channel count,
    // not a buffer-size count, so this must NOT be sized to play()'s
    // return value directly (that undersizes it by 2x for stereo and
    // corrupts the heap).
    std::vector<int16_t> mix_scratch;

    int   length_ms;        // total playback length in milliseconds
    std::unique_ptr<int[]> sublengths_ms; // per-song lengths in milliseconds
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
extern "C" class AudioPlugin *sidplayfp_plugin_init (void);


// EOF
