// src/audio/decoder.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef DECODER_H
#define DECODER_H

#include "audio/audio.h"
#include "library/playlist.h"
#include "io/io.h"

#include <memory>
#include <string>

/** Version of the decoder API.
 *
 * On every change in the decoder API this number will be changed, so
 * MOC will not load plugins compiled with older/newer decoder.h. */
#define DECODER_API_VERSION 7

/** Type of the decoder error. */
enum decoder_error_type
{
  ERROR_OK,     /*!< There was no error. */
  ERROR_STREAM, /*!< Recoverable error in the stream. */
  ERROR_FATAL   /*!< Fatal error in the stream - further decoding can't
            be performed. */
};

/** Decoder error.
 *
 * Describes decoder error. Fields don't need to be accessed directly,
 * there are functions to modify/access decoder_error object. */
struct decoder_error
{
  enum decoder_error_type type; /*!< Type of the error. */
  std::string err;              /*!< Error string. */
};

class AudioDecoder;

  /** @class AudioPlugin
   * The factory and metadata class for a decoder plugin.
   *
   * Describes decoder plugin - contains methods for format detection and stream creation. */
  class AudioPlugin
  {
  public:
    virtual ~AudioPlugin() = default;

    /** API version used by the plugin. */
    int api_version = DECODER_API_VERSION;

    /** Initialize the plugin.
     * This function is called once at application startup. Optional. */
    virtual void init() {}

    /** Cleanup the plugin.
     * This function is called once at application exit. Optional. */
    virtual void destroy() {}

    /** Open the resource.
     *
     * Open the given resource (file).
     *
     * \param uri URL to the resource.
     * \return Unique pointer to an AudioDecoder instance to decode the stream.
     */
    virtual std::unique_ptr<AudioDecoder> open(const char *uri) = 0;

    /** Get tags for a file.
     *
     * \param file File for which to get tags.
     * \param tags Pointer to the tags structure.
     * \param tags_sel OR'ed list of requested tags.
     */
    virtual void info(const char *file, struct file_tags *tags, const int tags_sel) {}

    /** Check if the file extension is for a file that this decoder supports.
     * \param ext Extension.
     * \return Non-zero if supported.
     */
    virtual int our_format_ext(const char *ext) { return 0; }

    /** Check if a stream with the given MIME type is supported by this decoder.
     * \param mime_type MIME type.
     * \return Non-zero if supported.
     */
    virtual int our_format_mime(const char *mime_type) { return 0; }

    /** Check if a buffer holding the first bytes of a file is in this
     * decoder's format, regardless of the file's name. Used as a fallback
     * when the decoder chosen by extension cannot open the file. Optional.
     * \param buf First bytes of the file.
     * \param len Number of valid bytes in buf.
     * \return The format's display name (static storage), or nullptr if
     * the data is not recognized. */
    virtual const char *our_format_data(const char *buf, size_t len)
    {
      return nullptr;
    }

    /** Get a 3-chars format name for a file.
     * \param file File for which we want the format name.
     * \return The format name, or an empty string if unknown.
     */
    virtual std::string get_name(const char *file) { return ""; }
  };

  /** @class AudioDecoder
   * Represents an active, opened audio stream.
   *
   * Contains methods for decoding, seeking, and managing the active stream state.
   */
  class AudioDecoder
  {
  public:
    virtual ~AudioDecoder() = default;

    /** Decode a piece of input.
     * \param buf Buffer to put data in.
     * \param buf_len Size of the buffer in bytes.
     * \param sound_params Parameters of the decoded sound.
     * \return Number of bytes written or 0 on EOF.
     */
    virtual int decode(char *buf, int buf_len, struct sound_params *sound_params) = 0;

    /** Seek in the stream.
     * \param sec Where to seek in seconds.
     * \return The position that we actually seek to or -1 on error.
     * -1 is not a fatal error and further decoding will be performed.
     */
    virtual int seek(int sec) = 0;

    /** Get the current bitrate.
     * \return Current bitrate in kbps or -1 if not available.
     * -1 is not a fatal error and further decoding will be performed.
     */
    virtual int get_bitrate() = 0;

    /** Get duration of the stream.
     * \return Duration in seconds or -1 on error.
     * -1 is not a fatal error and further decoding will be performed.
     */
    virtual int get_duration() = 0;

    /** Get error for the last decode() invocation.
     * \param error Pointer to the decoder_error object to fill.
     */
    virtual void get_error(struct decoder_error *error) = 0;

    /** Get current tags for the stream.
     * \param tags Pointer to tags to fill.
     * \return 1 if the tags were changed from the last call, 0 if not.
     */
    virtual int current_tags(struct file_tags *tags) { return 0; }

    /** Get the IO stream used by the decoder.
     *
     * This is used for fast interrupting of local file reads. Optional.
     *
     * \return Pointer to the used IO stream.
     */
    virtual struct io_stream *get_stream() { return nullptr; }

    /** Get the average bitrate.
     * \return Average bitrate in kbps or -1 if not available.
     */
    virtual int get_avg_bitrate() { return -1; }
  };

  /** Initialize decoder plugin.
   *
   * Each decoder plugin must export a function name plugin_init of this
   * type. The function must return a pointer to the plugin's AudioPlugin
   * instance.
   */

  typedef AudioPlugin *plugin_init_func();

  int is_sound_file(const char *name);
  AudioPlugin *get_decoder(const char *file);
  AudioPlugin *get_decoder_by_content(const char *file, const char **label);
  const char *get_decoder_name(const AudioPlugin *decoder);
  void decoder_init(int debug_info);
  void decoder_cleanup();
  /* Return short type name for the given file, or an empty string if
   * the type could not be determined. */
  std::string file_type_name(const char *file);


/** @defgroup decoder_error_funcs Decoder error functions
 *
 * These functions can be used to modify variables of the decoder_error
 * structure.
 */
/*@{*/

/** Fill decoder_error structure with an error.
 *
 * Fills decoder error variable with an error. It can be used like printf().
 *
 * \param error Pointer to the decoder_error object to fill.
 * \param type Type of the error.
 * \param add_errno If this value is non-zero, a space and a string
 * describing system error for errno equal to the value of add_errno
 * is appended to the error message.
 * \param format Format, like in the printf() function.
 */
void decoder_error(struct decoder_error *error,
                   const enum decoder_error_type type, const int add_errno,
                   const char *format, ...) ATTR_PRINTF(4, 5);

/** Clear decoder_error structure.
 *
 * Clear decoder_error structure. Set the system type to ERROR_OK and
 * the error message to empty. Releases all resources used by the error's
 * fields.
 *
 * \param error Pointer to the decoder_error object to be cleared.
 */
void decoder_error_clear(struct decoder_error *error);

/** Copy decoder_error variable.
 *
 * Copies the decoder_error variable to another decoder_error variable.
 *
 * \param dst Destination.
 * \param src Source.
 */
void decoder_error_copy(struct decoder_error *dst,
                        const struct decoder_error *src);

/** Return the error text from the decoder_error variable.
 *
 * Returns the error text from the decoder_error variable.  An empty
 * string may be returned if decoder_error() has not been called.
 *
 * \param error Pointer to the source decoder_error object.
 *
 * \return The address of the error text.
 */
const char *decoder_error_text(const struct decoder_error *error);

/** Initialize decoder_error variable.
 *
 * Initialize decoder_error variable and set the error to ERROR_OK with no
 * message.
 *
 * \param error Pointer to the decoder_error object to be initialised.
 */
void decoder_error_init(struct decoder_error *error);

/*@}*/

#endif

// EOF
