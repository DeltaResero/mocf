// src/utils/lists.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2009 Damian Pietras <daper@daper.net> and John Fitzgerald
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <vector>
#include <algorithm>

#include "core/common.h"
#include "utils/lists.h"

struct lists_strs
{
  std::vector<char*> strs;
};

/* Allocate a new list of strings and return its address. */
lists_t_strs *lists_strs_new(int reserve)
{
  assert(reserve >= 0);

  lists_t_strs *result = new lists_t_strs();
  if (reserve > 0)
  {
    result->strs.reserve(reserve);
  }

  return result;
}

/* Clear a list to an empty state. */
void lists_strs_clear(lists_t_strs *list)
{
  assert(list);

  for (char *s : list->strs)
  {
    free(s);
  }
  list->strs.clear();
}

/* Free all storage associated with a list of strings. */
void lists_strs_free(lists_t_strs *list)
{
  assert(list);

  lists_strs_clear(list);
  delete list;
}

/* Return the number of strings in a list. */
int lists_strs_size(const lists_t_strs *list)
{
  assert(list);

  return static_cast<int>(list->strs.size());
}

/* Return the total number of strings which could be held without growing. */
int lists_strs_capacity(const lists_t_strs *list)
{
  assert(list);

  return static_cast<int>(list->strs.capacity());
}

/* Return true iff the list has no members. */
bool lists_strs_empty(const lists_t_strs *list)
{
  assert(list);

  return list->strs.empty();
}

/* Given an index, return the string at that position in a list. */
char *lists_strs_at(const lists_t_strs *list, int index)
{
  assert(list);
  assert(index >= 0 && index < static_cast<int>(list->strs.size()));

  return list->strs[index];
}

/* Sort string list into an order determined by caller's comparitor. */
void lists_strs_sort(lists_t_strs *list, lists_t_compare *compare)
{
  assert(list);
  assert(compare);

  qsort(list->strs.data(), list->strs.size(), sizeof(char *), compare);
}

/* Reverse the order of entries in a list. */
void lists_strs_reverse(lists_t_strs *list)
{
  assert(list);

  std::reverse(list->strs.begin(), list->strs.end());
}

/* Take a string and push it onto the end of a list
 * (expanding the list if necessary). */
void lists_strs_push(lists_t_strs *list, char *s)
{
  assert(list);
  assert(s);

  list->strs.push_back(s);
}

/* Remove the last string on the list and return it, or NULL if the list
 * is empty. */
char *lists_strs_pop(lists_t_strs *list)
{
  assert(list);

  if (list->strs.empty())
  {
    return NULL;
  }

  char *result = list->strs.back();
  list->strs.pop_back();
  return result;
}

/* Replace the nominated string with a new one and return the old one. */
char *lists_strs_swap(lists_t_strs *list, int index, char *s)
{
  assert(list);
  assert(index >= 0 && index < static_cast<int>(list->strs.size()));
  assert(s);

  char *result = list->strs[index];
  list->strs[index] = s;

  return result;
}

/* Copy a string and append it to the end of a list. */
void lists_strs_append(lists_t_strs *list, const char *s)
{
  assert(list);
  assert(s);

  char *str = xstrdup(s);
  lists_strs_push(list, str);
}

/* Remove a string from the end of the list and free it. */
void lists_strs_remove(lists_t_strs *list)
{
  assert(list);

  char *str = lists_strs_pop(list);
  if (str)
  {
    free(str);
  }
}

/* Replace the nominated string with a copy of the new one
 * and free the old one. */
void lists_strs_replace(lists_t_strs *list, int index, const char *s)
{
  assert(list);
  assert(index >= 0 && index < static_cast<int>(list->strs.size()));

  char *str = xstrdup(s);
  str = lists_strs_swap(list, index, str);
  free(str);
}

/* Split a string at any delimiter in given string.  The resulting segments
 * are appended to the given string list.  Returns the number of tokens
 * appended. */
int lists_strs_split(lists_t_strs *list, const char *s, const char *delim)
{
  int result;
  char *str, *token, *saveptr;

  assert(list);
  assert(s);
  assert(delim);

  result = 0;
  str = xstrdup(s);
  token = strtok_r(str, delim, &saveptr);
  while (token)
  {
    result += 1;
    lists_strs_append(list, token);
    token = strtok_r(NULL, delim, &saveptr);
  }

  free(str);
  return result;
}

/* Tokenise a string and append the tokens to the list.
 * Returns the number of tokens appended. */
int lists_strs_tokenise(lists_t_strs *list, const char *s)
{
  assert(list);
  assert(s);

  return lists_strs_split(list, s, " \t");
}

/* Return the concatenation of all the strings in a list using the
 * given format for each, or NULL if the list is empty. */
char *lists_strs_fmt(const lists_t_strs *list, const char *fmt)
{
  int len, ix, rc;
  char *result, *ptr;
  const char *sep, *suffix;
  int prefix_len;

  assert(list);
  sep = strstr(fmt, "%s");
  assert(sep);

  prefix_len = (int)(sep - fmt);
  suffix = sep + 2;

  result = NULL;
  if (!lists_strs_empty(list))
  {
    len = 0;
    for (ix = 0; ix < lists_strs_size(list); ix += 1)
    {
      len += strlen(lists_strs_at(list, ix));
    }
    len += ix * (strlen(fmt) - 2);

    ptr = result = (char *)xmalloc(len + 1);
    for (ix = 0; ix < lists_strs_size(list); ix += 1)
    {
      rc = snprintf(ptr, len + 1, "%.*s%s%s",
                    prefix_len, fmt,
                    lists_strs_at(list, ix),
                    suffix);
      if (rc > len)
      {
        fatal("Allocated string area was too small!");
      }
      len -= rc;
      ptr += rc;
    }
  }

  return result;
}

/* Return the concatenation of all the strings in a list, or NULL
 * if the list is empty. */
char *lists_strs_cat(const lists_t_strs *list)
{
  assert(list);

  return lists_strs_fmt(list, "%s");
}

/* Return a "snapshot" of the given string list.  The returned memory is a
 * null-terminated list of pointers to the given list's strings copied into
 * memory allocated after the pointer list.  This list is suitable for passing
 * to functions which take such a list as an argument (e.g., execv()).
 * Invoking free() on the returned pointer also frees the strings. */
char **lists_strs_save(const lists_t_strs *list)
{
  int ix, size;
  char *ptr, **result;

  assert(list);

  size = 0;
  for (ix = 0; ix < lists_strs_size(list); ix += 1)
  {
    size += strlen(lists_strs_at(list, ix)) + 1;
  }
  size += sizeof(char *) * (lists_strs_size(list) + 1);
  result = (char **)xmalloc(size);
  ptr = (char *)(result + lists_strs_size(list) + 1);
  for (ix = 0; ix < lists_strs_size(list); ix += 1)
  {
    strcpy(ptr, lists_strs_at(list, ix));
    result[ix] = ptr;
    ptr += strlen(ptr) + 1;
  }
  result[ix] = NULL;

  return result;
}

/* Reload saved strings into a list.  The reloaded strings are appended
 * to the list.  The number of items reloaded is returned. */
int lists_strs_load(lists_t_strs *list, const char **saved)
{
  int size;

  assert(list);
  assert(saved);

  size = lists_strs_size(list);
  while (*saved)
  {
    lists_strs_append(list, *saved++);
  }

  return lists_strs_size(list) - size;
}

/* Given a string, return the index of the first list entry which matches
 * it.  If not found, return the total number of entries.
 * The comparison is case-insensitive. */
int lists_strs_find(lists_t_strs *list, const char *sought)
{
  int result;

  assert(list);
  assert(sought);

  for (result = 0; result < lists_strs_size(list); result += 1)
  {
    if (!strcasecmp(lists_strs_at(list, result), sought))
    {
      break;
    }
  }

  return result;
}

/* Given a string, return true iff it exists in the list. */
bool lists_strs_exists(lists_t_strs *list, const char *sought)
{
  bool result = false;

  assert(list);
  assert(sought);

  if (lists_strs_find(list, sought) < lists_strs_size(list))
  {
    result = true;
  }

  return result;
}

// EOF
