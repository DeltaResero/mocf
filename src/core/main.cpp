// src/core/main.cpp
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
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <locale.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <popt.h>

#include "core/common.h"
#include "core/server.h"
#include "ui/curses/interface.h"
#include "core/options.h"
#include "core/protocol.h"
#include "core/log.h"
#include "audio/decoder.h"
#include "library/files.h"

static int mocf_argc;
static const char **mocf_argv;
static int popt_next_val = 1;
static std::string render_popt_command_line();

/* List of MOC-specific environment variables. */
static struct
{
  const char *name;
  const char *desc;
} environment_variables[] = {
    {"MOCF_OPTS", "Additional command line options"},
    {"MOCF_POPTRC", "List of POPT configuration files"}};

struct parameters
{
  char *config_file;
  int no_config_file;
  int debug;
};

/* Check if a directory ./.moc exists and create if needed. */
static void check_moc_dir()
{
  std::string dir_name = create_file_name("");
  struct stat file_stat;

  /* strip trailing slash */
  if (!dir_name.empty() && dir_name.back() == '/')
    dir_name.pop_back();

  if (stat(dir_name.c_str(), &file_stat) == -1)
  {
    if (errno != ENOENT)
    {
      fatal("Error trying to check for " CONFIG_DIR " directory: %s",
            xstrerror(errno).c_str());
    }

    if (mkdir(dir_name.c_str(), 0700) == -1)
    {
      fatal("Can't create directory %s: %s", dir_name.c_str(),
            xstrerror(errno).c_str());
    }
  }
  else
  {
    if (!S_ISDIR(file_stat.st_mode) || access(dir_name.c_str(), W_OK))
    {
      fatal("%s is not a writable directory!", dir_name.c_str());
    }
  }
}

struct server_thread_args {
  struct engine_event_queue *eq;
};

static void *server_thread_func(void *arg)
{
  std::unique_ptr<server_thread_args> args(
      static_cast<server_thread_args *>(arg));
  server_init(args->eq);
  server_loop();
  return nullptr;
}

/* Run client and server in the same process. */
static void start_moc(const struct parameters *params, const std::vector<std::string> &args)
{
  pthread_t server_thread;
  struct engine_event_queue *eq;

  eq = engine_event_queue_new();

  auto th_args = std::make_unique<server_thread_args>();
  th_args->eq    = eq;

  if (params->debug)
  {
    FILE *logfp = fopen(create_file_name("mocf.log").c_str(), "a");
    if (!logfp)
    {
      fatal("Can't open log file: %s", xstrerror(errno).c_str());
    }
    log_init_stream(logfp, "mocf.log");
  }
  else
  {
    log_init_stream(nullptr, nullptr);
  }

  if (int rc = pthread_create(&server_thread, nullptr, server_thread_func,
                              th_args.get()); rc != 0)
  {
    fatal("pthread_create() failed: %s", xstrerror(rc).c_str());
  }

  /* The thread now owns th_args and will delete it when it exits. */
  th_args.release();

  xsignal(SIGPIPE, SIG_IGN);

  /* Block until the engine thread has finished initialising. */
  engine_wait_ready();

  init_interface(eq, args);
  interface_loop();
  interface_end();

  /* engine_quit() was called by interface_end(); wait for the thread. */
  pthread_join(server_thread, nullptr);

  engine_event_queue_free(eq);
}

static void show_version()
{
  int rc;
  struct utsname uts;

  putchar('\n');
  printf("          This is: %s\n", PACKAGE_NAME);
  printf("          Version: %s\n", PACKAGE_VERSION);

#ifdef PACKAGE_REVISION
  printf("         Revision: %s\n", PACKAGE_REVISION);
#endif

  /* Show build time */
#ifdef __DATE__
  printf("            Built: %s", __DATE__);
#ifdef __TIME__
  printf(" %s", __TIME__);
#endif
  putchar('\n');
#endif

  /* Show compiled-in components */
  printf("    Compiled with:");
#ifdef HAVE_OSS
  printf(" OSS");
#endif
#ifdef HAVE_SNDIO
  printf(" SNDIO");
#endif
#ifdef HAVE_PULSE
  printf(" PulseAudio");
#endif
#ifdef HAVE_ALSA
  printf(" ALSA");
#endif
#ifdef HAVE_JACK
  printf(" JACK");
#endif
#ifndef NDEBUG
  printf(" DEBUG");
#endif
#ifdef HAVE_SAMPLERATE
  printf(" resample");
#endif
  putchar('\n');

#ifdef PLUGINS_LIST
  printf("  Decoder plugins:%s\n", PLUGINS_LIST);
#endif

  rc = uname(&uts);
  if (rc == 0)
  {
    printf("       Running on: %s %s %s\n", uts.sysname, uts.release,
           uts.machine);
  }

  printf("           Author: DeltaResero (Original MOC by Damian Pietras)\n");
  printf("         Homepage: %s\n", PACKAGE_URL);
  printf("      Bug reports: %s\n", PACKAGE_BUGREPORT);
  printf("        Copyright: (C) 2025-2026 DeltaResero\n");
  printf("                   (C) 2003-2026 Damian Pietras and others\n");
  printf("          License: GNU General Public License, version 3 or later\n");
  putchar('\n');
}

/* Show program banner. */
static void show_banner()
{
  printf("%s (version %s", PACKAGE_NAME, PACKAGE_VERSION);
#ifdef PACKAGE_REVISION
  printf(", revision %s", PACKAGE_REVISION);
#endif
  printf(")\n");
}

static const char mocf_summary[] = "[OPTIONS] [FILE|DIR ...]";

/* Show program usage. */
static void show_usage(poptContext ctx)
{
  show_banner();
  poptSetOtherOptionHelp(ctx, mocf_summary);
  poptPrintUsage(ctx, stdout, 0);
}

/* Show program help. */
static void show_help(poptContext ctx)
{
  size_t ix;

  show_banner();
  poptSetOtherOptionHelp(ctx, mocf_summary);
  poptPrintHelp(ctx, stdout, 0);

  printf("\nEnvironment variables:\n\n");
  for (ix = 0; ix < std::size(environment_variables); ix += 1)
  {
    printf("  %-34s%s\n", environment_variables[ix].name,
           environment_variables[ix].desc);
  }
  printf("\n");
}

/* Show POPT-interpreted command line arguments. */
static void show_args()
{
  if (mocf_argc > 0)
  {
    char *str;

    str = getenv("MOCF_POPTRC");
    if (str)
    {
      printf("MOCF_POPTRC='%s' ", str);
    }

    str = getenv("MOCF_OPTS");
    if (str)
    {
      printf("MOCF_OPTS='%s' ", str);
    }

    std::string cmdline_str = render_popt_command_line();
    printf("%s\n", cmdline_str.c_str());
  }
}

/* Disambiguate the user's request. */
static void show_misc_cb(poptContext ctx,
                         enum poptCallbackReason unused1 ATTR_UNUSED,
                         const struct poptOption *opt,
                         const char *unused2 ATTR_UNUSED,
                         void *unused3 ATTR_UNUSED)
{
  switch (opt->shortName)
  {
    case 'V':
      show_version();
      break;
    case 'h':
      show_help(ctx);
      break;
    case 0:
      if (!strcmp(opt->longName, "echo-args"))
      {
        show_args();
      }
      else if (!strcmp(opt->longName, "usage"))
      {
        show_usage(ctx);
      }
      break;
  }

  exit(EXIT_SUCCESS);
}

enum
{
  CL_HANDLED = 0,
  CL_SDRIVER,
  CL_MUSICDIR,
  CL_THEME,
  CL_SETOPTION,
  CL_MOCDIR,
  CL_ASCII
};

static struct parameters params;

static struct poptOption general_opts[] = {
#ifndef NDEBUG
    {"debug", 'D', POPT_ARG_NONE, &params.debug, CL_HANDLED,
     "Turn on logging to a file", nullptr},
#endif
    {"moc-dir", 'M', POPT_ARG_STRING, nullptr, CL_MOCDIR,
     "Use the specified MOC directory instead of the default", "DIR"},
    {"music-dir", 'm', POPT_ARG_NONE, nullptr, CL_MUSICDIR, "Start in MusicDir",
     nullptr},
    {"config", 'C', POPT_ARG_STRING, &params.config_file, CL_HANDLED,
     "Use the specified config file instead of the default"
     " (conflicts with '--no-config')",
     "FILE"},
    {"no-config", 0, POPT_ARG_NONE, &params.no_config_file, CL_HANDLED,
     "Use program defaults rather than any config file"
     " (conflicts with '--config')",
     nullptr},
    {"set-option", 'O', POPT_ARG_STRING, nullptr, CL_SETOPTION,
     "Override the configuration option NAME with VALUE", "'NAME=VALUE'"},
    {"sound-driver", 'R', POPT_ARG_STRING, nullptr, CL_SDRIVER,
     "Use the first valid sound driver", "DRIVERS"},
    {"ascii", 'A', POPT_ARG_NONE, nullptr, CL_ASCII,
     "Use ASCII characters to draw lines", nullptr},
    {"theme", 'T', POPT_ARG_STRING, nullptr, CL_THEME,
     "Use the selected theme file (read from ~/.moc/themes if the path is not "
     "absolute)",
     "FILE"},
    POPT_TABLEEND};

static struct poptOption misc_opts[] = {
    {nullptr, 0, POPT_ARG_CALLBACK,
     reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(show_misc_cb)), 0,
     nullptr, nullptr},
    {"version", 'V', POPT_ARG_NONE, nullptr, 0, "Print version information", nullptr},
    {"echo-args", 0, POPT_ARG_NONE, nullptr, 0, "Print POPT-interpreted arguments",
     nullptr},
    {"usage", 0, POPT_ARG_NONE, nullptr, 0, "Print brief usage", nullptr},
    {"help", 'h', POPT_ARG_NONE, nullptr, 0, "Print extended usage", nullptr},
    POPT_TABLEEND};

static struct poptOption mocf_opts[] = {
    {nullptr, 0, POPT_ARG_INCLUDE_TABLE, general_opts, 0,
     "Options:", nullptr},
    {nullptr, 0, POPT_ARG_INCLUDE_TABLE, misc_opts, 0,
     "Miscellaneous options:", nullptr},
    POPT_AUTOALIAS POPT_TABLEEND};

/* Read the POPT configuration files as given in MOCF_POPTRC. */
static void read_mocf_poptrc(poptContext ctx, const char *env_poptrc)
{
  std::vector<std::string> files;
  std::string buf(env_poptrc);
  size_t start = 0;

  while (start <= buf.size())
  {
    size_t end = buf.find(':', start);
    std::string fn = (end == std::string::npos) ? buf.substr(start)
                                                 : buf.substr(start, end - start);
    start = (end == std::string::npos) ? buf.size() + 1 : end + 1;

    if (fn.empty())
    {
      continue;
    }

    if (!is_secure(fn.c_str()))
    {
      fatal("POPT config file is not secure: %s", fn.c_str());
    }

    int rc = poptReadConfigFile(ctx, fn.c_str());
    if (rc < 0)
    {
      fatal("Error reading POPT config file '%s': %s", fn.c_str(),
            poptStrerror(rc));
    }
  }
}

/* Check that the ~/.popt file is secure. */
static void check_popt_secure()
{
  const char *home;
  const char dot_popt[] = ".popt";

  home = get_home();
  std::string home_popt = std::string(home) + "/" + dot_popt;
  if (!is_secure(home_popt.c_str()))
  {
    fatal("POPT config file is not secure: %s", home_popt.c_str());
  }
}

/* Read the default POPT configuration file. */
static void read_default_poptrc(poptContext ctx)
{
  int rc;

  check_popt_secure();
  rc = poptReadDefaultConfig(ctx, 0);

  if (rc == POPT_ERROR_ERRNO)
  {
    int saved_errno = errno;

    fprintf(stderr,
            "\n"
            "WARNING: The following fatal error message may be bogus!\n"
            "         If you have an empty /etc/popt.d directory, try\n"
            "         adding an empty file to it.  If that does not fix\n"
            "         the problem then you have a genuine error.\n");

    errno = saved_errno;
  }

  if (rc != 0)
  {
    fatal("Error reading default POPT config file: %s", poptStrerror(rc));
  }
}

/* Read the POPT configuration files(s). */
static void read_popt_config(poptContext ctx)
{
  const char *env_poptrc;

  env_poptrc = getenv("MOCF_POPTRC");
  if (env_poptrc)
  {
    read_mocf_poptrc(ctx, env_poptrc);
  }
  else
  {
    read_default_poptrc(ctx);
  }
}

/* Prepend MOCF_OPTS to the command line. */
static void prepend_mocf_opts(poptContext ctx)
{
  int rc;
  const char *env_opts;

  env_opts = getenv("MOCF_OPTS");
  if (env_opts && strlen(env_opts))
  {
    int env_argc;
    const char **env_argv;

    rc = poptParseArgvString(env_opts, &env_argc, &env_argv);
    if (rc < 0)
    {
      fatal("Error parsing MOCF_OPTS: %s", poptStrerror(rc));
    }
    std::unique_ptr<char *, decltype(&::free)> env_argv_guard(
        const_cast<char **>(env_argv), &::free);

    rc = poptStuffArgs(ctx, env_argv);
    if (rc < 0)
    {
      fatal("Error prepending MOCF_OPTS: %s", poptStrerror(rc));
    }
  }
}

static const struct poptOption specials[] = {
    POPT_AUTOHELP POPT_AUTOALIAS POPT_TABLEEND};

/* Return true iff 'opt' is a POPT AutoHelp option. */
static inline bool is_autohelp(const struct poptOption *opt)
{
  const struct poptOption *autohelp = &specials[0];

  return opt->argInfo == autohelp->argInfo && opt->arg == autohelp->arg;
}

/* Return true iff 'opt' is a POPT AutoAlias option. */
static inline bool is_autoalias(const struct poptOption *opt)
{
  const struct poptOption *autoalias = &specials[1];

  return opt->argInfo == autoalias->argInfo && opt->arg == autoalias->arg;
}

/* Return true iff 'opt' is the POPT end-of-table marker. */
static inline bool is_tableend(const struct poptOption *opt)
{
  const struct poptOption *tableend = &specials[2];

  return opt->longName == tableend->longName &&
         opt->shortName == tableend->shortName && opt->arg == tableend->arg;
}

/* Return a copy of the POPT option table structure which is suitable
 * for rendering the POPT expansions of the command line. */
struct poptOption *clone_popt_options(struct poptOption *opts)
{
  size_t tally, ix, iy = 0;
  struct poptOption *result;

  assert(opts);

  for (tally = 1; !is_tableend(&opts[tally - 1]); tally += 1)
    ;

  result = new poptOption[tally]();

  for (ix = 0; ix < tally; ix += 1)
  {
    if (opts[ix].argInfo == POPT_ARG_CALLBACK)
    {
      continue;
    }

    if (is_autohelp(&opts[ix]))
    {
      continue;
    }

    if (is_autoalias(&opts[ix]))
    {
      continue;
    }

    memcpy(&result[iy], &opts[ix], sizeof(struct poptOption));

    if (is_tableend(&opts[ix]))
    {
      continue;
    }

    if (opts[ix].argInfo == POPT_ARG_INCLUDE_TABLE)
    {
      result[iy++].arg = clone_popt_options(static_cast<struct poptOption *>(opts[ix].arg));
      continue;
    }

    switch (result[iy].argInfo)
    {
      case POPT_ARG_STRING:
      case POPT_ARG_INT:
      case POPT_ARG_LONG:
      case POPT_ARG_FLOAT:
      case POPT_ARG_DOUBLE:
        result[iy].argInfo = POPT_ARG_STRING;
        break;
      case POPT_ARG_VAL:
        result[iy].argInfo = POPT_ARG_NONE;
        break;
      case POPT_ARG_NONE:
        break;
      default:
        fatal("Unknown POPT option table argInfo type: %d", result[iy].argInfo);
    }

    result[iy].arg = nullptr;
    result[iy++].val = popt_next_val++;
  }

  return result;
}

/* Free a copied POPT option table structure. */
void free_popt_clone(struct poptOption *opts)
{
  int ix;

  assert(opts);

  for (ix = 0; !is_tableend(&opts[ix]); ix += 1)
  {
    if (opts[ix].argInfo == POPT_ARG_INCLUDE_TABLE)
    {
      free_popt_clone(static_cast<struct poptOption *>(opts[ix].arg));
    }
  }

  delete[] opts;
}

/* Return a pointer to the copied POPT option table entry for which the
 * 'val' field matches 'wanted'.  */
struct poptOption *find_popt_option(struct poptOption *opts, int wanted)
{
  assert(opts);
  assert(in_range(wanted, popt_next_val));

  for (size_t ix = 0; !is_tableend(&opts[ix]); ix += 1)
  {
    struct poptOption *result;

    assert(opts[ix].argInfo != POPT_ARG_CALLBACK);

    if (opts[ix].val == wanted)
    {
      return &opts[ix];
    }

    switch (opts[ix].argInfo)
    {
      case POPT_ARG_INCLUDE_TABLE:
        result = find_popt_option(static_cast<struct poptOption *>(opts[ix].arg), wanted);
        if (result)
        {
          return result;
        }
        break;
      case POPT_ARG_STRING:
      case POPT_ARG_INT:
      case POPT_ARG_LONG:
      case POPT_ARG_FLOAT:
      case POPT_ARG_DOUBLE:
      case POPT_ARG_VAL:
      case POPT_ARG_NONE:
        break;
      default:
        fatal("Unknown POPT option table argInfo type: %d", opts[ix].argInfo);
    }
  }

  return nullptr;
}

static std::string render_popt_command_line()
{
  int rc;
  std::vector<std::string> cmdline;
  std::string result;
  const char **rest;

  struct poptOption *null_opts = clone_popt_options(mocf_opts);
  std::unique_ptr<struct poptOption, decltype(&free_popt_clone)>
      null_opts_guard(null_opts, free_popt_clone);

  poptContext ctx = poptGetContext("mocf", mocf_argc, mocf_argv, null_opts,
                                   POPT_CONTEXT_NO_EXEC);
  std::unique_ptr<poptContext_s, decltype(&poptFreeContext)> ctx_guard(
      ctx, poptFreeContext);

  read_popt_config(ctx);
  prepend_mocf_opts(ctx);

  cmdline.reserve(mocf_argc * 2);
  cmdline.push_back(mocf_argv[0]);

  while (true)
  {
    std::string str;
    const char *arg;
    struct poptOption *opt;

    rc = poptGetNextOpt(ctx);
    if (rc == -1)
    {
      break;
    }

    if (rc == POPT_ERROR_BADOPT)
    {
      cmdline.push_back(poptBadOption(ctx, 0));
      continue;
    }

    opt = find_popt_option(null_opts, rc);
    if (!opt)
    {
      return "Couldn't find option in copied option table!";
    }

    arg = poptGetOptArg(ctx);
    std::unique_ptr<char, decltype(&::free)> arg_guard(
        const_cast<char *>(arg), &::free);

    if (opt->longName)
    {
      str = arg
            ? std::string("--") + opt->longName + "='" + arg + "'"
            : std::string("--") + opt->longName;
    }
    else
    {
      str = arg
            ? std::string("-") + opt->shortName + " '" + arg + "'"
            : std::string(1, '-') + opt->shortName;
    }

    cmdline.push_back(std::move(str));
  }

  rest = poptGetArgs(ctx);
  if (rest)
  {
    while (*rest)
    {
      cmdline.push_back(*rest++);
    }
  }

  {
    std::string joined;
    for (const auto &s : cmdline)
    {
      joined += s;
      joined += ' ';
    }
    result = std::move(joined);
  }

  return result;
}

static void override_config_option(const char *arg, std::vector<std::string> *deferred)
{
  assert(arg != nullptr);

  const char *ptr = strchr(arg, '=');
  if (ptr == nullptr)
    fatal("Malformed override option: %s", arg);

  /* Allow for list append operator ("+="). */
  bool append = (ptr > arg && *(ptr - 1) == '+');

  auto name_opt = trim(arg, static_cast<size_t>(ptr - arg - (append ? 1 : 0)));
  if (!name_opt || name_opt->empty())
    fatal("Malformed override option: %s", arg);

  const std::string &name = *name_opt;
  enum option_type type = options_get_type(name.c_str());

  if (type == OPTION_LIST)
  {
    if (deferred)
    {
      deferred->push_back(arg);
      return;
    }
  }
  else if (append)
  {
    fatal("Malformed override option: %s", arg);
  }

  auto value_opt = trim(ptr + 1, strlen(ptr + 1));
  if (!value_opt || value_opt->empty())
    fatal("Malformed override option: %s", arg);

  std::string value = std::move(*value_opt);

  if (value.front() == '\'' || value.front() == '"')
  {
    size_t len = value.size();
    if (value.front() != value.back() || len < 2)
      fatal("Malformed override option: %s", arg);
    value = value.substr(1, len - 2);
  }

  if (!options_set_pair(name.c_str(), value.c_str(), append))
    fatal("Malformed override option: %s", arg);

  options_ignore_config(name.c_str());
}

/* Process the command line options. */
static void process_options(poptContext ctx, std::vector<std::string> *deferred)
{
  int rc;

  while ((rc = poptGetNextOpt(ctx)) >= 0)
  {
    const char *arg;

    arg = poptGetOptArg(ctx);
    std::unique_ptr<char, decltype(&::free)> arg_guard(
        const_cast<char *>(arg), &::free);

    switch (rc)
    {
      case CL_SDRIVER:
        if (!options_check_list("SoundDriver", arg))
        {
          fatal("No such sound driver: %s", arg);
        }
        options_set_list("SoundDriver", arg, false);
        options_ignore_config("SoundDriver");
        break;
      case CL_MUSICDIR:
        options_set_bool("StartInMusicDir", true);
        options_ignore_config("StartInMusicDir");
        break;
      case CL_THEME:
        options_set_str("ForceTheme", arg);
        break;
      case CL_SETOPTION:
        override_config_option(arg, deferred);
        break;
      case CL_MOCDIR:
        options_set_path("MOCDir", arg);
        options_ignore_config("MOCDir");
        break;
      case CL_ASCII:
        options_set_bool("ASCIILines", true);
        options_ignore_config("ASCIILines");
        break;
      default:
        show_usage(ctx);
        exit(EXIT_FAILURE);
    }
  }

  if (rc < -1)
  {
    const char *opt, *alias;

    opt = poptBadOption(ctx, 0);
    alias = poptBadOption(ctx, POPT_BADOPTION_NOALIAS);

    /* poptBadOption() with POPT_BADOPTION_NOALIAS fails to
     * return the correct option if poptStuffArgs() was used. */
    if (!strcmp(opt, alias) || getenv("MOCF_OPTS"))
    {
      fatal("%s: %s", opt, poptStrerror(rc));
    }
    else
    {
      fatal("%s (aliased by %s): %s", opt, alias, poptStrerror(rc));
    }
  }

  if (params.config_file && params.no_config_file)
  {
    fatal("Conflicting --config and --no-config options given!");
  }
}

/* Process the command line options and arguments. */
static std::vector<std::string> process_command_line(std::vector<std::string> *deferred)
{
  const char **rest;
  poptContext ctx;
  std::vector<std::string> result;

  assert(deferred != nullptr);

  ctx = poptGetContext("mocf", mocf_argc, mocf_argv, mocf_opts, 0);

  read_popt_config(ctx);
  prepend_mocf_opts(ctx);
  process_options(ctx, deferred);

  rest = poptGetArgs(ctx);
  if (rest)
  {
    while (*rest)
    {
      result.push_back(*rest++);
    }
  }

  poptFreeContext(ctx);

  return result;
}

static void process_deferred_overrides(std::vector<std::string> &deferred)
{
  bool cleared;
  const std::string marker = "*Marker*";

  std::vector<std::string> &decoders_option = options_get_list("PreferredDecoders");

  std::reverse(decoders_option.begin(), decoders_option.end());
  std::vector<std::string> config_decoders = decoders_option;
  decoders_option.clear();
  decoders_option.push_back(marker);

  for (const auto &item : deferred)
  {
    override_config_option(item.c_str(), nullptr);
  }

  cleared = decoders_option.empty() || decoders_option[0] != marker;
  std::reverse(decoders_option.begin(), decoders_option.end());
  if (!cleared)
  {
    decoders_option.pop_back();
    std::vector<std::string> override_decoders = decoders_option;
    decoders_option = config_decoders;
    decoders_option.insert(decoders_option.end(), override_decoders.begin(),
                           override_decoders.end());
  }
}

static void log_environment_variables()
{
#ifndef NDEBUG
  size_t ix;

  for (ix = 0; ix < std::size(environment_variables); ix += 1)
  {
    char *str;

    str = getenv(environment_variables[ix].name);
    if (str)
    {
      logit("%s='%s'", environment_variables[ix].name, str);
    }
  }
#endif
}

/* Log the command line which launched MOC. */
static void log_command_line()
{
#ifndef NDEBUG
  if (mocf_argc > 0)
  {
    std::string str;
    for (int ix = 0; ix < mocf_argc; ix += 1)
    {
      str += mocf_argv[ix];
      str += ' ';
    }
    logit("%s", str.c_str());
  }
  else
  {
    logit("No command line available");
  }
#endif
}

/* Log the command line as interpreted by POPT. */
static void log_popt_command_line()
{
#ifndef NDEBUG
  if (mocf_argc > 0)
  {
    std::string str = render_popt_command_line();
    logit("%s", str.c_str());
  }
#endif
}

int main(int argc, const char *argv[])
{
  try {
#if defined(__GLIBC__)
  /* Keep resident memory tracking the currently-playing file instead of
   * ratcheting up track after track.  glibc's mmap threshold is dynamic by
   * default: after freeing the large buffers a module decode allocates (a
   * module's pattern and sample data), the threshold
   * climbs, so subsequent large allocations are served from the sbrk arena
   * (never returned to the OS) rather than mmap (returned on free).  Pinning
   * the threshold keeps those large, short-lived allocations on mmap; a tight
   * trim threshold and a single arena keep the footprint close to what is
   * actually in use and both of which matter on low RAM targets */
  mallopt(M_MMAP_THRESHOLD, 128 * 1024);
  mallopt(M_TRIM_THRESHOLD, 128 * 1024);
  mallopt(M_ARENA_MAX, 1);
#endif

  std::vector<std::string> deferred_overrides, args;

  assert(argc >= 0);
  assert(argv != nullptr);
  assert(argv[argc] == nullptr);

  mocf_argc = argc;
  mocf_argv = argv;

#ifdef PACKAGE_REVISION
  logit("This is %s (revision %s)", PACKAGE_NAME, PACKAGE_REVISION);
#else
  logit("This is %s (version %s)", PACKAGE_NAME, PACKAGE_VERSION);
#endif

#ifdef CONFIGURATION
  logit("Configured:%s", CONFIGURATION);
#endif

#if !defined(NDEBUG)
  {
    int rc;
    struct utsname uts;

    rc = uname(&uts);
    if (rc == 0)
    {
      logit("Running on: %s %s %s", uts.sysname, uts.release, uts.machine);
    }
  }
#endif

  files_init();

  if (get_home() == nullptr)
  {
    fatal("Could not determine user's home directory!");
  }

  memset(&params, 0, sizeof(params));
  options_init();

  /* set locale according to the environment variables */
  if (!setlocale(LC_ALL, ""))
  {
    logit("Could not set locale!");
  }

  log_environment_variables();
  log_command_line();
  args = process_command_line(&deferred_overrides);
  log_popt_command_line();

  if (!params.no_config_file)
  {
    std::string default_config_path;
    const char *config_path;
    if (params.config_file)
    {
      if (!can_read_file(params.config_file))
      {
        fatal("Configuration file is not readable: %s", params.config_file);
      }
      config_path = params.config_file;
    }
    else
    {
      default_config_path = create_file_name("config");
      config_path = default_config_path.c_str();
    }
    options_parse(config_path);
  }

  process_deferred_overrides(deferred_overrides);

  check_moc_dir();

  io_init();
  decoder_init(params.debug);
  srand(time(nullptr));

  start_moc(&params, args);

  options_free();
  decoder_cleanup();
  io_cleanup();
  files_cleanup();
  common_cleanup();

  return EXIT_SUCCESS;
  } catch (const FatalException& e) {
    return EXIT_FATAL;
  }
}

// EOF
