// src/core/common.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2004 - 2005 Damian Pietras <daper@daper.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#undef malloc
#endif

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <strings.h>
#include <cctype>
#include <cassert>
#include <optional>
#include <unistd.h>
#include <ctime>
#include <sys/types.h>
#include <pwd.h>
#include <csignal>
#include <cerrno>
#include <string>
#include <mutex>
#include <algorithm>
#include "core/common.h"
#include "core/server.h"
#include "ui/curses/interface.h"
#include "ui/curses/interface_elements.h"
#include "core/log.h"
#include "core/options.h"

void internal_error(const char *file, int line, const char *function,
                    const char *format, ...)
{
  int saved_errno = errno;
  va_list va;
  std::string msg;

  va_start(va, format);
  msg = format_msg_va(format, va);
  va_end(va);

  server_error(file, line, function, msg.c_str());

  errno = saved_errno;
}

/* End program with a message. Use when an error occurs and we can't recover. */
void internal_fatal(const char *file LOGIT_ONLY, int line LOGIT_ONLY,
                    const char *function LOGIT_ONLY, const char *format, ...)
{
  va_list va;
  std::string msg;

  windows_reset();

  va_start(va, format);
  msg = format_msg_va(format, va);
  fprintf(stderr, "\nFATAL_ERROR: %s\n\n", msg.c_str());
#ifndef NDEBUG
  internal_logit(file, line, function, "FATAL ERROR: %s", msg.c_str());
#endif
  va_end(va);

  log_close();

  exit(EXIT_FATAL);
}

void *xmalloc(size_t size)
{
  void *p;

#ifndef HAVE_MALLOC
  size = std::max<size_t>(1, size);
#endif

  if ((p = malloc(size)) == nullptr)
  {
    fatal("Can't allocate memory!");
  }
  return p;
}

void *xcalloc(size_t nmemb, size_t size)
{
  void *p;

  if ((p = calloc(nmemb, size)) == nullptr)
  {
    fatal("Can't allocate memory!");
  }
  return p;
}

void *xrealloc(void *ptr, const size_t size)
{
  void *p;

  p = realloc(ptr, size);
  if (!p && size != 0)
  {
    fatal("Can't allocate memory!");
  }

  return p;
}

char *xstrdup(const char *s)
{
  char *n = nullptr;

  if (s)
  {
    n = strdup(s);
    if (n == nullptr)
    {
      fatal("Can't allocate memory!");
    }
  }

  return n;
}

/* Sleep for the specified number of 'ticks'. */
void xsleep(size_t ticks, size_t ticks_per_sec)
{
  assert(ticks_per_sec > 0);

  if (ticks > 0)
  {
    int rc;
    struct timespec delay = {.tv_sec = static_cast<time_t>(ticks)};

    if (ticks_per_sec > 1)
    {
      uint64_t nsecs;

      delay.tv_sec /= ticks_per_sec;
      nsecs = ticks % ticks_per_sec;

      if (nsecs > 0)
      {
        assert(nsecs < UINT64_MAX / UINT64_C(1000000000));

        delay.tv_nsec = nsecs * UINT64_C(1000000000);
        delay.tv_nsec /= ticks_per_sec;
      }
    }

    do
    {
      rc = nanosleep(&delay, &delay);
      if (rc == -1 && errno != EINTR)
      {
        fatal("nanosleep() failed: %s", xstrerror(errno).c_str());
      }
    } while (rc != 0);
  }
}

#if !HAVE_DECL_STRERROR_R
static std::mutex xstrerror_mtx;
#endif

#if !HAVE_DECL_STRERROR_R
/* Return error message as std::string (for strerror(3)). */
std::string xstrerror(int errnum)
{
  std::string result;

  std::lock_guard<std::mutex> lock(xstrerror_mtx);

  result = strerror(errnum);

  return result;
}
#endif

#if HAVE_DECL_STRERROR_R
/* Return error message as std::string (for strerror_r(3)). */
std::string xstrerror(int errnum)
{
  int saved_errno = errno;
  const char *err_str;
  char err_buf[256];

#ifdef STRERROR_R_CHAR_P
  /* strerror_r(3) is GNU variant. */
  err_str = strerror_r(errnum, err_buf, sizeof(err_buf));
#else
  /* strerror_r(3) is XSI variant. */
  if (strerror_r(errnum, err_buf, sizeof(err_buf)) < 0)
  {
    logit("Error %d occurred obtaining error description for %d", errno,
          errnum);
    strcpy(err_buf, "Error occurred obtaining error description");
  }
  err_str = err_buf;
#endif

  errno = saved_errno;

  return std::string(err_str);
}
#endif

/* A signal(2) which is both thread safe and POSIXly well defined. */
void xsignal(int signum, void (*func)(int))
{
  struct sigaction act;

  act.sa_handler = func;
  act.sa_flags = 0;
  sigemptyset(&act.sa_mask);

  if (sigaction(signum, &act, nullptr) == -1)
  {
    fatal("sigaction() failed: %s", xstrerror(errno).c_str());
  }
}

/* str_repl removed — had no call sites outside this translation unit. */

/* Extract a substring starting at 'src' for length 'len' and remove
 * any leading and trailing whitespace.  Return nullopt if unable.  */
std::optional<std::string> trim(const char *src, size_t len)
{
  const char *first, *last;

  for (last = &src[len - 1]; last >= src; last -= 1)
  {
    if (!isspace(static_cast<unsigned char>(*last)))
    {
      break;
    }
  }
  if (last < src)
  {
    return std::nullopt;
  }

  for (first = src; first <= last; first += 1)
  {
    if (!isspace(static_cast<unsigned char>(*first)))
    {
      break;
    }
  }
  if (first > last)
  {
    return std::nullopt;
  }

  last += 1;
  return std::string(first, static_cast<size_t>(last - first));
}

/* Format argument values according to 'format' and return it as a std::string. */
std::string format_msg(const char *format, ...)
{
  std::string result;
  va_list va;

  va_start(va, format);
  result = format_msg_va(format, va);
  va_end(va);

  return result;
}

/* Format a vararg list according to 'format' and return it as a std::string. */
std::string format_msg_va(const char *format, va_list va)
{
  int len;
  va_list va_copy;

  va_copy(va_copy, va);
  len = vsnprintf(nullptr, 0, format, va_copy);
  va_end(va_copy);

  std::string result(len + 1, '\0');
  vsnprintf(&result[0], len + 1, format, va);
  result.resize(len);

  return result;
}

/* Return true iff the argument would be a syntactically valid symbol.
 * (Note that the so-called "peculiar indentifiers" are disallowed here.) */
bool is_valid_symbol(const char *candidate)
{
  size_t len;
  bool result;
  const char *first = "+-.0123456789@";
  const char *valid = "abcdefghijklmnopqrstuvwxyz"
                      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                      "0123456789"
                      "@?!.+-*/<=>:$%^&_~";

  result = false;
  len = strlen(candidate);
  if (len > 0 && len == strspn(candidate, valid) &&
      strchr(first, candidate[0]) == nullptr)
  {
    result = true;
  }

  return result;
}

/* Return path to a file in MOC config directory. */
std::string create_file_name(const char *file)
{
  const char *moc_dir = options_get_str("MOCDir");
  std::string result = std::string(moc_dir) + "/" + file;

  if (result.size() >= PATH_MAX)
  {
    fatal("Path too long!");
  }

  return result;
}

int get_realtime(struct timespec *ts)
{
  int result;
#ifdef HAVE_CLOCK_GETTIME
  result = clock_gettime(CLOCK_REALTIME, ts);
#else
  struct timeval tv;

  result = gettimeofday(&tv, nullptr);
  if (result == 0)
  {
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000L;
  }
#endif
  return result;
}

/* Convert time in seconds to min:sec text format. */
std::string sec_to_min(int seconds)
{
  assert(seconds >= 0);

  char buff[32];

  if (seconds < 6000)
  {
    /* the time is less than 99:59 */
    int min = seconds / 60;
    int sec = seconds % 60;

    snprintf(buff, sizeof(buff), "%02d:%02d", min, sec);
  }
  else if (seconds < 10000 * 60)
  {
    /* the time is less than 9999 minutes */
    snprintf(buff, sizeof(buff), "%4dm", seconds / 60);
  }
  else
  {
    strcpy(buff, "!!!!!");
  }

  return std::string(buff);
}

/* Determine and return the path of the user's home directory. */
const char *get_home()
{
  static std::string home_str;
  static const char *home = nullptr;
  struct passwd *passwd;

  if (home == nullptr)
  {
    const char *env_home = getenv("HOME");
    if (env_home)
    {
      home_str = env_home;
      home = home_str.c_str();
    }
    else
    {
      errno = 0;
      passwd = getpwuid(geteuid());
      if (passwd)
      {
        home_str = passwd->pw_dir;
        home = home_str.c_str();
      }
      else if (errno != 0)
      {
        std::string err = xstrerror(errno);
        logit("getpwuid(%d): %s", geteuid(), err.c_str());
      }
    }
  }

  return home;
}

void common_cleanup()
{
}

// EOF
