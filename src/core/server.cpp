// src/core/server.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2003 - 2005 Damian Pietras <daper@daper.net>
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
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <queue>
#include <string>
#include <memory>
#include <utility>

#define DEBUG

#include "core/common.h"
#include "core/log.h"
#include "core/protocol.h"
#include "audio/audio.h"
#include "audio/outputs/oss.h"
#include "core/options.h"
#include "core/server.h"
#include "library/playlist.h"
#include "library/tags_cache.h"
#include "library/files.h"
#include "audio/processing/softmixer.h"
#include "audio/processing/equalizer.h"

/* -----------------------------------------------------------------------
 * Engine event queue — thread-safe queue + pipe wakeup
 * ----------------------------------------------------------------------- */

struct engine_event_queue
{
  std::queue<Event>  q;
  std::mutex         mtx;
  int                pipe_fd[2]; /* [0] = read (UI), [1] = write (engine) */
};

struct engine_event_queue *engine_event_queue_new(void)
{
  auto *eq = new engine_event_queue;

  if (pipe(eq->pipe_fd) < 0)
    fatal("pipe() failed for engine event queue: %s", xstrerror(errno).c_str());

  /* Make the write end non-blocking so audio callbacks never block. */
  int flags = fcntl(eq->pipe_fd[1], F_GETFL);
  if (flags == -1 || fcntl(eq->pipe_fd[1], F_SETFL, flags | O_NONBLOCK) == -1)
    fatal("fcntl() on event pipe failed: %s", xstrerror(errno).c_str());

  /* Make the read end non-blocking so drain loops never block. */
  flags = fcntl(eq->pipe_fd[0], F_GETFL);
  if (flags == -1 || fcntl(eq->pipe_fd[0], F_SETFL, flags | O_NONBLOCK) == -1)
    fatal("fcntl() on event pipe failed: %s", xstrerror(errno).c_str());

  return eq;
}

void engine_event_queue_free(struct engine_event_queue *eq)
{
  if (!eq) return;
  {
    std::lock_guard<std::mutex> lock(eq->mtx);
    while (!eq->q.empty())
    {
      eq->q.pop();
    }
  }
  close(eq->pipe_fd[0]);
  close(eq->pipe_fd[1]);
  delete eq;
}

int engine_event_queue_fd(const struct engine_event_queue *eq)
{
  return eq->pipe_fd[0];
}

/* Push an event onto the queue and wake the UI thread. */
static void eq_push(struct engine_event_queue *eq, int type, void *data)
{
  char w = 1;
  {
    std::lock_guard<std::mutex> lock(eq->mtx);
    eq->q.push({type, data});
  }
  /* Best-effort wakeup; non-blocking pipe so this never stalls. */
  if (write(eq->pipe_fd[1], &w, 1) < 0 && errno != EAGAIN)
    logit("Can't write to engine event pipe: %s", xstrerror(errno).c_str());
}

/* Drain all pending events + consume wakeup bytes.  Non-blocking. */
void engine_event_queue_flush(struct engine_event_queue *eq,
                              std::queue<Event> &dest)
{
  char buf[64];
  /* Consume any wakeup bytes (non-blocking, so returns when pipe is empty). */
  while (read(eq->pipe_fd[0], buf, sizeof(buf)) > 0)
    ;

  /* Splice the shared queue onto the tail of dest. */
  std::lock_guard<std::mutex> lock(eq->mtx);
  while (!eq->q.empty())
  {
    dest.push(std::move(eq->q.front()));
    eq->q.pop();
  }
}

/* Blocking variant: wait until at least one event arrives, then drain. */
void engine_event_queue_wait_flush(struct engine_event_queue *eq,
                                   std::queue<Event> &dest)
{
  /* First drain without blocking in case there is already something. */
  engine_event_queue_flush(eq, dest);
  if (!dest.empty())
    return;

  /* Nothing yet — block on the read end of the pipe. */
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(eq->pipe_fd[0], &fds);

  /* select() restores the O_NONBLOCK flag so reading will still work. */
  int fd_blocking = eq->pipe_fd[0];
  /* Temporarily make the read end blocking just for select. */
  (void)select(fd_blocking + 1, &fds, nullptr, nullptr, nullptr);

  engine_event_queue_flush(eq, dest);
}

/* -----------------------------------------------------------------------
 * Engine lifecycle — ready condvar + quit condvar
 * ----------------------------------------------------------------------- */

static std::mutex ready_mtx;
static std::condition_variable ready_cond;
static bool engine_ready_flag = false;

static std::mutex quit_mtx;
static std::condition_variable quit_cond;
static std::atomic<bool> server_quit{false};

void engine_signal_ready(void)
{
  std::lock_guard<std::mutex> lock(ready_mtx);
  engine_ready_flag = true;
  ready_cond.notify_all();
}

void engine_wait_ready(void)
{
  std::unique_lock<std::mutex> lock(ready_mtx);
  ready_cond.wait(lock, []{ return engine_ready_flag; });
}

void engine_quit(void)
{
  server_quit = true;
  quit_cond.notify_all();
}

/* -----------------------------------------------------------------------
 * Global engine event queue pointer (set by server_init, used by callbacks)
 * ----------------------------------------------------------------------- */
static struct engine_event_queue *g_eq = nullptr;

/* Thread ID of the engine thread (used in signal handling) */
static pthread_t server_tid;

/* Information about currently played file. */
static struct
{
  int avg_bitrate;
  int bitrate;
  int rate;
  int channels;
} sound_info = {-1, -1, -1, -1};

static unique_tags_cache tags_cache;

extern char **environ;

/* -----------------------------------------------------------------------
 * Signal handlers
 * ----------------------------------------------------------------------- */

static void sig_chld(int sig LOGIT_ONLY)
{
  int saved_errno;
  pid_t rc;

  log_signal(sig);

  saved_errno = errno;
  do
  {
    rc = waitpid(-1, nullptr, WNOHANG);
  } while (rc > 0);
  errno = saved_errno;
}

static void sig_exit(int sig)
{
  log_signal(sig);
  server_quit = true;
  quit_cond.notify_all();

  /* pthread_*() is not async-signal-safe, but we only call it when the
   * signal is received in a thread other than the server thread. */
  if (!pthread_equal(server_tid, pthread_self()))
  {
    pthread_kill(server_tid, sig);
  }
}

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static void redirect_output(FILE *stream)
{
  FILE *rc;

  if (stream == stdin)
    rc = freopen("/dev/null", "r", stream);
  else
    rc = freopen("/dev/null", "w", stream);

  if (!rc)
    fatal("Can't open /dev/null: %s", xstrerror(errno).c_str());
}

static void log_process_stack_size()
{
#if !defined(NDEBUG) && defined(HAVE_GETRLIMIT)
  int rc;
  struct rlimit limits;
  rc = getrlimit(RLIMIT_STACK, &limits);
  if (rc == 0)
    logit("Process's stack size: %u", (unsigned int)limits.rlim_cur);
#endif
}

static void log_pthread_stack_size()
{
#if !defined(NDEBUG) && defined(HAVE_PTHREAD_ATTR_GETSTACKSIZE)
  int rc;
  size_t stack_size;
  pthread_attr_t attr;
  rc = pthread_attr_init(&attr);
  if (rc) return;
  rc = pthread_attr_getstacksize(&attr, &stack_size);
  if (rc == 0)
    logit("PThread's stack size: %u", (unsigned int)stack_size);
  pthread_attr_destroy(&attr);
#endif
}

static void run_extern_cmd(const char *event)
{
  const char *command = options_get_str(event);

  if (command)
  {
    std::string command_str = command;
    char *args[2] = {command_str.data(), nullptr};

    switch (fork())
    {
      case 0:
        execve(command_str.c_str(), args, environ);
        fatal("Error when running %s command '%s': %s", event, command_str.c_str(),
              xstrerror(errno).c_str());
      case -1:
      {
        std::string err = xstrerror(errno);
        logit("Error when running %s command '%s': %s", event, command_str.c_str(), err.c_str());
        break;
      }
    }
  }
}

/* -----------------------------------------------------------------------
 * add_event_all — push an event to the UI event queue with data copy
 * ----------------------------------------------------------------------- */

static void add_event_all(const int event, const void *data)
{
  void *data_copy = nullptr;

  if (event == EV_STATE)
  {
    switch (audio_get_state())
    {
      case STATE_PLAY:
        break;
      case STATE_STOP:
        run_extern_cmd("OnStop");
        break;
    }
  }

  if (data)
  {
    if (event == EV_QUEUE_ADD)
    {
      data_copy = new plist_item{};
      plist_item_copy(static_cast<plist_item *>(data_copy),
                      static_cast<const plist_item *>(data));
    }
    else if (event == EV_QUEUE_DEL || event == EV_STATUS_MSG)
    {
      data_copy = new std::string(static_cast<const char *>(data));
    }
    else if (event == EV_SRV_ERROR)
    {
      data_copy = new srv_error_ev(*static_cast<const srv_error_ev *>(data));
    }
    else if (event == EV_QUEUE_MOVE)
    {
      data_copy = new move_ev_data(*static_cast<const move_ev_data *>(data));
    }
    else if (event == EV_FILE_TAGS)
    {
      const auto *src = static_cast<const tag_ev_response *>(data);
      auto *n = new tag_ev_response;
      n->file = src->file;
      n->tags = src->tags ? std::make_unique<file_tags>(*src->tags) : nullptr;
      data_copy = n;
    }
    else
    {
      logit("Unhandled data!");
    }
  }

  if (g_eq)
    eq_push(g_eq, event, data_copy);
  else if (data_copy)
    free_event_data(event, data_copy);
}

/* -----------------------------------------------------------------------
 * server_init — open audio, load tags cache, signal ready
 * ----------------------------------------------------------------------- */

void server_init(struct engine_event_queue *eq)
{
  logit("Starting MOC Engine");

  g_eq = eq;

  log_process_stack_size();
  log_pthread_stack_size();

  audio_initialize();
  tags_cache.reset(tags_cache_new(options_get_int("TagsCacheSize")));
  tags_cache_load(tags_cache.get(), create_file_name("cache").c_str());

  server_tid = pthread_self();
  xsignal(SIGTERM, sig_exit);
  xsignal(SIGINT, SIG_IGN);
  xsignal(SIGHUP, SIG_IGN);
  xsignal(SIGQUIT, sig_exit);
  xsignal(SIGPIPE, SIG_IGN);
  xsignal(SIGCHLD, sig_chld);

  logit("Running OnEngineStart");
  run_extern_cmd("OnEngineStart");

  /* Signal the main thread that we are ready to accept commands. */
  engine_signal_ready();
}

/* -----------------------------------------------------------------------
 * server_loop — wait for quit, then shut down
 * ----------------------------------------------------------------------- */

static void server_shutdown()
{
  logit("Engine exiting...");
  audio_exit();
  tags_cache.reset();
  logit("Running OnEngineStop");
  run_extern_cmd("OnEngineStop");
  logit("Engine exited");
  log_close();
}

void server_loop(void)
{
  logit("MOC engine started, pid: %d", getpid());

  log_circular_start();

  std::unique_lock<std::mutex> lock(quit_mtx);
  while (!server_quit.load())
  {
    quit_cond.wait_for(lock, std::chrono::seconds(1));
  }
  lock.unlock();

  log_circular_log();
  log_circular_stop();

  server_shutdown();
}

/* -----------------------------------------------------------------------
 * Engine→UI notification callbacks (called from player/tags threads)
 * ----------------------------------------------------------------------- */

void set_info_bitrate(const int bitrate)
{
  sound_info.bitrate = bitrate;
  add_event_all(EV_BITRATE, nullptr);
}

void set_info_channels(const int channels)
{
  sound_info.channels = channels;
  add_event_all(EV_CHANNELS, nullptr);
}

void set_info_rate(const int rate)
{
  sound_info.rate = rate;
  add_event_all(EV_RATE, nullptr);
}

void set_info_avg_bitrate(const int avg_bitrate)
{
  sound_info.avg_bitrate = avg_bitrate;
  add_event_all(EV_AVG_BITRATE, nullptr);
}

void state_change(void)  { add_event_all(EV_STATE, nullptr); }
void ctime_change(void)  { add_event_all(EV_CTIME, nullptr); }
void tags_change(void)   { add_event_all(EV_TAGS, nullptr); }

void status_msg(const char *msg) { add_event_all(EV_STATUS_MSG, msg); }

void tags_response(const char *file, const struct file_tags *tags)
{
  assert(file != nullptr);
  assert(tags != nullptr);

  if (!g_eq) return;

  auto *data = new tag_ev_response;
  data->file = file;
  data->tags = std::make_unique<file_tags>(*tags);
  eq_push(g_eq, EV_FILE_TAGS, data);
}

void ev_audio_start(void) { add_event_all(EV_AUDIO_START, nullptr); }
void ev_audio_stop(void)  { add_event_all(EV_AUDIO_STOP, nullptr); }

void server_queue_pop(const char *filename)
{
  debug("Queue pop -- broadcasting EV_QUEUE_DEL");
  add_event_all(EV_QUEUE_DEL, filename);
}

void engine_error(const char *file, const char *msg)
{
  struct srv_error_ev e;
  e.file = file;
  e.msg  = msg;
  add_event_all(EV_SRV_ERROR, &e);
}

void server_error(const char *file, int line, const char *function,
                  const char *msg)
{
  internal_logit(file, line, function, "ERROR: %s", msg);
  engine_error("", msg);
}

/* -----------------------------------------------------------------------
 * Sound-info getters (UI thread reads these after receiving EV_BITRATE etc.)
 * ----------------------------------------------------------------------- */

int engine_get_bitrate(void)     { return sound_info.bitrate; }
int engine_get_avg_bitrate(void) { return sound_info.avg_bitrate; }
int engine_get_rate(void)        { return sound_info.rate; }
int engine_get_channels(void)    { return sound_info.channels; }

/* -----------------------------------------------------------------------
 * Option sync
 * ----------------------------------------------------------------------- */

static int valid_sync_option(const char *name)
{
  return !strcasecmp(name, "ShowStreamErrors") ||
         !strcasecmp(name, "Repeat")           ||
         !strcasecmp(name, "Shuffle")           ||
         !strcasecmp(name, "AutoNext");
}

void engine_set_option(const char *name, bool val)
{
  if (!valid_sync_option(name))
  {
    logit("Ignoring request to set invalid option '%s'", name);
    return;
  }
  options_set_bool(name, val);
  add_event_all(EV_OPTIONS, nullptr);
}

/* -----------------------------------------------------------------------
 * Tags requests
 * ----------------------------------------------------------------------- */

void engine_request_file_tags(const char *file, int tags_sel)
{
  tags_cache_add_request(tags_cache.get(), file, tags_sel);
}

void engine_abort_tags_requests(const char *file)
{
  tags_cache_clear_up_to(tags_cache.get(), file);
}

/* -----------------------------------------------------------------------
 * Queue operations
 * ----------------------------------------------------------------------- */

void engine_queue_add(const char *file)
{
  logit("Adding '%s' to the queue", file);
  audio_queue_add(file);

  struct plist_item item{};
  item.file  = file;
  item.type  = file_type(file);
  item.mtime = get_mtime(file);

  add_event_all(EV_QUEUE_ADD, &item);
}

void engine_queue_del(const char *file)
{
  debug("Deleting '%s' from queue", file);
  audio_queue_delete(file);
  add_event_all(EV_QUEUE_DEL, file);
}

void engine_queue_clear(void)
{
  logit("Clearing the queue");
  audio_queue_clear();
  add_event_all(EV_QUEUE_CLEAR, nullptr);
}

std::unique_ptr<struct plist> engine_get_queue(void)
{
  return audio_queue_get_contents();
}

/* -----------------------------------------------------------------------
 * Compound commands
 * ----------------------------------------------------------------------- */

void engine_jump_to(int sec)
{
  if (sec < 0)
  {
    int percent = -sec;
    assert(percent >= 0 && percent <= 100);

    std::string file = audio_get_sname();
    if (file.empty())
    {
      return;
    }

    std::unique_ptr<struct file_tags> tags(
        tags_cache_get_immediate(tags_cache.get(), file.c_str(), TAGS_TIME));
    if (!tags || !(tags->filled & TAGS_TIME))
    {
      return;
    }

    sec = (tags->time * percent) / 100;
  }

  logit("Jumping to %ds", sec);
  audio_jump_to(sec);
}

void engine_toggle_mixer_channel(void)
{
  audio_toggle_mixer_channel();
  add_event_all(EV_MIXER_CHANGE, nullptr);
}

void engine_toggle_softmixer(void)
{
  softmixer_set_active(!softmixer_is_active());
  add_event_all(EV_MIXER_CHANGE, nullptr);
}

static void update_eq_name()
{
  char buffer[27];
  std::string n = equalizer_current_eqname();
  int l   = n.length();

  if (l > 14)
  {
    n.resize(14);
    n[13] = '.';
    n[12] = '.';
    n[11] = '.';
  }

  snprintf(buffer, sizeof(buffer), "EQ set to: %s", n.c_str());
  logit("%s", buffer);
  status_msg(buffer);
}

void engine_toggle_equalizer(void)
{
  equalizer_set_active(!equalizer_is_active());
  update_eq_name();
}

void engine_equalizer_refresh(void)
{
  equalizer_refresh();
  status_msg("Equalizer refreshed");
  logit("Equalizer refreshed");
}

void engine_equalizer_prev(void)
{
  equalizer_prev();
  update_eq_name();
}

void engine_equalizer_next(void)
{
  equalizer_next();
  update_eq_name();
}

void engine_toggle_make_mono(void)
{
  char buffer[128];
  softmixer_set_mono(!softmixer_is_mono());
  snprintf(buffer, sizeof(buffer), "Mono-Mixing set to: %s", softmixer_is_mono() ? "on" : "off");
  status_msg(buffer);
}

// EOF
