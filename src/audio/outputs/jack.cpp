// src/audio/outputs/jack.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Jack plugin for moc by Alex Norman <alex@neisis.net> 2005
// moc by Copyright (C) 2004 Damian Pietras <daper@daper.net>
// use at your own risk
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <algorithm>
#include <jack/jack.h>
#include <jack/types.h>
#include <jack/ringbuffer.h>

#define DEBUG

#include "core/common.h"
#include "audio/audio.h"
#include "core/log.h"
#include "core/options.h"
#include "audio/outputs/jack.h"

#define RINGBUF_SZ 32768

class JackOutput : public AudioOutput {
private:
    jack_client_t *client = nullptr;
    jack_port_t **output_port = nullptr;
    jack_ringbuffer_t *ringbuffer[2] = {nullptr, nullptr};
    jack_default_audio_sample_t volume = 1.0;
    int volume_integer = 100;
    bool playing = false;
    int rate = 0;
    volatile int our_xrun = 0;
    volatile int jack_shutdown_flag = 0;

    static int process_cb(jack_nframes_t nframes, void *arg);
    static int update_sample_rate_cb(jack_nframes_t new_rate, void *arg);
    static void error_cb(const char *msg);
    static void shutdown_cb(void *arg);

public:
    int init(struct output_driver_caps *caps) override;
    void shutdown() override;
    int open(struct sound_params *sound_params) override;
    void close() override;
    int play_audio(const char *buff, const size_t size);
    int play(const char *buff, const size_t size) override { return play_audio(buff, size); }
    int read_mixer() override;
    void set_mixer(int vol) override;
    int get_buff_fill() override;
    int reset() override;
    int get_rate() override;
    void toggle_mixer_channel() override;
    std::string get_mixer_channel_name() override;
};

int JackOutput::process_cb(jack_nframes_t nframes, void *arg)
{
  JackOutput *self = static_cast<JackOutput *>(arg);
  jack_default_audio_sample_t *out[2];

  if (nframes <= 0)
    return 0;

  out[0] = static_cast<jack_default_audio_sample_t *>(jack_port_get_buffer(self->output_port[0], nframes));
  out[1] = static_cast<jack_default_audio_sample_t *>(jack_port_get_buffer(self->output_port[1], nframes));

  if (self->playing)
  {
    size_t i;
    size_t avail_data = jack_ringbuffer_read_space(self->ringbuffer[1]);
    size_t avail_frames = avail_data / sizeof(jack_default_audio_sample_t);

    if (avail_frames > nframes)
    {
      avail_frames = nframes;
      avail_data = nframes * sizeof(jack_default_audio_sample_t);
    }

    jack_ringbuffer_read(self->ringbuffer[0], reinterpret_cast<char *>(out[0]), avail_data);
    jack_ringbuffer_read(self->ringbuffer[1], reinterpret_cast<char *>(out[1]), avail_data);

    if (avail_frames < nframes)
    {
      self->our_xrun = 1;
      for (i = avail_frames; i < nframes; i++)
      {
        out[0][i] = out[1][i] = 0.0;
      }
    }
  }
  else
  {
    size_t i;
    size_t size;
    size = jack_ringbuffer_read_space(self->ringbuffer[1]);
    jack_ringbuffer_read_advance(self->ringbuffer[0], size);
    jack_ringbuffer_read_advance(self->ringbuffer[1], size);

    for (i = 0; i < nframes; i++)
    {
      out[0][i] = 0.0;
      out[1][i] = 0.0;
    }
  }

  return 0;
}

int JackOutput::update_sample_rate_cb(jack_nframes_t new_rate, void *arg)
{
  JackOutput *self = static_cast<JackOutput *>(arg);
  self->rate = new_rate;
  return 0;
}

void JackOutput::error_cb(const char *msg) { error("JACK: %s", msg); }

void JackOutput::shutdown_cb(void *arg) {
  JackOutput *self = static_cast<JackOutput *>(arg);
  self->jack_shutdown_flag = 1;
}

int JackOutput::init(struct output_driver_caps *caps)
{
  const char *client_name = options_get_str("JackClientName");
  jack_set_error_function(error_cb);

#ifdef HAVE_JACK_CLIENT_OPEN
  jack_status_t status;
  jack_options_t options = JackNullOption;
  if (!options_get_bool("JackStartServer"))
    options = static_cast<jack_options_t>(options | JackNoStartServer);
  client = jack_client_open(client_name, options, &status, nullptr);
  if (client == nullptr)
  {
    error("jack_client_open() failed, status = 0x%2.0x", status);
    if (status & JackServerFailed)
      error("Unable to connect to JACK server");
    return 0;
  }
  if (status & JackServerStarted)
    printf("JACK server started\n");
#else
  client = jack_client_new(client_name);
  if (client == nullptr)
  {
    error("Cannot create client; JACK server not running?");
    return 0;
  }
#endif

  jack_shutdown_flag = 0;
  jack_on_shutdown(client, shutdown_cb, this);

  output_port = new jack_port_t *[2];
  output_port[0] = jack_port_register(client, "output0", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  output_port[1] = jack_port_register(client, "output1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

  ringbuffer[0] = jack_ringbuffer_create(RINGBUF_SZ);
  ringbuffer[1] = jack_ringbuffer_create(RINGBUF_SZ);

  jack_set_process_callback(client, process_cb, this);
  jack_set_sample_rate_callback(client, update_sample_rate_cb, this);
  if (jack_activate(client))
  {
    error("cannot activate client");
    return 0;
  }

  if (strcmp(options_get_str("JackOutLeft"), "nullptr"))
  {
    if (jack_connect(client, "mocf:output0", options_get_str("JackOutLeft")))
      fprintf(stderr, "%s is not a valid Jack Client / Port", options_get_str("JackOutLeft"));
  }
  if (strcmp(options_get_str("JackOutRight"), "nullptr"))
  {
    if (jack_connect(client, "mocf:output1", options_get_str("JackOutRight")))
      fprintf(stderr, "%s is not a valid Jack Client / Port", options_get_str("JackOutRight"));
  }

  caps->formats = SFMT_FLOAT;
  rate = jack_get_sample_rate(client);
  caps->max_channels = caps->min_channels = 2;
  caps->max_rate = caps->min_rate = rate;

  logit("jack init");
  return 1;
}

int JackOutput::open(struct sound_params *sound_params)
{
  if (sound_params->fmt != SFMT_FLOAT)
  {
    char fmt_name[SFMT_STR_MAX];
    error("Unsupported sound format: %s.", sfmt_str(sound_params->fmt, fmt_name, sizeof(fmt_name)));
    return 0;
  }
  if (sound_params->channels != 2)
  {
    error("Unsupported number of channels");
    return 0;
  }

  logit("jack open");
  playing = true;
  return 1;
}

void JackOutput::close()
{
  logit("jack close");
  playing = false;
}

int JackOutput::play_audio(const char *buff, const size_t size)
{
  size_t remain = size;
  size_t pos = 0;

  if (jack_shutdown_flag)
  {
    logit("Refusing to play, because there is no client thread.");
    return -1;
  }

  debug("Playing %zu bytes", size);

  if (our_xrun)
  {
    logit("xrun");
    our_xrun = 0;
  }

  while (remain && !jack_shutdown_flag)
  {
    size_t space;
    if ((space = jack_ringbuffer_write_space(ringbuffer[1])) > sizeof(jack_default_audio_sample_t))
    {
      size_t to_write;
      space *= 2;
      debug("Space in the ringbuffer: %zu bytes", space);
      to_write = std::min(space, remain);
      to_write /= sizeof(jack_default_audio_sample_t) * 2;
      remain -= to_write * sizeof(float) * 2;
      while (to_write--)
      {
        jack_default_audio_sample_t sample;
        sample = *reinterpret_cast<const jack_default_audio_sample_t *>(buff + pos) * volume;
        pos += sizeof(jack_default_audio_sample_t);
        jack_ringbuffer_write(ringbuffer[0], reinterpret_cast<char *>(&sample), sizeof(sample));

        sample = *reinterpret_cast<const jack_default_audio_sample_t *>(buff + pos) * volume;
        pos += sizeof(jack_default_audio_sample_t);
        jack_ringbuffer_write(ringbuffer[1], reinterpret_cast<char *>(&sample), sizeof(sample));
      }
    }
    else
    {
      debug("Sleeping for %uus", (unsigned int)(RINGBUF_SZ / (float)(audio_get_bps()) * 100000.0));
      xsleep(RINGBUF_SZ, audio_get_bps());
    }
  }

  if (jack_shutdown_flag)
    return -1;

  return size;
}

int JackOutput::read_mixer() { return volume_integer; }

void JackOutput::set_mixer(int vol)
{
  volume_integer = vol;
  volume = static_cast<jack_default_audio_sample_t>(expm1(static_cast<double>(vol) / 100.0) / (M_E - 1));
}

int JackOutput::get_buff_fill()
{
  return sizeof(float) * (jack_ringbuffer_read_space(ringbuffer[0]) + jack_ringbuffer_read_space(ringbuffer[1])) / sizeof(jack_default_audio_sample_t);
}

int JackOutput::reset()
{
  return 1;
}

void JackOutput::shutdown()
{
  jack_port_unregister(client, output_port[0]);
  jack_port_unregister(client, output_port[1]);
  delete[] output_port;
  jack_client_close(client);
  jack_ringbuffer_free(ringbuffer[0]);
  jack_ringbuffer_free(ringbuffer[1]);
}

int JackOutput::get_rate() { return rate; }

std::string JackOutput::get_mixer_channel_name() { return "soft mixer"; }

void JackOutput::toggle_mixer_channel() {}

std::unique_ptr<AudioOutput> create_jack_output() {
    return std::make_unique<JackOutput>();
}

// EOF
