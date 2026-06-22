// src/audio/outputs/out_buf.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Defining OUT_TEST causes the raw audio samples to be written
// to the file 'out_test' in the current directory for debugging.
// #define OUT_TEST
// Copyright (C) 2004,2005 Damian Pietras <daper@daper.net>
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
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <algorithm>

#ifdef OUT_TEST
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

/*#define DEBUG*/

#include "core/common.h"
#include "audio/audio.h"
#include "core/log.h"
#include "utils/fifo_buf.h"
#include "audio/outputs/out_buf.h"
#include "core/options.h"

/* Don't play more than this value (in seconds) in one audio_play().
 * This prevents locking. */
#define AUDIO_MAX_PLAY 0.1
#define AUDIO_MAX_PLAY_BYTES 32768

#ifdef OUT_TEST
static int fd;
#endif

static void set_realtime_prio()
{
#ifdef HAVE_SCHED_GET_PRIORITY_MAX
  int rc;

  if (options_get_bool("UseRealtimePriority"))
  {
    struct sched_param param;

    param.sched_priority = sched_get_priority_max(SCHED_RR);
    rc = pthread_setschedparam(pthread_self(), SCHED_RR, &param);
    if (rc != 0)
    {
      log_errno("Can't set realtime priority", rc);
    }
  }
#else
  logit("No sched_get_priority_max() function: "
        "realtime priority not used.");
#endif
}

/* Reading thread of the buffer. */
void OutBuf::read_thread()
{
  int audio_dev_closed = 0;
  int hw_paused = 0;

  logit("entering output buffer thread");

  set_realtime_prio();

  std::unique_lock<std::mutex> lock(mutex);

  while (true)
  {
    int played = 0;
    char play_buf[AUDIO_MAX_PLAY_BYTES];
    int play_buf_fill;
    int play_buf_pos = 0;

    if (reset_dev && !audio_dev_closed)
    {
      audio_reset();
      reset_dev = false;
    }

    if (is_stopped)
    {
      fifo->clear();
    }

    if (free_callback)
    {
      /* unlock the mutex to make calls to out_buf functions
       * possible in the callback */
      lock.unlock();
      free_callback();
      lock.lock();
    }

    debug("sending the signal");
    ready_cond.notify_all();

    if ((fifo->fill() == 0 || is_paused || is_stopped) &&
        !is_exit)
    {
      if (is_paused && !audio_dev_closed && !hw_paused)
      {
        if (audio_can_hw_pause())
        {
          logit("Corking the stream due to pause");
          audio_hw_pause();
          hw_paused = 1;
        }
        else
        {
          logit("Closing the device due to pause");
          audio_close();
          audio_dev_closed = 1;
        }
      }

      debug("waiting for something in the buffer");
      read_thread_waiting = true;
      play_cond.wait(lock);
      debug("something appeared in the buffer");
    }

    read_thread_waiting = false;

    if (hw_paused && !is_paused)
    {
      logit("Uncorking the stream after pause");
      audio_hw_unpause();
      hw_paused = 0;
    }
    else if (audio_dev_closed && !is_paused)
    {
      logit("Opening the device again after pause");
      if (!audio_open(nullptr))
      {
        logit("Can't reopen the device! sleeping...");
        xsleep(1, 1); /* there is no way to exit :( */
      }
      else
      {
        audio_dev_closed = 0;
      }
    }

    if (fifo->fill() == 0)
    {
      if (is_exit)
      {
        logit("exit");
        hardware_buf_fill = 0;
        break;
      }

      logit("buffer empty");
      continue;
    }

    if (is_paused)
    {
      logit("paused");
      continue;
    }

    if (is_stopped)
    {
      logit("stopped");
      continue;
    }

    if (!audio_dev_closed)
    {
      int audio_bpf;
      size_t play_buf_frames;

      audio_bpf = audio_get_bpf();
      play_buf_frames =
          static_cast<size_t>(std::min<double>(audio_get_bps() * AUDIO_MAX_PLAY, AUDIO_MAX_PLAY_BYTES)) /
          audio_bpf;
      play_buf_fill =
          fifo->get(play_buf, play_buf_frames * audio_bpf);
      lock.unlock();

      debug("playing %d bytes", play_buf_fill);

      while (play_buf_pos < play_buf_fill)
      {
        played = audio_send_pcm(play_buf + play_buf_pos,
                                play_buf_fill - play_buf_pos);

#ifdef OUT_TEST
        write(fd, play_buf + play_buf_pos, played);
#endif

        play_buf_pos += played;
      }

      /*logit ("done sending PCM");*/

      lock.lock();

      /* Update time */
      if (play_buf_fill && audio_get_bps())
      {
        time += play_buf_fill / static_cast<float>(audio_get_bps());
      }
      hardware_buf_fill = audio_get_buf_fill();
    }
  }

  // Lock goes out of scope here
  logit("exiting");
}

/* Allocate and initialize the buf structure, size is the buffer size. */
OutBuf::OutBuf(int size)
{
  assert(size > 0);

  fifo = std::make_unique<fifo_buf>(size);
  is_exit = false;
  is_paused = false;
  is_stopped = false;
  time = 0.0;
  reset_dev = false;
  hardware_buf_fill = 0;
  read_thread_waiting = false;
  free_callback = nullptr;

  tid = std::thread(&OutBuf::read_thread, this);
}

/* Wait for empty buffer, end playing, free resources allocated for the buf
 * structure.  Can be used only if nothing is played. */
OutBuf::~OutBuf()
{
  {
    std::lock_guard<std::mutex> lock(mutex);
    is_exit = true;
    play_cond.notify_one();
  }

  if (tid.joinable()) {
    tid.join();
  }

  /* Let other threads using this buffer know that the state of the
   * buffer has changed. */
  {
    std::lock_guard<std::mutex> lock(mutex);
    fifo->clear();
    ready_cond.notify_all();
  }

  fifo.reset();

  logit("buffer destroyed");

#ifdef OUT_TEST
  close(fd);
#endif
}

/* Put data at the end of the buffer, return 0 if nothing was put. */
int OutBuf::put(const char *data, int size)
{
  int pos = 0;

  /*logit ("got %d bytes to play", size);*/

  while (size)
  {
    int written;
    std::unique_lock<std::mutex> lock(mutex);

    if (fifo->space() == 0 && !is_stopped)
    {
      /*logit ("buffer full, waiting for the signal");*/
      ready_cond.wait(lock);
      /*logit ("buffer ready");*/
    }

    if (is_stopped)
    {
      logit("the buffer is stopped, refusing to write to the buffer");
      return 0;
    }

    written = fifo->put(data + pos, size);

    if (written)
    {
      play_cond.notify_one();
      size -= written;
      pos += written;
    }
  }

  return 1;
}

void OutBuf::pause()
{
  std::lock_guard<std::mutex> lock(mutex);
  is_paused = true;
}

void OutBuf::unpause()
{
  std::lock_guard<std::mutex> lock(mutex);
  is_paused = false;
  play_cond.notify_one();
}

/* Stop playing, after that buffer will refuse to play anything and ignore data
 * sent by buf_put(). */
void OutBuf::stop()
{
  logit("stopping the buffer");
  std::unique_lock<std::mutex> lock(mutex);
  is_stopped = true;
  is_paused = false;
  reset_dev = true;
  logit("sending signal");
  play_cond.notify_one();
  logit("waiting for signal");
  ready_cond.wait(lock);
  logit("done");
}

/* Reset the buffer state: this can by called ONLY when the buffer is stopped
 * and buf_put is not used! */
void OutBuf::reset()
{
  logit("resetting the buffer");

  std::lock_guard<std::mutex> lock(mutex);
  fifo->clear();
  is_stopped = false;
  is_paused = false;
  reset_dev = false;
  hardware_buf_fill = 0;
}

void OutBuf::time_set(const float t)
{
  std::lock_guard<std::mutex> lock(mutex);
  time = t;
}

/* Return the time in the audio which the user is currently hearing.
 * If unplayed samples still remain in the hardware buffer from the
 * previous audio then the value returned may be negative and it is
 * up to the caller to handle this appropriately in the context of
 * its own processing. */
int OutBuf::time_get()
{
  float time_f;
  int bps = audio_get_bps();

  std::lock_guard<std::mutex> lock(mutex);
  time_f = time - (bps ? hardware_buf_fill / static_cast<float>(bps) : 0);

  return static_cast<int>(roundf(time_f));
}

void OutBuf::set_free_callback(out_buf_free_callback callback)
{
  std::lock_guard<std::mutex> lock(mutex);
  free_callback = callback;
}

int OutBuf::get_free()
{
  int space;

  std::lock_guard<std::mutex> lock(mutex);
  space = fifo->space();

  return space;
}

int OutBuf::get_fill()
{
  int fill;

  std::lock_guard<std::mutex> lock(mutex);
  fill = fifo->fill();

  return fill;
}

/* Wait until the read thread will stop and wait for data to come.
 * This makes sure that the audio device isn't used (of course only if you
 * don't put anything in the buffer). */
void OutBuf::wait()
{
  logit("Waiting for read thread to suspend...");

  std::unique_lock<std::mutex> lock(mutex);
  while (!read_thread_waiting)
  {
    debug("waiting....");
    ready_cond.wait(lock);
  }

  logit("done");
}

// EOF
