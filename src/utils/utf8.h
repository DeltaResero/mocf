// src/utils/utf8.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef UTF8_H
#define UTF8_H

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

#include <cstdarg>
#ifdef HAVE_ICONV
#include <iconv.h>
#endif


/* parameter passed to wcswidth() as a maximum width */
#define WIDTH_MAX 2048

  void utf8_init();
  void utf8_cleanup();
  int xwaddstr(WINDOW *win, const char *str);
  int xwaddnstr(WINDOW *win, const char *str, const int n);
  int xmvwaddstr(WINDOW *win, const int y, const int x, const char *str);
  int xmvwaddnstr(WINDOW *win, const int y, const int x, const char *str,
                  const int n);
  int xwprintw(WINDOW *win, const char *fmt, ...) ATTR_PRINTF(2, 3);
  std::size_t strwidth(const char *s);
  char *xstrtail(const char *str, const int len);
  char *iconv_str(const iconv_t desc, const char *str);
  char *files_iconv_str(const char *str);
  char *xterm_iconv_str(const char *str);


#endif

// EOF
