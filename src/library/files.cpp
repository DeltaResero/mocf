// src/library/files.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2004 Damian Pietras <daper@daper.net>
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <algorithm>
#include <dirent.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <mutex>

#ifdef HAVE_LIBMAGIC
#include <magic.h>
#endif

#define DEBUG

#include "core/common.h"
#include "library/playlist.h"
#include "ui/curses/interface.h"
#include "audio/decoder.h"
#include "core/options.h"
#include "library/files.h"
#include "library/playlist_file.h"
#include "core/log.h"
#include "utils/utf8.h"

#define READ_LINE_INIT_SIZE 256

#ifdef HAVE_LIBMAGIC
static magic_t cookie = nullptr;
static std::optional<std::string> cached_file;
static std::string cached_result;
#endif

void files_init()
{
#ifdef HAVE_LIBMAGIC
  assert(cookie == nullptr);

  cookie = magic_open(MAGIC_SYMLINK | MAGIC_MIME | MAGIC_ERROR |
                      MAGIC_NO_CHECK_COMPRESS | MAGIC_NO_CHECK_ELF |
                      MAGIC_NO_CHECK_TAR | MAGIC_NO_CHECK_TOKENS |
                      MAGIC_NO_CHECK_FORTRAN | MAGIC_NO_CHECK_TROFF);
  if (cookie == nullptr)
  {
    log_errno("Error allocating magic cookie", errno);
  }
  else if (magic_load(cookie, nullptr) != 0)
  {
    logit("Error loading magic database: %s", magic_error(cookie));
    magic_close(cookie);
    cookie = nullptr;
  }
#endif
}

void files_cleanup()
{
#ifdef HAVE_LIBMAGIC
  cached_file.reset();
  cached_result.clear();
  magic_close(cookie);
  cookie = nullptr;
#endif
}

/* Return 1 if the file is a directory, 0 if not, -1 on error. */
int is_dir(const char *file)
{
  struct stat file_stat;

  if (stat(file, &file_stat) == -1)
  {
    std::string err = xstrerror(errno);
    error("Can't stat %s: %s", file, err.c_str());
    return -1;
  }
  return S_ISDIR(file_stat.st_mode) ? 1 : 0;
}

/* Return 1 if the file can be read by this user, 0 if not */
int can_read_file(const char *file) { return access(file, R_OK) == 0; }

enum file_type file_type(const char *file)
{
  struct stat file_stat;

  assert(file != nullptr);

  if (stat(file, &file_stat) == -1)
  {
    return F_OTHER; /* Ignore the file if stat() failed */
  }
  if (S_ISDIR(file_stat.st_mode))
  {
    return F_DIR;
  }
  if (is_sound_file(file))
  {
    return F_SOUND;
  }
  if (is_plist_file(file))
  {
    return F_PLAYLIST;
  }
  return F_OTHER;
}

/* Given a file name, return the mime type or empty string. */
std::string file_mime_type(const char *file ASSERT_ONLY)
{
  std::string result;

  assert(file != nullptr);

#ifdef HAVE_LIBMAGIC
  static std::mutex magic_mtx;

  if (cookie != nullptr)
  {
    std::lock_guard<std::mutex> lock(magic_mtx);
    if (cached_file && *cached_file == file)
    {
      result = cached_result;
    }
    else
    {
      cached_file.reset();
      cached_result.clear();
      const char *magic_res = magic_file(cookie, file);
      if (magic_res == nullptr)
      {
        logit("Error interrogating file: %s", magic_error(cookie));
      }
      else
      {
        result = magic_res;
        cached_file = file;
        cached_result = result;
      }
    }
  }
#endif

  return result;
}

/* Make a title from the file name for the item.  If hide_extn != 0,
 * strip the file name from extension. */
void make_file_title(struct plist *plist, const int num,
                     const bool hide_extension)
{
  assert(plist != nullptr);
  assert(in_range(num, plist->items.size()));
  assert(!plist_deleted(plist, num));

  std::string file = plist->items[num].file;

  if (hide_extension)
  {
    char *extn = ext_pos(file.c_str());
    if (extn)
    {
      file.resize(extn - file.c_str() - 1);
    }
  }

  if (options_get_bool("FileNamesIconv"))
  {
    file = files_iconv_str(file.c_str());
  }

  plist_set_title_file(plist, num, file.c_str());
}

/* Make a title from the tags for the item. */
void make_tags_title(struct plist *plist, const int num)
{
  bool hide_extn;

  assert(plist != nullptr);
  assert(in_range(num, plist->items.size()));
  assert(!plist_deleted(plist, num));

  if (!plist->items[num].title_tags.empty())
  {
    return;
  }

  assert(!plist->items[num].file.empty());

  if (!plist->items[num].tags->title.empty())
  {
    plist_set_title_tags(plist, num, build_title(plist->items[num].tags.get()).c_str());
    return;
  }

  hide_extn = options_get_bool("HideFileExtension");
  make_file_title(plist, num, hide_extn);
}

/* Switch playlist titles to title_file */
void switch_titles_file(struct plist *plist)
{
  int i;
  bool hide_extn;

  hide_extn = options_get_bool("HideFileExtension");

  for (i = 0; i < static_cast<int>(plist->items.size()); i++)
  {
    if (plist_deleted(plist, i))
    {
      continue;
    }

    if (plist->items[i].title_file.empty())
    {
      make_file_title(plist, i, hide_extn);
    }

    assert(!plist->items[i].title_file.empty());
  }
}

/* Switch playlist titles to title_tags */
void switch_titles_tags(struct plist *plist)
{
  int i;
  bool hide_extn;

  hide_extn = options_get_bool("HideFileExtension");

  for (i = 0; i < static_cast<int>(plist->items.size()); i++)
  {
    if (plist_deleted(plist, i))
    {
      continue;
    }

    if (plist->items[i].title_tags.empty() && plist->items[i].title_file.empty())
    {
      make_file_title(plist, i, hide_extn);
    }
  }
}

/* Add file to the directory path in buf resolving '../' and removing './'. */
/* buf must be absolute path. */
void resolve_path(char *buf, size_t size, const char *file)
{
  int rc;
  char *f;                 /* points to the char in *file we process */
  char path[2 * PATH_MAX]; /* temporary path */
  size_t len = 0;          /* number of characters in the buffer */

  assert(buf[0] == '/');

  rc = snprintf(path, sizeof(path), "%s/%s/", buf, file);
  if (rc >= ssizeof(path))
  {
    fatal("Path too long!");
  }

  f = path;
  while (*f)
  {
    if (!strncmp(f, "/../", 4))
    {
      char *slash = strrchr(buf, '/');

      assert(slash != nullptr);

      if (slash == buf)
      {
        /* make '/' from '/directory' */
        buf[1] = 0;
        len = 1;
      }
      else
      {
        /* strip one element */
        *(slash) = 0;
        len -= len - (slash - buf);
        buf[len] = 0;
      }

      f += 3;
    }
    else if (!strncmp(f, "/./", 3))
    {
      /* skip '/.' */
      f += 2;
    }
    else if (!strncmp(f, "//", 2))
    {
      /* remove double slash */
      f++;
    }
    else if (len == size - 1)
    {
      fatal("Path too long!");
    }
    else
    {
      buf[len++] = *(f++);
      buf[len] = 0;
    }
  }

  /* remove dot from '/dir/.' */
  if (len >= 2 && buf[len - 1] == '.' && buf[len - 2] == '/')
  {
    buf[--len] = 0;
  }

  /* strip trailing slash */
  if (len > 1 && buf[len - 1] == '/')
  {
    buf[--len] = 0;
  }
}

/* Read selected tags for a file into tags structure (or create it if nullptr).
 * If some tags are already present, don't read them.
 * If present_tags is nullptr, allocate new tags. */
struct file_tags *read_file_tags(const char *file, struct file_tags *tags,
                                 const int tags_sel)
{
  AudioPlugin *df;
  int needed_tags;

  assert(file != nullptr);

  if (tags == nullptr)
  {
    tags = new file_tags{};
  }

  needed_tags = ~tags->filled & tags_sel;
  if (!needed_tags)
  {
    debug("No need to read any tags");
    return tags;
  }

  df = get_decoder(file);
  if (!df)
  {
    logit("Can't find decoder functions for %s", file);
    return tags;
  }

  /* This makes sure that we don't cause a memory leak */
  assert(!((needed_tags & TAGS_COMMENTS) &&
           (!tags->title.empty() || !tags->artist.empty() || !tags->album.empty())));

  df->info(file, tags, needed_tags);

  tags->filled |= tags_sel;

  return tags;
}

/* Read the content of the directory, make an array of absolute paths for
 * all recognized files. Put directories, playlists and sound files
 * in proper structures. Return 0 on error.*/
int read_directory(const char *directory, std::vector<std::string> &dirs,
                   std::vector<std::string> &playlists, struct plist *plist)
{
  DIR *dir;
  struct dirent *entry;
  bool show_hidden = options_get_bool("ShowHiddenFiles");
  int dir_is_root;

  assert(directory != nullptr);
  assert(*directory == '/');
  assert(plist != nullptr);

  if (!(dir = opendir(directory)))
  {
    error_errno("Can't read directory", errno);
    return 0;
  }

  if (!strcmp(directory, "/"))
  {
    dir_is_root = 1;
  }
  else
  {
    dir_is_root = 0;
  }

  while ((entry = readdir(dir)))
  {
    int rc;
    char file[PATH_MAX];
    enum file_type type;

    if (user_wants_interrupt())
    {
      error("Interrupted! Not all files read!");
      break;
    }

    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
    {
      continue;
    }
    if (!show_hidden && entry->d_name[0] == '.')
    {
      continue;
    }

    rc = snprintf(file, sizeof(file), "%s/%s", dir_is_root ? "" : directory,
                  entry->d_name);
    if (rc >= ssizeof(file))
    {
      error("Path too long!");
      closedir(dir);
      return 0;
    }

    type = file_type(file);
    if (type == F_SOUND)
    {
      plist_add(plist, file);
    }
    else if (type == F_DIR)
    {
      dirs.push_back(file);
    }
    else if (type == F_PLAYLIST)
    {
      playlists.push_back(file);
    }
  }

  closedir(dir);

  return 1;
}

static bool dir_symlink_loop(const ino_t inode_no,
                             const std::vector<ino_t> &dir_stack)
{
  return std::find(dir_stack.begin(), dir_stack.end(), inode_no) !=
         dir_stack.end();
}

/* Recursively add files from the directory to the playlist.
 * Return 1 if OK (and even some errors), 0 if the user interrupted. */
static int read_directory_recurr_internal(const char *directory,
                                          struct plist *plist,
                                          std::vector<ino_t> &dir_stack)
{
  DIR *dir;
  struct dirent *entry;
  struct stat st;

  if (stat(directory, &st))
  {
    std::string err = xstrerror(errno);
    error("Can't stat %s: %s", directory, err.c_str());
    return 0;
  }

  assert(plist != nullptr);
  assert(directory != nullptr);

  if (dir_symlink_loop(st.st_ino, dir_stack))
  {
    logit("Detected symlink loop on %s", directory);
    return 1;
  }

  if (!(dir = opendir(directory)))
  {
    error_errno("Can't read directory", errno);
    return 1;
  }

  dir_stack.push_back(st.st_ino);

  while ((entry = readdir(dir)))
  {
    int rc;
    char file[PATH_MAX];
    enum file_type type;

    if (user_wants_interrupt())
    {
      error("Interrupted! Not all files read!");
      break;
    }

    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
    {
      continue;
    }
    rc = snprintf(file, sizeof(file), "%s/%s", directory, entry->d_name);
    if (rc >= ssizeof(file))
    {
      error("Path too long!");
      continue;
    }
    type = file_type(file);
    if (type == F_DIR)
    {
      read_directory_recurr_internal(file, plist, dir_stack);
    }
    else if (type == F_SOUND && plist_find_fname(plist, file) == -1)
    {
      plist_add(plist, file);
    }
  }

  dir_stack.pop_back();

  closedir(dir);
  return 1;
}

int read_directory_recurr(const char *directory, struct plist *plist)
{
  std::vector<ino_t> dir_stack;

  return read_directory_recurr_internal(directory, plist, dir_stack);
}

/* Return the file extension position or nullptr if the file has no extension. */
char *ext_pos(const char *file)
{
  const char *ext = strrchr(file, '.');
  const char *slash = strrchr(file, '/');

  /* don't treat dot in ./file or /.file as a dot before extension */
  if (ext && (!slash || slash < ext) && ext != file && *(ext - 1) != '/')
  {
    ext++;
  }
  else
  {
    ext = nullptr;
  }

  return const_cast<char *>(ext);
}

/* Read one line from a file, stripping the trailing newline.
 * Returns nullopt on EOF or error with no data read. */
std::optional<std::string> read_line(FILE *file)
{
  std::string line;
  char buf[READ_LINE_INIT_SIZE];
  bool got_data = false;

  while (fgets(buf, sizeof(buf), file) != nullptr)
  {
    got_data = true;
    size_t chunk = strlen(buf);

    if (chunk > 0 && buf[chunk - 1] == '\n')
    {
      line.append(buf, chunk - 1);  /* append without the newline */
      break;
    }

    line.append(buf, chunk);
  }

  if (!got_data)
    return std::nullopt;

  /* strip Windows-style trailing \r if present */
  if (!line.empty() && line.back() == '\r')
    line.pop_back();

  return line;
}

/* Find directories having a prefix of 'pattern'.
 * - If there are no matches, empty string is returned.
 * - If there is one such directory, it is returned with a trailing '/'.
 * - Otherwise the longest common prefix is returned (with no trailing '/').
 * (This is used for directory auto-completion.) */
std::string find_match_dir(const std::string& pattern)
{
  if (pattern.empty()) return "";

  size_t slash_pos = pattern.rfind('/');
  if (slash_pos == std::string::npos) return "";

  std::string search_dir;
  if (slash_pos == 0) search_dir = "/";
  else search_dir = pattern.substr(0, slash_pos);

  std::string name = pattern.substr(slash_pos + 1);

  DIR *dir = opendir(search_dir.c_str());
  if (!dir) return "";

  std::string matching_dir;
  bool unambiguous = true;
  struct dirent *entry;

  while ((entry = readdir(dir)))
  {
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") &&
        !strncmp(entry->d_name, name.c_str(), name.length()))
    {
      std::string path = search_dir;
      if (path != "/") path += "/";
      path += entry->d_name;

      if (is_dir(path.c_str()) == 1)
      {
        if (!matching_dir.empty())
        {
          size_t i = 0;
          while (i < matching_dir.length() && i < path.length() && matching_dir[i] == path[i]) i++;
          matching_dir.resize(i);
          unambiguous = false;
        }
        else
        {
          matching_dir = path;
        }
      }
    }
  }
  closedir(dir);

  if (!matching_dir.empty() && unambiguous)
  {
    matching_dir += "/";
  }

  return matching_dir;
}

/* Return != 0 if the file exists. */
int file_exists(const char *file)
{
  struct stat file_stat;

  if (!stat(file, &file_stat))
  {
    return 1;
  }

  /* Log any error other than non-existence. */
  if (errno != ENOENT)
  {
    log_errno("Error", errno);
  }

  return 0;
}

/* Get the modification time of a file. Return (time_t)-1 on error */
time_t get_mtime(const char *file)
{
  struct stat stat_buf;

  if (stat(file, &stat_buf) != -1)
  {
    return stat_buf.st_mtime;
  }

  return static_cast<time_t>(-1);
}

/* Convert file path to absolute path */
std::string absolute_path(const char *path, const char *cwd)
{
  assert(path);
  assert(cwd);

  if (path[0] != '/')
  {
    char tmp[2 * PATH_MAX];
    strncpy(tmp, cwd, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = 0;
    resolve_path(tmp, sizeof(tmp), path);
    return std::string(tmp);
  }

  return std::string(path);
}

/* Check that a file which may cause other applications to be invoked
 * is secure against tampering. */
bool is_secure(const char *file)
{
  struct stat sb;

  assert(file && file[0]);

  if (stat(file, &sb) == -1)
  {
    return true;
  }
  if (!S_ISREG(sb.st_mode))
  {
    return false;
  }
  if (sb.st_mode & (S_IWGRP | S_IWOTH))
  {
    return false;
  }
  if (sb.st_uid != 0 && sb.st_uid != geteuid())
  {
    return false;
  }

  return true;
}

// EOF
