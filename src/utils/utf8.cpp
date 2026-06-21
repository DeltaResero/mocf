// src/utils/utf8.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2005,2006 Damian Pietras <daper@daper.net>
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
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>
#include <string>

#ifdef HAVE_ICONV
#include <iconv.h>
#endif
#ifdef HAVE_NL_TYPES_H
#include <nl_types.h>
#endif
#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif

#if defined HAVE_NCURSESW_CURSES_H
#include <ncursesw/curses.h>
#elif defined HAVE_NCURSESW_H
#include <ncursesw.h>
#elif defined HAVE_NCURSES_CURSES_H
#include <ncurses/curses.h>
#elif defined HAVE_NCURSES_H
#include <ncurses.h>
#elif defined HAVE_CURSES_H
#include <curses.h>
#endif

#include "core/common.h"
#include "core/log.h"
#include "core/options.h"
#include "utils/utf8.h"

static char *terminal_charset = nullptr;
static int using_utf8 = 0;

static iconv_t iconv_desc = (iconv_t)(-1);
static iconv_t files_iconv_desc = (iconv_t)(-1);
static iconv_t xterm_iconv_desc = (iconv_t)(-1);

/* Return a string converted using iconv().
 * If for_file_name is not 0, use the conversion defined for file names.
 * For nullptr returns empty string. */
std::string iconv_str(const iconv_t desc, const char *str)
{
  if (!str)
  {
    return "";
  }
  if (desc == (iconv_t)(-1))
  {
    return str;
  }

  char buf[512];
  std::string str_copy = str;
#ifdef FREEBSD
  const char *inbuf = str_copy.c_str();
#else
  char *inbuf = &str_copy[0];
#endif
  size_t inbytesleft = str_copy.length();
  std::string result;

  iconv(desc, nullptr, nullptr, nullptr, nullptr);

  while (inbytesleft)
  {
    char *outbuf = buf;
    size_t outbytesleft = sizeof(buf) - 1;

    if (iconv(desc, &inbuf, &inbytesleft, &outbuf, &outbytesleft) ==
        static_cast<size_t>(-1))
    {
      if (errno == EILSEQ)
      {
        inbuf++;
        inbytesleft--;
        if (!--outbytesleft)
        {
          *outbuf = 0;
          result += buf;
          break;
        }
        *(outbuf++) = '#';
      }
      else if (errno == EINVAL)
      {
        *(outbuf++) = '#';
        *outbuf = 0;
        result += buf;
        break;
      }
      else if (errno == E2BIG)
      {
        *outbuf = 0;
        result += buf;
        continue;
      }
    }
    *outbuf = 0;
    result += buf;
  }

  return result;
}

std::string files_iconv_str(const char *str)
{
  return iconv_str(files_iconv_desc, str);
}

std::string xterm_iconv_str(const char *str)
{
  return iconv_str(xterm_iconv_desc, str);
}

int xwaddstr(WINDOW *win, const char *str)
{
  int res;

  if (using_utf8)
  {
    res = waddstr(win, str);
  }
  else
  {
    std::string lstr = iconv_str(iconv_desc, str);
    res = waddstr(win, lstr.c_str());
  }

  return res;
}

/* Convert multi-byte sequence to wide characters.  Change invalid UTF-8
 * sequences to '?'.  'dest' can be nullptr as in mbstowcs().
 * If 'invalid_char' is not nullptr it will be set to 1 if an invalid character
 * appears in the string, otherwise 0. */
static size_t xmbstowcs(wchar_t *dest, const char *src, size_t len,
                        int *invalid_char)
{
  mbstate_t ps;
  size_t count = 0;

  assert(src != nullptr);
  assert(!dest || len > 0);

  memset(&ps, 0, sizeof(ps));

  if (dest)
  {
    memset(dest, 0, len * sizeof(wchar_t));
  }

  if (invalid_char)
  {
    *invalid_char = 0;
  }

  while (src && (len || !dest))
  {
    size_t res;

    res = mbsrtowcs(dest, &src, len, &ps);
    if (res != static_cast<size_t>(-1))
    {
      count += res;
      src = nullptr;
    }
    else
    {
      size_t converted;

      src++;
      if (dest)
      {
        converted = wcslen(dest);
        dest += converted;
        count += converted;
        len -= converted;

        if (len > 1)
        {
          *dest = L'?';
          dest++;
          *dest = L'\0';
          len--;
        }
        else
        {
          *(dest - 1) = L'\0';
        }
      }
      else
      {
        count++;
      }
      memset(&ps, 0, sizeof(ps));

      if (invalid_char)
      {
        *invalid_char = 1;
      }
    }
  }

  return count;
}

int xwaddnstr(WINDOW *win, const char *str, const int n)
{
  int res, width, inv_char;
  size_t size, num_chars;

  assert(n > 0);
  assert(str != nullptr);

  std::string mstr = iconv_str(iconv_desc, str);

  size = xmbstowcs(nullptr, mstr.c_str(), -1, nullptr) + 1;
  std::vector<wchar_t> ucs(size);
  xmbstowcs(ucs.data(), mstr.c_str(), size, &inv_char);
  width = wcswidth(ucs.data(), WIDTH_MAX);

  if (width == -1)
  {
    size_t clidx;
    for (clidx = 0; clidx < size - 1; clidx++)
    {
      if (wcwidth(ucs[clidx]) == -1)
      {
        ucs[clidx] = L'?';
      }
    }
    width = wcswidth(ucs.data(), WIDTH_MAX);
    inv_char = 1;
  }

  if (width > n)
  {
    while (width > n)
    {
      width -= wcwidth(ucs[--size]);
    }
    ucs[size] = L'\0';
  }

  num_chars = wcstombs(nullptr, ucs.data(), 0);
  std::vector<char> lstr(num_chars + 1);

  if (inv_char)
  {
    wcstombs(lstr.data(), ucs.data(), num_chars + 1);
  }
  else
  {
    snprintf(lstr.data(), num_chars + 1, "%s", mstr.c_str());
  }

  res = waddstr(win, lstr.data());

  return res;
}

int xmvwaddstr(WINDOW *win, const int y, const int x, const char *str)
{
  int res;

  if (using_utf8)
  {
    res = mvwaddstr(win, y, x, str);
  }
  else
  {
    std::string lstr = iconv_str(iconv_desc, str);
    res = mvwaddstr(win, y, x, lstr.c_str());
  }

  return res;
}

int xmvwaddnstr(WINDOW *win, const int y, const int x, const char *str,
                const int n)
{
  int res;

  if (using_utf8)
  {
    res = mvwaddnstr(win, y, x, str, n);
  }
  else
  {
    std::string lstr = iconv_str(iconv_desc, str);
    res = mvwaddnstr(win, y, x, lstr.c_str(), n);
  }

  return res;
}

int xwprintw(WINDOW *win, const char *fmt, ...)
{
  va_list va;
  int res;
  std::string buf;

  va_start(va, fmt);
  buf = format_msg_va(fmt, va);
  va_end(va);

  if (using_utf8)
  {
    res = waddstr(win, buf.c_str());
  }
  else
  {
    std::string lstr = iconv_str(iconv_desc, buf.c_str());
    res = waddstr(win, lstr.c_str());
  }

  return res;
}

static void iconv_cleanup()
{
  if (iconv_desc != (iconv_t)(-1) && iconv_close(iconv_desc) == -1)
  {
    log_errno("iconv_close() failed", errno);
  }
}

void utf8_init()
{
#ifdef HAVE_NL_LANGINFO_CODESET
#ifdef HAVE_NL_LANGINFO
  terminal_charset = xstrdup(nl_langinfo(CODESET));
  assert(terminal_charset != nullptr);

  if (!strcmp(terminal_charset, "UTF-8"))
  {
#ifdef HAVE_NCURSESW
    logit("Using UTF8 output");
    using_utf8 = 1;
#else  /* HAVE_NCURSESW */
    terminal_charset = xstrdup("US-ASCII");
    logit("Using US-ASCII conversion - compiled without libncursesw");
#endif /* HAVE_NCURSESW */
  }
  else
  {
    logit("Terminal character set: %s", terminal_charset);
  }
#else  /* HAVE_NL_LANGINFO */
  terminal_charset = xstrdup("US-ASCII");
  logit("Assuming US-ASCII terminal character set");
#endif /* HAVE_NL_LANGINFO */
#endif /* HAVE_NL_LANGINFO_CODESET */

  if (!using_utf8 && terminal_charset)
  {
    iconv_desc = iconv_open(terminal_charset, "UTF-8");
    if (iconv_desc == (iconv_t)(-1))
    {
      log_errno("iconv_open() failed", errno);
    }
  }

  if (options_get_bool("FileNamesIconv"))
  {
    files_iconv_desc = iconv_open("UTF-8", "");
  }

  if (options_get_bool("NonUTFXterm"))
  {
    xterm_iconv_desc = iconv_open("", "UTF-8");
  }
}

void utf8_cleanup()
{
  if (terminal_charset)
  {
    free(terminal_charset);
  }
  iconv_cleanup();
}

/* Return the number of columns the string occupies when displayed. */
size_t strwidth(const char *s)
{
  size_t size;
  size_t width;

  assert(s != nullptr);

  size = xmbstowcs(nullptr, s, -1, nullptr) + 1;
  std::vector<wchar_t> ucs(size);
  xmbstowcs(ucs.data(), s, size, nullptr);
  width = wcswidth(ucs.data(), WIDTH_MAX);

  return width;
}

/* Return a string containing the tail of 'str' up to a
 * maximum of 'len' characters (in columns occupied on the screen). */
std::string xstrtail(const char *str, const int len)
{
  size_t size;
  int width;

  assert(str != nullptr);
  assert(len > 0);

  size = xmbstowcs(nullptr, str, -1, nullptr) + 1;
  std::vector<wchar_t> ucs(size);
  xmbstowcs(ucs.data(), str, size, nullptr);
  wchar_t *ucs_tail = ucs.data();

  width = wcswidth(ucs.data(), WIDTH_MAX);
  assert(width >= 0);

  while (width > len)
  {
    width -= wcwidth(*ucs_tail++);
  }

  size = wcstombs(nullptr, ucs_tail, 0) + 1;
  std::vector<char> tail_buf(size);
  wcstombs(tail_buf.data(), ucs_tail, size);

  return std::string(tail_buf.data());
}

// EOF
