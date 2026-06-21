// src/audio/audio.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Contributors:
// - Kamil Tarkowski <kamilt@interia.pl> - "previous" request
// Copyright (C) 2004-2006 Damian Pietras <daper@daper.net>
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
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

#define DEBUG

#include "core/common.h"
#include "core/server.h"
#include "audio/decoder.h"
#include "library/playlist.h"
#include "core/log.h"

#ifdef HAVE_PULSE
#include "audio/outputs/pulse.h"
#endif
#ifdef HAVE_OSS
#include "audio/outputs/oss.h"
#endif
#ifdef HAVE_SNDIO
#include "audio/outputs/sndio_out.h"
#endif
#ifdef HAVE_ALSA
#include "audio/outputs/alsa.h"
#endif
#ifndef NDEBUG
#include "audio/outputs/null_out.h"
#endif
#ifdef HAVE_JACK
#include "audio/outputs/jack.h"
#endif

#include "audio/processing/softmixer.h"
#include "audio/processing/equalizer.h"

#include "audio/outputs/out_buf.h"
#include "core/protocol.h"
#include "core/options.h"
#include "library/player.h"
#include "audio/audio.h"
#include "library/files.h"
#include "io/io.h"
#include "audio/conversion/audio_conversion.h"

static std::thread playing_thread;
static std::atomic<bool> play_thread_running{false};

/* currently played file */
static int curr_playing = -1;
/* file we played before playing songs from queue */
static std::string before_queue_fname;
static std::string curr_playing_fname;
/* This flag is set 1 if audio_play() was called with nonempty queue,
 * so we know that when the queue is empty, we should play the regular
 * playlist from the beginning. */
static bool started_playing_in_queue = false;
static std::mutex curr_playing_mtx;

static std::unique_ptr<OutBuf> out_buf;
static std::unique_ptr<AudioOutput> hw;
static struct output_driver_caps hw_caps; /* capabilities of the output
               driver */

/* Player state. */
static std::atomic<int> state{STATE_STOP};
static std::atomic<int> prev_state{STATE_STOP};

/* requests for playing thread */
static std::atomic<bool> stop_playing{false};
static std::atomic<bool> play_next{false};
static std::atomic<bool> play_prev{false};
static std::mutex request_mtx;

/* Playlists. */
static struct plist playlist;
static struct plist shuffled_plist;
static struct plist queue;
static struct plist *curr_plist; /* currently used playlist */
static std::mutex plist_mtx;

/* Is the audio device opened? */
static bool audio_opened = false;

/* Current sound parameters (with which the device is opened). */
static struct sound_params driver_sound_params = {0, 0, 0};

/* Sound parameters requested by the decoder. */
static struct sound_params req_sound_params = {0, 0, 0};

static std::unique_ptr<AudioConversion> sound_conv;
static bool need_audio_conversion = false;

static int current_mixer = 0;

/* Make a human readable description of the sound sample format(s).
 * Put the description in msg which is of size buf_size.
 * Return msg. */
char *sfmt_str(const long format, char *msg, const size_t buf_size)
{
  assert(sound_format_ok(format));

  assert(buf_size > 0);
  msg[0] = 0;

  if (format & SFMT_S8)
  {
    strncat(msg, ", 8-bit signed", buf_size - strlen(msg) - 1);
  }
  if (format & SFMT_U8)
  {
    strncat(msg, ", 8-bit unsigned", buf_size - strlen(msg) - 1);
  }
  if (format & SFMT_S16)
  {
    strncat(msg, ", 16-bit signed", buf_size - strlen(msg) - 1);
  }
  if (format & SFMT_U16)
  {
    strncat(msg, ", 16-bit unsigned", buf_size - strlen(msg) - 1);
  }
  if (format & SFMT_S24)
  {
    strncat(msg, ", 24-bit signed (as 32-bit samples)",
            buf_size - strlen(msg) - 1);
  }
  if (format & SFMT_U24)
  {
    strncat(msg, ", 24-bit unsigned (as 32-bit samples)",
            buf_size - strlen(msg) - 1);
  }
  if (format & SFMT_S24_3)
  {
    strncat(msg, ", 24-bit signed (in 3bytes format)",
            buf_size - strlen(msg) - 1);
  }
  if (format & SFMT_U24_3)
  {
    strncat(msg, ", 24-bit unsigned (in 3bytes format)",
            buf_size - strlen(msg) - 1);
  }
  if (format & SFMT_S32)
  {
    strncat(msg, ", 32-bit signed", buf_size - strlen(msg) - 1);
  }
  if (format & SFMT_U32)
  {
    strncat(msg, ", 32-bit unsigned", buf_size - strlen(msg) - 1);
  }
  if (format & SFMT_FLOAT)
  {
    strncat(msg, ", float", buf_size - strlen(msg) - 1);
  }

  if (format & SFMT_LE)
  {
    strncat(msg, " little-endian", buf_size - strlen(msg) - 1);
  }
  else if (format & SFMT_BE)
  {
    strncat(msg, " big-endian", buf_size - strlen(msg) - 1);
  }
  if (format & SFMT_NE)
  {
    strncat(msg, " (native)", buf_size - strlen(msg) - 1);
  }

  /* skip first ", " */
  if (msg[0])
  {
    memmove(msg, msg + 2, strlen(msg) + 1);
  }

  return msg;
}

/* Return != 0 if fmt1 and fmt2 have the same sample width. */
int sfmt_same_bps(const long fmt1, const long fmt2)
{
  if (fmt1 & (SFMT_S8 | SFMT_U8) && fmt2 & (SFMT_S8 | SFMT_U8))
  {
    return 1;
  }
  if (fmt1 & (SFMT_S16 | SFMT_U16) && fmt2 & (SFMT_S16 | SFMT_U16))
  {
    return 1;
  }
  if (fmt1 & (SFMT_S24 | SFMT_U24) && fmt2 & (SFMT_S24 | SFMT_U24))
  {
    return 1;
  }
  if (fmt1 & (SFMT_S24_3 | SFMT_U24_3) && fmt2 & (SFMT_S24_3 | SFMT_U24_3))
  {
    return 1;
  }
  if (fmt1 & (SFMT_S32 | SFMT_U32) && fmt2 & (SFMT_S32 | SFMT_U32))
  {
    return 1;
  }
  if (fmt1 & fmt2 & SFMT_FLOAT)
  {
    return 1;
  }

  return 0;
}

/* Return the best matching sample format for the requested format and
 * available format mask. */
static long sfmt_best_matching(const long formats_with_endian,
                               const long req_with_endian)
{
  long formats = formats_with_endian & SFMT_MASK_FORMAT;
  long req = req_with_endian & SFMT_MASK_FORMAT;
  long endian = formats_with_endian & SFMT_MASK_ENDIANNESS;
  long best = 0;

  char fmt_name1[SFMT_STR_MAX] DEBUG_ONLY;
  char fmt_name2[SFMT_STR_MAX] DEBUG_ONLY;

  if (formats & req)
  {
    best = req;
  }
  else if (req == SFMT_S8 || req == SFMT_U8)
  {
    if (formats & SFMT_S8)
    {
      best = SFMT_S8;
    }
    else if (formats & SFMT_U8)
    {
      best = SFMT_U8;
    }
    else if (formats & SFMT_S16)
    {
      best = SFMT_S16;
    }
    else if (formats & SFMT_U16)
    {
      best = SFMT_U16;
    }
    else if (formats & SFMT_S24_3)
    {
      best = SFMT_S24_3;
    }
    else if (formats & SFMT_U24_3)
    {
      best = SFMT_U24_3;
    }
    else if (formats & SFMT_S24)
    {
      best = SFMT_S24;
    }
    else if (formats & SFMT_U24)
    {
      best = SFMT_U24;
    }
    else if (formats & SFMT_S32)
    {
      best = SFMT_S32;
    }
    else if (formats & SFMT_U32)
    {
      best = SFMT_U32;
    }
    else if (formats & SFMT_FLOAT)
    {
      best = SFMT_FLOAT;
    }
  }
  else if (req == SFMT_S16 || req == SFMT_U16)
  {
    if (formats & SFMT_S16)
    {
      best = SFMT_S16;
    }
    else if (formats & SFMT_U16)
    {
      best = SFMT_U16;
    }
    else if (formats & SFMT_S24_3)
    {
      best = SFMT_S24_3;
    }
    else if (formats & SFMT_U24_3)
    {
      best = SFMT_U24_3;
    }
    else if (formats & SFMT_S24)
    {
      best = SFMT_S24;
    }
    else if (formats & SFMT_U24)
    {
      best = SFMT_U24;
    }
    else if (formats & SFMT_S32)
    {
      best = SFMT_S32;
    }
    else if (formats & SFMT_U32)
    {
      best = SFMT_U32;
    }
    else if (formats & SFMT_FLOAT)
    {
      best = SFMT_FLOAT;
    }
    else if (formats & SFMT_S8)
    {
      best = SFMT_S8;
    }
    else if (formats & SFMT_U8)
    {
      best = SFMT_U8;
    }
  }
  else if (req == SFMT_S24 || req == SFMT_U24)
  {
    if (formats & SFMT_S24)
    {
      best = SFMT_S24;
    }
    else if (formats & SFMT_U24)
    {
      best = SFMT_U24;
    }
    else if (formats & SFMT_S24_3)
    {
      best = SFMT_S24_3;
    }
    else if (formats & SFMT_U24_3)
    {
      best = SFMT_U24_3;
    }
    else if (formats & SFMT_S32)
    {
      best = SFMT_S32;
    }
    else if (formats & SFMT_U32)
    {
      best = SFMT_U32;
    }
    else if (formats & SFMT_FLOAT)
    {
      best = SFMT_FLOAT;
    }
    else if (formats & SFMT_S16)
    {
      best = SFMT_S16;
    }
    else if (formats & SFMT_U16)
    {
      best = SFMT_U16;
    }
    else if (formats & SFMT_S8)
    {
      best = SFMT_S8;
    }
    else if (formats & SFMT_U8)
    {
      best = SFMT_U8;
    }
  }
  else if (req == SFMT_S24_3 || req == SFMT_U24_3)
  {
    if (formats & SFMT_S24_3)
    {
      best = SFMT_S24_3;
    }
    else if (formats & SFMT_U24_3)
    {
      best = SFMT_U24_3;
    }
    else if (formats & SFMT_S24)
    {
      best = SFMT_S24;
    }
    else if (formats & SFMT_U24)
    {
      best = SFMT_U24;
    }
    else if (formats & SFMT_S32)
    {
      best = SFMT_S32;
    }
    else if (formats & SFMT_U32)
    {
      best = SFMT_U32;
    }
    else if (formats & SFMT_FLOAT)
    {
      best = SFMT_FLOAT;
    }
    else if (formats & SFMT_S16)
    {
      best = SFMT_S16;
    }
    else if (formats & SFMT_U16)
    {
      best = SFMT_U16;
    }
    else if (formats & SFMT_S8)
    {
      best = SFMT_S8;
    }
    else if (formats & SFMT_U8)
    {
      best = SFMT_U8;
    }
  }
  else if (req == SFMT_S32 || req == SFMT_U32)
  {
    if (formats & SFMT_S32)
    {
      best = SFMT_S32;
    }
    else if (formats & SFMT_U32)
    {
      best = SFMT_U32;
    }
    else if (formats & SFMT_FLOAT)
    {
      best = SFMT_FLOAT;
    }
    else if (formats & SFMT_S24_3)
    {
      best = SFMT_S24_3;
    }
    else if (formats & SFMT_U24_3)
    {
      best = SFMT_U24_3;
    }
    else if (formats & SFMT_S24)
    {
      best = SFMT_S24;
    }
    else if (formats & SFMT_U24)
    {
      best = SFMT_U24;
    }
    else if (formats & SFMT_S16)
    {
      best = SFMT_S16;
    }
    else if (formats & SFMT_U16)
    {
      best = SFMT_U16;
    }
    else if (formats & SFMT_S8)
    {
      best = SFMT_S8;
    }
    else if (formats & SFMT_U8)
    {
      best = SFMT_U8;
    }
  }
  else if (req == SFMT_FLOAT)
  {
    if (formats & SFMT_S32)
    {
      best = SFMT_S32;
    }
    else if (formats & SFMT_U32)
    {
      best = SFMT_U32;
    }
    else if (formats & SFMT_S24_3)
    {
      best = SFMT_S24_3;
    }
    else if (formats & SFMT_U24_3)
    {
      best = SFMT_U24_3;
    }
    else if (formats & SFMT_S24)
    {
      best = SFMT_S24;
    }
    else if (formats & SFMT_U24)
    {
      best = SFMT_U24;
    }
    else if (formats & SFMT_S16)
    {
      best = SFMT_S16;
    }
    else if (formats & SFMT_U16)
    {
      best = SFMT_U16;
    }
    else if (formats & SFMT_S8)
    {
      best = SFMT_S8;
    }
    else if (formats & SFMT_U8)
    {
      best = SFMT_U8;
    }
  }

  assert(best != 0);

  if (!(best & (SFMT_S8 | SFMT_U8 | SFMT_FLOAT)))
  {
    assert((endian & SFMT_LE) ^ (endian & SFMT_BE));
    best |= endian;
  }

  debug("Chose %s as the best matching %s",
        sfmt_str(best, fmt_name1, sizeof(fmt_name1)),
        sfmt_str(req_with_endian, fmt_name2, sizeof(fmt_name2)));

  return best;
}

/* Return the number of bytes per sample for the given format. */
int sfmt_Bps(const long format)
{
  int Bps = -1;

  switch (format & SFMT_MASK_FORMAT)
  {
    case SFMT_S8:
    case SFMT_U8:
      Bps = 1;
      break;
    case SFMT_S16:
    case SFMT_U16:
      Bps = 2;
      break;
    case SFMT_S24_3:
    case SFMT_U24_3:
      Bps = 3;
      break;
    case SFMT_S32:
    case SFMT_U32:
    case SFMT_S24:
    case SFMT_U24:
      Bps = 4;
      break;
    case SFMT_FLOAT:
      Bps = sizeof(float);
      break;
  }

  assert(Bps > 0);

  return Bps;
}

/* Move to the next file depending on the options set, the user
 * request and whether or not there are files in the queue. */
static void go_to_another_file()
{
  bool shuffle = options_get_bool("Shuffle");
  bool go_next = (play_next || options_get_bool("AutoNext"));
  int curr_playing_curr_pos;

  std::lock_guard<std::mutex> lock1(curr_playing_mtx);
  std::lock_guard<std::mutex> lock2(plist_mtx);

  /* If we move forward in the playlist and there are some songs in
   * the queue, then play them. */
  if (plist_count(&queue) && go_next)
  {
    logit("Playing file from queue");

    if (before_queue_fname.empty() && !curr_playing_fname.empty())
    {
      before_queue_fname = curr_playing_fname;
    }

    curr_plist = &queue;
    curr_playing = plist_next(&queue, -1);

    server_queue_pop(queue.items[curr_playing].file.c_str());
    plist_delete(&queue, curr_playing);
  }
  else
  {
    /* If we just finished playing files from the queue and the
     * appropriate option is set, continue with the file played
     * before playing the queue. */
    if (!before_queue_fname.empty() && options_get_bool("QueueNextSongReturn"))
    {
      curr_playing_fname = before_queue_fname;
      before_queue_fname.clear();
    }

    if (shuffle)
    {
      curr_plist = &shuffled_plist;

      if (plist_count(&playlist) && !plist_count(&shuffled_plist))
      {
        plist_cat(&shuffled_plist, &playlist);
        plist_shuffle(&shuffled_plist);

        if (!curr_playing_fname.empty())
        {
          plist_swap_first_fname(&shuffled_plist, curr_playing_fname.c_str());
        }
      }
    }
    else
    {
      curr_plist = &playlist;
    }

    curr_playing_curr_pos = plist_find_fname(curr_plist, curr_playing_fname.c_str());

    /* If we came from the queue and the last file in
     * queue wasn't in the playlist, we try to revert to
     * the QueueNextSongReturn == true behaviour. */
    if (curr_playing_curr_pos == -1 && !before_queue_fname.empty())
    {
      curr_playing_curr_pos = plist_find_fname(curr_plist, before_queue_fname.c_str());
    }

    if (play_prev && plist_count(curr_plist))
    {
      logit("Playing previous...");

      if (curr_playing_curr_pos == -1 || started_playing_in_queue)
      {
        curr_playing = plist_prev(curr_plist, -1);
        started_playing_in_queue = false;
      }
      else
      {
        curr_playing = plist_prev(curr_plist, curr_playing_curr_pos);
      }

      if (curr_playing == -1)
      {
        if (options_get_bool("Repeat"))
        {
          curr_playing = plist_last(curr_plist);
        }
        logit("Beginning of the list.");
      }
      else
      {
        logit("Previous item.");
      }
    }
    else if (go_next && plist_count(curr_plist))
    {
      logit("Playing next...");

      if (curr_playing_curr_pos == -1 || started_playing_in_queue)
      {
        curr_playing = plist_next(curr_plist, -1);
        started_playing_in_queue = false;
      }
      else
      {
        curr_playing = plist_next(curr_plist, curr_playing_curr_pos);
      }

      if (curr_playing == -1 && options_get_bool("Repeat"))
      {
        if (shuffle)
        {
          plist_clear(&shuffled_plist);
          plist_cat(&shuffled_plist, &playlist);
          plist_shuffle(&shuffled_plist);
        }
        curr_playing = plist_next(curr_plist, -1);
        logit("Going back to the first item.");
      }
      else if (curr_playing == -1)
      {
        logit("End of the list");
      }
      else
      {
        logit("Next item");
      }
    }
    else if (!options_get_bool("Repeat"))
    {
      curr_playing = -1;
    }
    else
    {
      debug("Repeating file");
    }

    before_queue_fname.clear();
  }
}

static void play_thread_func()
{
  logit("Entering playing thread");

  while (curr_playing != -1)
  {
    std::string file;

    {
        std::lock_guard<std::mutex> lock(plist_mtx);
        file = plist_get_file(curr_plist, curr_playing);
    }

    play_next = false;
    play_prev = false;

    if (!file.empty())
    {
      std::string next_file;

      {
          std::lock_guard<std::mutex> lock1(curr_playing_mtx);
          std::lock_guard<std::mutex> lock2(plist_mtx);
          logit("Playing item %d: %s", curr_playing, file.c_str());

          curr_playing_fname = file;

          out_buf->time_set(0.0);

          int next = plist_next(curr_plist, curr_playing);
          next_file = next != -1 ? plist_get_file(curr_plist, next) : std::string{};
      }

      player(file.c_str(), next_file.empty() ? nullptr : next_file.c_str(), out_buf.get());

      set_info_rate(0);
      set_info_bitrate(0);
      set_info_channels(1);
      out_buf->time_set(0.0);
    }

    if (stop_playing)
    {
      std::lock_guard<std::mutex> lock(curr_playing_mtx);
      curr_playing = -1;
      logit("stopped");
    }
    else
    {
      go_to_another_file();
    }
  }

  prev_state = state.load();
  state = STATE_STOP;
  state_change();

  curr_playing_fname.clear();

  audio_close();
  logit("Exiting");
}

void audio_reset()
{
  if (hw)
  {
    hw->reset();
  }
}

void audio_stop()
{
  if (play_thread_running)
  {
    logit("audio_stop()");
    {
        std::lock_guard<std::mutex> lock(request_mtx);
        stop_playing = true;
    }
    player_stop();
    logit("joining playing_thread");
    if (playing_thread.joinable()) {
        playing_thread.join();
    }
    play_thread_running = false;
    stop_playing = false;
    logit("done stopping");
  }
  else if (state == STATE_PAUSE)
  {
    /* Paused internet stream - we are in fact stopped already. */
    curr_playing_fname.clear();

    prev_state = state.load();
    state = STATE_STOP;
    state_change();
  }
}

/* Start playing from the file fname. If fname is an empty string,
 * start playing from the first file on the list. */
void audio_play(const char *fname)
{
  audio_stop();
  player_reset();

  {
      std::lock_guard<std::mutex> lock1(curr_playing_mtx);
      std::lock_guard<std::mutex> lock2(plist_mtx);

      /* If we have songs in the queue and fname is empty string, start
       * playing file from the queue. */
      if (plist_count(&queue) && !(*fname))
      {
        curr_plist = &queue;
        curr_playing = plist_next(&queue, -1);

        /* remove the file from queue */
        server_queue_pop(queue.items[curr_playing].file.c_str());
        plist_delete(curr_plist, curr_playing);

        started_playing_in_queue = true;
      }
      else if (options_get_bool("Shuffle"))
      {
        plist_clear(&shuffled_plist);
        plist_cat(&shuffled_plist, &playlist);
        plist_shuffle(&shuffled_plist);
        plist_swap_first_fname(&shuffled_plist, fname);

        curr_plist = &shuffled_plist;

        if (*fname)
        {
          curr_playing = plist_find_fname(curr_plist, fname);
        }
        else if (plist_count(curr_plist))
        {
          curr_playing = plist_next(curr_plist, -1);
        }
        else
        {
          curr_playing = -1;
        }
      }
      else
      {
        curr_plist = &playlist;

        if (*fname)
        {
          curr_playing = plist_find_fname(curr_plist, fname);
        }
        else if (plist_count(curr_plist))
        {
          curr_playing = plist_next(curr_plist, -1);
        }
        else
        {
          curr_playing = -1;
        }
      }
  }

  play_thread_running = true;
  playing_thread = std::thread(play_thread_func);
}

void audio_next()
{
  if (play_thread_running)
  {
    play_next = true;
    player_stop();
  }
}

void audio_prev()
{
  if (play_thread_running)
  {
    play_prev = true;
    player_stop();
  }
}

void audio_pause()
{
  std::lock_guard<std::mutex> lock1(curr_playing_mtx);
  std::lock_guard<std::mutex> lock2(plist_mtx);

  if (curr_playing != -1)
  {
    out_buf->pause();

    prev_state = state.load();
    state = STATE_PAUSE;
    state_change();
  }
}

void audio_unpause()
{
  std::lock_guard<std::mutex> lock(curr_playing_mtx);
  if (curr_playing != -1)
  {
    out_buf->unpause();
    prev_state = state.load();
    state = STATE_PLAY;
    state_change();
  }
}

static void reset_sound_params(struct sound_params *params)
{
  params->rate = 0;
  params->channels = 0;
  params->fmt = 0;
}

/* Return 0 on error. If sound params == nullptr, open the device using
 * the previous parameters. */
int audio_open(struct sound_params *sound_params)
{
  int res;
  static struct sound_params last_params = {0, 0, 0};

  if (!sound_params)
  {
    sound_params = &last_params;
  }
  else
  {
    last_params = *sound_params;
  }

  assert(sound_format_ok(sound_params->fmt));

  if (audio_opened)
  {
    if (sound_params_eq(req_sound_params, *sound_params))
    {
      if (audio_get_bps() >= 88200)
      {
        logit("Audio device already opened with such parameters.");
        return 1;
      }

      /* Not closing the device would cause that much
       * sound from the previous file to stay in the buffer
       * and the user will hear old data, so close it. */
      logit("Reopening device due to low bps.");
    }

    audio_close();
  }

  req_sound_params = *sound_params;

  /* Set driver_sound_params to parameters supported by the driver that
   * are nearly the requested parameters. */

  int max_rate = options_get_int("MaxSamplerate");

  switch (options_get_int("EnableResample"))
  {
    case 2:
      assert(max_rate > 0);

      driver_sound_params.rate = max_rate;
      logit("Setting forced output sample.");
      break;
    case 1:
      max_rate = (max_rate == 0) || (hw_caps.max_rate < max_rate)
                     ? hw_caps.max_rate
                     : max_rate;
      driver_sound_params.rate =
          CLAMP(hw_caps.min_rate, req_sound_params.rate, max_rate);

      /* check if it is possible to chose a sample rate which would be a
       * multiple of req sample rate */
      if (driver_sound_params.rate > req_sound_params.rate &&
          driver_sound_params.rate % req_sound_params.rate != 0)
      {
        if (req_sound_params.rate * 2 >= hw_caps.min_rate &&
            req_sound_params.rate * 2 <= max_rate)
        {
          driver_sound_params.rate = req_sound_params.rate * 2;
        }
        else if (req_sound_params.rate * 3 >= hw_caps.min_rate &&
                 req_sound_params.rate * 3 <= max_rate)
        {
          driver_sound_params.rate = req_sound_params.rate * 3;
        }
        else if (req_sound_params.rate * 4 >= hw_caps.min_rate &&
                 req_sound_params.rate * 4 <= max_rate)
        {
          driver_sound_params.rate = req_sound_params.rate * 4;
        }
      }

      break;
    default:
      driver_sound_params.rate = req_sound_params.rate;
  }
  logit("Requested sample rate: %dHz, output sample rate: %dHz",
        req_sound_params.rate, driver_sound_params.rate);

  driver_sound_params.fmt =
      sfmt_best_matching(hw_caps.formats, req_sound_params.fmt);

  /* number of channels */
  driver_sound_params.channels = CLAMP(
      hw_caps.min_channels, req_sound_params.channels, hw_caps.max_channels);

  res = hw->open(&driver_sound_params);

  if (res)
  {
    char fmt_name[SFMT_STR_MAX] LOGIT_ONLY;

    driver_sound_params.rate = hw->get_rate();
    debug("Driver sfmt: 0x%lX, req sfmt 0x%lX", driver_sound_params.fmt,
          req_sound_params.fmt);
    debug("Driver channels: %d, req channels %d", driver_sound_params.channels,
          req_sound_params.channels);
    debug("Driver rate: %d, req rate %d", driver_sound_params.rate,
          req_sound_params.rate);
    if (driver_sound_params.fmt != req_sound_params.fmt ||
        driver_sound_params.channels != req_sound_params.channels ||
        (req_sound_params.rate != driver_sound_params.rate))
    {
      logit("Conversion of the sound is needed.");
      try {
        sound_conv = std::make_unique<AudioConversion>(req_sound_params, driver_sound_params);
        need_audio_conversion = true;
      } catch (const std::exception& e) {
        hw->close();
        reset_sound_params(&req_sound_params);
        return 0;
      }
    }
    audio_opened = true;

    logit("Requested sound parameters: %s, %d channels, %dHz",
          sfmt_str(req_sound_params.fmt, fmt_name, sizeof(fmt_name)),
          req_sound_params.channels, req_sound_params.rate);
    logit("Driver sound parameters: %s, %d channels, %dHz",
          sfmt_str(driver_sound_params.fmt, fmt_name, sizeof(fmt_name)),
          driver_sound_params.channels, driver_sound_params.rate);
  }

  return res;
}

int audio_send_buf(const char *buf, const size_t size)
{
  int res;

  if (need_audio_conversion)
  {
    std::vector<char> converted = sound_conv->process(buf, size);
    res = out_buf->put(converted.data(), converted.size());
  }
  else
  {
    res = out_buf->put(buf, size);
  }

  return res;
}

/* Get the current audio format bytes per frame value.
 * May return 0 if the audio device is closed. */
int audio_get_bpf()
{
  return driver_sound_params.channels *
         (driver_sound_params.fmt ? sfmt_Bps(driver_sound_params.fmt) : 0);
}

/* Get the current audio format bytes per second value.
 * May return 0 if the audio device is closed. */
int audio_get_bps() { return driver_sound_params.rate * audio_get_bpf(); }

int audio_get_buf_fill() { return hw ? hw->get_buff_fill() : 0; }

int audio_can_hw_pause() { return hw ? hw->can_hw_pause() : 0; }

void audio_hw_pause()
{
  if (hw)
    hw->hw_pause();
}

void audio_hw_unpause()
{
  if (hw)
    hw->hw_unpause();
}

int audio_send_pcm(const char *buf, const size_t size)
{
  std::vector<char> equalized;
  std::vector<char> softmixed;

  if (equalizer_is_active())
  {
    equalized.assign(buf, buf + size);
    equalizer_process_buffer(equalized.data(), size, &driver_sound_params);
    buf = equalized.data();
  }

  if (softmixer_is_active() || softmixer_is_mono())
  {
    softmixed.assign(buf, buf + size);
    softmixer_process_buffer(softmixed.data(), size, &driver_sound_params);
    buf = softmixed.data();
  }

  int played = hw->play(buf, size);

  if (played < 0)
  {
    fatal("Audio output error!");
  }

  return played;
}

/* Get current time of the song in seconds. */
int audio_get_time()
{
  return state.load() != STATE_STOP ? out_buf->time_get() : 0;
}

void audio_close()
{
  if (audio_opened)
  {
    reset_sound_params(&req_sound_params);
    reset_sound_params(&driver_sound_params);
    hw->close();
    if (need_audio_conversion)
    {
      sound_conv.reset();
      need_audio_conversion = false;
    }
    audio_opened = false;
  }
}

/* Try to initialize drivers from the list and fill funcs with
 * those of the first working driver. */
static void find_working_driver(const std::vector<std::string> &drivers)
{
  hw.reset();

  for (const auto &driver : drivers)
  {
    const char *name = driver.c_str();

#ifdef HAVE_SNDIO
    if (!strcasecmp(name, "sndio"))
    {
      hw = create_sndio_output();
      printf("Trying SNDIO...\n");
      if (hw->init(&hw_caps)) return;
      hw.reset();
    }
#endif

#ifdef HAVE_PULSE
    if (!strcasecmp(name, "pulseaudio"))
    {
      hw = create_pulse_output();
      printf("Trying PulseAudio...\n");
      if (hw->init(&hw_caps)) return;
      hw.reset();
    }
#endif

#ifdef HAVE_OSS
    if (!strcasecmp(name, "oss"))
    {
      hw = create_oss_output();
      printf("Trying OSS...\n");
      if (hw->init(&hw_caps)) return;
      hw.reset();
    }
#endif

#ifdef HAVE_ALSA
    if (!strcasecmp(name, "alsa"))
    {
      hw = create_alsa_output();
      printf("Trying ALSA...\n");
      if (hw->init(&hw_caps)) return;
      hw.reset();
    }
#endif

#ifdef HAVE_JACK
    if (!strcasecmp(name, "jack"))
    {
      hw = create_jack_output();
      printf("Trying JACK...\n");
      if (hw->init(&hw_caps)) return;
      hw.reset();
    }
#endif

#ifndef NDEBUG
    if (!strcasecmp(name, "null"))
    {
      hw = create_null_output();
      printf("Trying null...\n");
      if (hw->init(&hw_caps)) return;
      hw.reset();
    }
#endif
  }

  fatal("No valid sound driver!");
}

static void
print_output_capabilities(const struct output_driver_caps *caps LOGIT_ONLY)
{
  char fmt_name[SFMT_STR_MAX] LOGIT_ONLY;

  logit("Sound driver capabilities: channels %d - %d, sample rate %u - %u, "
        "formats: %s",
        caps->min_channels, caps->max_channels, caps->min_rate, caps->max_rate,
        sfmt_str(caps->formats, fmt_name, sizeof(fmt_name)));
}

static long decode_masked_formats(const std::vector<std::string> &list)
{
  long fmt = 0;
  auto exists = [&list](const char *s) {
    return std::find(list.begin(), list.end(), s) != list.end();
  };

  if (exists("S8"))
  {
    fmt |= SFMT_S8;
  }
  if (exists("U8"))
  {
    fmt |= SFMT_U8;
  }
  if (exists("S16"))
  {
    fmt |= SFMT_S16;
  }
  if (exists("U16"))
  {
    fmt |= SFMT_U16;
  }
  if (exists("S24"))
  {
    fmt |= SFMT_S24;
  }
  if (exists("U24"))
  {
    fmt |= SFMT_U24;
  }
  if (exists("S24_3"))
  {
    fmt |= SFMT_S24_3;
  }
  if (exists("U24_3"))
  {
    fmt |= SFMT_U24_3;
  }
  if (exists("S32"))
  {
    fmt |= SFMT_S32;
  }
  if (exists("U32"))
  {
    fmt |= SFMT_U32;
  }
  if (exists("FLOAT"))
  {
    fmt |= SFMT_FLOAT;
  }

  if (list.size() != static_cast<size_t>(__builtin_popcount(fmt)))
  {
    fatal("Incorrect setting for MaskOutputFormats");
    return 0;
  }
  else
  {
    return fmt;
  }
}

void audio_initialize()
{
  long masked_formats;
  int max_channels;

  find_working_driver(options_get_list("SoundDriver"));

  if (hw_caps.max_channels < hw_caps.min_channels)
  {
    fatal("Error initializing audio device: "
          "device reports incorrect number of channels.");
  }
  if (!sound_format_ok(hw_caps.formats))
  {
    fatal("Error initializing audio device: "
          "device reports no usable formats.");
  }

  print_output_capabilities(&hw_caps);
  masked_formats = decode_masked_formats(options_get_list("MaskOutputFormats"));
  if (masked_formats & hw_caps.formats)
  {
    logit("Applying mask %lX to formats", masked_formats);
    hw_caps.formats &= ~masked_formats;
    if (!sound_format_ok(hw_caps.formats))
    {
      fatal("No available sound formats after applying format mask. "
            "Consider ammending MaskOutputFormats.");
    }
  }

  if (!sound_format_ok(hw_caps.formats))
  {
    fatal("No available sound formats after applying MaskOutputFormats.");
  }

  max_channels = options_get_int("MaxChannels");
  if (max_channels > 0)
  {
    hw_caps.max_channels = max_channels;
  }

  out_buf = std::make_unique<OutBuf>(options_get_int("OutputBuffer") * 1024);

  softmixer_init();
  equalizer_init();

  plist_init(&playlist);
  plist_init(&shuffled_plist);
  plist_init(&queue);
  player_init();
}

void audio_exit()
{
  audio_stop();
  if (hw)
  {
    hw->shutdown();
  }
  out_buf.reset();
  plist_free(&playlist);
  plist_free(&shuffled_plist);
  plist_free(&queue);
  player_cleanup();

  softmixer_shutdown();
  equalizer_shutdown();
}

void audio_seek(const int sec)
{
  int playing;

  {
      std::lock_guard<std::mutex> lock(curr_playing_mtx);
      playing = curr_playing;
  }

  if (playing != -1 && state.load() == STATE_PLAY)
  {
    player_seek(sec);
  }
  else
  {
    logit("Seeking when nothing is played.");
  }
}

void audio_jump_to(const float sec)
{
  int playing;

  {
      std::lock_guard<std::mutex> lock(curr_playing_mtx);
      playing = curr_playing;
  }

  if (playing != -1 && state.load() == STATE_PLAY)
  {
    player_jump_to(sec);
  }
  else
  {
    logit("Jumping when nothing is played.");
  }
}

int audio_get_state() { return state.load(); }

int audio_get_prev_state() { return prev_state.load(); }

void audio_plist_add(const char *file)
{
  std::lock_guard<std::mutex> lock(plist_mtx);
  plist_clear(&shuffled_plist);
  if (plist_find_fname(&playlist, file) == -1)
  {
    plist_add(&playlist, file);
  }
  else
  {
    logit("Wanted to add a file already present: %s", file);
  }
}

void audio_queue_add(const char *file)
{
  std::lock_guard<std::mutex> lock(plist_mtx);
  if (plist_find_fname(&queue, file) == -1)
  {
    plist_add(&queue, file);
  }
  else
  {
    logit("Wanted to add a file already present: %s", file);
  }
}

void audio_plist_clear()
{
  std::lock_guard<std::mutex> lock(plist_mtx);
  plist_clear(&shuffled_plist);
  plist_clear(&playlist);
}

void audio_queue_clear()
{
  std::lock_guard<std::mutex> lock(plist_mtx);
  plist_clear(&queue);
}

std::string audio_get_sname()
{
  std::lock_guard<std::mutex> lock(curr_playing_mtx);
  return curr_playing_fname;
}

int audio_get_mixer()
{
  if (current_mixer == 2)
  {
    return softmixer_get_value();
  }

  return hw->read_mixer();
}

void audio_set_mixer(const int val)
{
  if (!RANGE(0, val, 100))
  {
    logit("Tried to set mixer to volume out of range.");
    return;
  }

  if (current_mixer == 2)
  {
    softmixer_set_value(val);
  }
  else
  {
    hw->set_mixer(val);
  }
}

void audio_plist_delete(const char *file)
{
  int num;

  std::lock_guard<std::mutex> lock(plist_mtx);
  num = plist_find_fname(&playlist, file);
  if (num != -1)
  {
    plist_delete(&playlist, num);
  }

  num = plist_find_fname(&shuffled_plist, file);
  if (num != -1)
  {
    plist_delete(&shuffled_plist, num);
  }
}

void audio_queue_delete(const char *file)
{
  int num;

  std::lock_guard<std::mutex> lock(plist_mtx);
  num = plist_find_fname(&queue, file);
  if (num != -1)
  {
    plist_delete(&queue, num);
  }
}

/* Get the time of a file if the file is on the playlist and
 * the time is available. */
int audio_get_ftime(const char *file)
{
  int i;
  int time;
  time_t mtime;

  mtime = get_mtime(file);

  std::lock_guard<std::mutex> lock(plist_mtx);
  i = plist_find_fname(&playlist, file);
  if (i != -1)
  {
    time = get_item_time(&playlist, i);
    if (time != -1)
    {
      if (playlist.items[i].mtime == mtime)
      {
        debug("Found time for %s", file);
        return time;
      }
      logit("mtime for %s has changed", file);
    }
  }

  return -1;
}

/* Set the time for a file on the playlist. */
void audio_plist_set_time(const char *file, const int time)
{
  int i;

  std::lock_guard<std::mutex> lock(plist_mtx);
  if ((i = plist_find_fname(&playlist, file)) != -1)
  {
    plist_set_item_time(&playlist, i, time);
    playlist.items[i].mtime = get_mtime(file);
    debug("Setting time for %s", file);
  }
  else
  {
    logit("Request for updating time for a file not present on the"
          " playlist!");
  }
}

/* Notify that the state was changed (used by the player). */
void audio_state_started_playing()
{
  prev_state = state.load();
  state = STATE_PLAY;
  state_change();
}

/* Swap 2 files on the playlist. */
void audio_plist_move(const char *file1, const char *file2)
{
  std::lock_guard<std::mutex> lock(plist_mtx);
  plist_swap_files(&playlist, file1, file2);
}

/* Return a copy of the song queue.  We cannot just return constant
 * pointer, because it will be used in a different thread.
 * It obviously needs to be freed after use. */
struct plist *audio_queue_get_contents()
{
  struct plist *ret = new plist;
  plist_init(ret);

  std::lock_guard<std::mutex> lock(plist_mtx);
  plist_cat(ret, &queue);

  return ret;
}


std::string audio_get_mixer_channel_name()
{
  if (current_mixer == 2)
  {
    return softmixer_name();
  }

  return hw->get_mixer_channel_name();
}

void audio_toggle_mixer_channel()
{
  std::string old_name = audio_get_mixer_channel_name();

  for (int i = 0; i < 2; i++)
  {
    int prev_mixer = current_mixer;
    current_mixer = (current_mixer + 1) % 3;

    if (current_mixer < 2)
    {
      hw->toggle_mixer_channel();
      if (prev_mixer == 2)
      {
        softmixer_set_active(0);
      }
    }
    else
    {
      softmixer_set_active(1);
    }

    if (audio_get_mixer_channel_name() != old_name)
    {
      break;
    }
  }
}

// EOF
