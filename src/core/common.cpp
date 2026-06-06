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
#include <unistd.h>
#include <ctime>
#include <sys/types.h>
#include <pwd.h>
#include <pthread.h>
#include <csignal>
#include <cerrno>
#include <string>
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
  size = MAX(1, size);
#endif

  if ((p = malloc(size)) == NULL)
  {
    fatal("Can't allocate memory!");
  }
  return p;
}

void *xcalloc(size_t nmemb, size_t size)
{
  void *p;

  if ((p = calloc(nmemb, size)) == NULL)
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
  char *n = NULL;

  if (s)
  {
    n = strdup(s);
    if (n == NULL)
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
static pthread_mutex_t xstrerror_mtx = PTHREAD_MUTEX_INITIALIZER;
#endif

#if !HAVE_DECL_STRERROR_R
/* Return error message as std::string (for strerror(3)). */
std::string xstrerror(int errnum)
{
  std::string result;

  LOCK(xstrerror_mtx);

  result = strerror(errnum);

  UNLOCK(xstrerror_mtx);

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

  if (sigaction(signum, &act, 0) == -1)
  {
    fatal("sigaction() failed: %s", xstrerror(errno).c_str());
  }
}

char *str_repl(char *target, const char *oldstr, const char *newstr)
{
  size_t oldstr_len = strlen(oldstr);
  size_t newstr_len = strlen(newstr);
  size_t target_len = strlen(target);
  size_t target_max = target_len;
  size_t s, p;
  char *needle;

  for (s = 0; (needle = strstr(target + s, oldstr)) != NULL; s = p + newstr_len)
  {
    target_len += newstr_len - oldstr_len;
    p = needle - target;
    if (target_len + 1 > target_max)
    {
      target_max = MAX(target_len + 1, target_max * 2);
      target = static_cast<char *>(xrealloc(target, target_max));
    }
    memmove(target + p + newstr_len, target + p + oldstr_len,
            target_len - p - newstr_len + 1);
    memcpy(target + p, newstr, newstr_len);
  }

  target = static_cast<char *>(xrealloc(target, target_len + 1));

  return target;
}

/* Extract a substring starting at 'src' for length 'len' and remove
 * any leading and trailing whitespace.  Return NULL if unable.  */
char *trim(const char *src, size_t len)
{
  const char *first, *last;

  for (last = &src[len - 1]; last >= src; last -= 1)
  {
    if (!isspace(*last))
    {
      break;
    }
  }
  if (last < src)
  {
    return nullptr;
  }

  for (first = src; first <= last; first += 1)
  {
    if (!isspace(*first))
    {
      break;
    }
  }
  if (first > last)
  {
    return nullptr;
  }

  last += 1;
  size_t result_len = static_cast<size_t>(last - first);
  char *result = new char[result_len + 1];
  memcpy(result, first, result_len);
  result[result_len] = '\0';

  return result;
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

  std::string result(len, '\0');
  vsnprintf(&result[0], len + 1, format, va);

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
      strchr(first, candidate[0]) == NULL)
  {
    result = true;
  }

  return result;
}

/* Return path to a file in MOC config directory. NOT THREAD SAFE */
char *create_file_name(const char *file)
{
  int rc;
  static char fname[PATH_MAX];
  char *moc_dir = options_get_str("MOCDir");

  rc = snprintf(fname, sizeof(fname), "%s/%s", moc_dir, file);

  if (rc >= ssizeof(fname))
  {
    fatal("Path too long!");
  }

  return fname;
}

int get_realtime(struct timespec *ts)
{
  int result;
#ifdef HAVE_CLOCK_GETTIME
  result = clock_gettime(CLOCK_REALTIME, ts);
#else
  struct timeval tv;

  result = gettimeofday(&tv, NULL);
  if (result == 0)
  {
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000L;
  }
#endif
  return result;
}

/* Convert time in second to min:sec text format.
   'buff' must be at least 32 chars long. */
void sec_to_min(char *buff, const int seconds)
{
  assert(seconds >= 0);

  if (seconds < 6000)
  {
    /* the time is less than 99:59 */
    int min, sec;

    min = seconds / 60;
    sec = seconds % 60;

    snprintf(buff, 32, "%02d:%02d", min, sec);
  }
  else if (seconds < 10000 * 60)
  {
    /* the time is less than 9999 minutes */
    snprintf(buff, 32, "%4dm", seconds / 60);
  }
  else
  {
    strcpy(buff, "!!!!!");
  }
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

char *pathstrcpy(char *restrict dst, const char *restrict src)
{
  size_t len = strnlen(src, PATH_MAX);
  if (len == PATH_MAX)
  {
    fatal("Path too long!");
  }
  return static_cast<char *>(memcpy(dst, src, len + 1));
}

void common_cleanup()
{
#if !HAVE_DECL_STRERROR_R
  int rc;

  rc = pthread_mutex_destroy(&xstrerror_mtx);
  if (rc != 0)
  {
    logit("Can't destroy xstrerror_mtx: %s", strerror(rc));
  }
#endif
}

// EOF
