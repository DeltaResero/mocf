// src/ui/curses/menu.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef MENU_H
#define MENU_H

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

#include "library/files.h"
#include <map>
#include <string>
#include <vector>
#include <memory>


  enum menu_request
  {
    REQ_UP,
    REQ_DOWN,
    REQ_PGUP,
    REQ_PGDOWN,
    REQ_TOP,
    REQ_BOTTOM
  };

  enum menu_align
  {
    MENU_ALIGN_RIGHT,
    MENU_ALIGN_LEFT
  };

#define FILE_TIME_STR_SZ 6
#define FILE_FORMAT_SZ 4

  struct menu_item
  {
    std::string title;     /* Title of the item */
    enum menu_align align; /* Align of the title */
    int num;               /* Position of the item starting from 0. */

    /* Curses attributes in different states: */
    int attr_normal;
    int attr_sel;
    int attr_marked;
    int attr_sel_marked;

    /* Associated file: */
    std::string file;
    enum file_type type;

    /* Additional information shown: */
    char time[FILE_TIME_STR_SZ];     /* File time string */
    char format[FILE_FORMAT_SZ];     /* File format */
    int queue_pos;                   /* Position in the queue */
  };

  struct menu
  {
    WINDOW *win;
    std::vector<std::unique_ptr<menu_item>> items;
    int top_idx;       /* index of first visible item */
    int selected_idx;  /* index of selected item */
    int marked_idx;    /* index of the marked item or -1 */

    /* position and size */
    int posx;
    int posy;
    int width;
    int height;

    /* Flags for displaying information about the file. */
    int show_time;
    bool show_format;

    int info_attr_normal; /* attributes for information about the file */
    int info_attr_sel;
    int info_attr_marked;
    int info_attr_sel_marked;
    int number_items; /* display item number (position) */

    std::map<std::string, struct menu_item*> search_tree; /* Map for searching by file name */
  };

  /* Menu state: relative (to the first item) positions of the top and selected
   * items. */
  struct menu_state
  {
    int top_item;
    int selected_item;
  };

  struct menu *menu_new(WINDOW *win, const int posx, const int posy,
                        const int width, const int height);
  struct menu_item *menu_add(struct menu *menu, const char *title,
                             const enum file_type type, const char *file);

  void menu_item_set_attr_normal(struct menu_item *mi, const int attr);
  void menu_item_set_attr_sel(struct menu_item *mi, const int attr);
  void menu_item_set_attr_sel_marked(struct menu_item *mi, const int attr);
  void menu_item_set_attr_marked(struct menu_item *mi, const int attr);

  void menu_item_set_time(struct menu_item *mi, const char *time);
  void menu_item_set_format(struct menu_item *mi, const char *format);
  void menu_item_set_queue_pos(struct menu_item *mi, const int pos);

  void menu_free(struct menu *menu);
  void menu_driver(struct menu *menu, const enum menu_request req);
  void menu_setcurritem_title(struct menu *menu, const char *title);
  void menu_setcurritem_file(struct menu *menu, const char *file);
  void menu_draw(const struct menu *menu, const int active);
  void menu_mark_item(struct menu *menu, const char *file);
  void menu_set_state(struct menu *menu, const struct menu_state *st);
  void menu_get_state(const struct menu *menu, struct menu_state *st);
  void menu_update_size(struct menu *menu, const int posx, const int posy,
                        const int width, const int height);
  void menu_unmark_item(struct menu *menu);
  struct menu *menu_filter_pattern(const struct menu *menu,
                                   const char *pattern);
  void menu_set_show_time(struct menu *menu, const int t);
  void menu_set_show_format(struct menu *menu, const bool t);
  void menu_set_info_attr_normal(struct menu *menu, const int attr);
  void menu_set_info_attr_sel(struct menu *menu, const int attr);
  void menu_set_info_attr_marked(struct menu *menu, const int attr);
  void menu_set_info_attr_sel_marked(struct menu *menu, const int attr);
  void menu_set_items_numbering(struct menu *menu, const int number);
  enum file_type menu_item_get_type(const struct menu_item *mi);
  const std::string &menu_item_get_file(const struct menu_item *mi);
  struct menu_item *menu_curritem(struct menu *menu);
  void menu_item_set_title(struct menu_item *mi, const char *title);
  int menu_nitems(const struct menu *menu);
  struct menu_item *menu_find(struct menu *menu, const char *fname);
  void menu_del_item(struct menu *menu, const char *fname);
  void menu_item_set_align(struct menu_item *mi, const enum menu_align align);
  int menu_is_visible(const struct menu *menu, const struct menu_item *mi);
  void menu_swap_items(struct menu *menu, const char *file1, const char *file2);
  void menu_make_visible(struct menu *menu, const char *file);
  void menu_set_cursor(const struct menu *m);


#endif

// EOF
