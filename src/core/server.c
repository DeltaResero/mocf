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
#include <sys/socket.h>
#include <sys/select.h>
#ifdef HAVE_GETRLIMIT
#include <sys/resource.h>
#endif
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
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

struct client
{
  int socket;             /* -1 if inactive */
  struct event_queue events;
  pthread_mutex_t events_mtx;
  int serial;         /* used for generating unique serial numbers */
};

static struct client clients[CLIENTS_MAX];

/* Thread ID of the server thread. */
static pthread_t server_tid;

/* Pipe used to wake up the server from select() from another thread. */
static int wake_up_pipe[2];

/* Internal socketpair end passed from main (UI connection). */
static int server_sock = -1;

/* Set to 1 when a signal arrived causing the program to exit. */
static volatile int server_quit = 0;

/* Information about currently played file */
static struct
{
  int avg_bitrate;
  int bitrate;
  int rate;
  int channels;
} sound_info = {-1, -1, -1, -1};

static struct tags_cache *tags_cache;

extern char **environ;

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

  // FIXME (JCF): pthread_*() are not async-signal-safe and
  //              should not be used within signal handlers.
  if (!pthread_equal(server_tid, pthread_self()))
  {
    pthread_kill(server_tid, sig);
  }
}

static void clients_init()
{
  int i;

  for (i = 0; i < CLIENTS_MAX; i++)
  {
    clients[i].socket = -1;
    pthread_mutex_init(&clients[i].events_mtx, NULL);
  }
}

static void clients_cleanup()
{
  int i, rc;

  for (i = 0; i < CLIENTS_MAX; i++)
  {
    clients[i].socket = -1;
    rc = pthread_mutex_destroy(&clients[i].events_mtx);
    if (rc != 0)
    {
      log_errno("Can't destroy events mutex", rc);
    }
  }
}

/* Register the single UI connection. */
static void add_client(int sock)
{
  assert(clients[0].socket == -1);

  LOCK(clients[0].events_mtx);
  event_queue_free(&clients[0].events);
  event_queue_init(&clients[0].events);
  UNLOCK(clients[0].events_mtx);
  clients[0].socket = sock;
  tags_cache_clear_queue(tags_cache);
}

static void del_client(struct client *cli)
{
  cli->socket = -1;
  LOCK(cli->events_mtx);
  event_queue_free(&cli->events);
  tags_cache_clear_queue(tags_cache);
  UNLOCK(cli->events_mtx);
}

/* Check if the process with given PID exists. Return != 0 if so. */
static void wake_up_server()
{
  int w = 1;

  debug("Waking up the server");

  if (write(wake_up_pipe[1], &w, sizeof(w)) < 0)
  {
    log_errno("Can't wake up the server: (write() failed)", errno);
  }
}

static void redirect_output(FILE *stream)
{
  FILE *rc;

  if (stream == stdin)
  {
    rc = freopen("/dev/null", "r", stream);
  }
  else
  {
    rc = freopen("/dev/null", "w", stream);
  }

  if (!rc)
  {
    fatal("Can't open /dev/null: %s", xstrerror(errno));
  }
}

static void log_process_stack_size()
{
#if !defined(NDEBUG) && defined(HAVE_GETRLIMIT)
  int rc;
  struct rlimit limits;

  rc = getrlimit(RLIMIT_STACK, &limits);
  if (rc == 0)
  {
    logit("Process's stack size: %u", (unsigned int)limits.rlim_cur);
  }
#endif
}

static void log_pthread_stack_size()
{
#if !defined(NDEBUG) && defined(HAVE_PTHREAD_ATTR_GETSTACKSIZE)
  int rc;
  size_t stack_size;
  pthread_attr_t attr;

  rc = pthread_attr_init(&attr);
  if (rc)
  {
    return;
  }

  rc = pthread_attr_getstacksize(&attr, &stack_size);
  if (rc == 0)
  {
    logit("PThread's stack size: %u", (unsigned int)stack_size);
  }

  pthread_attr_destroy(&attr);
#endif
}

/* Handle running external command on requested event. */
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

/* Initialize the audio engine thread. */
void server_init(int internal_sock, int debugging, int foreground)
{
  logit("Starting MOC Server");

  assert(server_sock == -1);

  if (foreground)
  {
    log_init_stream(stdout, "stdout");
  }
  else
  {
    FILE *logfp;

    logfp = NULL;
    if (debugging)
    {
      logfp = fopen(SERVER_LOG, "a");
      if (!logfp)
      {
        fatal("Can't open server log file: %s", xstrerror(errno));
      }
    }
    log_init_stream(logfp, SERVER_LOG);
  }

  if (pipe(wake_up_pipe) < 0)
  {
    fatal("pipe() failed: %s", xstrerror(errno));
  }

  server_sock = internal_sock;

  /* Log stack sizes so stack overflows can be debugged. */
  log_process_stack_size();
  log_pthread_stack_size();

  clients_init();
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

  return;
}

/* Send EV_DATA and the integer value. Return 0 on error. */
static int send_data_int(const struct client *cli, const int data)
{
  assert(cli->socket != -1);

  if (!send_int(cli->socket, EV_DATA) || !send_int(cli->socket, data))
  {
    return 0;
  }

  return 1;
}

/* Send EV_DATA and the boolean value. Return 0 on error. */
static int send_data_bool(const struct client *cli, const bool data)
{
  assert(cli->socket != -1);

  if (!send_int(cli->socket, EV_DATA) || !send_int(cli->socket, data ? 1 : 0))
  {
    return 0;
  }

  return 1;
}

/* Send EV_DATA and the string value. Return 0 on error. */
static int send_data_str(const struct client *cli, const char *str)
{
  if (!send_int(cli->socket, EV_DATA) || !send_str(cli->socket, str))
  {
    return 0;
  }
  return 1;
}

/* Add event to the client's queue */
static void add_event(struct client *cli, const int event, void *data)
{
  LOCK(cli->events_mtx);
  event_push(&cli->events, event, data);
  UNLOCK(cli->events_mtx);
}

static void add_event_all(const int event, const void *data)
{
  int i;
  int added = 0;

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

  for (i = 0; i < CLIENTS_MAX; i++)
  {
    void *data_copy = NULL;

    if (clients[i].socket == -1)
    {
      continue;
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

    add_event(&clients[i], event, data_copy);
    added++;
  }

  if (added)
  {
    wake_up_server();
  }
  else
  {
    debug("No events have been added because there are no clients");
  }
}

/* Send events from the queue. Return 0 on error. */
static int flush_events(struct client *cli)
{
  enum noblock_io_status st = NB_IO_OK;

  LOCK(cli->events_mtx);
  while (!event_queue_empty(&cli->events) &&
         (st = event_send_noblock(cli->socket, &cli->events)) == NB_IO_OK)
    ;
  UNLOCK(cli->events_mtx);

  return st != NB_IO_ERR ? 1 : 0;
}

/* Send events to clients whose sockets are ready to write. */
static void send_events(fd_set *fds)
{
  int i;

  for (i = 0; i < CLIENTS_MAX; i++)
  {
    if (clients[i].socket != -1 && FD_ISSET(clients[i].socket, fds))
    {
      debug("Flushing events for client %d", i);
      if (!flush_events(&clients[i]))
      {
        close(clients[i].socket);
        del_client(&clients[i]);
      }
    }
  }
}

/* End playing and cleanup. */
static void server_shutdown()
{
  logit("Server exiting...");
  audio_exit();
  tags_cache_free(tags_cache);
  tags_cache = NULL;
  logit("Running OnServerStop");
  run_extern_cmd("OnServerStop");
  close(wake_up_pipe[0]);
  close(wake_up_pipe[1]);
  logit("Server exited");
  log_close();
}

/* Handle CMD_LIST_ADD, return 1 if ok or 0 on error. */
static int req_list_add(struct client *cli)
{
  char *file;

  file = get_str(cli->socket);
  if (!file)
  {
    return 0;
  }

  logit("Adding '%s' to the list", file);

  audio_plist_add(file);
  free(file);

  return 1;
}

/* Handle CMD_QUEUE_ADD, return 1 if ok or 0 on error. */
static int req_queue_add(const struct client *cli)
{
  char *file;
  struct plist_item *item;

  file = get_str(cli->socket);
  if (!file)
  {
    return 0;
  }

  logit("Adding '%s' to the queue", file);

  audio_queue_add(file);

  /* Wrap the filename in struct plist_item.
   * We don't need tags, because the player gets them
   * when playing the file. This may change if there is
   * support for viewing/reordering the queue and here
   * is the place to read the tags and fill them into
   * the item. */

  item = plist_new_item();
  item->file = xstrdup(file);
  item->type = file_type(file);
  item->mtime = get_mtime(file);

  add_event_all(EV_QUEUE_ADD, item);

  plist_free_item_fields(item);
  free(item);
  free(file);

  return 1;
}

/* Handle CMD_PLAY, return 1 if ok or 0 on error. */
static int req_play(struct client *cli)
{
  char *file;

  if (!(file = get_str(cli->socket)))
  {
    return 0;
  }

  logit("Playing %s", *file ? file : "first element on the list");
  audio_play(file);
  free(file);

  return 1;
}

/* Handle CMD_SEEK, return 1 if ok or 0 on error */
static int req_seek(struct client *cli)
{
  int sec;

  if (!get_int(cli->socket, &sec))
  {
    return 0;
  }

  logit("Seeking %ds", sec);
  audio_seek(sec);

  return 1;
}

/* Handle CMD_JUMP_TO, return 1 if ok or 0 on error */
static int req_jump_to(struct client *cli)
{
  int sec;

  if (!get_int(cli->socket, &sec))
  {
    return 0;
  }

  if (sec < 0)
  {
    sec = -sec; /* percentage */
    assert(sec >= 0 && sec <= 100);

    char *file = audio_get_sname();
    if (!file || !*file)
    {
      free(file);
      return 0;
    }

    struct file_tags *tags;
    tags = tags_cache_get_immediate(tags_cache, file, TAGS_TIME);
    assert(tags && tags->filled & TAGS_TIME);

    sec = (tags->time * sec) / 100;
    tags_free(tags);
    free(file);
  }

  logit("Jumping to %ds", sec);
  audio_jump_to(sec);

  return 1;
}

/* Report an error logging it and sending a message to the client. */
void server_error(const char *file, int line, const char *function,
                  const char *msg)
{
  internal_logit(file, line, function, "ERROR: %s", msg);
  add_event_all(EV_SRV_ERROR, msg);
}

/* Send the song name to the client. Return 0 on error. */
static int send_sname(struct client *cli)
{
  int status = 1;
  char *sname = audio_get_sname();

  if (!send_data_str(cli, sname ? sname : ""))
  {
    status = 0;
  }
  free(sname);

  return status;
}

/* Return 0 if an option is valid when getting/setting with the client. */
static int valid_sync_option(const char *name)
{
  return !strcasecmp(name, "ShowStreamErrors") || !strcasecmp(name, "Repeat") ||
         !strcasecmp(name, "Shuffle") || !strcasecmp(name, "AutoNext");
}

/* Send requested option value to the client. Return 1 if OK. */
static int send_option(struct client *cli)
{
  char *name;

  if (!(name = get_str(cli->socket)))
  {
    return 0;
  }

  /* We can send only a few options, others make no sense here. */
  if (!valid_sync_option(name))
  {
    logit("Client wanted to get invalid option '%s'", name);
    free(name);
    return 0;
  }

  /* All supported options are boolean type. */
  if (!send_data_bool(cli, options_get_bool(name)))
  {
    free(name);
    return 0;
  }

  free(name);
  return 1;
}

/* Get and set an option from the client. Return 1 on error. */
static int get_set_option(struct client *cli)
{
  char *name;
  int val;

  if (!(name = get_str(cli->socket)))
  {
    return 0;
  }
  if (!valid_sync_option(name))
  {
    logit("Client requested setting invalid option '%s'", name);
    return 0;
  }
  if (!get_int(cli->socket, &val))
  {
    free(name);
    return 0;
  }

  options_set_bool(name, val ? true : false);
  free(name);

  add_event_all(EV_OPTIONS, NULL);

  return 1;
}

/* Set the mixer to the value provided by the client. Return 0 on error. */
static int set_mixer(struct client *cli)
{
  int val;

  if (!get_int(cli->socket, &val))
  {
    return 0;
  }

  audio_set_mixer(val);
  return 1;
}

/* Delete an item from the playlist. Return 0 on error. */
static int delete_item(struct client *cli)
{
  char *file;

  if (!(file = get_str(cli->socket)))
  {
    return 0;
  }

  debug("Request for deleting %s", file);

  audio_plist_delete(file);
  free(file);
  return 1;
}

static int req_queue_del(const struct client *cli)
{
  char *file;

  if (!(file = get_str(cli->socket)))
  {
    return 0;
  }

  debug("Deleting '%s' from queue", file);

  audio_queue_delete(file);
  add_event_all(EV_QUEUE_DEL, file);
  free(file);

  return 1;
}

/* Client requested we send the queue so we get it from audio.c and
 * send it to the client. */
static int req_send_queue(struct client *cli)
{
  int i;
  struct plist *queue;

  logit("Client with fd %d wants queue... sending it", cli->socket);

  if (!send_int(cli->socket, EV_DATA))
  {
    logit("Error while sending response; disconnecting the client");
    close(cli->socket);
    del_client(cli);
    return 0;
  }

  queue = audio_queue_get_contents();

  for (i = 0; i < queue->num; i++)
  {
    if (!plist_deleted(queue, i))
    {
      if (!send_item(cli->socket, &queue->items[i]))
      {
        logit("Error sending queue; disconnecting the client");
        close(cli->socket);
        del_client(cli);
        free(queue);
        return 0;
      }
    }
  }

  plist_free(queue);
  free(queue);

  if (!send_item(cli->socket, NULL))
  {
    logit("Error while sending end of playlist mark; "
          "disconnecting the client");
    close(cli->socket);
    del_client(cli);
    return 0;
  }

  logit("Queue sent");
  return 1;
}

/* Handle command that synchronises the playlists between interfaces
 * (except forwarding the whole list). Return 0 on error. */
/* Handle CMD_PLIST_GET_SERIAL. Return 0 on error. */
static int req_plist_get_serial(struct client *cli)
{
  if (!send_data_int(cli, audio_plist_get_serial()))
  {
    return 0;
  }
  return 1;
}

/* Handle CMD_PLIST_SET_SERIAL. Return 0 on error. */
static int req_plist_set_serial(struct client *cli)
{
  int serial;

  if (!get_int(cli->socket, &serial))
  {
    return 0;
  }

  if (serial < 0)
  {
    logit("Client wants to set bad serial number");
    return 0;
  }

  debug("Setting the playlist serial number to %d", serial);
  audio_plist_set_serial(serial);

  return 1;
}

/* Generate a unique playlist serial number. */
static int gen_serial(void)
{
  static int seed = 0;
  int serial;

  do
  {
    serial = (seed << 8);
    seed = (seed + 1) & 0xFF;
  } while (serial == audio_plist_get_serial());

  debug("Generated serial %d", serial);

  return serial;
}

/* Send the unique number to the client. Return 0 on error. */
static int send_serial(struct client *cli)
{
  if (!send_data_int(cli, gen_serial()))
  {
    logit("Error when sending serial");
    return 0;
  }
  return 1;
}

/* Handle CMD_GET_MIXER_CHANNEL_NAME. Return 0 on error. */
int req_get_mixer_channel_name(struct client *cli)
{
  int status = 1;
  char *name = audio_get_mixer_channel_name();

  if (!send_data_str(cli, name ? name : ""))
  {
    status = 0;
  }
  free(name);

  return status;
}

/* Handle CMD_TOGGLE_MIXER_CHANNEL. */
void req_toggle_mixer_channel()
{
  audio_toggle_mixer_channel();
  add_event_all(EV_MIXER_CHANGE, NULL);
}

/* Handle CMD_TOGGLE_SOFTMIXER. */
void req_toggle_softmixer()
{
  softmixer_set_active(!softmixer_is_active());
  add_event_all(EV_MIXER_CHANGE, NULL);
}

void update_eq_name()
{
  char buffer[27];

  char *n = equalizer_current_eqname();

  int l = strlen(n);

  /* Status message can only take strings up to 25 chars
   * (Without terminating zero).
   * The message header has 11 chars (EQ set to...).
   */
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

void req_toggle_equalizer()
{
  equalizer_set_active(!equalizer_is_active());

  update_eq_name();
}

void req_equalizer_refresh()
{
  equalizer_refresh();

  status_msg("Equalizer refreshed");

  logit("Equalizer refreshed");
}

void req_equalizer_prev()
{
  equalizer_prev();

  update_eq_name();
}

void req_equalizer_next()
{
  equalizer_next();

  update_eq_name();
}

void req_toggle_make_mono()
{
  char buffer[128];

  softmixer_set_mono(!softmixer_is_mono());

  sprintf(buffer, "Mono-Mixing set to: %s", softmixer_is_mono() ? "on" : "off");

  status_msg(buffer);
}

/* Handle CMD_GET_FILE_TAGS. Return 0 on error. */
static int get_file_tags(const int cli_id)
{
  char *file;
  int tags_sel;

  if (!(file = get_str(clients[cli_id].socket)))
  {
    return 0;
  }
  if (!get_int(clients[cli_id].socket, &tags_sel))
  {
    free(file);
    return 0;
  }

  tags_cache_add_request(tags_cache, file, tags_sel);
  free(file);

  return 1;
}

static int abort_tags_requests(const int cli_id)
{
  char *file;

  if (!(file = get_str(clients[cli_id].socket)))
  {
    return 0;
  }

  tags_cache_clear_up_to(tags_cache, file);
  free(file);

  return 1;
}

/* Handle CMD_LIST_MOVE. Return 0 on error. */
static int req_list_move(struct client *cli)
{
  char *from;
  char *to;

  if (!(from = get_str(cli->socket)))
  {
    return 0;
  }
  if (!(to = get_str(cli->socket)))
  {
    free(from);
    return 0;
  }

  audio_plist_move(from, to);

  free(from);
  free(to);

  return 1;
}

/* Receive a command from the client and execute it. */
static void handle_command(const int client_id)
{
  int cmd;
  int err = 0;
  struct client *cli = &clients[client_id];

  if (!get_int(cli->socket, &cmd))
  {
    logit("Failed to get command from the client");
    close(cli->socket);
    del_client(cli);
    return;
  }

  switch (cmd)
  {
    case CMD_QUIT:
      logit("Exit request from the client");
      close(cli->socket);
      del_client(cli);
      server_quit = 1;
      break;
    case CMD_LIST_CLEAR:
      logit("Clearing the list");
      audio_plist_clear();
      break;
    case CMD_LIST_ADD:
      if (!req_list_add(cli))
      {
        err = 1;
      }
      break;
    case CMD_PLAY:
      if (!req_play(cli))
      {
        err = 1;
      }
      break;
    case CMD_PAUSE:
      audio_pause();
      break;
    case CMD_UNPAUSE:
      audio_unpause();
      break;
    case CMD_STOP:
      audio_stop();
      break;
    case CMD_GET_CTIME:
      if (!send_data_int(cli, MAX(0, audio_get_time())))
      {
        err = 1;
      }
      break;
    case CMD_SEEK:
      if (!req_seek(cli))
      {
        err = 1;
      }
      break;
    case CMD_JUMP_TO:
      if (!req_jump_to(cli))
      {
        err = 1;
      }
      break;
    case CMD_GET_SNAME:
      if (!send_sname(cli))
      {
        err = 1;
      }
      break;
    case CMD_GET_STATE:
      if (!send_data_int(cli, audio_get_state()))
      {
        err = 1;
      }
      break;
    case CMD_GET_BITRATE:
      if (!send_data_int(cli, sound_info.bitrate))
      {
        err = 1;
      }
      break;
    case CMD_GET_AVG_BITRATE:
      if (!send_data_int(cli, sound_info.avg_bitrate))
      {
        err = 1;
      }
      break;
    case CMD_GET_RATE:
      if (!send_data_int(cli, sound_info.rate))
      {
        err = 1;
      }
      break;
    case CMD_GET_CHANNELS:
      if (!send_data_int(cli, sound_info.channels))
      {
        err = 1;
      }
      break;
    case CMD_NEXT:
      audio_next();
      break;
    case CMD_PREV:
      audio_prev();
      break;
    case CMD_PING:
      if (!send_int(cli->socket, EV_PONG))
      {
        err = 1;
      }
      break;
    case CMD_GET_OPTION:
      if (!send_option(cli))
      {
        err = 1;
      }
      break;
    case CMD_SET_OPTION:
      if (!get_set_option(cli))
      {
        err = 1;
      }
      break;
    case CMD_GET_MIXER:
      if (!send_data_int(cli, audio_get_mixer()))
      {
        err = 1;
      }
      break;
    case CMD_SET_MIXER:
      if (!set_mixer(cli))
      {
        err = 1;
      }
      break;
    case CMD_DELETE:
      if (!delete_item(cli))
      {
        err = 1;
      }
      break;
    case CMD_GET_SERIAL:
      if (!send_serial(cli))
      {
        err = 1;
      }
      break;
    case CMD_PLIST_GET_SERIAL:
      if (!req_plist_get_serial(cli))
      {
        err = 1;
      }
      break;
    case CMD_PLIST_SET_SERIAL:
      if (!req_plist_set_serial(cli))
      {
        err = 1;
      }
      break;
    case CMD_TOGGLE_MIXER_CHANNEL:
      req_toggle_mixer_channel();
      break;
    case CMD_TOGGLE_SOFTMIXER:
      req_toggle_softmixer();
      break;
    case CMD_GET_MIXER_CHANNEL_NAME:
      if (!req_get_mixer_channel_name(cli))
      {
        err = 1;
      }
      break;
    case CMD_GET_FILE_TAGS:
      if (!get_file_tags(client_id))
      {
        err = 1;
      }
      break;
    case CMD_ABORT_TAGS_REQUESTS:
      if (!abort_tags_requests(client_id))
      {
        err = 1;
      }
      break;
    case CMD_LIST_MOVE:
      if (!req_list_move(cli))
      {
        err = 1;
      }
      break;
    case CMD_TOGGLE_EQUALIZER:
      req_toggle_equalizer();
      break;
    case CMD_EQUALIZER_REFRESH:
      req_equalizer_refresh();
      break;
    case CMD_EQUALIZER_PREV:
      req_equalizer_prev();
      break;
    case CMD_EQUALIZER_NEXT:
      req_equalizer_next();
      break;
    case CMD_TOGGLE_MAKE_MONO:
      req_toggle_make_mono();
      break;
    case CMD_QUEUE_ADD:
      if (!req_queue_add(cli))
      {
        err = 1;
      }
      break;
    case CMD_QUEUE_DEL:
      if (!req_queue_del(cli))
      {
        err = 1;
      }
      break;
    case CMD_QUEUE_CLEAR:
      logit("Clearing the queue");
      audio_queue_clear();
      add_event_all(EV_QUEUE_CLEAR, NULL);
      break;
    case CMD_GET_QUEUE:
      if (!req_send_queue(cli))
      {
        err = 1;
      }
      break;
    default:
      logit("Bad command (0x%x) from the client", cmd);
      err = 1;
  }

  if (err)
  {
    logit("Closing client connection due to error");
    close(cli->socket);
    del_client(cli);
  }
}

/* Add clients file descriptors to fds. */
static void add_clients_fds(fd_set *read, fd_set *write)
{
  int i;

  for (i = 0; i < CLIENTS_MAX; i++)
  {
    if (clients[i].socket != -1)
    {
      FD_SET(clients[i].socket, read);

      LOCK(clients[i].events_mtx);
      if (!event_queue_empty(&clients[i].events))
      {
        FD_SET(clients[i].socket, write);
      }
      UNLOCK(clients[i].events_mtx);
    }
  }
}

/* Return the maximum fd from clients and the argument. */
static int max_fd(int max)
{
  int i;

  if (wake_up_pipe[0] > max)
  {
    max = wake_up_pipe[0];
  }

  for (i = 0; i < CLIENTS_MAX; i++)
  {
    if (clients[i].socket > max)
    {
      max = clients[i].socket;
    }
  }
  return max;
}

/* Handle clients whose fds are ready to read. */
static void handle_clients(fd_set *fds)
{
  int i;

  for (i = 0; i < CLIENTS_MAX; i++)
  {
    if (clients[i].socket != -1 && FD_ISSET(clients[i].socket, fds))
    {
      handle_command(i);
    }
  }
}

/* Close all client connections sending EV_EXIT. */
static void close_clients()
{
  int i;

  for (i = 0; i < CLIENTS_MAX; i++)
  {
    if (clients[i].socket != -1)
    {
      send_int(clients[i].socket, EV_EXIT);
      close(clients[i].socket);
      del_client(&clients[i]);
    }
  }
}

/* Run the audio engine event loop until quit. */
void server_loop()
{
  logit("MOC server started, pid: %d", getpid());

  assert(server_sock != -1);
  add_client(server_sock);
  server_sock = -1;

  log_circular_start();

  do
  {
    int res;
    fd_set fds_write, fds_read;

    FD_ZERO(&fds_read);
    FD_ZERO(&fds_write);
    FD_SET(wake_up_pipe[0], &fds_read);
    add_clients_fds(&fds_read, &fds_write);

    res = 0;
    if (!server_quit)
    {
      res = select(max_fd(-1) + 1, &fds_read, &fds_write, NULL, NULL);
    }

    if (res == -1 && errno != EINTR && !server_quit)
    {
      fatal("select() failed: %s", xstrerror(errno));
    }

    if (!server_quit && res >= 0)
    {
      if (FD_ISSET(wake_up_pipe[0], &fds_read))
      {
        int w;

        logit("Got 'wake up'");

        if (read(wake_up_pipe[0], &w, sizeof(w)) < 0)
        {
          fatal("Can't read wake up signal: %s", xstrerror(errno));
        }
      }

      send_events(&fds_write);
      handle_clients(&fds_read);
    }

    if (server_quit)
    {
      logit("Exiting...");
    }

  } while (!server_quit);

  log_circular_log();
  log_circular_stop();

  close_clients();
  clients_cleanup();
  server_shutdown();
}

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

/* Notify the client about change of the player state. */
void state_change() { add_event_all(EV_STATE, NULL); }

void ctime_change() { add_event_all(EV_CTIME, NULL); }

void tags_change() { add_event_all(EV_TAGS, NULL); }

void status_msg(const char *msg) { add_event_all(EV_STATUS_MSG, msg); }

void tags_response(const char *file, const struct file_tags *tags)
{
  assert(file != NULL);
  assert(tags != NULL);

  if (clients[0].socket != -1)
  {
    struct tag_ev_response *data =
        (struct tag_ev_response *)xmalloc(sizeof(struct tag_ev_response));

    data->file = xstrdup(file);
    data->tags = tags_dup(tags);

    add_event(&clients[0], EV_FILE_TAGS, data);
    wake_up_server();
  }
}

void ev_audio_start() { add_event_all(EV_AUDIO_START, NULL); }

void ev_audio_stop() { add_event_all(EV_AUDIO_STOP, NULL); }

/* Announce to clients that first file from the queue is being played
 * and therefore needs to be removed from it */
/* XXX: this function is called from player thread and add_event_all
 *      imho doesn't properly lock all shared variables -- possible
 *      race condition??? */
void server_queue_pop(const char *filename)
{
  debug("Queue pop -- broadcasting EV_QUEUE_DEL");
  add_event_all(EV_QUEUE_DEL, filename);
}

// EOF
