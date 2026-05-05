// src/library/files.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef FILES_H
#define FILES_H

#include <stdio.h>
#include "utils/lists.h"
#include "library/playlist.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define FILES_LIST_INIT_SIZE 64

  void files_init();
  void files_cleanup();
  int read_directory(const char *directory, lists_t_strs *dirs,
                     lists_t_strs *playlists, struct plist *plist);
  int read_directory_recurr(const char *directory, struct plist *plist);
  void resolve_path(char *buf, size_t size, const char *file);
  char *ext_pos(const char *file);
  enum file_type file_type(const char *file);
  char *file_mime_type(const char *file);
  char *read_line(FILE *file);
  char *find_match_dir(char *dir);
  int file_exists(const char *file);
  time_t get_mtime(const char *file);
  struct file_tags *read_file_tags(const char *file,
                                   struct file_tags *present_tags,
                                   const int tags_sel);
  void switch_titles_file(struct plist *plist);
  void switch_titles_tags(struct plist *plist);
  void make_tags_title(struct plist *plist, const int num);
  void make_file_title(struct plist *plist, const int num,
                       const bool hide_extension);
  int is_dir(const char *file);
  int can_read_file(const char *file);
  char *absolute_path(const char *path, const char *cwd);
  bool is_secure(const char *file);

#ifdef __cplusplus
}
#endif

#endif

// EOF
