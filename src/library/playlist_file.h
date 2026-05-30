// src/library/playlist_file.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef PLAYLIST_FILE_H
#define PLAYLIST_FILE_H

#ifdef __cplusplus
extern "C"
{
#endif

  int plist_load(struct plist *plist, const char *fname, const char *cwd);
  int plist_save(struct plist *plist, const char *file, const bool save_tags);
  int is_plist_file(const char *name);

#ifdef __cplusplus
}
#endif

#endif

// EOF
