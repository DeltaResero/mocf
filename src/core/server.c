// src/core/server.c
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

#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <fcntl.h>
#include <assert.h>

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

#define SERVER_LOG "mocf_server_log"

/* -----------------------------------------------------------------------
 * Engine event queue — thread-safe linked-list + pipe wakeup
 * ----------------------------------------------------------------------- */

struct engine_event_queue
{
  struct event_queue q;
  pthread_mutex_t    mtx;
  int                pipe_fd[2]; /* [0] = read (UI), [1] = write (engine) */
};

struct engine_event_queue *engine_event_queue_new(void)
{
  struct engine_event_queue *eq =
      (struct engine_event_queue *)xmalloc(sizeof(*eq));
  event_queue_init(&eq->q);
  pthread_mutex_init(&eq->mtx, NULL);

  if (pipe(eq->pipe_fd) < 0)
    fatal("pipe() failed for engine event queue: %s", xstrerror(errno));

  /* Make the write end non-blocking so audio callbacks never block. */
  int flags = fcntl(eq->pipe_fd[1], F_GETFL);
  if (flags == -1 || fcntl(eq->pipe_fd[1], F_SETFL, flags | O_NONBLOCK) == -1)
    fatal("fcntl() on event pipe failed: %s", xstrerror(errno));

  /* Make the read end non-blocking so drain loops never block. */
  flags = fcntl(eq->pipe_fd[0], F_GETFL);
  if (flags == -1 || fcntl(eq->pipe_fd[0], F_SETFL, flags | O_NONBLOCK) == -1)
    fatal("fcntl() on event pipe failed: %s", xstrerror(errno));

  return eq;
}

void engine_event_queue_free(struct engine_event_queue *eq)
{
  if (!eq) return;
  LOCK(eq->mtx);
  event_queue_free(&eq->q);
  UNLOCK(eq->mtx);
  pthread_mutex_destroy(&eq->mtx);
  close(eq->pipe_fd[0]);
  close(eq->pipe_fd[1]);
  free(eq);
}

int engine_event_queue_fd(const struct engine_event_queue *eq)
{
  return eq->pipe_fd[0];
}

/* Push an event onto the queue and wake the UI thread. */
static void eq_push(struct engine_event_queue *eq, int type, void *data)
{
  char w = 1;
  LOCK(eq->mtx);
  event_push(&eq->q, type, data);
  UNLOCK(eq->mtx);
  /* Best-effort wakeup; non-blocking pipe so this never stalls. */
  if (write(eq->pipe_fd[1], &w, 1) < 0 && errno != EAGAIN)
    logit("Can't write to engine event pipe: %s", xstrerror(errno));
}

/* Drain all pending events + consume wakeup bytes.  Non-blocking. */
void engine_event_queue_flush(struct engine_event_queue *eq,
                              struct event_queue *dest)
{
  char buf[64];
  /* Consume any wakeup bytes (non-blocking, so returns when pipe is empty). */
  while (read(eq->pipe_fd[0], buf, sizeof(buf)) > 0)
    ;

  /* Splice the shared queue onto the tail of dest. */
  LOCK(eq->mtx);
  if (!event_queue_empty(&eq->q))
  {
    if (event_queue_empty(dest))
    {
      *dest = eq->q;
    }
    else
    {
      dest->tail->next = eq->q.head;
      dest->tail       = eq->q.tail;
    }
    event_queue_init(&eq->q);
  }
  UNLOCK(eq->mtx);
}

/* Blocking variant: wait until at least one event arrives, then drain. */
void engine_event_queue_wait_flush(struct engine_event_queue *eq,
                                   struct event_queue *dest)
{
  /* First drain without blocking in case there is already something. */
  engine_event_queue_flush(eq, dest);
  if (!event_queue_empty(dest))
    return;

  /* Nothing yet — block on the read end of the pipe. */
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(eq->pipe_fd[0], &fds);

  /* select() restores the O_NONBLOCK flag so reading will still work. */
  int fd_blocking = eq->pipe_fd[0];
  /* Temporarily make the read end blocking just for select. */
  (void)select(fd_blocking + 1, &fds, NULL, NULL, NULL);

  engine_event_queue_flush(eq, dest);
}

/* -----------------------------------------------------------------------
 * Engine lifecycle — ready condvar + quit condvar
 * ----------------------------------------------------------------------- */

static pthread_mutex_t ready_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ready_cond = PTHREAD_COND_INITIALIZER;
static int             engine_ready_flag = 0;

static pthread_mutex_t quit_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  quit_cond = PTHREAD_COND_INITIALIZER;
static volatile int    server_quit = 0;

void engine_signal_ready(void)
{
  pthread_mutex_lock(&ready_mtx);
  engine_ready_flag = 1;
  pthread_cond_signal(&ready_cond);
  pthread_mutex_unlock(&ready_mtx);
}

void engine_wait_ready(void)
{
  pthread_mutex_lock(&ready_mtx);
  while (!engine_ready_flag)
    pthread_cond_wait(&ready_cond, &ready_mtx);
  pthread_mutex_unlock(&ready_mtx);
}

void engine_quit(void)
{
  pthread_mutex_lock(&quit_mtx);
  server_quit = 1;
  pthread_cond_signal(&quit_cond);
  pthread_mutex_unlock(&quit_mtx);
}

/* -----------------------------------------------------------------------
 * Global engine event queue pointer (set by server_init, used by callbacks)
 * ----------------------------------------------------------------------- */
static struct engine_event_queue *g_eq = NULL;

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

static struct tags_cache *tags_cache;

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
    rc = waitpid(-1, NULL, WNOHANG);
  } while (rc > 0);
  errno = saved_errno;
}

static void sig_exit(int sig)
{
  log_signal(sig);
  server_quit = 1;

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
    fatal("Can't open /dev/null: %s", xstrerror(errno));
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
  char *command;

  command = xstrdup(options_get_str(event));

  if (command)
  {
    char *args[2], *err;

    args[0] = xstrdup(command);
    args[1] = NULL;

    switch (fork())
    {
      case 0:
        execve(command, args, environ);
        fatal("Error when running %s command '%s': %s", event, command,
              xstrerror(errno));
      case -1:
        err = xstrerror(errno);
        logit("Error when running %s command '%s': %s", event, command, err);
        free(err);
        break;
    }

    free(command);
    free(args[0]);
  }
}

/* -----------------------------------------------------------------------
 * add_event_all — push an event to the UI event queue with data copy
 * ----------------------------------------------------------------------- */

static void add_event_all(const int event, const void *data)
{
  void *data_copy = NULL;

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
      data_copy = plist_new_item();
      plist_item_copy(data_copy, data);
    }
    else if (event == EV_QUEUE_DEL ||
             event == EV_STATUS_MSG || event == EV_SRV_ERROR)
    {
      data_copy = xstrdup(data);
    }
    else if (event == EV_QUEUE_MOVE)
    {
      data_copy = move_ev_data_dup((struct move_ev_data *)data);
    }
    else if (event == EV_FILE_TAGS)
    {
      data_copy = tag_ev_data_dup((struct tag_ev_response *)data);
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

void server_init(struct engine_event_queue *eq, int debugging, int foreground)
{
  logit("Starting MOC Engine");

  g_eq = eq;

  if (foreground)
  {
    log_init_stream(stdout, "stdout");
  }
  else
  {
    FILE *logfp = NULL;
    if (debugging)
    {
      logfp = fopen(SERVER_LOG, "a");
      if (!logfp)
        fatal("Can't open server log file: %s", xstrerror(errno));
    }
    log_init_stream(logfp, SERVER_LOG);
  }

  log_process_stack_size();
  log_pthread_stack_size();

  audio_initialize();
  tags_cache = tags_cache_new(options_get_int("TagsCacheSize"));
  tags_cache_load(tags_cache, create_file_name("cache"));

  server_tid = pthread_self();
  xsignal(SIGTERM, sig_exit);
  xsignal(SIGINT, foreground ? sig_exit : SIG_IGN);
  xsignal(SIGHUP, SIG_IGN);
  xsignal(SIGQUIT, sig_exit);
  xsignal(SIGPIPE, SIG_IGN);
  xsignal(SIGCHLD, sig_chld);

  logit("Running OnServerStart");
  run_extern_cmd("OnServerStart");

  /* Signal the main thread that we are ready to accept commands. */
  engine_signal_ready();
}

/* -----------------------------------------------------------------------
 * server_loop — wait for quit, then shut down
 * ----------------------------------------------------------------------- */

static void server_shutdown(void)
{
  logit("Engine exiting...");
  audio_exit();
  tags_cache_free(tags_cache);
  tags_cache = NULL;
  logit("Running OnServerStop");
  run_extern_cmd("OnServerStop");
  logit("Engine exited");
  log_close();
}

void server_loop(void)
{
  logit("MOC engine started, pid: %d", getpid());

  log_circular_start();

  pthread_mutex_lock(&quit_mtx);
  while (!server_quit)
  {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    pthread_cond_timedwait(&quit_cond, &quit_mtx, &ts);
  }
  pthread_mutex_unlock(&quit_mtx);

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
  add_event_all(EV_BITRATE, NULL);
}

void set_info_channels(const int channels)
{
  sound_info.channels = channels;
  add_event_all(EV_CHANNELS, NULL);
}

void set_info_rate(const int rate)
{
  sound_info.rate = rate;
  add_event_all(EV_RATE, NULL);
}

void set_info_avg_bitrate(const int avg_bitrate)
{
  sound_info.avg_bitrate = avg_bitrate;
  add_event_all(EV_AVG_BITRATE, NULL);
}

void state_change(void)  { add_event_all(EV_STATE, NULL); }
void ctime_change(void)  { add_event_all(EV_CTIME, NULL); }
void tags_change(void)   { add_event_all(EV_TAGS, NULL); }

void status_msg(const char *msg) { add_event_all(EV_STATUS_MSG, msg); }

void tags_response(const char *file, const struct file_tags *tags)
{
  assert(file != NULL);
  assert(tags != NULL);

  if (!g_eq) return;

  struct tag_ev_response *data =
      (struct tag_ev_response *)xmalloc(sizeof(struct tag_ev_response));
  data->file = xstrdup(file);
  data->tags = tags_dup(tags);
  eq_push(g_eq, EV_FILE_TAGS, data);
}

void ev_audio_start(void) { add_event_all(EV_AUDIO_START, NULL); }
void ev_audio_stop(void)  { add_event_all(EV_AUDIO_STOP, NULL); }

void server_queue_pop(const char *filename)
{
  debug("Queue pop -- broadcasting EV_QUEUE_DEL");
  add_event_all(EV_QUEUE_DEL, filename);
}

void server_error(const char *file, int line, const char *function,
                  const char *msg)
{
  internal_logit(file, line, function, "ERROR: %s", msg);
  add_event_all(EV_SRV_ERROR, msg);
}

/* -----------------------------------------------------------------------
 * Sound-info getters (UI thread reads these after receiving EV_BITRATE etc.)
 * ----------------------------------------------------------------------- */

int engine_get_bitrate(void)     { return sound_info.bitrate; }
int engine_get_avg_bitrate(void) { return sound_info.avg_bitrate; }
int engine_get_rate(void)        { return sound_info.rate; }
int engine_get_channels(void)    { return sound_info.channels; }

/* -----------------------------------------------------------------------
 * Serial number generator
 * ----------------------------------------------------------------------- */

int engine_gen_serial(void)
{
  static int seed = 0;
  int serial;

  do
  {
    serial = (seed << 8);
    seed   = (seed + 1) & 0xFF;
  } while (serial == audio_plist_get_serial());

  debug("Generated serial %d", serial);
  return serial;
}

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
  add_event_all(EV_OPTIONS, NULL);
}

/* -----------------------------------------------------------------------
 * Tags requests
 * ----------------------------------------------------------------------- */

void engine_request_file_tags(const char *file, int tags_sel)
{
  tags_cache_add_request(tags_cache, file, tags_sel);
}

void engine_abort_tags_requests(const char *file)
{
  tags_cache_clear_up_to(tags_cache, file);
}

/* -----------------------------------------------------------------------
 * Queue operations
 * ----------------------------------------------------------------------- */

void engine_queue_add(const char *file)
{
  struct plist_item *item;

  logit("Adding '%s' to the queue", file);
  audio_queue_add(file);

  item = plist_new_item();
  item->file  = xstrdup(file);
  item->type  = file_type(file);
  item->mtime = get_mtime(file);

  add_event_all(EV_QUEUE_ADD, item);

  plist_free_item_fields(item);
  free(item);
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
  add_event_all(EV_QUEUE_CLEAR, NULL);
}

struct plist *engine_get_queue(void)
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

    char *file = audio_get_sname();
    if (!file || !*file)
    {
      free(file);
      return;
    }

    struct file_tags *tags;
    tags = tags_cache_get_immediate(tags_cache, file, TAGS_TIME);
    if (!tags || !(tags->filled & TAGS_TIME))
    {
      tags_free(tags);
      free(file);
      return;
    }

    sec = (tags->time * percent) / 100;
    tags_free(tags);
    free(file);
  }

  logit("Jumping to %ds", sec);
  audio_jump_to(sec);
}

void engine_toggle_mixer_channel(void)
{
  audio_toggle_mixer_channel();
  add_event_all(EV_MIXER_CHANGE, NULL);
}

void engine_toggle_softmixer(void)
{
  softmixer_set_active(!softmixer_is_active());
  add_event_all(EV_MIXER_CHANGE, NULL);
}

static void update_eq_name(void)
{
  char buffer[27];
  char *n = equalizer_current_eqname();
  int l   = strlen(n);

  if (l > 14)
  {
    n[14] = 0;
    n[13] = '.';
    n[12] = '.';
    n[11] = '.';
  }

  sprintf(buffer, "EQ set to: %s", n);
  logit("%s", buffer);
  free(n);
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
  sprintf(buffer, "Mono-Mixing set to: %s", softmixer_is_mono() ? "on" : "off");
  status_msg(buffer);
}

// EOF
