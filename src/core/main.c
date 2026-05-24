// src/core/main.c
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <locale.h>
#include <assert.h>
#include <popt.h>

#include "core/common.h"
#include "core/server.h"
#include "ui/curses/interface.h"
#include "core/options.h"
#include "core/protocol.h"
#include "core/log.h"
#include "audio/decoder.h"
#include "utils/lists.h"
#include "library/files.h"

static int mocf_argc;
static const char **mocf_argv;
static int popt_next_val = 1;
static char *render_popt_command_line();

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
  char *dir_name = create_file_name("");
  struct stat file_stat;

  /* strip trailing slash */
  dir_name[strlen(dir_name) - 1] = 0;

  if (stat(dir_name, &file_stat) == -1)
  {
    if (errno != ENOENT)
    {
      fatal("Error trying to check for " CONFIG_DIR " directory: %s",
            xstrerror(errno));
    }

    if (mkdir(dir_name, 0700) == -1)
    {
      fatal("Can't create directory %s: %s", dir_name, xstrerror(errno));
    }
  }
  else
  {
    if (!S_ISDIR(file_stat.st_mode) || access(dir_name, W_OK))
    {
      fatal("%s is not a writable directory!", dir_name);
    }
  }
}

struct server_thread_args {
  struct engine_event_queue *eq;
  int debug;
  int foreground;
};

static void *server_thread_func(void *arg)
{
  struct server_thread_args *args = (struct server_thread_args *)arg;
  set_me_server();
  server_init(args->eq, args->debug, args->foreground);
  server_loop();
  free(args);
  return NULL;
}

/* Run client and server in the same process. */
static void start_moc(const struct parameters *params, lists_t_strs *args)
{
  pthread_t server_thread;
  struct server_thread_args *th_args;
  struct engine_event_queue *eq;

  eq = engine_event_queue_new();

  th_args = (struct server_thread_args *)xmalloc(sizeof(*th_args));
  th_args->eq         = eq;
  th_args->debug      = params->debug;
  th_args->foreground = 1;

  if (pthread_create(&server_thread, NULL, server_thread_func, th_args) != 0)
  {
    fatal("pthread_create() failed: %s", xstrerror(errno));
  }

  xsignal(SIGPIPE, SIG_IGN);

  /* Block until the engine thread has finished initialising. */
  engine_wait_ready();

  init_interface(eq, params->debug, args);
  interface_loop();
  interface_end();

  /* engine_quit() was called by interface_end(); wait for the thread. */
  pthread_join(server_thread, NULL);

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
  for (ix = 0; ix < ARRAY_SIZE(environment_variables); ix += 1)
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

    str = render_popt_command_line();
    printf("%s\n", str);
    free(str);
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
     "Turn on logging to a file", NULL},
#endif
    {"moc-dir", 'M', POPT_ARG_STRING, NULL, CL_MOCDIR,
     "Use the specified MOC directory instead of the default", "DIR"},
    {"music-dir", 'm', POPT_ARG_NONE, NULL, CL_MUSICDIR, "Start in MusicDir",
     NULL},
    {"config", 'C', POPT_ARG_STRING, &params.config_file, CL_HANDLED,
     "Use the specified config file instead of the default"
     " (conflicts with '--no-config')",
     "FILE"},
    {"no-config", 0, POPT_ARG_NONE, &params.no_config_file, CL_HANDLED,
     "Use program defaults rather than any config file"
     " (conflicts with '--config')",
     NULL},
    {"set-option", 'O', POPT_ARG_STRING, NULL, CL_SETOPTION,
     "Override the configuration option NAME with VALUE", "'NAME=VALUE'"},
    {"sound-driver", 'R', POPT_ARG_STRING, NULL, CL_SDRIVER,
     "Use the first valid sound driver", "DRIVERS"},
    {"ascii", 'A', POPT_ARG_NONE, NULL, CL_ASCII,
     "Use ASCII characters to draw lines", NULL},
    {"theme", 'T', POPT_ARG_STRING, NULL, CL_THEME,
     "Use the selected theme file (read from ~/.moc/themes if the path is not "
     "absolute)",
     "FILE"},
    POPT_TABLEEND};

static struct poptOption misc_opts[] = {
    {NULL, 0, POPT_ARG_CALLBACK, (void *)(uintptr_t)show_misc_cb, 0, NULL,
     NULL},
    {"version", 'V', POPT_ARG_NONE, NULL, 0, "Print version information", NULL},
    {"echo-args", 0, POPT_ARG_NONE, NULL, 0, "Print POPT-interpreted arguments",
     NULL},
    {"usage", 0, POPT_ARG_NONE, NULL, 0, "Print brief usage", NULL},
    {"help", 'h', POPT_ARG_NONE, NULL, 0, "Print extended usage", NULL},
    POPT_TABLEEND};

static struct poptOption mocf_opts[] = {
    {NULL, 0, POPT_ARG_INCLUDE_TABLE, general_opts, 0,
     "Options:", NULL},
    {NULL, 0, POPT_ARG_INCLUDE_TABLE, misc_opts, 0,
     "Miscellaneous options:", NULL},
    POPT_AUTOALIAS POPT_TABLEEND};

/* Read the POPT configuration files as given in MOCF_POPTRC. */
static void read_mocf_poptrc(poptContext ctx, const char *env_poptrc)
{
  int ix, rc, count;
  lists_t_strs *files;

  files = lists_strs_new(4);
  count = lists_strs_split(files, env_poptrc, ":");

  for (ix = 0; ix < count; ix += 1)
  {
    const char *fn;

    fn = lists_strs_at(files, ix);
    if (!strlen(fn))
    {
      continue;
    }

    if (!is_secure(fn))
    {
      fatal("POPT config file is not secure: %s", fn);
    }

    rc = poptReadConfigFile(ctx, fn);
    if (rc < 0)
    {
      fatal("Error reading POPT config file '%s': %s", fn, poptStrerror(rc));
    }
  }

  lists_strs_free(files);
}

/* Check that the ~/.popt file is secure. */
static void check_popt_secure()
{
  int len;
  const char *home, dot_popt[] = ".popt";
  char *home_popt;

  home = get_home();
  len = strlen(home) + strlen(dot_popt) + 2;
  home_popt = xcalloc(len, sizeof(char));
  snprintf(home_popt, len, "%s/%s", home, dot_popt);
  if (!is_secure(home_popt))
  {
    fatal("POPT config file is not secure: %s", home_popt);
  }
  free(home_popt);
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

    rc = poptStuffArgs(ctx, env_argv);
    if (rc < 0)
    {
      fatal("Error prepending MOCF_OPTS: %s", poptStrerror(rc));
    }

    free(env_argv);
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

  result = xcalloc(tally, sizeof(struct poptOption));

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
      result[iy++].arg = clone_popt_options(opts[ix].arg);
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

    result[iy].arg = NULL;
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
      free_popt_clone(opts[ix].arg);
    }
  }

  free(opts);
}

/* Return a pointer to the copied POPT option table entry for which the
 * 'val' field matches 'wanted'.  */
struct poptOption *find_popt_option(struct poptOption *opts, int wanted)
{
  assert(opts);
  assert(LIMIT(wanted, popt_next_val));

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
        result = find_popt_option(opts[ix].arg, wanted);
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

  return NULL;
}

/* Render the command line as interpreted by POPT. */
static char *render_popt_command_line()
{
  int rc;
  lists_t_strs *cmdline;
  char *result;
  const char **rest;
  poptContext ctx;
  struct poptOption *null_opts;

  null_opts = clone_popt_options(mocf_opts);

  ctx = poptGetContext("mocf", mocf_argc, mocf_argv, null_opts,
                       POPT_CONTEXT_NO_EXEC);

  read_popt_config(ctx);
  prepend_mocf_opts(ctx);

  cmdline = lists_strs_new(mocf_argc * 2);
  lists_strs_append(cmdline, mocf_argv[0]);

  while (1)
  {
    size_t len;
    char *str;
    const char *arg;
    struct poptOption *opt;

    rc = poptGetNextOpt(ctx);
    if (rc == -1)
    {
      break;
    }

    if (rc == POPT_ERROR_BADOPT)
    {
      lists_strs_append(cmdline, poptBadOption(ctx, 0));
      continue;
    }

    opt = find_popt_option(null_opts, rc);
    if (!opt)
    {
      result = xstrdup("Couldn't find option in copied option table!");
      goto err;
    }

    arg = poptGetOptArg(ctx);

    if (opt->longName)
    {
      len = strlen(opt->longName) + 3;
      if (arg)
      {
        len += strlen(arg) + 3;
      }
      str = xmalloc(len);

      if (arg)
      {
        snprintf(str, len, "--%s='%s'", opt->longName, arg);
      }
      else
      {
        snprintf(str, len, "--%s", opt->longName);
      }
    }
    else
    {
      len = 3;
      if (arg)
      {
        len += strlen(arg) + 3;
      }
      str = xmalloc(len);

      if (arg)
      {
        snprintf(str, len, "-%c '%s'", opt->shortName, arg);
      }
      else
      {
        snprintf(str, len, "-%c", opt->shortName);
      }
    }

    lists_strs_push(cmdline, str);
    free((void *)arg);
  }

  rest = poptGetArgs(ctx);
  if (rest)
  {
    lists_strs_load(cmdline, rest);
  }

  result = lists_strs_fmt(cmdline, "%s ");

err:
  poptFreeContext(ctx);
  free_popt_clone(null_opts);
  lists_strs_free(cmdline);

  return result;
}

static void override_config_option(const char *arg, lists_t_strs *deferred)
{
  int len;
  bool append;
  const char *ptr;
  char *name, *value;
  enum option_type type;

  assert(arg != NULL);

  ptr = strchr(arg, '=');
  if (ptr == NULL)
  {
    goto error;
  }

  /* Allow for list append operator ("+="). */
  append = (ptr > arg && *(ptr - 1) == '+');

  name = trim(arg, ptr - arg - (append ? 1 : 0));
  if (!name || !name[0])
  {
    goto error;
  }
  type = options_get_type(name);

  if (type == OPTION_LIST)
  {
    if (deferred)
    {
      lists_strs_append(deferred, arg);
      free(name);
      return;
    }
  }
  else if (append)
  {
    goto error;
  }

  value = trim(ptr + 1, strlen(ptr + 1));
  if (!value || !value[0])
  {
    goto error;
  }

  if (value[0] == '\'' || value[0] == '"')
  {
    len = strlen(value);
    if (value[0] != value[len - 1])
    {
      goto error;
    }
    if (strlen(value) < 2)
    {
      goto error;
    }
    memmove(value, value + 1, len - 2);
    value[len - 2] = 0x00;
  }

  if (!options_set_pair(name, value, append))
  {
    goto error;
  }
  options_ignore_config(name);

  free(name);
  free(value);
  return;

error:
  fatal("Malformed override option: %s", arg);
}

/* Process the command line options. */
static void process_options(poptContext ctx, lists_t_strs *deferred)
{
  int rc;

  while ((rc = poptGetNextOpt(ctx)) >= 0)
  {
    const char *arg;

    arg = poptGetOptArg(ctx);

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

    free((void *)arg);
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
static lists_t_strs *process_command_line(lists_t_strs *deferred)
{
  const char **rest;
  poptContext ctx;
  lists_t_strs *result;

  assert(deferred != NULL);

  ctx = poptGetContext("mocf", mocf_argc, mocf_argv, mocf_opts, 0);

  read_popt_config(ctx);
  prepend_mocf_opts(ctx);
  process_options(ctx, deferred);

  result = lists_strs_new(4);
  rest = poptGetArgs(ctx);
  if (rest)
  {
    lists_strs_load(result, rest);
  }

  poptFreeContext(ctx);

  return result;
}

static void process_deferred_overrides(lists_t_strs *deferred)
{
  int ix;
  bool cleared;
  const char marker[] = "*Marker*";
  char **config_decoders;
  lists_t_strs *decoders_option;

  /* We need to shuffle the PreferredDecoders list into the
   * right order as we load any deferred overriding options. */

  decoders_option = options_get_list("PreferredDecoders");
  lists_strs_reverse(decoders_option);
  config_decoders = lists_strs_save(decoders_option);
  lists_strs_clear(decoders_option);
  lists_strs_append(decoders_option, marker);

  for (ix = 0; ix < lists_strs_size(deferred); ix += 1)
  {
    override_config_option(lists_strs_at(deferred, ix), NULL);
  }

  cleared = lists_strs_empty(decoders_option) ||
            strcmp(lists_strs_at(decoders_option, 0), marker) != 0;
  lists_strs_reverse(decoders_option);
  if (!cleared)
  {
    char **override_decoders;

    free(lists_strs_pop(decoders_option));
    override_decoders = lists_strs_save(decoders_option);
    lists_strs_clear(decoders_option);
    lists_strs_load(decoders_option, (const char **)config_decoders);
    lists_strs_load(decoders_option, (const char **)override_decoders);
    free(override_decoders);
  }
  free(config_decoders);
}

static void log_environment_variables()
{
#ifndef NDEBUG
  size_t ix;

  for (ix = 0; ix < ARRAY_SIZE(environment_variables); ix += 1)
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
  lists_t_strs *cmdline;
  char *str;

  cmdline = lists_strs_new(mocf_argc);
  if (lists_strs_load(cmdline, mocf_argv) > 0)
  {
    str = lists_strs_fmt(cmdline, "%s ");
  }
  else
  {
    str = xstrdup("No command line available");
  }
  logit("%s", str);
  free(str);
  lists_strs_free(cmdline);
#endif
}

/* Log the command line as interpreted by POPT. */
static void log_popt_command_line()
{
#ifndef NDEBUG
  if (mocf_argc > 0)
  {
    char *str;

    str = render_popt_command_line();
    logit("%s", str);
    free(str);
  }
#endif
}

int main(int argc, const char *argv[])
{
  lists_t_strs *deferred_overrides, *args;

  assert(argc >= 0);
  assert(argv != NULL);
  assert(argv[argc] == NULL);

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

  if (get_home() == NULL)
  {
    fatal("Could not determine user's home directory!");
  }

  memset(&params, 0, sizeof(params));
  options_init();
  deferred_overrides = lists_strs_new(4);

  /* set locale according to the environment variables */
  if (!setlocale(LC_ALL, ""))
  {
    logit("Could not set locale!");
  }

  log_environment_variables();
  log_command_line();
  args = process_command_line(deferred_overrides);
  log_popt_command_line();

  if (!params.no_config_file)
  {
    if (params.config_file)
    {
      if (!can_read_file(params.config_file))
      {
        fatal("Configuration file is not readable: %s", params.config_file);
      }
    }
    else
    {
      params.config_file = create_file_name("config");
    }
    options_parse(params.config_file);
  }

  process_deferred_overrides(deferred_overrides);
  lists_strs_free(deferred_overrides);
  deferred_overrides = NULL;

  check_moc_dir();

  io_init();
  decoder_init(params.debug);
  srand(time(NULL));

  start_moc(&params, args);

  lists_strs_free(args);
  options_free();
  decoder_cleanup();
  io_cleanup();
  files_cleanup();
  common_cleanup();

  return EXIT_SUCCESS;
}

// EOF
