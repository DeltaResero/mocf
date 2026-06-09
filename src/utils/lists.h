// src/utils/lists.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef LISTS_H
#define LISTS_H

#include <string>

#include "core/common.h"

typedef struct lists_strs lists_t_strs;
typedef bool lists_t_compare(const std::string &, const std::string &);

/* List administration functions. */
lists_t_strs *lists_strs_new(int reserve);
void lists_strs_clear(lists_t_strs *list);
void lists_strs_free(lists_t_strs *list);
int lists_strs_size(const lists_t_strs *list);
int lists_strs_capacity(const lists_t_strs *list);
bool lists_strs_empty(const lists_t_strs *list);

/* List member access functions. */
const std::string &lists_strs_at(const lists_t_strs *list, int index);

/* List mutating functions. */
void lists_strs_sort(lists_t_strs *list, lists_t_compare *compare);
void lists_strs_reverse(lists_t_strs *list);

/* Ownership transferring functions. */
void lists_strs_push(lists_t_strs *list, std::string s);
std::string lists_strs_pop(lists_t_strs *list);
std::string lists_strs_swap(lists_t_strs *list, int index, std::string s);

/* Ownership preserving functions. */
void lists_strs_append(lists_t_strs *list, const std::string &s);
void lists_strs_remove(lists_t_strs *list);
void lists_strs_replace(lists_t_strs *list, int index, const std::string &s);

/* Helper functions. */
int lists_strs_split(lists_t_strs *list, const char *s, const char *delim);
int lists_strs_tokenise(lists_t_strs *list, const char *s);
std::string lists_strs_fmt(const lists_t_strs *list, const char *fmt);
std::string lists_strs_cat(const lists_t_strs *list);
char **lists_strs_save(const lists_t_strs *list);
int lists_strs_load(lists_t_strs *list, const char **saved);
int lists_strs_find(lists_t_strs *list, const std::string &sought);
bool lists_strs_exists(lists_t_strs *list, const std::string &sought);

#endif

// EOF
