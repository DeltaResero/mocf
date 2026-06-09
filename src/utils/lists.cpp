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

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <strings.h>
#include <utility>
#include <vector>

#include "core/common.h"
#include "utils/lists.h"

struct lists_strs
{
  std::vector<std::string> strs;
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

  list->strs.clear();
}

/* Free all storage associated with a list of strings. */
void lists_strs_free(lists_t_strs *list)
{
  assert(list);

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

/* Given an index, return a reference to the string at that position in a list. */
const std::string &lists_strs_at(const lists_t_strs *list, int index)
{
  assert(list);
  assert(index >= 0 && index < static_cast<int>(list->strs.size()));

  return list->strs[index];
}

/* Sort string list into an order determined by caller's comparator. */
void lists_strs_sort(lists_t_strs *list, lists_t_compare *compare)
{
  assert(list);
  assert(compare);

  std::sort(list->strs.begin(), list->strs.end(), compare);
}

/* Reverse the order of entries in a list. */
void lists_strs_reverse(lists_t_strs *list)
{
  assert(list);

  std::reverse(list->strs.begin(), list->strs.end());
}

/* Take a string by value and push it onto the end of a list. */
void lists_strs_push(lists_t_strs *list, std::string s)
{
  assert(list);

  list->strs.push_back(std::move(s));
}

/* Remove the last string on the list and return it.
 * The list must not be empty. */
std::string lists_strs_pop(lists_t_strs *list)
{
  assert(list);
  assert(!list->strs.empty());

  std::string result = std::move(list->strs.back());
  list->strs.pop_back();
  return result;
}

/* Replace the nominated string with a new one and return the old one. */
std::string lists_strs_swap(lists_t_strs *list, int index, std::string s)
{
  assert(list);
  assert(index >= 0 && index < static_cast<int>(list->strs.size()));

  std::string result = std::move(list->strs[index]);
  list->strs[index] = std::move(s);
  return result;
}

/* Copy a string and append it to the end of a list. */
void lists_strs_append(lists_t_strs *list, const std::string &s)
{
  assert(list);

  list->strs.push_back(s);
}

/* Remove the string from the end of the list and discard it. */
void lists_strs_remove(lists_t_strs *list)
{
  assert(list);

  if (!list->strs.empty())
  {
    list->strs.pop_back();
  }
}

/* Replace the nominated string with a copy of the new one. */
void lists_strs_replace(lists_t_strs *list, int index, const std::string &s)
{
  assert(list);
  assert(index >= 0 && index < static_cast<int>(list->strs.size()));

  list->strs[index] = s;
}

/* Split a string at any delimiter in given string.  The resulting segments
 * are appended to the given string list.  Returns the number of tokens
 * appended. */
int lists_strs_split(lists_t_strs *list, const char *s, const char *delim)
{
  assert(list);
  assert(s);
  assert(delim);

  std::string buf(s);
  char *saveptr;
  char *token = strtok_r(buf.data(), delim, &saveptr);
  int result = 0;

  while (token)
  {
    list->strs.emplace_back(token);
    result += 1;
    token = strtok_r(nullptr, delim, &saveptr);
  }

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
 * given format for each, or an empty string if the list is empty. */
std::string lists_strs_fmt(const lists_t_strs *list, const char *fmt)
{
  assert(list);

  const char *sep = strstr(fmt, "%s");
  assert(sep);

  if (lists_strs_empty(list))
  {
    return {};
  }

  const std::string prefix(fmt, sep - fmt);
  const std::string suffix(sep + 2);
  std::string result;

  for (const auto &s : list->strs)
  {
    result += prefix;
    result += s;
    result += suffix;
  }

  return result;
}

/* Return the concatenation of all the strings in a list, or an empty
 * string if the list is empty. */
std::string lists_strs_cat(const lists_t_strs *list)
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
  assert(list);

  int count = lists_strs_size(list);
  size_t size = sizeof(char *) * (count + 1);

  for (int ix = 0; ix < count; ix += 1)
  {
    size += lists_strs_at(list, ix).size() + 1;
  }

  char **result = static_cast<char **>(xmalloc(size));
  char *ptr = reinterpret_cast<char *>(result + count + 1);

  for (int ix = 0; ix < count; ix += 1)
  {
    const std::string &s = lists_strs_at(list, ix);
    std::memcpy(ptr, s.c_str(), s.size() + 1);
    result[ix] = ptr;
    ptr += s.size() + 1;
  }

  result[count] = nullptr;
  return result;
}

/* Reload saved strings into a list.  The reloaded strings are appended
 * to the list.  The number of items reloaded is returned. */
int lists_strs_load(lists_t_strs *list, const char **saved)
{
  assert(list);
  assert(saved);

  int size = lists_strs_size(list);
  while (*saved)
  {
    lists_strs_append(list, *saved++);
  }

  return lists_strs_size(list) - size;
}

/* Given a string, return the index of the first list entry which matches
 * it.  If not found, return the total number of entries.
 * The comparison is case-insensitive. */
int lists_strs_find(lists_t_strs *list, const std::string &sought)
{
  assert(list);

  int result = 0;
  for (const auto &s : list->strs)
  {
    if (strcasecmp(s.c_str(), sought.c_str()) == 0)
    {
      return result;
    }
    result += 1;
  }

  return result;
}

/* Given a string, return true iff it exists in the list. */
bool lists_strs_exists(lists_t_strs *list, const std::string &sought)
{
  assert(list);

  return lists_strs_find(list, sought) < lists_strs_size(list);
}

// EOF
