// src/core/log.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2004 Damian Pietras <daper@daper.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cassert>
#include <pthread.h>
#include <ctime>
#include <cerrno>
#include <csignal>

#include <vector>

#include "core/common.h"
#include "core/log.h"
#include "core/options.h"

#ifndef NDEBUG
static FILE *logfp = NULL; /* logging file stream */

static enum { UNINITIALISED, BUFFERING, LOGGING } logging_state = UNINITIALISED;

static std::vector<std::string> buffered_log;
static int log_records_spilt = 0;

static std::vector<std::string> circular_log;
static bool circular_log_enabled = false;
static int circular_ptr = 0;

static pthread_mutex_t logging_mtx = PTHREAD_MUTEX_INITIALIZER;

static struct
{
  int sig;
  const char *name;
  volatile uint64_t raised;
  uint64_t logged;
} sig_info[] = {{SIGINT, "SIGINT", 0, 0},     {SIGHUP, "SIGHUP", 0, 0},
                {SIGQUIT, "SIGQUIT", 0, 0},   {SIGTERM, "SIGTERM", 0, 0},
                {SIGCHLD, "SIGCHLD", 0, 0},
#ifdef SIGWINCH
                {SIGWINCH, "SIGWINCH", 0, 0},
#endif
                {0, "SIG other", 0, 0}};
#endif

#ifndef NDEBUG
void log_signal(int sig)
{
  int ix = 0;

  while (sig_info[ix].sig && sig_info[ix].sig != sig)
  {
    ix += 1;
  }

  sig_info[ix].raised += 1;
}
#endif

#ifndef NDEBUG
static inline void flush_log(void)
{
  int rc;

  if (logfp)
  {
    do
    {
      rc = fflush(logfp);
    } while (rc != 0 && errno == EINTR);
  }
}
#endif

#ifndef NDEBUG
static void locked_logit(const char *file, const int line, const char *function,
                         const char *msg)
{
  int len;
  char time_str[20];
  struct timespec utc_time;
  time_t tv_sec;
  struct tm tm_time;
  const char fmt[] = "%s.%06ld: %s:%d %s(): %s\n";

  assert(logging_state == BUFFERING || logging_state == LOGGING);
  assert(logging_state != BUFFERING || !logfp);
  assert(logging_state != BUFFERING || !circular_log_enabled);
  assert(logging_state != LOGGING || logfp || !circular_log_enabled);

  if (logging_state == LOGGING && !logfp)
  {
    return;
  }

  get_realtime(&utc_time);
  tv_sec = utc_time.tv_sec;
  localtime_r(&tv_sec, &tm_time);
  strftime(time_str, sizeof(time_str), "%b %e %T", &tm_time);

  if (logfp && !circular_log_enabled)
  {
    fprintf(logfp, fmt, time_str, utc_time.tv_nsec / 1000L, file, line,
            function, msg);
    return;
  }

  len = snprintf(NULL, 0, fmt, time_str, utc_time.tv_nsec / 1000L, file, line,
                 function, msg);
  std::string log_str(len + 1, '\0');
  snprintf(&log_str[0], len + 1, fmt, time_str, utc_time.tv_nsec / 1000L, file, line,
           function, msg);
  log_str.resize(len);

  if (logging_state == BUFFERING)
  {
    buffered_log.push_back(log_str);
    return;
  }

  assert(circular_log_enabled);

  if (circular_ptr == static_cast<int>(circular_log.capacity()))
  {
    circular_ptr = 0;
  }
  if (circular_ptr < static_cast<int>(circular_log.size()))
  {
    circular_log[circular_ptr] = log_str;
  }
  else
  {
    circular_log.push_back(log_str);
  }
  circular_ptr += 1;
}
#endif

#ifndef NDEBUG
static void log_signals_raised(void)
{
  size_t ix;

  for (ix = 0; ix < ARRAY_SIZE(sig_info); ix += 1)
  {
    while (sig_info[ix].raised > sig_info[ix].logged)
    {
      locked_logit(__FILE__, __LINE__, __func__, sig_info[ix].name);
      sig_info[ix].logged += 1;
    }
  }
}
#endif

/* Put something into the log.  If built with logging disabled,
 * this function is provided as a stub so independant plug-ins
 * configured with logging enabled can still resolve it. */
void internal_logit(const char *file LOGIT_ONLY, const int line LOGIT_ONLY,
                    const char *function LOGIT_ONLY,
                    const char *format LOGIT_ONLY, ...)
{
#ifndef NDEBUG
  int saved_errno = errno;
  std::string msg;
  va_list va;

  LOCK(logging_mtx);

  if (!logfp)
  {
    switch (logging_state)
    {
      case UNINITIALISED:
        buffered_log.reserve(128);
        logging_state = BUFFERING;
        break;
      case BUFFERING:
        /* Don't let storage run away on us. */
        if (buffered_log.size() < buffered_log.capacity())
        {
          break;
        }
        log_records_spilt += 1;
      case LOGGING:
        goto end;
    }
  }

  log_signals_raised();

  va_start(va, format);
  msg = format_msg_va(format, va);
  va_end(va);
  locked_logit(file, line, function, msg.c_str());

  flush_log();

  log_signals_raised();

end:
  UNLOCK(logging_mtx);

  errno = saved_errno;
#endif
}

void log_init_stream(FILE *f LOGIT_ONLY, const char *fn LOGIT_ONLY)
{
#ifndef NDEBUG
  std::string msg;

  LOCK(logging_mtx);

  logfp = f;

  if (logging_state == BUFFERING)
  {
    if (logfp)
    {
      for (const auto &entry : buffered_log)
      {
        fprintf(logfp, "%s", entry.c_str());
      }
    }
    buffered_log.clear();
    buffered_log.shrink_to_fit();
  }

  logging_state = LOGGING;
  if (!logfp)
  {
    goto end;
  }

  msg = format_msg("Writing log to: %s", fn);
  locked_logit(__FILE__, __LINE__, __func__, msg.c_str());

  if (log_records_spilt > 0)
  {
    msg = format_msg("%d log records spilt", log_records_spilt);
    locked_logit(__FILE__, __LINE__, __func__, msg.c_str());
  }

  flush_log();

end:
  UNLOCK(logging_mtx);
#endif
}

/* Start circular logging (if enabled). */
void log_circular_start()
{
#ifndef NDEBUG
  int circular_size;

  assert(logging_state == LOGGING);
  assert(!circular_log_enabled);

  if (!logfp)
  {
    return;
  }

  circular_size = options_get_int("CircularLogSize");
  if (circular_size > 0)
  {
    LOCK(logging_mtx);

    circular_log.reserve(circular_size);
    circular_log_enabled = true;
    circular_ptr = 0;

    UNLOCK(logging_mtx);
  }
#endif
}

/* Internal circular log reset. */
#ifndef NDEBUG
static inline void locked_circular_reset()
{
  circular_log.clear();
  circular_ptr = 0;
}
#endif

/* Reset the circular log (if enabled). */
void log_circular_reset()
{
#ifndef NDEBUG
  assert(logging_state == LOGGING);

  if (!circular_log_enabled)
  {
    return;
  }

  LOCK(logging_mtx);

  locked_circular_reset();

  UNLOCK(logging_mtx);
#endif
}

/* Write circular log (if enabled) to the log file. */
void log_circular_log()
{
#ifndef NDEBUG
  size_t ix;

  assert(logging_state == LOGGING && (logfp || !circular_log_enabled));

  if (!circular_log_enabled)
  {
    return;
  }

  LOCK(logging_mtx);

  fprintf(logfp, "\n* Circular Log Starts *\n\n");

  for (ix = circular_ptr; ix < circular_log.size(); ix += 1)
  {
    fprintf(logfp, "%s", circular_log[ix].c_str());
  }

  fflush(logfp);

  for (ix = 0; ix < static_cast<size_t>(circular_ptr); ix += 1)
  {
    fprintf(logfp, "%s", circular_log[ix].c_str());
  }

  fprintf(logfp, "\n* Circular Log Ends *\n\n");

  fflush(logfp);

  locked_circular_reset();

  UNLOCK(logging_mtx);
#endif
}

/* Stop circular logging (if enabled). */
void log_circular_stop()
{
#ifndef NDEBUG
  assert(logging_state == LOGGING);

  if (!circular_log_enabled)
  {
    return;
  }

  LOCK(logging_mtx);

  circular_log.clear();
  circular_log.shrink_to_fit();
  circular_log_enabled = false;
  circular_ptr = 0;

  UNLOCK(logging_mtx);
#endif
}

void log_close()
{
#ifndef NDEBUG
  LOCK(logging_mtx);

  if (!(logfp == stdout || logfp == stderr || logfp == NULL))
  {
    fclose(logfp);
    logfp = NULL;
  }

  buffered_log.clear();
  buffered_log.shrink_to_fit();

  log_records_spilt = 0;

  UNLOCK(logging_mtx);
#endif
}

// EOF
