// src/audio/outputs/oss.cpp
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
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>

#ifdef HAVE_SYS_SOUNDCARD_H
#include <sys/soundcard.h>
#else
#include <soundcard.h>
#endif

#include "core/common.h"
#include "core/server.h"
#include "audio/audio.h"
#include "core/log.h"
#include "core/options.h"
#include "audio/outputs/oss.h"

#if OSS_VERSION >= 0x40000 || SOUND_VERSION >= 0x40000
#define OSSv4_MIXER
#else
#define OSSv3_MIXER
#endif

static const struct
{
  const char *name;
  const int num;
} mixer_channels[] = {{"pcm", SOUND_MIXER_PCM},
                      {"master", SOUND_MIXER_VOLUME},
                      {"speaker", SOUND_MIXER_SPEAKER}};

#define MIXER_CHANNELS_NUM (std::size(mixer_channels))

class OssOutput : public AudioOutput {
private:
    bool started = false;
    volatile int dsp_fd = -1;
#ifdef OSSv3_MIXER
    int mixer_fd = -1;
    int mixer_channel1 = -1;
    int mixer_channel2 = -1;
    int mixer_channel_current = -1;
#endif
    struct sound_params params = {0, 0, 0};

    int open_dev();
    int set_capabilities(struct output_driver_caps *caps);
    int oss_mixer_name_to_channel(const char *name);
    int set_params();

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
};

int OssOutput::open_dev()
{
  if ((dsp_fd = ::open(options_get_str("OSSDevice"), O_WRONLY)) == -1)
  {
    std::string err = xstrerror(errno);
    error("Can't open %s: %s", options_get_str("OSSDevice"), err.c_str());
    return 0;
  }

  logit("Audio device opened");

  return 1;
}

int OssOutput::set_capabilities(struct output_driver_caps *caps)
{
  int format_mask;

  if (!open_dev())
  {
    error("Can't open the device.");
    return 0;
  }

  if (ioctl(dsp_fd, SNDCTL_DSP_GETFMTS, &format_mask) == -1)
  {
    error_errno("Can't get supported audio formats", errno);
    ::close(dsp_fd);
    dsp_fd = -1;
    return 0;
  }

  caps->formats = 0;
  if (format_mask & AFMT_S8) caps->formats |= SFMT_S8;
  if (format_mask & AFMT_U8) caps->formats |= SFMT_U8;

  if (format_mask & AFMT_S16_LE) caps->formats |= SFMT_S16 | SFMT_LE;
  if (format_mask & AFMT_S16_BE) caps->formats |= SFMT_S16 | SFMT_BE;

#if defined(AFMT_S32_LE) && defined(AFMT_S32_BE)
  if (format_mask & AFMT_S32_LE) caps->formats |= SFMT_S32 | SFMT_LE;
  if (format_mask & AFMT_S32_BE) caps->formats |= SFMT_S32 | SFMT_BE;
#endif

#if defined(AFMT_S24_LE) && defined(AFMT_S24_BE)
  if (format_mask & AFMT_S24_LE) caps->formats |= SFMT_S24 | SFMT_LE;
  if (format_mask & AFMT_S24_BE) caps->formats |= SFMT_S24 | SFMT_BE;
#endif

#if defined(AFMT_FLOAT)
  if (format_mask & AFMT_FLOAT) caps->formats |= SFMT_FLOAT | SFMT_NE;
#endif

  if (!caps->formats)
  {
    error("The driver claims that no format known to me is supported. I will assume that SFMT_S8 and SFMT_S16 (native endian) are supported.");
    caps->formats = SFMT_S8 | SFMT_S16 | SFMT_NE;
  }

  caps->min_rate = AUDIO_RATE_MIN;
  caps->max_rate = AUDIO_RATE_MAX;

  caps->min_channels = caps->max_channels = 1;
  if (ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &caps->min_channels))
  {
    error_errno("Can't set number of channels", errno);
    ::close(dsp_fd);
    dsp_fd = -1;
    return 0;
  }

  ::close(dsp_fd);
  dsp_fd = -1;

  if (!open_dev())
  {
    error("Can't open the device.");
    return 0;
  }

  if (caps->min_channels != 1) caps->min_channels = 2;
  caps->max_channels = 2;
  if (ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &caps->max_channels))
  {
    error_errno("Can't set number of channels", errno);
    ::close(dsp_fd);
    dsp_fd = -1;
    return 0;
  }

  if (caps->max_channels != 2)
  {
    if (caps->min_channels == 2)
    {
      error("Can't get any supported number of channels.");
      ::close(dsp_fd);
      dsp_fd = -1;
      return 0;
    }
    caps->max_channels = 1;
  }

  ::close(dsp_fd);
  dsp_fd = -1;

  return 1;
}

int OssOutput::read_mixer()
{
  int vol;

  if (!started) return -1;

#ifdef OSSv3_MIXER
  if (mixer_fd != -1 && mixer_channel_current != -1)
  {
    if (ioctl(mixer_fd, MIXER_READ(mixer_channel_current), &vol) == -1)
#else
  if (dsp_fd != -1)
  {
    if (ioctl(dsp_fd, SNDCTL_DSP_GETPLAYVOL, &vol) == -1)
#endif
    {
      error("Can't read from mixer");
    }
    else
    {
      return ((vol & 0xFF) + ((vol >> 8) & 0xFF)) / 2;
    }
  }

  return -1;
}

int OssOutput::oss_mixer_name_to_channel(const char *name)
{
  for (size_t ix = 0; ix < MIXER_CHANNELS_NUM; ix += 1)
  {
    if (!strcasecmp(mixer_channels[ix].name, name))
      return ix;
  }
  return -1;
}

int OssOutput::init(struct output_driver_caps *caps)
{
#ifdef OSSv3_MIXER
  mixer_fd = ::open(options_get_str("OSSMixerDevice"), O_RDWR);
  if (mixer_fd == -1)
  {
    std::string err = xstrerror(errno);
    error("Can't open mixer device %s: %s", options_get_str("OSSMixerDevice"), err.c_str());
  }
  else
  {
    mixer_channel1 = oss_mixer_name_to_channel(options_get_symb("OSSMixerChannel1"));
    mixer_channel2 = oss_mixer_name_to_channel(options_get_symb("OSSMixerChannel2"));

    if (mixer_channel1 == -1) fatal("Bad first OSS mixer channel!");
    if (mixer_channel2 == -1) fatal("Bad second OSS mixer channel!");

    mixer_channel_current = mixer_channel1;
    if (read_mixer() == -1) mixer_channel1 = -1;

    mixer_channel_current = mixer_channel2;
    if (read_mixer() == -1) mixer_channel2 = -1;

    if (mixer_channel1 != -1) mixer_channel_current = mixer_channel1;
  }
#endif

  return set_capabilities(caps);
}

void OssOutput::shutdown()
{
#ifdef OSSv3_MIXER
  if (mixer_fd != -1)
  {
    ::close(mixer_fd);
    mixer_fd = -1;
  }
#endif
}

void OssOutput::close()
{
  if (dsp_fd != -1)
  {
    ::close(dsp_fd);
    dsp_fd = -1;
    logit("Audio device closed");
  }

  started = false;
  params.channels = 0;
  params.rate = 0;
  params.fmt = 0;
}

int OssOutput::set_params()
{
  int req_format;
  int req_channels;
  char fmt_name[SFMT_STR_MAX];

  switch (params.fmt & SFMT_MASK_FORMAT)
  {
    case SFMT_S8: req_format = AFMT_S8; break;
    case SFMT_U8: req_format = AFMT_U8; break;
    case SFMT_S16:
      req_format = (params.fmt & SFMT_LE) ? AFMT_S16_LE : AFMT_S16_BE;
      break;
#if defined(AFMT_S24_LE) && defined(AFMT_S24_BE)
    case SFMT_S24:
      req_format = (params.fmt & SFMT_LE) ? AFMT_S24_LE : AFMT_S24_BE;
      break;
#endif
#if defined(AFMT_S32_LE) && defined(AFMT_S32_BE)
    case SFMT_S32:
      req_format = (params.fmt & SFMT_LE) ? AFMT_S32_LE : AFMT_S32_BE;
      break;
#endif
#if defined(AFMT_FLOAT)
    case SFMT_FLOAT:
      req_format = AFMT_S32_FLOAT;
      break;
#endif
    default:
      error("Format %s is not supported by the device", sfmt_str(params.fmt, fmt_name, sizeof(fmt_name)));
      return 0;
  }

  if (ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &req_format) == -1)
  {
    error_errno("Can't set audio format", errno);
    close();
    return 0;
  }

  req_channels = params.channels;
  if (ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &req_channels) == -1)
  {
    std::string err = xstrerror(errno);
    error("Can't set number of channels to %d: %s", params.channels, err.c_str());
    close();
    return 0;
  }
  if (params.channels != req_channels)
  {
    error("Can't set number of channels to %d, device doesn't support this value", params.channels);
    close();
    return 0;
  }

  if (ioctl(dsp_fd, SNDCTL_DSP_SPEED, &params.rate) == -1)
  {
    std::string err = xstrerror(errno);
    error("Can't set sampling rate to %d: %s", params.rate, err.c_str());
    close();
    return 0;
  }

  logit("Audio parameters set to: %s, %d channels, %dHz", sfmt_str(params.fmt, fmt_name, sizeof(fmt_name)), params.channels, params.rate);
  return 1;
}

int OssOutput::open(struct sound_params *sound_params)
{
  params = *sound_params;

  if (!open_dev()) return 0;

  if (!set_params())
  {
    close();
    return 0;
  }

  started = true;
  return 1;
}

int OssOutput::play(const char *buff, const size_t size)
{
  ssize_t ssize = static_cast<ssize_t>(size);
  ssize_t count = 0;

  if (dsp_fd == -1)
  {
    error("Can't play: audio device isn't opened!");
    return -1;
  }

  while (count < ssize)
  {
    ssize_t rc = ::write(dsp_fd, buff + count, ssize - count);
    if (rc == -1)
    {
      error_errno("Error writing pcm sound", errno);
      return -1;
    }
    count += rc;
  }
  return count;
}

void OssOutput::set_mixer(int vol)
{
#ifdef OSSv3_MIXER
  if (mixer_fd != -1)
#else
  if (dsp_fd != -1)
#endif
  {
    vol = std::clamp(vol, 0, 100);
    vol |= vol << 8;
#ifdef OSSv3_MIXER
    if (ioctl(mixer_fd, MIXER_WRITE(mixer_channel_current), &vol) == -1)
#else
    if (ioctl(dsp_fd, SNDCTL_DSP_SETPLAYVOL, &vol) == -1)
#endif
    {
      error("Can't set mixer: ioctl() failed");
    }
  }
}

int OssOutput::get_buff_fill()
{
  audio_buf_info buff_info;
  if (dsp_fd == -1) return 0;
  if (ioctl(dsp_fd, SNDCTL_DSP_GETOSPACE, &buff_info) == -1)
  {
    error("SNDCTL_DSP_GETOSPACE failed");
    return 0;
  }
  return (buff_info.fragstotal * buff_info.fragsize) - buff_info.bytes;
}

int OssOutput::reset()
{
  if (dsp_fd == -1)
  {
    logit("Reset when audio device is not opened");
    return 0;
  }

  logit("Resetting audio device");
  if (ioctl(dsp_fd, SNDCTL_DSP_RESET, nullptr) == -1)
  {
    error("Resetting audio device failed");
  }
  close();
  
  if (!open_dev() || !set_params())
  {
    error("Failed to open audio device after resetting");
    return 0;
  }
  return 1;
}

void OssOutput::toggle_mixer_channel()
{
#ifdef OSSv3_MIXER
  if (mixer_channel_current == mixer_channel1 && mixer_channel2 != -1)
    mixer_channel_current = mixer_channel2;
  else if (mixer_channel1 != -1)
    mixer_channel_current = mixer_channel1;
#endif
}

std::string OssOutput::get_mixer_channel_name()
{
#ifdef OSSv3_MIXER
  if (mixer_channel_current == mixer_channel1)
    return options_get_symb("OSSMixerChannel1");
  return options_get_symb("OSSMixerChannel2");
#else
  return "mocf";
#endif
}

int OssOutput::get_rate() { return params.rate; }

std::unique_ptr<AudioOutput> create_oss_output() {
    return std::make_unique<OssOutput>();
}

// EOF
