// src/ui/curses/menu.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2002 - 2006 Damian Pietras <daper@daper.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

#include "core/common.h"
#include "ui/curses/menu.h"

#include "utils/utf8.h"

/* Draw menu item on a given position from the top of the menu. */
static void draw_item(const struct menu *menu, const struct menu_item *mi,
                      const int pos, const int item_info_pos, int title_space,
                      const int number_space, const int draw_selected)
{
  int title_width, queue_pos_len = 0;
  int ix, x;
  int y ATTR_UNUSED; /* OpenBSD flags this as unused. */
  char buf[32];
  int title_attr, info_attr;

  assert(menu != nullptr);
  assert(mi != nullptr);
  assert(pos >= 0);
  assert(item_info_pos > menu->posx ||
         (!menu->show_time && !menu->show_format));
  assert(title_space > 0);
  assert(number_space == 0 || number_space >= 2);

  wmove(menu->win, pos, menu->posx);

  if (draw_selected && mi->num == menu->selected_idx && mi->num == menu->marked_idx)
  {
    title_attr = mi->attr_sel_marked;
    info_attr = menu->info_attr_sel_marked;
  }
  else if (draw_selected && mi->num == menu->selected_idx)
  {
    title_attr = mi->attr_sel;
    info_attr = menu->info_attr_sel;
  }
  else if (mi->num == menu->marked_idx)
  {
    title_attr = mi->attr_marked;
    info_attr = menu->info_attr_marked;
  }
  else
  {
    title_attr = mi->attr_normal;
    info_attr = menu->info_attr_normal;
  }

  if (number_space)
  {
    wattrset(menu->win, info_attr);
    xwprintw(menu->win, "%*d ", number_space - 1, mi->num + 1);
  }

  /* Set attributes */
  wattrset(menu->win, title_attr);

  /* Compute the length of the queue position if nonzero */
  if (mi->queue_pos)
  {
    snprintf(buf, sizeof(buf), "%d", mi->queue_pos);
    queue_pos_len = strlen(buf) + 2;
    title_space -= queue_pos_len;
  }

  title_width = strwidth(mi->title.c_str());

  getyx(menu->win, y, x);
  if (title_width <= title_space || mi->align == MENU_ALIGN_LEFT)
  {
    xwaddnstr(menu->win, mi->title.c_str(), title_space);
  }
  else
  {
    std::string tail = xstrtail(mi->title.c_str(), title_space);
    xwaddstr(menu->win, tail.c_str());
  }

  /* Fill the remainder of the title field with spaces. */
  if (mi->num == menu->selected_idx)
  {
    getyx(menu->win, y, ix);
    while (ix < x + title_space)
    {
      waddch(menu->win, ' ');
      ix += 1;
    }
  }

  /* Description. */
  wattrset(menu->win, info_attr);
  wmove(menu->win, pos, item_info_pos - queue_pos_len);

  /* Position in queue. */
  if (mi->queue_pos)
  {
    xwaddstr(menu->win, "[");
    xwaddstr(menu->win, buf);
    xwaddstr(menu->win, "]");
  }

  if ((menu->show_time && *mi->time) ||
      (menu->show_format && *mi->format))
  {
    bool first = true;
    xwprintw(menu->win, "[");

    if (menu->show_time)
    {
      if (!first)
      {
        xwprintw(menu->win, "|");
      }
      if (!strcmp(mi->time, "ERROR"))
      {
        wattrset(menu->win, mi->attr_normal);
        xwprintw(menu->win, "%*s", FILE_TIME_STR_SZ - 1, mi->time);
        wattrset(menu->win, info_attr);
      }
      else
      {
        xwprintw(menu->win, "%*s", FILE_TIME_STR_SZ - 1, mi->time);
      }
      first = false;
    }
    if (menu->show_format && *mi->format)
    {
      if (!first)
      {
        xwprintw(menu->win, "|");
      }
      xwprintw(menu->win, "%*s", FILE_FORMAT_SZ - 1, mi->format);
    }
    xwprintw(menu->win, "]");
  }
}

void menu_draw(const struct menu *menu, const int active)
{
  int title_width;
  int info_pos;
  int number_space = 0;
  int number_details = 0;

  assert(menu != nullptr);

  if (menu->number_items)
  {
    int count = menu->items.size() / 10;

    number_space = 2; /* begin from 1 digit and a space char */
    while (count)
    {
      count /= 10;
      number_space++;
    }
  }
  else
  {
    number_space = 0;
  }

  title_width = menu->width;

  if (menu->show_time)
  {
    ++number_details;
    title_width -= FILE_TIME_STR_SZ - 1; /* e.g. "00:00" */
  }
  if (menu->show_format)
  {
    ++number_details;
    title_width -= FILE_FORMAT_SZ - 1; /* e.g. "MP3" */
  }
  if (number_details)
  {
    title_width -= 2 /* brackets */ + number_details - 1 /* separators */;
  }

  info_pos = title_width;

  title_width -= number_space;

  for (int i = menu->top_idx; i < (int)menu->items.size() && i - menu->top_idx < menu->height; ++i)
  {
    draw_item(menu, menu->items[i].get(), i - menu->top_idx + menu->posy,
              menu->posx + info_pos, title_width, number_space, active);
  }
}

/* Move the cursor to the selected file. */
void menu_set_cursor(const struct menu *m)
{
  assert(m != nullptr);

  if (m->selected_idx >= 0 && m->selected_idx < (int)m->items.size())
  {
    wmove(m->win, m->selected_idx - m->top_idx + m->posy, m->posx);
  }
}



/* menu_items must be malloc()ed memory! */
struct menu *menu_new(WINDOW *win, const int posx, const int posy,
                      const int width, const int height)
{
  struct menu *menu;

  assert(win != nullptr);
  assert(posx >= 0);
  assert(posy >= 0);
  assert(width > 0);
  assert(height > 0);

  menu = new struct menu;

  menu->win = win;
  menu->top_idx = 0;
  menu->selected_idx = 0;
  menu->posx = posx;
  menu->posy = posy;
  menu->width = width;
  menu->height = height;
  menu->marked_idx = -1;
  menu->show_time = 0;
  menu->show_format = false;
  menu->info_attr_normal = A_NORMAL;
  menu->info_attr_sel = A_NORMAL;
  menu->info_attr_marked = A_NORMAL;
  menu->info_attr_sel_marked = A_NORMAL;
  menu->number_items = 0;


  return menu;
}

struct menu_item *menu_add(struct menu *menu, const char *title,
                           const enum file_type type, const char *file)
{
  assert(menu != nullptr);
  assert(title != nullptr);

  auto mi = std::make_unique<menu_item>();

  mi->title = title;
  mi->type = type;
  mi->file = file ? file : "";
  mi->num = menu->items.size();

  mi->attr_normal = A_NORMAL;
  mi->attr_sel = A_NORMAL;
  mi->attr_marked = A_NORMAL;
  mi->attr_sel_marked = A_NORMAL;
  mi->align = MENU_ALIGN_LEFT;

  mi->time[0] = 0;
  mi->format[0] = 0;
  mi->queue_pos = 0;

  menu_item *raw_mi = mi.get();
  menu->items.push_back(std::move(mi));

  if (file)
  {
    menu->search_tree[file] = raw_mi;
  }

  return raw_mi;
}

static struct menu_item *menu_add_from_item(struct menu *menu,
                                            const struct menu_item *mi)
{
  struct menu_item *new_item;

  assert(menu != nullptr);
  assert(mi != nullptr);

  new_item = menu_add(menu, mi->title.c_str(), mi->type,
                      mi->file.empty() ? nullptr : mi->file.c_str());

  new_item->attr_normal = mi->attr_normal;
  new_item->attr_sel = mi->attr_sel;
  new_item->attr_marked = mi->attr_marked;
  new_item->attr_sel_marked = mi->attr_sel_marked;

  memcpy(new_item->time, mi->time, sizeof(new_item->time));
  memcpy(new_item->format, mi->format, sizeof(new_item->format));

  return new_item;
}

void menu_update_size(struct menu *menu, const int posx, const int posy,
                      const int width, const int height)
{
  assert(menu != nullptr);
  assert(posx >= 0);
  assert(posy >= 0);
  assert(width > 0);
  assert(height > 0);

  menu->posx = posx;
  menu->posy = posy;
  menu->width = width;
  menu->height = height;

  if (menu->selected_idx >= menu->top_idx + menu->height)
  {
    menu->selected_idx = menu->top_idx + menu->height - 1;
    if (menu->selected_idx >= (int)menu->items.size())
    {
      menu->selected_idx = menu->items.empty() ? 0 : menu->items.size() - 1;
    }
  }
}

void menu_free(struct menu *menu)
{
  assert(menu != nullptr);

  menu->items.clear();
  menu->search_tree.clear();

  delete menu;
}

void menu_driver(struct menu *menu, const enum menu_request req)
{
  assert(menu != nullptr);

  if (menu->items.empty())
  {
    return;
  }

  if (req == REQ_DOWN && menu->selected_idx < (int)menu->items.size() - 1)
  {
    menu->selected_idx++;
    if (menu->selected_idx >= menu->top_idx + menu->height)
    {
      menu->top_idx = menu->selected_idx - menu->height / 2;
      if (menu->top_idx > (int)menu->items.size() - menu->height)
      {
        menu->top_idx = menu->items.size() - menu->height;
      }
    }
  }
  else if (req == REQ_UP && menu->selected_idx > 0)
  {
    menu->selected_idx--;
    if (menu->top_idx > menu->selected_idx)
    {
      menu->top_idx = menu->selected_idx - menu->height / 2;
    }
  }
  else if (req == REQ_PGDOWN && menu->selected_idx < (int)menu->items.size() - 1)
  {
    if (menu->selected_idx + menu->height - 1 < (int)menu->items.size() - 1)
    {
      menu->selected_idx += menu->height - 1;
      menu->top_idx += menu->height - 1;
      if (menu->top_idx > (int)menu->items.size() - menu->height)
      {
        menu->top_idx = menu->items.size() - menu->height;
      }
    }
    else
    {
      menu->selected_idx = menu->items.size() - 1;
      menu->top_idx = menu->items.size() - menu->height;
    }
  }
  else if (req == REQ_PGUP && menu->selected_idx > 0)
  {
    if (menu->selected_idx - menu->height + 1 > 0)
    {
      menu->selected_idx -= menu->height - 1;
      menu->top_idx -= menu->height - 1;
    }
    else
    {
      menu->selected_idx = 0;
      menu->top_idx = 0;
    }
  }
  else if (req == REQ_TOP)
  {
    menu->selected_idx = 0;
    menu->top_idx = 0;
  }
  else if (req == REQ_BOTTOM)
  {
    menu->selected_idx = menu->items.size() - 1;
    menu->top_idx = menu->selected_idx - menu->height + 1;
  }

  if (menu->top_idx < 0)
  {
    menu->top_idx = 0;
  }
}

/* Return the index of the currently selected item. */
struct menu_item *menu_curritem(struct menu *menu)
{
  assert(menu != nullptr);

  if (menu->items.empty() || menu->selected_idx < 0 || menu->selected_idx >= (int)menu->items.size())
  {
    return nullptr;
  }

  return menu->items[menu->selected_idx].get();
}

static void make_item_visible(struct menu *menu, struct menu_item *mi)
{
  assert(menu != nullptr);
  assert(mi != nullptr);

  if (mi->num < menu->top_idx || mi->num >= menu->top_idx + menu->height)
  {
    menu->top_idx = mi->num - menu->height / 2;

    if (menu->top_idx > (int)menu->items.size() - menu->height)
    {
      menu->top_idx = menu->items.size() - menu->height;
    }
    if (menu->top_idx < 0)
    {
      menu->top_idx = 0;
    }
  }

  if (menu->selected_idx < menu->top_idx ||
      menu->selected_idx >= menu->top_idx + menu->height)
  {
    menu->selected_idx = mi->num;
  }
}

/* Make this item selected */
static void menu_setcurritem(struct menu *menu, struct menu_item *mi)
{
  assert(menu != nullptr);
  assert(mi != nullptr);

  menu->selected_idx = mi->num;
  make_item_visible(menu, mi);
}

/* Make the item with this title selected. */
void menu_setcurritem_title(struct menu *menu, const char *title)
{
  /* Find it */
  for (int i = menu->top_idx; i < (int)menu->items.size(); ++i)
  {
    if (menu->items[i]->title == title)
    {
      menu_setcurritem(menu, menu->items[i].get());
      break;
    }
  }
}

static struct menu_item *menu_find_by_position(struct menu *menu, const int num)
{
  assert(menu != nullptr);

  if (num >= 0 && num < (int)menu->items.size())
  {
    return menu->items[num].get();
  }

  return nullptr;
}

void menu_set_state(struct menu *menu, const struct menu_state *st)
{
  assert(menu != nullptr);

  menu->selected_idx = st->selected_item;
  if (menu->selected_idx < 0 || menu->selected_idx >= (int)menu->items.size())
  {
    menu->selected_idx = menu->items.empty() ? 0 : menu->items.size() - 1;
  }

  menu->top_idx = st->top_item;
  if (menu->top_idx < 0 || menu->top_idx >= (int)menu->items.size())
  {
    menu->top_idx = menu->items.empty() ? 0 : menu->items.size() - menu->height;
    if (menu->top_idx < 0)
    {
      menu->top_idx = 0;
    }
  }
}

void menu_set_items_numbering(struct menu *menu, const int number)
{
  assert(menu != nullptr);

  menu->number_items = number;
}

void menu_get_state(const struct menu *menu, struct menu_state *st)
{
  assert(menu != nullptr);

  st->top_item = menu->top_idx;
  st->selected_item = menu->selected_idx;
}

void menu_unmark_item(struct menu *menu)
{
  assert(menu != nullptr);
  menu->marked_idx = -1;
}

/* Make a new menu from elements matching pattern. */
struct menu *menu_filter_pattern(const struct menu *menu, const char *pattern)
{
  struct menu *new_menu;

  assert(menu != nullptr);
  assert(pattern != nullptr);

  new_menu = menu_new(menu->win, menu->posx, menu->posy, menu->width, menu->height);
  menu_set_show_time(new_menu, menu->show_time);
  menu_set_show_format(new_menu, menu->show_format);
  menu_set_info_attr_normal(new_menu, menu->info_attr_normal);
  menu_set_info_attr_sel(new_menu, menu->info_attr_sel);
  menu_set_info_attr_marked(new_menu, menu->info_attr_marked);
  menu_set_info_attr_sel_marked(new_menu, menu->info_attr_sel_marked);

  for (const auto& mi : menu->items)
  {
    if (strcasestr(mi->title.c_str(), pattern))
    {
      menu_add_from_item(new_menu, mi.get());
    }
  }

  if (menu->marked_idx >= 0 && menu->marked_idx < (int)menu->items.size())
  {
    menu_mark_item(new_menu, menu->items[menu->marked_idx]->file.c_str());
  }

  return new_menu;
}

void menu_item_set_attr_normal(struct menu_item *mi, const int attr)
{
  assert(mi != nullptr);

  mi->attr_normal = attr;
}

void menu_item_set_attr_sel(struct menu_item *mi, const int attr)
{
  assert(mi != nullptr);

  mi->attr_sel = attr;
}

void menu_item_set_attr_sel_marked(struct menu_item *mi, const int attr)
{
  assert(mi != nullptr);

  mi->attr_sel_marked = attr;
}

void menu_item_set_attr_marked(struct menu_item *mi, const int attr)
{
  assert(mi != nullptr);

  mi->attr_marked = attr;
}

void menu_item_set_time(struct menu_item *mi, const char *time)
{
  assert(mi != nullptr);

  size_t len = strnlen(time, sizeof(mi->time) - 1);
  memcpy(mi->time, time, len);
  mi->time[len] = '\0';
}

void menu_item_set_format(struct menu_item *mi, const char *format)
{
  assert(mi != nullptr);
  assert(format != nullptr);

  size_t len = strnlen(format, sizeof(mi->format) - 1);
  memcpy(mi->format, format, len);
  mi->format[len] = '\0';
}
void menu_item_set_queue_pos(struct menu_item *mi, const int pos)
{
  assert(mi != nullptr);

  mi->queue_pos = pos;
}

void menu_set_show_time(struct menu *menu, const int t)
{
  assert(menu != nullptr);

  menu->show_time = t;
}

void menu_set_show_format(struct menu *menu, const bool t)
{
  assert(menu != nullptr);

  menu->show_format = t;
}

void menu_set_info_attr_normal(struct menu *menu, const int attr)
{
  assert(menu != nullptr);

  menu->info_attr_normal = attr;
}

void menu_set_info_attr_sel(struct menu *menu, const int attr)
{
  assert(menu != nullptr);

  menu->info_attr_sel = attr;
}

void menu_set_info_attr_marked(struct menu *menu, const int attr)
{
  assert(menu != nullptr);

  menu->info_attr_marked = attr;
}

void menu_set_info_attr_sel_marked(struct menu *menu, const int attr)
{
  assert(menu != nullptr);

  menu->info_attr_sel_marked = attr;
}

enum file_type menu_item_get_type(const struct menu_item *mi)
{
  assert(mi != nullptr);

  return mi->type;
}

const std::string &menu_item_get_file(const struct menu_item *mi)
{
  assert(mi != nullptr);

  return mi->file;
}

void menu_item_set_title(struct menu_item *mi, const char *title)
{
  assert(mi != nullptr);

  mi->title = title;
}

int menu_nitems(const struct menu *menu)
{
  assert(menu != nullptr);

  return menu->items.size();
}

struct menu_item *menu_find(struct menu *menu, const char *fname)
{
  assert(menu != nullptr);
  assert(fname != nullptr);

  auto it = menu->search_tree.find(fname);
  if (it == menu->search_tree.end())
  {
    return nullptr;
  }

  return it->second;
}

void menu_mark_item(struct menu *menu, const char *file)
{
  struct menu_item *item;

  assert(menu != nullptr);
  assert(file != nullptr);

  item = menu_find(menu, file);
  if (item)
  {
    menu->marked_idx = item->num;
  }
}

static void menu_renumber_items(struct menu *menu)
{
  assert(menu != nullptr);

  for (size_t i = 0; i < menu->items.size(); ++i)
  {
    menu->items[i]->num = i;
  }
}

static void menu_delete(struct menu *menu, struct menu_item *mi)
{
  assert(menu != nullptr);
  assert(mi != nullptr);

  int idx = mi->num;

  if (!mi->file.empty())
  {
    menu->search_tree.erase(mi->file);
  }

  menu->items.erase(menu->items.begin() + idx);
  menu_renumber_items(menu);

  if (menu->marked_idx == idx)
  {
    menu->marked_idx = -1;
  }
  else if (menu->marked_idx > idx)
  {
    menu->marked_idx--;
  }

  if (menu->selected_idx == idx)
  {
    if (menu->selected_idx >= (int)menu->items.size())
    {
      menu->selected_idx = menu->items.empty() ? 0 : menu->items.size() - 1;
    }
  }
  else if (menu->selected_idx > idx)
  {
    menu->selected_idx--;
  }

  if (menu->top_idx == idx)
  {
    if (menu->top_idx >= (int)menu->items.size())
    {
      menu->top_idx = menu->items.empty() ? 0 : menu->items.size() - 1;
    }
  }
  else if (menu->top_idx > idx)
  {
    menu->top_idx--;
  }

  if (menu->top_idx < 0)
  {
    menu->top_idx = 0;
  }
}

void menu_del_item(struct menu *menu, const char *fname)
{
  struct menu_item *mi;

  assert(menu != nullptr);
  assert(fname != nullptr);

  mi = menu_find(menu, fname);
  assert(mi != nullptr);

  menu_delete(menu, mi);
}

void menu_item_set_align(struct menu_item *mi, const enum menu_align align)
{
  assert(mi != nullptr);

  mi->align = align;
}

void menu_setcurritem_file(struct menu *menu, const char *file)
{
  struct menu_item *mi;

  assert(menu != nullptr);
  assert(file != nullptr);

  mi = menu_find(menu, file);
  if (mi)
  {
    menu_setcurritem(menu, mi);
  }
}
/* Return non-zero value if the item in in the visible part of the menu. */
int menu_is_visible(const struct menu *menu, const struct menu_item *mi)
{
  assert(menu != nullptr);
  assert(mi != nullptr);

  if (mi->num >= menu->top_idx && mi->num < menu->top_idx + menu->height)
  {
    return 1;
  }

  return 0;
}

static void menu_items_swap(struct menu *menu, struct menu_item *mi1,
                            struct menu_item *mi2)
{
  assert(menu != nullptr);
  assert(mi1 != nullptr);
  assert(mi2 != nullptr);
  assert(mi1 != mi2);

  int idx1 = mi1->num;
  int idx2 = mi2->num;

  std::swap(menu->items[idx1], menu->items[idx2]);
  menu->items[idx1]->num = idx1;
  menu->items[idx2]->num = idx2;

  if (menu->selected_idx == idx1)
  {
    menu->selected_idx = idx2;
  }
  else if (menu->selected_idx == idx2)
  {
    menu->selected_idx = idx1;
  }

  if (menu->marked_idx == idx1)
  {
    menu->marked_idx = idx2;
  }
  else if (menu->marked_idx == idx2)
  {
    menu->marked_idx = idx1;
  }
}

void menu_swap_items(struct menu *menu, const char *file1, const char *file2)
{
  struct menu_item *mi1, *mi2;

  assert(menu != nullptr);
  assert(file1 != nullptr);
  assert(file2 != nullptr);

  if ((mi1 = menu_find(menu, file1)) && (mi2 = menu_find(menu, file2)) &&
      mi1 != mi2)
  {
    menu_items_swap(menu, mi1, mi2);

    /* make sure that the selected item is visible */
    menu_setcurritem(menu, menu->items[menu->selected_idx].get());
  }
}

/* Make sure that this file is visible in the menu. */
void menu_make_visible(struct menu *menu, const char *file)
{
  struct menu_item *mi;

  assert(menu != nullptr);
  assert(file != nullptr);

  if ((mi = menu_find(menu, file)))
  {
    make_item_visible(menu, mi);
  }
}

// EOF
