// src/audio/outputs/pulse.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// PulseAudio backend.
// FEATURES:
// Does not autostart a PulseAudio server, but uses an already-started
// one, which should be better than alsa-through-pulse.
// Supports control of either our stream's or our entire sink's volume
// while we are actually playing. Volume control while paused is
// intentionally unsupported: the PulseAudio documentation strongly
// suggests not passing in an initial volume when creating a stream
// (allowing the server to track this instead), and we do not know
// which sink to control if we do not have a stream open.
// IMPLEMENTATION:
// Most client-side (resource allocation) errors are fatal. Failure to
// create a server context or stream is not fatal (and MOC should cope
// with these failures too), but server communication failures later
// on are currently not handled (MOC has no great way for us to tell
// it we no longer work, and I am not sure if attempting to reconnect
// is worth it or even a good idea).
// The pulse "simple" API is too simple: it combines connecting to the
// server and opening a stream into one operation, while I want to
// connect to the server when MOC starts (and fall back to a different
// backend if there is no server), and I cannot open a stream at that
// time since I do not know the audio format yet.
// PulseAudio strongly recommends we use a high-latency connection,
// which the MOC frontend code might not expect from its audio
// backend. We'll see.
// We map MOC's percentage volumes linearly to pulse's PA_VOLUME_MUTED
// (0) .. PA_VOLUME_NORM range. This is what the PulseAudio docs recommend
// ( http://pulseaudio.org/wiki/WritingVolumeControlUIs ). It does mean
// PulseAudio volumes above PA_VOLUME_NORM do not work well with MOC.
// Comments in audio.h claim "All functions are executed only by one
// thread" (referring to the function in the hw_funcs struct). This is
// a blatant lie. Most of them are invoked off the "output buffer"
// thread (out_buf.c) but at least the "playing" thread (audio.c)
// calls audio_close which calls our close function. We can mostly
// ignore this problem because we serialize on the pulseaudio threaded
// mainloop lock. But it does mean that functions that are normally
// only called between open and close (like reset) are sometimes
// called without us having a stream. Bulletproof, therefore:
// serialize setting/unsetting our global stream using the threaded
// mainloop lock, and check for that stream being non-null before
// using it.
// I am not convinced there are no further dragons lurking here: can
// the "playing" thread(s) close and reopen our output stream while
// the "output buffer" thread is sending output there? We can bail if
// our stream is simply closed, but we do not currently detect it
// being reopened and no longer using the same sample format, which
// might have interesting results...
// Also, read_mixer is called from the main server thread (handling
// commands). This crashed me once when it got at a stream that was in
// the "creating" state and therefore did not have a valid stream
// index yet. Fixed by only assigning to the stream global when the
// stream is valid.
// Copyright (C) 2011 Marien Zwart <marienz@marienz.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include <pulse/proplist.h>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define DEBUG

#include <cmath>
#include <algorithm>
#include <pulse/pulseaudio.h>
#include "core/common.h"
#include "core/log.h"
#include "audio/audio.h"
#include "audio/outputs/pulse.h"

#define PULSE_VOLUME_SAVE_FILE "pulse_volume"

/* Upper bound on how much audio may sit ahead of the listener, matching
 * alsa.cpp. mocf hands the driver at most AUDIO_MAX_PLAY (100ms) per
 * write regardless, so bounding this costs no extra wakeups. */
#define BUFFER_MAX_USEC 300000

class PulseOutput : public AudioOutput {
private:
    pa_threaded_mainloop *mainloop = nullptr;
    pa_context *context = nullptr;
    uint32_t pa_default_sink_index = 0;
    pa_stream *stream = nullptr;
    int showing_sink_volume = 0;
    int stream_volume = 100;

    void pulse_load_volume();
    void pulse_save_volume();

    static void context_state_callback(pa_context *context, void *userdata);
    static void stream_state_callback(pa_stream *stream, void *userdata);
    static void stream_write_callback(pa_stream *stream, size_t nbytes, void *userdata);
    static void volume_cb(const pa_cvolume *v, void *userdata);
    static void sink_volume_cb(pa_context *c, const pa_sink_info *i, int eol, void *userdata);
    static void sink_input_volume_cb(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata);
    static void flush_callback(pa_stream *s, int success, void *userdata);
    static void sink_name_cb(pa_context *c, const pa_sink_info *i, int eol, void *userdata);
    static void cork_callback(pa_stream *s, int success, void *userdata);

    struct VolumeCbData {
        PulseOutput *self;
        int *result;
    };

    struct StringCbData {
        PulseOutput *self;
        std::string *result;
    };

    struct IntCbData {
        PulseOutput *self;
        int *result;
    };

public:
    int init(struct output_driver_caps *caps) override;
    void shutdown() override;
    int open(struct sound_params *sound_params) override;
    void close() override;
    int play(const char *buff, const size_t size) override;
    int read_mixer() override;
    void set_mixer(int vol) override;
    int get_buff_fill() override;
    int reset() override;
    int get_rate() override;
    void toggle_mixer_channel() override;
    std::string get_mixer_channel_name() override;
    void hw_pause() override;
    void hw_unpause() override;
    bool can_hw_pause() const override { return true; }
};

void PulseOutput::pulse_load_volume()
{
  std::string cfname = create_file_name(PULSE_VOLUME_SAVE_FILE);
  FILE *cf = fopen(cfname.c_str(), "r");

  if (!cf) return;

  int vol;
  if (fscanf(cf, "%d", &vol) == 1 && vol >= 0 && vol <= 100)
  {
    stream_volume = vol;
    logit("Pulse: loaded saved stream volume: %d%%", stream_volume);
  }

  fclose(cf);
}

void PulseOutput::pulse_save_volume()
{
  std::string cfname = create_file_name(PULSE_VOLUME_SAVE_FILE);
  FILE *cf = fopen(cfname.c_str(), "w");

  if (!cf)
  {
    logit("Pulse: unable to save stream volume");
    return;
  }

  fprintf(cf, "%d\n", stream_volume);
  fclose(cf);
  logit("Pulse: saved stream volume: %d%%", stream_volume);
}

void PulseOutput::context_state_callback(pa_context *context ATTR_UNUSED, void *userdata)
{
  pa_threaded_mainloop *m = static_cast<pa_threaded_mainloop *>(userdata);
  pa_threaded_mainloop_signal(m, 0);
}

void PulseOutput::stream_state_callback(pa_stream *stream ATTR_UNUSED, void *userdata)
{
  pa_threaded_mainloop *m = static_cast<pa_threaded_mainloop *>(userdata);
  pa_threaded_mainloop_signal(m, 0);
}

void PulseOutput::stream_write_callback(pa_stream *stream ATTR_UNUSED, size_t nbytes ATTR_UNUSED, void *userdata)
{
  pa_threaded_mainloop *m = static_cast<pa_threaded_mainloop *>(userdata);
  pa_threaded_mainloop_signal(m, 0);
}

int PulseOutput::init(struct output_driver_caps *caps)
{
  pa_context *c;
  pa_proplist *proplist;

  assert(!mainloop);
  assert(!context);

  mainloop = pa_threaded_mainloop_new();
  if (!mainloop) fatal("Cannot create PulseAudio mainloop");
  if (pa_threaded_mainloop_start(mainloop) < 0) fatal("Cannot start PulseAudio mainloop");

  proplist = pa_proplist_new();
  if (!proplist) fatal("Cannot allocate PulseAudio proplist");

  pa_proplist_sets(proplist, PA_PROP_APPLICATION_VERSION, PACKAGE_VERSION);
  pa_proplist_sets(proplist, PA_PROP_MEDIA_ROLE, "music");
  pa_proplist_sets(proplist, PA_PROP_APPLICATION_ID, "net.daper.mocf");

  pa_threaded_mainloop_lock(mainloop);

  c = pa_context_new_with_proplist(pa_threaded_mainloop_get_api(mainloop), PACKAGE_NAME, proplist);
  pa_proplist_free(proplist);

  if (!c) fatal("Cannot allocate PulseAudio context");

  pa_context_set_state_callback(c, context_state_callback, mainloop);

  pa_context_connect(c, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr);

  while (true)
  {
    pa_context_state_t state = pa_context_get_state(c);
    if (state == PA_CONTEXT_READY) break;
    if (!PA_CONTEXT_IS_GOOD(state))
    {
      error("PulseAudio connection failed: %s", pa_strerror(pa_context_errno(c)));
      goto unlock_and_fail;
    }
    debug("waiting for context to become ready...");
    pa_threaded_mainloop_wait(mainloop);
  }

  context = c;
  pa_threaded_mainloop_unlock(mainloop);

  caps->min_channels = 1;
  caps->max_channels = 6;
  caps->min_rate = AUDIO_RATE_MIN;
  caps->max_rate = AUDIO_RATE_MAX;
  caps->formats = (SFMT_S8 | SFMT_S16 | SFMT_S32 | SFMT_FLOAT | SFMT_NE);

  pulse_load_volume();

  return 1;

unlock_and_fail:
  pa_context_unref(c);
  pa_threaded_mainloop_unlock(mainloop);
  pa_threaded_mainloop_stop(mainloop);
  pa_threaded_mainloop_free(mainloop);
  mainloop = nullptr;
  return 0;
}

void PulseOutput::shutdown()
{
  pulse_save_volume();

  pa_threaded_mainloop_lock(mainloop);

  pa_context_disconnect(context);
  pa_context_unref(context);
  context = nullptr;

  pa_threaded_mainloop_unlock(mainloop);

  pa_threaded_mainloop_stop(mainloop);
  pa_threaded_mainloop_free(mainloop);
  mainloop = nullptr;
}

int PulseOutput::open(struct sound_params *sound_params)
{
  pa_sample_spec ss;
  pa_buffer_attr ba;
  pa_stream *s;

  assert(!stream);
  ba.fragsize = static_cast<uint32_t>(-1);
  ba.prebuf = static_cast<uint32_t>(-1);
  ba.minreq = static_cast<uint32_t>(-1);
  ba.maxlength = static_cast<uint32_t>(-1);

  ss.channels = sound_params->channels;
  ss.rate = sound_params->rate;
  switch (sound_params->fmt)
  {
    case SFMT_U8: ss.format = PA_SAMPLE_U8; break;
    case SFMT_S16 | SFMT_LE: ss.format = PA_SAMPLE_S16LE; break;
    case SFMT_S16 | SFMT_BE: ss.format = PA_SAMPLE_S16BE; break;
    case SFMT_FLOAT:
    case SFMT_FLOAT | SFMT_LE: ss.format = PA_SAMPLE_FLOAT32LE; break;
    case SFMT_FLOAT | SFMT_BE: ss.format = PA_SAMPLE_FLOAT32BE; break;
    case SFMT_S32 | SFMT_LE: ss.format = PA_SAMPLE_S32LE; break;
    case SFMT_S32 | SFMT_BE: ss.format = PA_SAMPLE_S32BE; break;
    default: fatal("pulse: got unrequested format");
  }

  /* With PA_STREAM_ADJUST_LATENCY the target buffer length is the total
   * latency the server aims for. Left unset, the server default runs to
   * seconds; bound it as ALSA does so the reported position tracks what
   * is actually being heard. */
  ba.tlength = pa_usec_to_bytes(BUFFER_MAX_USEC, &ss);

  debug("opening stream");

  pa_threaded_mainloop_lock(mainloop);

  s = pa_stream_new(context, "music", &ss, nullptr);
  if (!s) fatal("pulse: stream allocation failed");

  pa_stream_set_state_callback(s, stream_state_callback, mainloop);
  pa_stream_set_write_callback(s, stream_write_callback, mainloop);

  pa_stream_connect_playback(s, nullptr, &ba,
                             static_cast<pa_stream_flags_t>(
                                 PA_STREAM_INTERPOLATE_TIMING |
                                 PA_STREAM_AUTO_TIMING_UPDATE |
                                 PA_STREAM_ADJUST_LATENCY),
                             nullptr, nullptr);

  while (true)
  {
    pa_stream_state_t state = pa_stream_get_state(s);
    if (state == PA_STREAM_READY) break;
    if (!PA_STREAM_IS_GOOD(state))
    {
      error("PulseAudio stream connection failed");
      goto fail;
    }
    debug("waiting for stream to become ready...");
    pa_threaded_mainloop_wait(mainloop);
  }

  stream = s;

  {
    pa_cvolume v;
    pa_operation *op;
    pa_cvolume_set(&v, 1, stream_volume * PA_VOLUME_NORM / 100);
    op = pa_context_set_sink_input_volume(context, pa_stream_get_index(stream),
                                          &v, nullptr, nullptr);
    pa_operation_unref(op);
    logit("Pulse: applied stream_volume %d%% to new stream", stream_volume);
  }

  pa_threaded_mainloop_unlock(mainloop);

  return 1;

fail:
  pa_stream_unref(s);
  pa_threaded_mainloop_unlock(mainloop);
  return 0;
}

void PulseOutput::close()
{
  pa_operation *op;
  int result = 0;

  debug("closing stream");

  pa_threaded_mainloop_lock(mainloop);
  
  IntCbData data = {this, &result};
  op = pa_stream_drain(stream, flush_callback, &data);
  while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
  {
    pa_threaded_mainloop_wait(mainloop);
  }
  pa_operation_unref(op);

  pa_stream_disconnect(stream);
  pa_stream_unref(stream);
  stream = nullptr;

  pa_threaded_mainloop_unlock(mainloop);
}

int PulseOutput::play(const char *buff, const size_t size)
{
  size_t offset = 0;

  debug("Got %d bytes to play", (int)size);

  pa_threaded_mainloop_lock(mainloop);

  while (stream)
  {
    size_t towrite = std::min(pa_stream_writable_size(stream), size - offset);
    debug("writing %d bytes", (int)towrite);

    if (pa_stream_write(stream, buff + offset, towrite, nullptr, 0,
                        PA_SEEK_RELATIVE))
    {
      error("pa_stream_write failed");
    }

    offset += towrite;

    if (offset >= size)
    {
      break;
    }

    pa_threaded_mainloop_wait(mainloop);
  }

  pa_threaded_mainloop_unlock(mainloop);

  debug("Done playing!");

  return size;
}

void PulseOutput::volume_cb(const pa_cvolume *v, void *userdata)
{
  VolumeCbData *data = static_cast<VolumeCbData *>(userdata);

  if (v)
  {
    *data->result = ceil(100.0 * pa_cvolume_avg(v) / PA_VOLUME_NORM);
  }

  pa_threaded_mainloop_signal(data->self->mainloop, 0);
}

void PulseOutput::sink_volume_cb(pa_context *c ATTR_UNUSED, const pa_sink_info *i,
                                 int eol ATTR_UNUSED, void *userdata)
{
  volume_cb(i ? &i->volume : nullptr, userdata);
}

void PulseOutput::sink_input_volume_cb(pa_context *c ATTR_UNUSED,
                                       const pa_sink_input_info *i,
                                       int eol ATTR_UNUSED,
                                       void *userdata)
{
  volume_cb(i ? &i->volume : nullptr, userdata);
}

int PulseOutput::read_mixer()
{
  pa_operation *op;
  int result = 0;

  debug("read mixer");

  pa_threaded_mainloop_lock(mainloop);

  VolumeCbData data = {this, &result};

  if (showing_sink_volume)
  {
    op = pa_context_get_sink_info_by_index(
        context,
        stream ? pa_stream_get_device_index(stream) : pa_default_sink_index,
        sink_volume_cb, &data);
  }
  else if (stream)
  {
    op = pa_context_get_sink_input_info(context, pa_stream_get_index(stream),
                                        sink_input_volume_cb, &data);
  }
  else
  {
    result = stream_volume;
  }

  if (showing_sink_volume || stream)
  {
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
    {
      pa_threaded_mainloop_wait(mainloop);
    }

    pa_operation_unref(op);
  }

  pa_threaded_mainloop_unlock(mainloop);

  return result;
}

void PulseOutput::set_mixer(int vol)
{
  pa_cvolume v;
  pa_operation *op;

  pa_cvolume_set(&v, 1, vol * PA_VOLUME_NORM / 100);

  pa_threaded_mainloop_lock(mainloop);

  if (showing_sink_volume)
  {
    op = pa_context_set_sink_volume_by_index(
        context,
        stream ? pa_stream_get_device_index(stream) : pa_default_sink_index, &v,
        nullptr, nullptr);
  }
  else
  {
    stream_volume = vol;

    if (stream)
    {
      op = pa_context_set_sink_input_volume(context, pa_stream_get_index(stream),
                                            &v, nullptr, nullptr);
      pa_operation_unref(op);
    }
  }

  pa_threaded_mainloop_unlock(mainloop);
}

int PulseOutput::get_buff_fill()
{
  pa_usec_t buffered_usecs = 0;
  int buffered_bytes = 0;
  int negative = 0;

  pa_threaded_mainloop_lock(mainloop);

  if (stream &&
      pa_stream_get_latency(stream, &buffered_usecs, &negative) >= 0)
  {
    /* A negative latency means the stream is running behind; nothing of
     * ours is left queued, so report an empty buffer rather than the
     * magnitude of the shortfall. */
    if (negative)
    {
      buffered_usecs = 0;
    }

    buffered_bytes =
        pa_usec_to_bytes(buffered_usecs, pa_stream_get_sample_spec(stream));
  }

  pa_threaded_mainloop_unlock(mainloop);

  debug("buffer fill: %d usec / %d bytes", (int)buffered_usecs,
        (int)buffered_bytes);

  return buffered_bytes;
}

void PulseOutput::flush_callback(pa_stream *s ATTR_UNUSED, int success,
                                 void *userdata)
{
  IntCbData *data = static_cast<IntCbData *>(userdata);

  *data->result = success;

  pa_threaded_mainloop_signal(data->self->mainloop, 0);
}

int PulseOutput::reset()
{
  pa_operation *op;
  int result = 0;

  debug("reset requested");

  pa_threaded_mainloop_lock(mainloop);

  if (stream)
  {
    IntCbData data = {this, &result};
    op = pa_stream_flush(stream, flush_callback, &data);

    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
    {
      pa_threaded_mainloop_wait(mainloop);
    }

    pa_operation_unref(op);
  }
  else
  {
    logit("pulse_reset() called without a stream");
  }

  pa_threaded_mainloop_unlock(mainloop);

  return result;
}

int PulseOutput::get_rate()
{
  int result;

  pa_threaded_mainloop_lock(mainloop);

  if (stream)
  {
    result = pa_stream_get_sample_spec(stream)->rate;
  }
  else
  {
    error("get_rate called without a stream");
    result = 0;
  }

  pa_threaded_mainloop_unlock(mainloop);

  return result;
}

void PulseOutput::toggle_mixer_channel()
{
  showing_sink_volume = !showing_sink_volume;
}

void PulseOutput::sink_name_cb(pa_context *c ATTR_UNUSED, const pa_sink_info *i,
                               int eol ATTR_UNUSED, void *userdata)
{
  StringCbData *data = static_cast<StringCbData *>(userdata);

  if (i && data->result->empty())
  {
    const char *desc = pa_proplist_gets(i->proplist, PA_PROP_DEVICE_DESCRIPTION);
    if (desc) *data->result = desc;
  }

  pa_threaded_mainloop_signal(data->self->mainloop, 0);
}

std::string PulseOutput::get_mixer_channel_name()
{
  std::string result;

  pa_threaded_mainloop_lock(mainloop);

  if (showing_sink_volume)
  {
    pa_operation *op;
    StringCbData data = {this, &result};
    op = pa_context_get_sink_info_by_index(
        context,
        stream ? pa_stream_get_device_index(stream) : pa_default_sink_index,
        sink_name_cb, &data);

    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
    {
      pa_threaded_mainloop_wait(mainloop);
    }

    pa_operation_unref(op);
  }
  else
  {
    result = "PulseStream";
  }

  pa_threaded_mainloop_unlock(mainloop);

  if (result.empty())
  {
    result = "disconnected";
  }

  return result;
}

void PulseOutput::cork_callback(pa_stream *s ATTR_UNUSED, int success,
                                void *userdata)
{
  IntCbData *data = static_cast<IntCbData *>(userdata);

  *data->result = success;

  pa_threaded_mainloop_signal(data->self->mainloop, 0);
}

void PulseOutput::hw_pause()
{
  pa_operation *op;
  int result = 0;

  debug("corking stream for pause");

  pa_threaded_mainloop_lock(mainloop);

  if (stream)
  {
    IntCbData data = {this, &result};
    op = pa_stream_cork(stream, 1, cork_callback, &data);

    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
    {
      pa_threaded_mainloop_wait(mainloop);
    }

    pa_operation_unref(op);
  }
  else
  {
    logit("pulse_hw_pause() called without a stream");
  }

  pa_threaded_mainloop_unlock(mainloop);
}

void PulseOutput::hw_unpause()
{
  pa_operation *op;
  int result = 0;

  debug("uncorking stream for unpause");

  pa_threaded_mainloop_lock(mainloop);

  if (stream)
  {
    IntCbData data = {this, &result};
    op = pa_stream_cork(stream, 0, cork_callback, &data);

    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
    {
      pa_threaded_mainloop_wait(mainloop);
    }

    pa_operation_unref(op);
  }
  else
  {
    logit("pulse_hw_unpause() called without a stream");
  }

  pa_threaded_mainloop_unlock(mainloop);
}

std::unique_ptr<AudioOutput> create_pulse_output() {
    return std::make_unique<PulseOutput>();
}

// EOF
