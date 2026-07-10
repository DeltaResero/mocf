// src/audio/decoders/flac/flac.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// The code is based on libxmms-flac written by Josh Coalson.
// Copyright (C) 2005 Damian Pietras <daper@daper.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>
#include <algorithm>
#include <FLAC/all.h>

#define DEBUG

#include "core/common.h"
#include "audio/audio.h"
#include "audio/decoder.h"
#include "core/server.h"
#include "core/log.h"
#include "io/io.h"

#define MAX_SUPPORTED_CHANNELS 8

#define SAMPLES_PER_WRITE 512
#define SAMPLE_BUFFER_SIZE                                                     \
  ((FLAC__MAX_BLOCK_SIZE + SAMPLES_PER_WRITE) * MAX_SUPPORTED_CHANNELS *       \
   (32 / 8))

class FlacDecoder : public AudioDecoder
{
public:
  FLAC__StreamDecoder *decoder;
  unique_io_stream stream;
  int bitrate;
  int avg_bitrate;
  int abort; /* abort playing (due to an error) */

  unsigned int length;
  FLAC__uint64 total_samples;

  FLAC__byte sample_buffer[SAMPLE_BUFFER_SIZE];
  unsigned int sample_buffer_fill;

  /* sound parameters */
  unsigned int bits_per_sample;
  unsigned int sample_rate;
  unsigned int channels;

  FLAC__uint64 last_decode_position;

  int ok; /* was this stream successfully opened? */
  struct decoder_error error;

  FlacDecoder();
  ~FlacDecoder() override;

  int decode(char *buf, int buf_len, struct sound_params *sound_params) override;
  int seek(int sec) override;
  int get_bitrate() override;
  int get_duration() override;
  void get_error(struct decoder_error *error) override;
  int get_avg_bitrate() override;
};

/* Convert FLAC big-endian data into PCM little-endian. */
static size_t pack_pcm_signed(FLAC__byte *data,
                              const FLAC__int32 *const input[],
                              unsigned int wide_samples, unsigned int channels,
                              unsigned int bps)
{
  FLAC__byte *const start = data;
  FLAC__int32 sample;
  const FLAC__int32 *input_;
  unsigned int samples, channel;
  unsigned int bytes_per_sample;
  unsigned int incr;

  bytes_per_sample = bps / 8;
  incr = bytes_per_sample * channels;

  for (channel = 0; channel < channels; channel++)
  {
    samples = wide_samples;
    data = start + bytes_per_sample * channel;
    input_ = input[channel];

    while (samples--)
    {
      sample = *input_++;

      switch (bps)
      {
        case 8:
          data[0] = sample;
          break;
        case 16:
          data[0] = static_cast<FLAC__byte>(sample >> 8);
          data[1] = static_cast<FLAC__byte>(sample);
          break;
        case 24:
          data[0] = static_cast<FLAC__byte>(sample >> 16);
          data[1] = static_cast<FLAC__byte>(sample >> 8);
          data[2] = static_cast<FLAC__byte>(sample);
          break;
        case 32:
          data[0] = static_cast<FLAC__byte>(sample >> 24);
          data[1] = static_cast<FLAC__byte>(sample >> 16);
          data[2] = static_cast<FLAC__byte>(sample >> 8);
          data[3] = static_cast<FLAC__byte>(sample);
          break;
      }

      data += incr;
    }
  }

  debug("Converted %u bytes", wide_samples * channels * bytes_per_sample);

  return wide_samples * channels * bytes_per_sample;
}

static FLAC__StreamDecoderWriteStatus
write_cb(const FLAC__StreamDecoder *unused ATTR_UNUSED,
         const FLAC__Frame *frame, const FLAC__int32 *const buffer[],
         void *client_data)
{
  FlacDecoder *data = static_cast<FlacDecoder *>(client_data);
  const unsigned int wide_samples = frame->header.blocksize;

  if (data->abort)
  {
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  data->sample_buffer_fill =
      pack_pcm_signed(data->sample_buffer, buffer, wide_samples, data->channels,
                      data->bits_per_sample);

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void metadata_cb(const FLAC__StreamDecoder *unused ATTR_UNUSED,
                        const FLAC__StreamMetadata *metadata, void *client_data)
{
  FlacDecoder *data = static_cast<FlacDecoder *>(client_data);

  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO)
  {
    debug("Got metadata info");

    data->total_samples = metadata->data.stream_info.total_samples;
    data->bits_per_sample = metadata->data.stream_info.bits_per_sample;
    data->channels = metadata->data.stream_info.channels;
    data->sample_rate = metadata->data.stream_info.sample_rate;
    if (data->total_samples > 0)
    {
      data->length = data->total_samples / data->sample_rate;
    }
  }
}

static void error_cb(const FLAC__StreamDecoder *unused ATTR_UNUSED,
                     FLAC__StreamDecoderErrorStatus status, void *client_data)
{
  FlacDecoder *data = static_cast<FlacDecoder *>(client_data);

  if (status != FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC)
  {
    debug("Aborting due to error");
    data->abort = 1;
  }
  else
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "FLAC: lost sync");
  }
}

static FLAC__StreamDecoderReadStatus
read_cb(const FLAC__StreamDecoder *unused ATTR_UNUSED, FLAC__byte buffer[],
        size_t *bytes, void *client_data)
{
  FlacDecoder *data = static_cast<FlacDecoder *>(client_data);
  ssize_t res;

  res = io_read(data->stream.get(), buffer, *bytes);

  if (res > 0)
  {
    *bytes = res;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
  }

  if (res == 0)
  {
    *bytes = 0;
    /* not sure why this works, but if it ain't broke... */
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }

  error("read error: %s", io_strerror(data->stream.get()));

  return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
}

static FLAC__StreamDecoderSeekStatus
seek_cb(const FLAC__StreamDecoder *unused ATTR_UNUSED,
        FLAC__uint64 absolute_byte_offset, void *client_data)
{
  FlacDecoder *data = static_cast<FlacDecoder *>(client_data);

  return io_seek(data->stream.get(), absolute_byte_offset, SEEK_SET) >= 0
             ? FLAC__STREAM_DECODER_SEEK_STATUS_OK
             : FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
}

static FLAC__StreamDecoderTellStatus
tell_cb(const FLAC__StreamDecoder *unused ATTR_UNUSED,
        FLAC__uint64 *absolute_byte_offset, void *client_data)
{
  FlacDecoder *data = static_cast<FlacDecoder *>(client_data);

  *absolute_byte_offset = io_tell(data->stream.get());

  return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus
length_cb(const FLAC__StreamDecoder *unused ATTR_UNUSED,
          FLAC__uint64 *stream_length, void *client_data)
{
  off_t file_size;
  FlacDecoder *data = static_cast<FlacDecoder *>(client_data);

  file_size = io_file_size(data->stream.get());
  if (file_size == -1)
  {
    return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
  }

  *stream_length = file_size;

  return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool eof_cb(const FLAC__StreamDecoder *unused ATTR_UNUSED,
                         void *client_data)
{
  FlacDecoder *data = static_cast<FlacDecoder *>(client_data);

  return io_eof(data->stream.get());
}

FlacDecoder::FlacDecoder()
{
  decoder_error_init(&error);
  decoder = nullptr;
  bitrate = -1;
  avg_bitrate = -1;
  abort = 0;
  sample_buffer_fill = 0;
  last_decode_position = 0;
  length = -1;
  ok = 0;
}

FlacDecoder::~FlacDecoder()
{
  if (decoder)
  {
    FLAC__stream_decoder_finish(decoder);
    FLAC__stream_decoder_delete(decoder);
  }

  decoder_error_clear(&error);
}

static std::unique_ptr<AudioDecoder> flac_open_internal(const char *file, const int buffered)
{
  auto data = std::make_unique<FlacDecoder>();

  data->stream.reset(io_open(file, buffered));
  if (!io_ok(data->stream.get()))
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't load file: %s",
                  io_strerror(data->stream.get()));
    return data;
  }

  if (!(data->decoder = FLAC__stream_decoder_new()))
  {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "FLAC__stream_decoder_new() failed");
    return data;
  }

  FLAC__stream_decoder_set_md5_checking(data->decoder, false);

  FLAC__stream_decoder_set_metadata_ignore_all(data->decoder);
  FLAC__stream_decoder_set_metadata_respond(data->decoder,
                                            FLAC__METADATA_TYPE_STREAMINFO);

  if (FLAC__stream_decoder_init_stream(
          data->decoder, read_cb, seek_cb, tell_cb, length_cb, eof_cb, write_cb,
          metadata_cb, error_cb, data.get()) != FLAC__STREAM_DECODER_INIT_STATUS_OK)
  {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "FLAC__stream_decoder_init() failed");
    return data;
  }

  if (!FLAC__stream_decoder_process_until_end_of_metadata(data->decoder))
  {
    decoder_error(
        &data->error, ERROR_FATAL, 0,
        "FLAC__stream_decoder_process_until_end_of_metadata() failed.");
    return data;
  }

  data->ok = 1;

  if (data->length > 0)
  {
    off_t data_size = io_file_size(data->stream.get());
    if (data_size > 0)
    {
      FLAC__uint64 pos;

      if (FLAC__stream_decoder_get_decode_position(data->decoder, &pos))
      {
        data_size -= pos;
      }
      data->avg_bitrate = data_size * 8 / data->length;
    }
  }

  debug("File opened. Channels %d. Samplerate %d. Duration %d. Avg_Bitrate %d.",
        data->channels, data->sample_rate, data->length, data->avg_bitrate);
  return data;
}

static void fill_tag(FLAC__StreamMetadata_VorbisComment_Entry *comm,
                     struct file_tags *tags)
{
  const FLAC__byte *eq;

  eq = static_cast<const FLAC__byte *>(memchr(comm->entry, '=', comm->length));
  if (!eq)
  {
    return;
  }

  std::string name(reinterpret_cast<char *>(comm->entry), eq - comm->entry);
  size_t value_length = comm->length - (eq - comm->entry + 1);

  if (value_length == 0)
  {
    return;
  }

  std::string value(reinterpret_cast<const char *>(eq + 1), value_length);

  if (!strcasecmp(name.c_str(), "title"))
  {
    tags->title = value;
  }
  else if (!strcasecmp(name.c_str(), "artist"))
  {
    tags->artist = value;
  }
  else if (!strcasecmp(name.c_str(), "album"))
  {
    tags->album = value;
  }
  else if (!strcasecmp(name.c_str(), "tracknumber") || !strcasecmp(name.c_str(), "track"))
  {
    tags->track = static_cast<int>(strtol(value.c_str(), nullptr, 10));
  }
}

static void get_vorbiscomments(const char *filename, struct file_tags *tags)
{
  FLAC__Metadata_SimpleIterator *iterator =
      FLAC__metadata_simple_iterator_new();
  FLAC__bool got_vorbis_comments = false;

  debug("Reading comments for %s", filename);

  if (!iterator)
  {
    logit("FLAC__metadata_simple_iterator_new() failed.");
    return;
  }

  if (!FLAC__metadata_simple_iterator_init(iterator, filename, true, true))
  {
    logit("FLAC__metadata_simple_iterator_init failed.");
    FLAC__metadata_simple_iterator_delete(iterator);
    return;
  }

  do
  {
    if (FLAC__metadata_simple_iterator_get_block_type(iterator) ==
        FLAC__METADATA_TYPE_VORBIS_COMMENT)
    {
      FLAC__StreamMetadata *block;

      block = FLAC__metadata_simple_iterator_get_block(iterator);
      if (block)
      {
        unsigned int i;
        const FLAC__StreamMetadata_VorbisComment *vc =
            &block->data.vorbis_comment;

        for (i = 0; i < vc->num_comments; i++)
        {
          fill_tag(&vc->comments[i], tags);
        }

        FLAC__metadata_object_delete(block);
        got_vorbis_comments = true;
      }
    }
  } while (!got_vorbis_comments &&
           FLAC__metadata_simple_iterator_next(iterator));

  FLAC__metadata_simple_iterator_delete(iterator);
}

int FlacDecoder::seek(int sec)
{
  FLAC__uint64 target_sample;

  if (static_cast<unsigned int>(sec) > length)
  {
    return -1;
  }

  target_sample = static_cast<FLAC__uint64>((static_cast<double>(sec) / static_cast<double>(length)) *
                                 static_cast<double>(total_samples));

  if (FLAC__stream_decoder_seek_absolute(decoder, target_sample))
  {
    return sec;
  }

  logit("FLAC__stream_decoder_seek_absolute() failed.");

  return -1;
}

int FlacDecoder::decode(char *buf, int buf_len,
                       struct sound_params *sound_params)
{
  unsigned int to_copy;
  int bytes_per_sample_val;
  FLAC__uint64 decode_position;

  bytes_per_sample_val = bits_per_sample / 8;

  switch (bytes_per_sample_val)
  {
    case 1:
      sound_params->fmt = SFMT_S8;
      break;
    case 2:
      sound_params->fmt = SFMT_S16 | SFMT_BE;
      break;
    case 3:
      sound_params->fmt = SFMT_S24_3 | SFMT_BE;
      break;
    case 4:
      sound_params->fmt = SFMT_S32 | SFMT_BE;
      break;
  }

  sound_params->rate = sample_rate;
  sound_params->channels = channels;

  decoder_error_clear(&error);

  if (!sample_buffer_fill)
  {
    debug("decoding...");

    if (FLAC__stream_decoder_get_state(decoder) ==
        FLAC__STREAM_DECODER_END_OF_STREAM)
    {
      logit("EOF");
      return 0;
    }

    if (!FLAC__stream_decoder_process_single(decoder))
    {
      decoder_error(&error, ERROR_FATAL, 0,
                    "Read error processing frame.");
      return 0;
    }

    /* Count the bitrate */
    if (!FLAC__stream_decoder_get_decode_position(decoder,
                                                  &decode_position))
    {
      decode_position = 0;
    }
    if (decode_position > last_decode_position)
    {
      int bytes_per_sec = bytes_per_sample_val * sample_rate * channels;

      bitrate = (decode_position - last_decode_position) * 8.0 /
                      (sample_buffer_fill / static_cast<float>(bytes_per_sec)) / 1000;
    }

    last_decode_position = decode_position;
  }
  else
  {
    debug("Some date remain in the buffer.");
  }

  debug("Decoded %d bytes", sample_buffer_fill);

  to_copy = std::min(static_cast<unsigned int>(buf_len), sample_buffer_fill);
  memcpy(buf, sample_buffer, to_copy);
  memmove(sample_buffer, sample_buffer + to_copy,
          sample_buffer_fill - to_copy);
  sample_buffer_fill -= to_copy;

  return to_copy;
}

int FlacDecoder::get_bitrate()
{
  return bitrate;
}

int FlacDecoder::get_avg_bitrate()
{
  return avg_bitrate / 1000;
}

int FlacDecoder::get_duration()
{
  int result = -1;

  if (ok)
  {
    result = length;
  }

  return result;
}

void FlacDecoder::get_error(struct decoder_error *out_error)
{
  decoder_error_copy(out_error, &error);
}

class FlacPlugin : public AudioPlugin
{
public:
  std::unique_ptr<AudioDecoder> open(const char *file) override
  {
    return flac_open_internal(file, 1);
  }

  void info(const char *file_name, struct file_tags *info, const int tags_sel) override
  {
    if (tags_sel & TAGS_TIME)
    {
      auto data = flac_open_internal(file_name, 0);
      if (static_cast<FlacDecoder*>(data.get())->ok)
      {
        info->time = static_cast<FlacDecoder*>(data.get())->length;
      }
    }

    if (tags_sel & TAGS_COMMENTS)
    {
      get_vorbiscomments(file_name, info);
    }
  }

  void get_name(const char *unused ATTR_UNUSED, char buf[4]) override
  {
    std::memcpy(buf, "FLC", sizeof("FLC"));
  }

  int our_format_ext(const char *ext) override
  {
    return !strcasecmp(ext, "flac") || !strcasecmp(ext, "fla");
  }

  int our_format_mime(const char *mime) override
  {
    return !strcasecmp(mime, "audio/flac") ||
           !strncasecmp(mime, "audio/flac;", 11) ||
           !strcasecmp(mime, "audio/x-flac") ||
           !strncasecmp(mime, "audio/x-flac;", 13);
  }
};

static FlacPlugin flac_plugin;
extern "C" class AudioPlugin *flac_plugin_init() { return &flac_plugin; }

// EOF
