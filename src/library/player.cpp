// src/library/player.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2004-2005 Damian Pietras <daper@daper.net>
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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <pthread.h>

#define DEBUG

#include "core/common.h"
#include "core/log.h"
#include "audio/decoder.h"
#include "audio/audio.h"
#include "audio/outputs/out_buf.h"
#include "core/server.h"
#include "core/options.h"
#include "library/player.h"
#include "library/files.h"
#include "library/playlist.h"
#include "utils/md5.h"

#define PCM_BUF_SIZE (36 * 1024)
#define PREBUFFER_THRESHOLD (18 * 1024)

enum request
{
  REQ_NOTHING,
  REQ_SEEK,
  REQ_STOP,
  REQ_PAUSE,
  REQ_UNPAUSE
};

struct bitrate_list_node
{
  struct bitrate_list_node *next;
  int time;
  int bitrate;
};

/* List of points where bitrate has changed. We use it to show bitrate at the
 * right time when playing, because the output buffer may be big and decoding
 * may be many seconds ahead of what the user can hear. */
struct bitrate_list
{
  struct bitrate_list_node *head;
  struct bitrate_list_node *tail;
  pthread_mutex_t mtx;
};

struct md5_data
{
  bool okay;
  long len;
  struct md5_ctx ctx;
};

struct precache
{
  char *file;                 /* the file to precache */
  char buf[2 * PCM_BUF_SIZE]; /* PCM buffer with precached data */
  int buf_fill;
  int ok;                           /* 1 if precache succeed */
  struct sound_params sound_params; /* of the sound in the buffer */
  AudioPlugin *f;                /* decoder functions for precached file */
  std::unique_ptr<AudioDecoder> decoder_data;
  int running;   /* if the precache thread is running */
  pthread_t tid; /* tid of the precache thread */
  struct bitrate_list bitrate_list;
  int decoded_time; /* how much sound we decoded in seconds */
};

struct precache precache;

/* Request conditional and mutex. */
static pthread_cond_t request_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t request_cond_mtx = PTHREAD_MUTEX_INITIALIZER;

static enum request request = REQ_NOTHING;
static int req_seek;

/* Source of the played stream tags. */
static enum {
  TAGS_SOURCE_DECODER /* tags from the decoder (e.g., id3tags, vorbis comments)
                       */
} tags_source;

/* Tags of the currently played file. */
static struct file_tags *curr_tags = NULL;

/* Mutex for curr_tags and tags_source. */
static pthread_mutex_t curr_tags_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Stream associated with the currently playing decoder. */
static struct io_stream *decoder_stream = NULL;
static pthread_mutex_t decoder_stream_mtx = PTHREAD_MUTEX_INITIALIZER;

static struct bitrate_list bitrate_list;

static void bitrate_list_init(struct bitrate_list *b)
{
  assert(b != NULL);

  b->head = NULL;
  b->tail = NULL;
  pthread_mutex_init(&b->mtx, NULL);
}

static void bitrate_list_empty(struct bitrate_list *b)
{
  assert(b != NULL);

  LOCK(b->mtx);
  if (b->head)
  {
    while (b->head)
    {
      struct bitrate_list_node *t = b->head->next;

      delete b->head;
      b->head = t;
    }

    b->tail = NULL;
  }

  debug("Bitrate list elements removed.");

  UNLOCK(b->mtx);
}

static void bitrate_list_destroy(struct bitrate_list *b)
{
  int rc;

  assert(b != NULL);

  bitrate_list_empty(b);

  rc = pthread_mutex_destroy(&b->mtx);
  if (rc != 0)
  {
    log_errno("Can't destroy bitrate list mutex", rc);
  }
}

static void bitrate_list_add(struct bitrate_list *b, const int time,
                             const int bitrate)
{
  assert(b != NULL);

  LOCK(b->mtx);
  if (!b->tail)
  {
    b->head = b->tail = new bitrate_list_node;
    b->tail->next = NULL;
    b->tail->time = time;
    b->tail->bitrate = bitrate;

    debug("Adding bitrate %d at time %d", bitrate, time);
  }
  else if (b->tail->bitrate != bitrate && b->tail->time != time)
  {
    assert(b->tail->time < time);

    b->tail->next = new bitrate_list_node;
    b->tail = b->tail->next;
    b->tail->next = NULL;
    b->tail->time = time;
    b->tail->bitrate = bitrate;

    debug("Appending bitrate %d at time %d", bitrate, time);
  }
  else if (b->tail->bitrate == bitrate)
  {
    debug("Not adding bitrate %d at time %d because the bitrate"
          " hasn't changed",
          bitrate, time);
  }
  else
  {
    debug("Not adding bitrate %d at time %d because it is for"
          " the same time as the last bitrate",
          bitrate, time);
  }
  UNLOCK(b->mtx);
}

static int bitrate_list_get(struct bitrate_list *b, const int time)
{
  int bitrate = -1;

  assert(b != NULL);

  LOCK(b->mtx);
  if (b->head)
  {
    while (b->head->next && b->head->next->time <= time)
    {
      struct bitrate_list_node *o = b->head;

      b->head = o->next;
      debug("Removing old bitrate %d for time %d", o->bitrate, o->time);
      delete o;
    }

    bitrate = b->head->bitrate /*b->head->time + 1000*/;
    debug("Getting bitrate for time %d (%d)", time, bitrate);
  }
  else
  {
    debug("Getting bitrate for time %d (no bitrate information)", time);
    bitrate = -1;
  }
  UNLOCK(b->mtx);

  return bitrate;
}

static void update_time()
{
  static int last_time = 0;
  int ctime = audio_get_time();

  if (ctime >= 0 && ctime != last_time)
  {
    last_time = ctime;
    ctime_change();
    set_info_bitrate(bitrate_list_get(&bitrate_list, ctime));
  }
}

static void *precache_thread(void *data)
{
  struct precache *precache = (struct precache *)data;
  int decoded;
  struct sound_params new_sound_params;
  struct decoder_error err;

  precache->buf_fill = 0;
  precache->sound_params.channels = 0; /* mark that sound_params were not
            yet filled. */
  precache->decoded_time = 0.0;
  precache->f = get_decoder(precache->file);
  assert(precache->f != NULL);

  precache->decoder_data = precache->f->open(precache->file);
  precache->decoder_data->get_error(&err);
  if (err.type != ERROR_OK)
  {
    logit("Failed to open the file for precache: %s", err.err.c_str());
    decoder_error_clear(&err);
    precache->decoder_data.reset();
    return NULL;
  }

  audio_plist_set_time(precache->file,
                       precache->decoder_data->get_duration());

  /* Stop at PCM_BUF_SIZE, because when we decode too much, there is no
   * place where we can put the data that doesn't fit into the buffer. */
  while (precache->buf_fill < PCM_BUF_SIZE)
  {
    decoded = precache->decoder_data->decode(precache->buf + precache->buf_fill,
                                  PCM_BUF_SIZE, &new_sound_params);

    if (!decoded)
    {
      /* EOF so fast? We can't pass this information
       * in precache, so give up. */
      logit("EOF when precaching.");
      precache->decoder_data.reset();
      return NULL;
    }

    precache->decoder_data->get_error(&err);

    if (err.type == ERROR_FATAL)
    {
      logit("Error reading file for precache: %s", err.err.c_str());
      decoder_error_clear(&err);
      precache->decoder_data.reset();
      return NULL;
    }

    if (!precache->sound_params.channels)
    {
      precache->sound_params = new_sound_params;
    }
    else if (!sound_params_eq(precache->sound_params, new_sound_params))
    {
      /* There is no way to store sound with two different
       * parameters in the buffer, give up with
       * precaching. (this should never happen). */
      logit("Sound parameters have changed when precaching.");
      decoder_error_clear(&err);
      precache->decoder_data.reset();
      return NULL;
    }

    bitrate_list_add(&precache->bitrate_list, precache->decoded_time,
                     precache->decoder_data->get_bitrate());

    precache->buf_fill += decoded;
    precache->decoded_time +=
        decoded / (float)(sfmt_Bps(new_sound_params.fmt) *
                          new_sound_params.rate * new_sound_params.channels);

    if (err.type != ERROR_OK)
    {
      decoder_error_clear(&err);
      break; /* Don't lose the error message */
    }
  }

  precache->ok = 1;
  logit("Successfully precached file (%d bytes)", precache->buf_fill);
  return NULL;
}

static void start_precache(struct precache *precache, const char *file)
{
  int rc;

  assert(!precache->running);
  assert(file != NULL);

  precache->file = xstrdup(file);
  bitrate_list_init(&precache->bitrate_list);
  logit("Precaching file %s", file);
  precache->ok = 0;
  rc = pthread_create(&precache->tid, NULL, precache_thread, precache);
  if (rc != 0)
  {
    log_errno("Could not run precache thread", rc);
  }
  else
  {
    precache->running = 1;
  }
}

static void precache_wait(struct precache *precache)
{
  int rc;

  if (precache->running)
  {
    debug("Waiting for precache thread...");
    rc = pthread_join(precache->tid, NULL);
    if (rc != 0)
    {
      fatal("pthread_join() for precache thread failed: %s", xstrerror(rc));
    }
    precache->running = 0;
    debug("done");
  }
  else
  {
    debug("Precache thread is not running");
  }
}

static void precache_reset(struct precache *precache)
{
  assert(!precache->running);

  if (precache->ok)
  {
    precache->decoder_data.reset();
  }

  precache->ok = 0;
  if (precache->file)
  {
    free(precache->file);
    precache->file = NULL;
    bitrate_list_destroy(&precache->bitrate_list);
  }
}

void player_init()
{
  precache.file = NULL;
  precache.running = 0;
  precache.ok = 0;
}

static void show_tags(const struct file_tags *tags DEBUG_ONLY)
{
  debug("TAG[title]: %s", tags->title.empty() ? "N/A" : tags->title.c_str());
  debug("TAG[album]: %s", tags->album.empty() ? "N/A" : tags->album.c_str());
  debug("TAG[artist]: %s", tags->artist.empty() ? "N/A" : tags->artist.c_str());
  debug("TAG[track]: %d", tags->track);
}

/* Update tags if tags from the decoder or the stream are available. */
static void update_tags(AudioDecoder *decoder_data,
                        struct io_stream *s ATTR_UNUSED)
{
  int tags_changed = 0;
  struct file_tags *new_tags;

  new_tags = tags_new();

  LOCK(curr_tags_mtx);
  if (decoder_data->current_tags(new_tags) &&
      !new_tags->title.empty())
  {
    tags_changed = 1;
    tags_copy(curr_tags, new_tags);
    logit("Tags change from the decoder");
    tags_source = TAGS_SOURCE_DECODER;
    show_tags(curr_tags);
  }

  if (tags_changed)
  {
    tags_change();
  }

  tags_free(new_tags);

  UNLOCK(curr_tags_mtx);
}

/* Called when some free space in the output buffer appears. */
static void buf_free_cb()
{
  LOCK(request_cond_mtx);
  pthread_cond_broadcast(&request_cond);
  UNLOCK(request_cond_mtx);

  update_time();
}

/* Decoder loop for already opened and probably running for some time decoder.
 * next_file will be precached at eof. */
static void decode_loop(std::unique_ptr<AudioDecoder> &decoder_data,
                        const char *next_file, struct out_buf *out_buf,
                        struct sound_params *sound_params, struct md5_data *md5,
                        const float already_decoded_sec)
{
  bool eof = false;
  bool stopped = false;
  char buf[PCM_BUF_SIZE];
  int decoded = 0;
  struct sound_params new_sound_params;
  bool sound_params_change = false;
  float decode_time = already_decoded_sec; /* the position of the decoder
                                              (in seconds) */

  out_buf_set_free_callback(out_buf, buf_free_cb);

  LOCK(curr_tags_mtx);
  curr_tags = tags_new();
  UNLOCK(curr_tags_mtx);

  LOCK(decoder_stream_mtx);
  decoder_stream = decoder_data->get_stream();
  UNLOCK(decoder_stream_mtx);

  status_msg("Playing...");

  while (1)
  {
    debug("loop...");

    LOCK(request_cond_mtx);
    if (!eof && !decoded)
    {
      struct decoder_error err;

      UNLOCK(request_cond_mtx);

      decoded = decoder_data->decode(buf, sizeof(buf), &new_sound_params);

      if (decoded)
      {
        decode_time += decoded / (float)(sfmt_Bps(new_sound_params.fmt) *
                                         new_sound_params.rate *
                                         new_sound_params.channels);
      }

      decoder_data->get_error(&err);
      if (err.type != ERROR_OK)
      {
        md5->okay = false;
        if (err.type != ERROR_STREAM || options_get_bool("ShowStreamErrors"))
        {
          error("%s", err.err.c_str());
        }
        decoder_error_clear(&err);
      }

      if (!decoded)
      {
        eof = true;
        logit("EOF from decoder");
      }
      else
      {
        debug("decoded %d bytes", decoded);
        if (!sound_params_eq(new_sound_params, *sound_params))
        {
          sound_params_change = true;
        }

        bitrate_list_add(&bitrate_list, decode_time,
                         decoder_data->get_bitrate());
        update_tags(decoder_data.get(), decoder_stream);
      }
    }

    /* Wait, if there is no space in the buffer to put the decoded
     * data or EOF occurred and there is something in the buffer. */
    else if (decoded > out_buf_get_free(out_buf) ||
             (eof && out_buf_get_fill(out_buf)))
    {
      debug("waiting...");
      if (eof && !precache.file && next_file &&
          file_type(next_file) == F_SOUND && options_get_bool("Precache") &&
          options_get_bool("AutoNext"))
      {
        start_precache(&precache, next_file);
      }
      pthread_cond_wait(&request_cond, &request_cond_mtx);
      UNLOCK(request_cond_mtx);
    }
    else
    {
      UNLOCK(request_cond_mtx);
    }

    /* When clearing request, we must make sure, that another
     * request will not arrive at the moment, so we check if
     * the request has changed. */
    if (request == REQ_STOP)
    {
      logit("stop");
      stopped = true;
      md5->okay = false;
      out_buf_stop(out_buf);

      LOCK(request_cond_mtx);
      if (request == REQ_STOP)
      {
        request = REQ_NOTHING;
      }
      UNLOCK(request_cond_mtx);

      break;
    }
    else if (request == REQ_SEEK)
    {
      int decoder_seek;

      logit("seeking");
      md5->okay = false;
      req_seek = MAX(0, req_seek);
      if ((decoder_seek = decoder_data->seek(req_seek)) == -1)
      {
        logit("error when seeking");
      }
      else
      {
        out_buf_stop(out_buf);
        out_buf_reset(out_buf);
        out_buf_time_set(out_buf, decoder_seek);
        bitrate_list_empty(&bitrate_list);
        decode_time = decoder_seek;
        eof = false;
        decoded = 0;
      }

      LOCK(request_cond_mtx);
      if (request == REQ_SEEK)
      {
        request = REQ_NOTHING;
      }
      UNLOCK(request_cond_mtx);
    }
    else if (!eof && decoded <= out_buf_get_free(out_buf) &&
             !sound_params_change)
    {
      debug("putting into the buffer %d bytes", decoded);
#if !defined(NDEBUG) && defined(DEBUG)
      if (md5->okay)
      {
        md5->len += decoded;
        md5_process_bytes(buf, decoded, &md5->ctx);
      }
#endif
      audio_send_buf(buf, decoded);
      decoded = 0;
    }
    else if (!eof && sound_params_change && out_buf_get_fill(out_buf) == 0)
    {
      logit("Sound parameters have changed.");
      *sound_params = new_sound_params;
      sound_params_change = false;
      set_info_channels(sound_params->channels);
      set_info_rate(sound_params->rate / 1000);
      out_buf_wait(out_buf);
      if (!audio_open(sound_params))
      {
        md5->okay = false;
        break;
      }
    }
    else if (eof && out_buf_get_fill(out_buf) == 0)
    {
      logit("played everything");
      break;
    }
  }

  status_msg("");

  LOCK(decoder_stream_mtx);
  decoder_stream = NULL;
  decoder_data.reset();
  UNLOCK(decoder_stream_mtx);

  bitrate_list_destroy(&bitrate_list);

  LOCK(curr_tags_mtx);
  if (curr_tags)
  {
    tags_free(curr_tags);
    curr_tags = NULL;
  }
  UNLOCK(curr_tags_mtx);

  out_buf_wait(out_buf);

  if (stopped || !options_get_bool("AutoNext"))
  {
    precache_wait(&precache);
    precache_reset(&precache);
  }
}

#if !defined(NDEBUG) && defined(DEBUG)
static void log_md5_sum(const char *file, struct sound_params sound_params,
                        AudioPlugin *f, uint8_t *md5, long md5_len)
{
  unsigned int ix, bps;
  char md5sum[MD5_DIGEST_SIZE * 2 + 1], format;
  const char *fn, *endian;

  for (ix = 0; ix < MD5_DIGEST_SIZE; ix += 1)
  {
    snprintf(&md5sum[ix * 2], 3, "%02x", md5[ix]);
  }
  md5sum[MD5_DIGEST_SIZE * 2] = 0x00;

  switch (sound_params.fmt & SFMT_MASK_FORMAT)
  {
    case SFMT_S8:
    case SFMT_S16:
    case SFMT_S24:
    case SFMT_S32:
      format = 's';
      break;
    case SFMT_U8:
    case SFMT_U16:
    case SFMT_U24:
    case SFMT_U32:
      format = 'u';
      break;
    case SFMT_FLOAT:
      format = 'f';
      break;
    default:
      debug("Unknown sound format: 0x%04lx", sound_params.fmt);
      return;
  }

  bps = sfmt_Bps(sound_params.fmt) * 8;

  endian = "";
  if (format != 'f' && bps != 8)
  {
    if (sound_params.fmt & SFMT_LE)
    {
      endian = "le";
    }
    else if (sound_params.fmt & SFMT_BE)
    {
      endian = "be";
    }
  }

  fn = strrchr(file, '/');
  fn = fn ? fn + 1 : file;
  debug("MD5(%s) = %s %ld %s %c%u%s %d %d", fn, md5sum, md5_len,
        get_decoder_name(f), format, bps, endian, sound_params.channels,
        sound_params.rate);
}
#endif

/* Play a file (disk file) using the given decoder. next_file is precached. */
static void play_file(const char *file, AudioPlugin *f,
                      const char *next_file, struct out_buf *out_buf)
{
  std::unique_ptr<AudioDecoder> decoder_data;
  struct sound_params sound_params = {0, 0, 0};
  float already_decoded_time;
  struct md5_data md5;

#if !defined(NDEBUG) && defined(DEBUG)
  md5.okay = true;
  md5.len = 0;
  md5_init_ctx(&md5.ctx);
#endif

  out_buf_reset(out_buf);

  precache_wait(&precache);

  if (precache.ok && strcmp(precache.file, file))
  {
    logit("The precached file is not the file we want.");
    precache_reset(&precache);
  }

  if (precache.ok && !strcmp(precache.file, file))
  {
    struct decoder_error err;

    logit("Using precached file");

    assert(f == precache.f);

    sound_params = precache.sound_params;
    decoder_data = std::move(precache.decoder_data);

    /* We now own decoder_data; prevent precache_reset() from closing it. */
    precache.ok = 0;

    set_info_channels(sound_params.channels);
    set_info_rate(sound_params.rate / 1000);

    if (!audio_open(&sound_params))
    {
      md5.okay = false;
      precache.decoder_data.reset();
      precache_reset(&precache);
      return;
    }

#if !defined(NDEBUG) && defined(DEBUG)
    md5.len += precache.buf_fill;
    md5_process_bytes(precache.buf, precache.buf_fill, &md5.ctx);
#endif

    audio_send_buf(precache.buf, precache.buf_fill);

    decoder_data->get_error(&err);
    if (err.type != ERROR_OK)
    {
      md5.okay = false;
      if (err.type != ERROR_STREAM || options_get_bool("ShowStreamErrors"))
      {
        error("%s", err.err.c_str());
      }
      decoder_error_clear(&err);
    }

    already_decoded_time = precache.decoded_time;

    {
      int avg = decoder_data->get_avg_bitrate();
      set_info_avg_bitrate(avg != -1 ? avg : 0);
    }

    bitrate_list_init(&bitrate_list);
    bitrate_list.head = precache.bitrate_list.head;
    bitrate_list.tail = precache.bitrate_list.tail;

    /* don't free list elements when resetting precache */
    precache.bitrate_list.head = NULL;
    precache.bitrate_list.tail = NULL;
  }
  else
  {
    struct decoder_error err;

    status_msg("Opening...");
    decoder_data = f->open(file);
    decoder_data->get_error(&err);
    if (err.type != ERROR_OK)
    {
      decoder_data.reset();
      status_msg("");
      engine_error(file, err.err.c_str());
      decoder_error_clear(&err);
      logit("Can't open file, exiting");
      return;
    }

    already_decoded_time = 0.0;
    {
      int avg = decoder_data->get_avg_bitrate();
      set_info_avg_bitrate(avg != -1 ? avg : 0);
    }
    bitrate_list_init(&bitrate_list);
  }

  audio_plist_set_time(file, decoder_data->get_duration());
  audio_state_started_playing();
  precache_reset(&precache);

  decode_loop(decoder_data, next_file, out_buf, &sound_params, &md5,
              already_decoded_time);

#if !defined(NDEBUG) && defined(DEBUG)
  if (md5.okay)
  {
    uint8_t buf[MD5_DIGEST_SIZE];

    md5_finish_ctx(&md5.ctx, buf);
    log_md5_sum(file, sound_params, f, buf, md5.len);
  }
#endif
}

/* Open a file, decode it and put output into the buffer. At the end, start
 * precaching next_file. */
void player(const char *file, const char *next_file, struct out_buf *out_buf)
{
  AudioPlugin *f;

  f = get_decoder(file);
  LOCK(decoder_stream_mtx);
  decoder_stream = NULL;
  UNLOCK(decoder_stream_mtx);

  if (!f)
  {
    error("Can't get decoder for %s", file);
    return;
  }

  ev_audio_start();
  play_file(file, f, next_file, out_buf);
  ev_audio_stop();

  logit("exiting");
}

void player_cleanup()
{
  int rc;

  rc = pthread_mutex_destroy(&request_cond_mtx);
  if (rc != 0)
  {
    log_errno("Can't destroy request mutex", rc);
  }
  rc = pthread_mutex_destroy(&curr_tags_mtx);
  if (rc != 0)
  {
    log_errno("Can't destroy tags mutex", rc);
  }
  rc = pthread_mutex_destroy(&decoder_stream_mtx);
  if (rc != 0)
  {
    log_errno("Can't destroy decoder_stream mutex", rc);
  }
  rc = pthread_cond_destroy(&request_cond);
  if (rc != 0)
  {
    log_errno("Can't destroy request condition", rc);
  }

  precache_wait(&precache);
  precache_reset(&precache);
}

void player_reset() { request = REQ_NOTHING; }

void player_stop()
{
  logit("requesting stop");
  request = REQ_STOP;

  LOCK(decoder_stream_mtx);
  if (decoder_stream)
  {
    logit("decoder_stream present, aborting...");
    io_abort(decoder_stream);
  }
  UNLOCK(decoder_stream_mtx);

  LOCK(request_cond_mtx);
  pthread_cond_signal(&request_cond);
  UNLOCK(request_cond_mtx);
}

void player_seek(const int sec)
{
  int time;

  time = audio_get_time();
  if (time >= 0)
  {
    request = REQ_SEEK;
    req_seek = sec + time;
    LOCK(request_cond_mtx);
    pthread_cond_signal(&request_cond);
    UNLOCK(request_cond_mtx);
  }
}

void player_jump_to(const int sec)
{
  request = REQ_SEEK;
  req_seek = sec;
  LOCK(request_cond_mtx);
  pthread_cond_signal(&request_cond);
  UNLOCK(request_cond_mtx);
}

/* Stop playing, clear the output buffer, but allow to unpause by starting
 * playing the same stream.  This is useful for Internet streams that can't
 * be really paused. */
void player_pause()
{
  request = REQ_PAUSE;
  LOCK(request_cond_mtx);
  pthread_cond_signal(&request_cond);
  UNLOCK(request_cond_mtx);
}

void player_unpause()
{
  request = REQ_UNPAUSE;
  LOCK(request_cond_mtx);
  pthread_cond_signal(&request_cond);
  UNLOCK(request_cond_mtx);
}

// EOF
