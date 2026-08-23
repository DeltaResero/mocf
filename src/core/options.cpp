// src/core/options.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2004 - 2006 Damian Pietras <daper@daper.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cassert>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <regex.h>
#include <unordered_map>
#include <variant>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "core/common.h"
#include "library/files.h"
#include "core/log.h"
#include "core/options.h"

using OptionValue = std::variant<int, bool, std::optional<std::string>, std::vector<std::string>>;

struct Option;
using CheckFunc = bool(*)(const Option&, int, const std::string&);

struct Option {
    std::string name;
    option_type type;
    OptionValue value;
    bool ignore_in_config = false;
    bool set_in_config = false;

    std::vector<int> int_constraints;
    std::vector<std::string> str_constraints;

    CheckFunc check = nullptr;
};

static std::unordered_map<std::string, Option> options_map;

/* Guards the value of boolean options only.  The map itself gains and
 * loses entries solely in options_init() and options_free(), so looking an
 * option up needs no lock; the engine changing Repeat or Shuffle while a
 * playing thread reads it does. */
static std::shared_mutex bool_value_mtx;

static std::string to_lower(const std::string& s) {
    std::string res = s;
    for (char& c : res) c = std::tolower(c);
    return res;
}

/* Split a colon-separated string into its non-empty segments, mirroring
 * the behaviour of the old lists_strs_split(list, s, ":"): consecutive
 * and leading/trailing colons do not produce empty entries. */
static std::vector<std::string> split_colon_list(const char* s) {
    std::vector<std::string> result;
    std::string buf(s);
    size_t start = 0;

    while (start < buf.size()) {
        size_t end = buf.find(':', start);
        if (end == std::string::npos) {
            result.push_back(buf.substr(start));
            break;
        }
        if (end > start) result.push_back(buf.substr(start, end - start));
        start = end + 1;
    }

    return result;
}

static Option* find_option(const std::string& name, int type_mask) {
    auto it = options_map.find(to_lower(name));
    if (it != options_map.end()) {
        if (type_mask == OPTION_ANY || (it->second.type & type_mask)) {
            return &it->second;
        }
    }
    return nullptr;
}

/* Check that a value falls within the specified range(s). */
static bool check_range(const Option& opt, int int_val, const std::string& str_val) {
    if (opt.type == OPTION_INT) {
        for (size_t i = 0; i + 1 < opt.int_constraints.size(); i += 2) {
            if (int_val >= opt.int_constraints[i] && int_val <= opt.int_constraints[i+1]) return true;
        }
    } else {
        for (size_t i = 0; i + 1 < opt.str_constraints.size(); i += 2) {
            if (strcasecmp(str_val.c_str(), opt.str_constraints[i].c_str()) >= 0 &&
                strcasecmp(str_val.c_str(), opt.str_constraints[i+1].c_str()) <= 0) return true;
        }
    }
    return false;
}

/* Check that a value is one of the specified values. */
static bool check_discrete(const Option& opt, int int_val, const std::string& str_val) {
    if (opt.type == OPTION_INT) {
        for (int c : opt.int_constraints) {
            if (int_val == c) return true;
        }
    } else {
        for (const std::string& c : opt.str_constraints) {
            if (strcasecmp(str_val.c_str(), c.c_str()) == 0) return true;
        }
    }
    return false;
}

/* Check that a string length falls within the specified range(s). */
static bool check_length(const Option& opt, int int_val, const std::string& str_val) {
    int len = str_val.length();
    for (size_t i = 0; i + 1 < opt.int_constraints.size(); i += 2) {
        if (len >= opt.int_constraints[i] && len <= opt.int_constraints[i+1]) return true;
    }
    return false;
}

/* Check that a string has a function-like syntax. */
static bool check_function(const Option& opt, int int_val, const std::string& str_val) {
    static regex_t preg;
    static bool initialized = false;
    if (!initialized) {
        regcomp(&preg, "^[a-z0-9/-]+\\([^,) ]*(,[^,) ]*)*\\)$", REG_EXTENDED | REG_ICASE | REG_NOSUB);
        initialized = true;
    }
    return regexec(&preg, str_val.c_str(), 0, nullptr, 0) == 0;
}

/* Always pass a value as valid. */
static bool check_true(const Option& opt, int int_val, const std::string& str_val) {
    return true;
}

static void add_int(const std::string& name, int value, CheckFunc check, const std::vector<int>& constraints = {}) {
    Option opt;
    opt.name = name;
    opt.type = OPTION_INT;
    opt.value = value;
    opt.check = check;
    opt.int_constraints = constraints;
    options_map[to_lower(name)] = opt;
}

static void add_bool(const std::string& name, bool value) {
    Option opt;
    opt.name = name;
    opt.type = OPTION_BOOL;
    opt.value = value;
    opt.check = check_true;
    options_map[to_lower(name)] = opt;
}

static void add_str(const std::string& name, const char* value, CheckFunc check, const std::vector<std::string>& s_constraints = {}, const std::vector<int>& i_constraints = {}) {
    Option opt;
    opt.name = name;
    opt.type = OPTION_STR;
    if (value) opt.value = std::optional<std::string>(value);
    else opt.value = std::optional<std::string>(std::nullopt);
    opt.check = check;
    opt.str_constraints = s_constraints;
    opt.int_constraints = i_constraints;
    options_map[to_lower(name)] = opt;
}

static void add_path(const std::string& name, const char* value, CheckFunc check, const std::vector<std::string>& s_constraints = {}, const std::vector<int>& i_constraints = {}) {
    Option opt;
    opt.name = name;
    opt.type = OPTION_PATH;
    if (value && value[0] == '~') {
        std::string path = std::string(get_home()) + "/" + ((value[1] == '/') ? value + 2 : value + 1);
        if (path.size() >= PATH_MAX) fatal("Path too long!");
        opt.value = std::optional<std::string>(path);
    } else {
        if (value) opt.value = std::optional<std::string>(value);
        else opt.value = std::optional<std::string>(std::nullopt);
    }
    opt.check = check;
    opt.str_constraints = s_constraints;
    opt.int_constraints = i_constraints;
    options_map[to_lower(name)] = opt;
}

static void add_symb(const std::string& name, const char* value, const std::vector<std::string>& constraints) {
    Option opt;
    opt.name = name;
    opt.type = OPTION_SYMB;
    opt.check = check_discrete;
    opt.str_constraints = constraints;

    bool found = false;
    for (const auto& c : constraints) {
        if (!is_valid_symbol(c.c_str())) fatal("Invalid symbol in '%s' constraint list!", name.c_str());
        if (strcasecmp(c.c_str(), value) == 0) {
            opt.value = std::optional<std::string>(c);
            found = true;
            break;
        }
    }
    if (!found) fatal("Invalid default value symbol in '%s'!", name.c_str());
    options_map[to_lower(name)] = opt;
}

static void add_list(const std::string& name, const char* value, CheckFunc check, const std::vector<std::string>& s_constraints = {}, const std::vector<int>& i_constraints = {}) {
    Option opt;
    opt.name = name;
    opt.type = OPTION_LIST;
    opt.value = value ? split_colon_list(value) : std::vector<std::string>();
    opt.check = check;
    opt.str_constraints = s_constraints;
    opt.int_constraints = i_constraints;
    options_map[to_lower(name)] = opt;
}

/* Set an integer option to the value. */
void options_set_int(const char *name, const int value)
{
  Option* opt = find_option(name, OPTION_INT);
  if (!opt) fatal("Tried to set wrong option '%s'!", name);
  opt->value = value;
}

/* Set a boolean option to the value. */
void options_set_bool(const char *name, const bool value)
{
  Option* opt = find_option(name, OPTION_BOOL);
  if (!opt) fatal("Tried to set wrong option '%s'!", name);
  std::unique_lock<std::shared_mutex> lock(bool_value_mtx);
  opt->value = value;
}

/* Set a symbol option to the value. */
void options_set_symb(const char *name, const char *value)
{
  Option* opt = find_option(name, OPTION_SYMB);
  if (!opt) fatal("Tried to set wrong option '%s'!", name);

  bool found = false;
  for (const auto& c : opt->str_constraints) {
      if (strcasecmp(c.c_str(), value) == 0) {
          opt->value = std::optional<std::string>(c);
          found = true;
          break;
      }
  }
  if (!found) fatal("Tried to set '%s' to unknown symbol '%s'!", name, value);
}

/* Set a string option to the value. The string is duplicated. */
void options_set_str(const char *name, const char *value)
{
  Option* opt = find_option(name, OPTION_STR);
  if (!opt) fatal("Tried to set wrong option '%s'!", name);
  opt->value = value ? std::optional<std::string>(value) : std::nullopt;
}

/* Set a path option to the value. The string is duplicated. */
void options_set_path(const char *name, const char *value)
{
  Option* opt = find_option(name, OPTION_PATH);
  if (!opt) fatal("Tried to set wrong option '%s'!", name);

  if (value && value[0] == '~')
  {
    std::string path = std::string(get_home()) + "/" +
                       ((value[1] == '/') ? value + 2 : value + 1);
    if (path.size() >= PATH_MAX)
    {
      fatal("Path too long!");
    }
    opt->value = std::optional<std::string>(path);
  }
  else
  {
    opt->value = value ? std::optional<std::string>(value) : std::nullopt;
  }
}

/* Set list option values to the colon separated value. */
void options_set_list(const char *name, const char *value, bool append)
{
  Option* opt = find_option(name, OPTION_LIST);
  if (!opt) fatal("Tried to set wrong option '%s'!", name);

  auto& list = std::get<std::vector<std::string>>(opt->value);
  if (!append && !list.empty())
  {
    list.clear();
  }
  if (value) {
    std::vector<std::string> tokens = split_colon_list(value);
    list.insert(list.end(), tokens.begin(), tokens.end());
  }
}

/* Given a type, a name and a value, set that option's value.
 * Return false on error. */
bool options_set_pair(const char *name, const char *value, bool append)
{
  int num;
  char *end;
  bool val;

  switch (options_get_type(name))
  {
    case OPTION_INT:
      num = strtol(value, &end, 10);
      if (*end)
      {
        return false;
      }
      if (!options_check_int(name, num))
      {
        return false;
      }
      options_set_int(name, num);
      break;

    case OPTION_BOOL:
      if (!strcasecmp(value, "yes"))
      {
        val = true;
      }
      else if (!strcasecmp(value, "no"))
      {
        val = false;
      }
      else
      {
        return false;
      }
      options_set_bool(name, val);
      break;

    case OPTION_STR:
      if (!options_check_str(name, value))
      {
        return false;
      }
      options_set_str(name, value);
      break;

    case OPTION_PATH:
      if (!options_check_str(name, value))
      {
        return false;
      }
      options_set_path(name, value);
      break;

    case OPTION_SYMB:
      if (!options_check_symb(name, value))
      {
        return false;
      }
      options_set_symb(name, value);
      break;

    case OPTION_LIST:
      if (!options_check_list(name, value))
      {
        return false;
      }
      options_set_list(name, value, append);
      break;

    case OPTION_FREE:
    case OPTION_ANY:
      return false;
  }

  return true;
}

void options_ignore_config(const char *name)
{
  Option* opt = find_option(name, OPTION_ANY);
  if (!opt) fatal("Tried to set wrong option '%s'!", name);
  opt->ignore_in_config = true;
}

/* Make a table of options and its default values. */
void options_init()
{
  options_map.clear();

  add_bool("ReadTags", true);
  add_path("MusicDir", nullptr, check_true);
  add_bool("StartInMusicDir", false);
  add_int("CircularLogSize", 0, check_range, {0, INT_MAX});
  add_symb("Sort", "FileName", {"FileName"});
  add_bool("ShowStreamErrors", false);
  add_bool("MP3IgnoreCRCErrors", true);
  add_bool("Repeat", false);
  add_bool("Shuffle", false);
  add_bool("ForceShufflePlaylistOnly", false);
  add_bool("AutoNext", true);
  add_str("FormatString", "%(n:%n :)%(a:%a - :)%(t:%t:)%(A: \\(%A\\):)", check_true);
  add_int("InputBuffer", 512, check_range, {32, INT_MAX});
  add_int("OutputBuffer", 128, check_range, {128, INT_MAX});

#ifdef OPENBSD
  add_list("SoundDriver", "SNDIO:JACK:OSS", check_discrete, {"SNDIO", "PulseAudio", "Jack", "ALSA", "OSS", "null"});
#else
  add_list("SoundDriver", "PulseAudio:Jack:ALSA:OSS", check_discrete, {"SNDIO", "PulseAudio", "Jack", "ALSA", "OSS", "null"});
#endif

  add_str("JackClientName", "mocf", check_true);
  add_bool("JackStartServer", false);
  add_str("JackOutLeft", "system:playback_1", check_true);
  add_str("JackOutRight", "system:playback_2", check_true);

  add_str("OSSDevice", "/dev/dsp", check_true);
  add_str("OSSMixerDevice", "/dev/mixer", check_true);
  add_symb("OSSMixerChannel1", "pcm", {"pcm", "master", "speaker"});
  add_symb("OSSMixerChannel2", "master", {"pcm", "master", "speaker"});

  add_str("ALSADevice", "default", check_true);
  add_str("ALSAMixer1", "PCM", check_true);
  add_str("ALSAMixer2", "Master", check_true);

  add_bool("Softmixer_SaveState", true);
  add_bool("Equalizer_SaveState", true);

  add_bool("ShowHiddenFiles", false);
  add_bool("HideFileExtension", false);
  add_bool("ShowFormat", true);
  add_symb("ShowTime", "IfAvailable", {"yes", "no", "IfAvailable"});
  add_bool("ShowTimePercent", false);

  add_list("ScreenTerms", "screen:screen-w:vt100", check_true);

  add_list("XTerms",
           "xterm:"
           "xterm-colour:xterm-color:"
           "xterm-256colour:xterm-256color:"
           "rxvt:rxvt-unicode:"
           "rxvt-unicode-256colour:rxvt-unicode-256color:"
           "eterm",
           check_true);

  add_str("Theme", nullptr, check_true);
  add_str("XTermTheme", nullptr, check_true);
  add_str("ForceTheme", nullptr, check_true);
  add_path("MOCDir", "~/.mocf", check_true);
  add_bool("UseMMap", false);
  add_bool("UseMimeMagic", false);
  add_str("ID3v1TagsEncoding", "WINDOWS-1250", check_true);
  add_bool("EnforceTagsEncoding", false);
  add_bool("FileNamesIconv", false);
  add_bool("NonUTFXterm", false);
  add_bool("Precache", true);
  add_bool("SavePlaylist", true);

  add_bool("SavePlaylistTags", false);
  add_str("Keymap", nullptr, check_true);
  add_bool("ASCIILines", false);

  add_path("FastDir1", nullptr, check_true);
  add_path("FastDir2", nullptr, check_true);
  add_path("FastDir3", nullptr, check_true);
  add_path("FastDir4", nullptr, check_true);
  add_path("FastDir5", nullptr, check_true);
  add_path("FastDir6", nullptr, check_true);
  add_path("FastDir7", nullptr, check_true);
  add_path("FastDir8", nullptr, check_true);
  add_path("FastDir9", nullptr, check_true);
  add_path("FastDir10", nullptr, check_true);

  add_int("SeekTime", 1, check_range, {1, INT_MAX});
  add_int("SilentSeekTime", 5, check_range, {1, INT_MAX});

  add_list("PreferredDecoders",
           "aac(aac,ffmpeg):m4a(ffmpeg):"
           "mpc(musepack,*,ffmpeg):mpc8(musepack,*,ffmpeg):"
           "sid(sidplayfp,*):mus(sidplayfp,*):"
           "wav(sndfile,*,ffmpeg):"
           "wv(wavpack,*,ffmpeg):"
           "audio/aac(aac):audio/aacp(aac):audio/m4a(ffmpeg):"
           "audio/wav(sndfile,*):"
           "ogg(vorbis,*,ffmpeg):oga(vorbis,*,ffmpeg):ogv(ffmpeg):"
           "application/ogg(vorbis):audio/ogg(vorbis):"
           "flac(flac,*,ffmpeg):"
           "opus(opus,ffmpeg):"
           "spx(speex)",
           check_function);

  add_symb("ResampleMethod", "Linear", {"SincBestQuality", "SincMediumQuality", "SincFastest", "ZeroOrderHold", "Linear"});
  add_int("EnableResample", 1, check_range, {0, 2});
  add_int("MaxSamplerate", 0, check_range, {0, 500000});
  add_int("MaxChannels", 0, check_range, {0, 500000});
  add_list("MaskOutputFormats", "", check_true);
  add_int("MixerBarWidth", 30, check_range, {10, INT_MAX});
  add_bool("UseRealtimePriority", false);
  add_int("TagsCacheSize", 256, check_range, {0, INT_MAX});
  add_bool("PlaylistNumbering", true);

  add_list("Layout1", "directory(0,0,50%,100%):playlist(50%,0,FILL,100%)", check_function);
  add_list("Layout2", "directory(0,0,100%,100%):playlist(0,0,100%,100%)", check_function);
  add_list("Layout3", nullptr, check_function);

  add_bool("FollowPlayedFile", true);

  add_bool("UseCursorSelection", false);
  add_bool("SetXtermTitle", true);
  add_bool("SetScreenTitle", true);
  add_bool("PlaylistFullPaths", true);
  add_bool("SaveRelativePlaylists", true);

  add_str("BlockDecorators", "`\"'", check_length, {}, {3, 3});
  add_int("MessageLingerTime", 3, check_range, {0, INT_MAX});
  add_bool("PrefixQueuedMessages", true);
  add_str("ErrorMessagesQueued", "!", check_true);

  add_symb("XMP_ResamplingMode", "LINEAR", {"NEAREST", "LINEAR", "SPLINE"});
  add_int("XMP_Channels", 2, check_discrete, {1, 2});
  add_int("XMP_Bits", 16, check_discrete, {8, 16});
  add_int("XMP_Frequency", 48000, check_discrete, {11025, 22050, 44100, 48000});
  add_bool("XMP_LowPassFilter", false);
  add_int("XMP_Amplification", 0, check_range, {0, 3});
  add_int("XMP_LoopCount", 0, check_range, {0, 1});

  add_int("SidPlayFP_DefaultSongLength", 180, check_range, {0, INT_MAX});
  add_int("SidPlayFP_MinimumSongLength", 0, check_range, {0, INT_MAX});
  add_str("SidPlayFP_Database", nullptr, check_true);
  add_int("SidPlayFP_Frequency", 48000, check_range, {4000, 48000});
  add_bool("SidPlayFP_StartAtStart", true);
  add_bool("SidPlayFP_PlaySubTunes", true);
  add_int("SidPlayFP_SIDModel", 0, check_range, {0, 2});

  add_int("GME_Frequency", 44100, check_range, {4000, 48000});
  add_bool("GME_PlaySubTunes", false);

  add_bool("AAC_HEAACUpsampling", true);

  add_path("OnEngineStart", nullptr, check_true);
  add_path("OnEngineStop", nullptr, check_true);
  add_path("OnStop", nullptr, check_true);

  add_bool("QueueNextSongReturn", false);
}

/* Return 1 if a parameter to an integer option is valid. */
int options_check_int(const char *name, const int val)
{
  Option* opt = find_option(name, OPTION_INT);
  if (!opt) return 0;
  return opt->check(*opt, val, "") ? 1 : 0;
}

/* Return 1 if a parameter to a boolean option is valid.  This may seem
 * pointless but it provides a consistant interface, ensures the existence
 * of the option and checks the value where true booleans are emulated with
 * other types. */
int options_check_bool(const char *name, const bool val)
{
  Option* opt = find_option(name, OPTION_BOOL);
  if (!opt) return 0;
  return 1;
}

/* Return 1 if a parameter to a string option is valid. */
int options_check_str(const char *name, const char *val)
{
  Option* opt = find_option(name, OPTION_STR | OPTION_PATH);
  if (!opt) return 0;
  return opt->check(*opt, 0, val ? val : "") ? 1 : 0;
}

/* Return 1 if a parameter to a symbol option is valid. */
int options_check_symb(const char *name, const char *val)
{
  Option* opt = find_option(name, OPTION_SYMB);
  if (!opt) return 0;
  return check_discrete(*opt, 0, val ? val : "") ? 1 : 0;
}

/* Return 1 if a parameter to a list option is valid. */
int options_check_list(const char *name, const char *val)
{
  Option* opt = find_option(name, OPTION_LIST);
  if (!opt) return 0;

  for (const auto& item : split_colon_list(val)) {
      if (!opt->check(*opt, 0, item)) {
          return 0;
      }
  }
  return 1;
}

/* Return 1 if the named option was defaulted. */
int options_was_defaulted(const char *name)
{
  Option* opt = find_option(name, OPTION_ANY);
  if (!opt) return 0;
  return (!opt->set_in_config && !opt->ignore_in_config) ? 1 : 0;
}

/* Find and substitute variables enclosed by '${...}'.  Variables are
 * substituted first from the environment then, if not found, from
 * the configuration options.  Strings of the form '$${' are reduced to
 * '${' and not substituted.  The result is returned as a new string. */
static std::string substitute_variable(const char *name_in, const std::string &value_in)
{
  static const char accept[] = "abcdefghijklmnopqrstuvwxyz"
                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                               "0123456789_";

  std::string result = value_in;
  size_t pos = 0;

  for (;;)
  {
    size_t dollar = result.find("${", pos);
    if (dollar == std::string::npos)
    {
      break;
    }

    /* Escape "$${". */
    if (dollar > 0 && result[dollar - 1] == '$')
    {
      result.erase(dollar - 1, 1);
      pos = dollar + 1;
      continue;
    }

    /* Find where the substitution variable name ends. */
    size_t name_start = dollar + 2;
    size_t len = strspn(result.c_str() + name_start, accept);
    if (len == 0)
    {
      fatal("Error in config file option '%s':\n"
            "             substitution variable name is missing!",
            name_in);
    }
    std::string name = result.substr(name_start, len);

    /* Find default substitution or closing brace. */
    size_t after_name = name_start + len;
    std::string dflt;
    bool has_dflt = false;
    size_t end_pos;

    if (after_name < result.size() && result[after_name] == '}')
    {
      end_pos = after_name;
    }
    else if (result.compare(after_name, 2, ":-") == 0)
    {
      size_t dflt_start = after_name + 2;
      size_t brace = result.find('}', dflt_start);
      if (brace == std::string::npos)
      {
        fatal("Error in config file option '%s': "
              "unterminated '${%s:-'!",
              name_in, name.c_str());
      }
      dflt = result.substr(dflt_start, brace - dflt_start);
      has_dflt = true;
      end_pos = brace;
    }
    else if (after_name >= result.size())
    {
      fatal("Error in config file option '%s': "
            "unterminated '${'!",
            name_in);
    }
    else
    {
      fatal("Error in config file option '%s':\n"
            "             expecting  ':-' or '}' found '%c'!",
            name_in, result[after_name]);
    }

    /* Fetch environment variable or configuration option value. */
    std::string value;
    bool has_value = false;
    const char *env = getenv(name.c_str());
    if (env)
    {
      value = env;
      has_value = true;
    }
    else if (find_option(name.c_str(), OPTION_ANY) != nullptr)
    {
      char buf[16];

      switch (options_get_type(name.c_str()))
      {
        case OPTION_INT:
          snprintf(buf, sizeof(buf), "%d", options_get_int(name.c_str()));
          value = buf;
          has_value = true;
          break;
        case OPTION_BOOL:
          value = options_get_bool(name.c_str()) ? "yes" : "no";
          has_value = true;
          break;
        case OPTION_STR:
        case OPTION_PATH:
        {
          const char *s = options_get_str(name.c_str());
          if (s)
          {
            value = s;
            has_value = true;
          }
          break;
        }
        case OPTION_SYMB:
        {
          const char *s = options_get_symb(name.c_str());
          if (s)
          {
            value = s;
            has_value = true;
          }
          break;
        }
        case OPTION_LIST:
        {
          const std::vector<std::string> &list = options_get_list(name.c_str());
          if (!list.empty())
          {
            for (const auto &item : list)
            {
              value += item;
              value += ':';
            }
            value.pop_back();
            has_value = true;
          }
          break;
        }
        case OPTION_FREE:
        case OPTION_ANY:
          break;
      }
    }

    std::string replacement;
    if (has_value && !value.empty())
    {
      replacement = value;
    }
    else if (has_dflt)
    {
      replacement = dflt;
    }
    else
    {
      fatal("Error in config file option '%s':\n"
            "             substitution variable '%s' not set or null!",
            name_in, name.c_str());
    }

    result.replace(dollar, end_pos - dollar + 1, replacement);

    /* Go look for another substitution, starting after the replacement. */
    pos = dollar + replacement.size();
  }

  return result;
}

/* Set an option read from the configuration file. Return false on error. */
static bool set_option(const char *name, const char *value_in, bool append)
{
  Option* opt = find_option(name, OPTION_ANY);
  if (!opt) {
      fprintf(stderr, "Wrong option name: '%s'.", name);
      return false;
  }

  if (opt->ignore_in_config)
  {
    return true;
  }

  if (append && opt->type != OPTION_LIST)
  {
    fprintf(stderr, "Only list valued options can be appended to ('%s').",
            name);
    return false;
  }

  if (!append && opt->set_in_config)
  {
    fprintf(stderr,
            "Tried to set an option that has been already "
            "set in the config file ('%s').",
            name);
    return false;
  }

  opt->set_in_config = true;

  /* Substitute environmental variables. */
  std::string value = substitute_variable(name, value_in);

  if (!options_set_pair(name, value.c_str(), append))
  {
    return false;
  }

  return true;
}

/* Check if values of options make sense. This only checks options that can't
 * be checked without parsing the whole file. */
static void sanity_check()
{
  if ((options_get_int("EnableResample") == 2) &&
      (options_get_int("MaxSamplerate") == 0))
  {
    fatal("You need to set MaxSamplerate when EnableResample is set to 2.");
  }
}

/* Maximum lengths for option names/values read from the config file.
 * These are defensive limits against malformed/corrupt config files,
 * not a reflection of real option name/value sizes. */
static constexpr size_t kMaxOptNameLen = 30;
static constexpr size_t kMaxOptValueLen = 512;

/* Parse the configuration file. */
void options_parse(const char *config_file)
{
  int ch;
  int comm = 0;  /* comment? */
  int eq = 0;    /* equal character appeared? */
  int quote = 0; /* are we in quotes? */
  int esc = 0;
  bool plus = false;   /* plus character appeared? */
  bool append = false; /* += (list append) appeared */
  bool sp = false;     /* first post-name space detected */
  std::string opt_name;
  std::string opt_value;
  int line = 1;
  FILE *file;

  if (!is_secure(config_file))
  {
    fatal("Configuration file is not secure: %s", config_file);
  }

  if (!(file = fopen(config_file, "r")))
  {
    log_errno("Can't open config file", errno);
    return;
  }

  while ((ch = getc(file)) != EOF)
  {
    /* Skip comment */
    if (comm && ch != '\n')
    {
      continue;
    }

    /* Check for "+=" (list append) */
    if (ch != '=' && plus)
    {
      fatal("Error in config file: stray '+' on line %d!", line);
    }

    /* Interpret parameter */
    if (ch == '\n')
    {
      comm = 0;

      if (!opt_name.empty())
      {
        if (opt_value.empty() && strncasecmp(opt_name.c_str(), "Layout", 6))
        {
          fatal("Error in config file: "
                "missing option value on line %d!",
                line);
        }
        if (!set_option(opt_name.c_str(), opt_value.c_str(), append))
        {
          fatal("Error in config file on line %d!", line);
        }
      }

      opt_name.clear();
      opt_value.clear();
      eq = 0;
      quote = 0;
      esc = 0;
      append = false;
      sp = false;

      line++;
    }

    /* Turn on comment */
    else if (ch == '#' && !quote)
    {
      comm = 1;
    }

    /* Turn on quote */
    else if (!quote && !esc && (ch == '"'))
    {
      quote = 1;
    }

    /* Turn off quote */
    else if (!esc && quote && ch == '"')
    {
      quote = 0;
    }

    else if (!esc && !eq && ch == '+')
    {
      plus = true;
    }

    else if (ch == '=' && !quote)
    {
      if (eq)
      {
        fatal("Error in config file: stray '=' on line %d!", line);
      }
      if (opt_name.empty())
      {
        fatal("Error in config file: "
              "missing option name on line %d!",
              line);
      }
      append = plus;
      plus = false;
      eq = 1;
    }

    /* Turn on escape */
    else if (ch == '\\' && !esc)
    {
      esc = 1;
    }

    /* Embedded blank detection */
    else if (!eq && !opt_name.empty() && isblank(ch))
    {
      sp = true;
    }
    else if (!eq && sp && !isblank(ch))
    {
      fatal("Error in config file: "
            "embedded blank in option name on line %d!",
            line);
    }

    /* Add char to parameter value */
    else if ((!isblank(ch) || quote) && eq)
    {
      if (esc && ch != '"')
      {
        if (opt_value.size() == kMaxOptValueLen)
        {
          fatal("Error in config file: "
                "option value on line %d is too long!",
                line);
        }
        opt_value.push_back('\\');
      }

      if (opt_value.size() == kMaxOptValueLen)
      {
        fatal("Error in config file: "
              "option value on line %d is too long!",
              line);
      }
      opt_value.push_back(static_cast<char>(ch));
      esc = 0;
    }

    /* Add char to parameter name */
    else if (!isblank(ch) || quote)
    {
      if (opt_name.size() == kMaxOptNameLen)
      {
        fatal("Error in config file: "
              "option name on line %d is too long!",
              line);
      }
      opt_name.push_back(static_cast<char>(ch));
      esc = 0;
    }
  }

  if (!opt_name.empty() || !opt_value.empty())
  {
    fatal("Parse error at the end of the config file (need end of "
          "line?)!");
  }

  sanity_check();

  fclose(file);
}

void options_free()
{
  options_map.clear();
}

int options_get_int(const char *name)
{
  Option* opt = find_option(name, OPTION_INT);
  if (!opt) fatal("Tried to get wrong option '%s'!", name);
  return std::get<int>(opt->value);
}

bool options_get_bool(const char *name)
{
  Option* opt = find_option(name, OPTION_BOOL);
  if (!opt) fatal("Tried to get wrong option '%s'!", name);
  std::shared_lock<std::shared_mutex> lock(bool_value_mtx);
  return std::get<bool>(opt->value);
}

const char *options_get_str(const char *name)
{
  Option* opt = find_option(name, OPTION_STR | OPTION_PATH);
  if (!opt) fatal("Tried to get wrong option '%s'!", name);
  auto& val = std::get<std::optional<std::string>>(opt->value);
  return val ? val.value().c_str() : nullptr;
}

const char *options_get_symb(const char *name)
{
  Option* opt = find_option(name, OPTION_SYMB);
  if (!opt) fatal("Tried to get wrong option '%s'!", name);
  auto& val = std::get<std::optional<std::string>>(opt->value);
  return val ? val.value().c_str() : nullptr;
}

std::vector<std::string> &options_get_list(const char *name)
{
  Option* opt = find_option(name, OPTION_LIST);
  if (!opt) fatal("Tried to get wrong option '%s'!", name);
  return std::get<std::vector<std::string>>(opt->value);
}

enum option_type options_get_type(const char *name)
{
  Option* opt = find_option(name, OPTION_ANY);
  if (!opt) return OPTION_FREE;
  return opt->type;
}

// EOF
