// src/ui/curses/interface.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2004 - 2006 Damian Pietras <daper@daper.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <csignal>
#include <dirent.h>
#include <locale.h>
#include <sys/select.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#ifdef HAVE_SYS_INOTIFY_H
#include <sys/inotify.h>
#endif

#define DEBUG

#include "core/common.h"
#include "core/log.h"
#include "ui/curses/interface_elements.h"
#include "ui/curses/interface.h"
#include "library/playlist.h"
#include "library/playlist_file.h"
#include "core/protocol.h"
#include "core/server.h"
#include "audio/audio.h"
#include "ui/input/keys.h"
#include "core/options.h"
#include "library/files.h"
#include "audio/decoder.h"
#include "ui/themes.h"
#include "audio/processing/softmixer.h"
#include "utils/utf8.h"

#define PLAYLIST_FILE "playlist.m3u"
#define QUEUE_CLEAR_THRESH 128

static std::atomic<want_quit> want_quit_flag{NO_QUIT};
static std::atomic<bool> wants_interrupt_flag{false};
#ifdef SIGWINCH
static std::atomic<bool> want_resize_flag{false};
#endif

static void sig_quit(int sig LOGIT_ONLY) {
  log_signal(sig);
  want_quit_flag = QUIT_APP;
}

static void sig_interrupt(int sig LOGIT_ONLY) {
  log_signal(sig);
  wants_interrupt_flag = true;
}

#ifdef SIGWINCH
static void sig_winch(int sig LOGIT_ONLY) {
  log_signal(sig);
  want_resize_flag = true;
}
#endif

int user_wants_interrupt() { return wants_interrupt_flag; }
static void clear_interrupt() { wants_interrupt_flag = false; }

static bool is_subdir(const char *dir1, const char *dir2) {
  return !strncmp(dir1, dir2, strlen(dir1));
}

static bool sort_strcmp_func(const std::string &a, const std::string &b) {
  return strcoll(a.c_str(), b.c_str()) < 0;
}

static bool sort_dirs_func(const std::string &a, const std::string &b) {
  if (a == "../") return true;
  if (b == "../") return false;
  return strcmp(a.c_str(), b.c_str()) < 0;
}

static void sanitise_string(std::string &str) {
  for (size_t i = 0; i < str.size(); i++) {
    if (str[i] != ' ' && isspace(static_cast<unsigned char>(str[i]))) {
      str.resize(i);
      break;
    }
  }
}

static time_t rounded_time() {
  struct timespec exact_time;
  if (get_realtime(&exact_time) == -1) {
    interface_fatal("get_realtime() failed: %s", xstrerror(errno).c_str());
  }
  time_t curr_time = exact_time.tv_sec;
  if (exact_time.tv_nsec > 500000000L) {
    curr_time += 1;
  }
  return curr_time;
}

static void add_themes_to_list(std::vector<std::string> &themes, const char *themes_dir) {
  DIR *dir;
  struct dirent *entry;
  if (!(dir = opendir(themes_dir))) {
    logit("Can't open themes directory %s: %s", themes_dir, xstrerror(errno).c_str());
    return;
  }
  while ((entry = readdir(dir))) {
    char file[PATH_MAX];
    if (entry->d_name[0] == '.') continue;
    if (entry->d_name[strlen(entry->d_name) - 1] == '~') continue;
    if (snprintf(file, sizeof(file), "%s/%s", themes_dir, entry->d_name) >= ssizeof(file)) continue;
    themes.push_back(file);
  }
  closedir(dir);
}

static bool themes_cmp(const std::string &a, const std::string &b) {
  int result = strcoll(strrchr(a.c_str(), '/') + 1, strrchr(b.c_str(), '/') + 1);
  if (result != 0) return result < 0;
  return strcoll(a.c_str(), b.c_str()) < 0;
}

static int add_themes_to_menu(const char *user_themes, const char *system_themes) {
  std::vector<std::string> themes;
  themes.reserve(16);
  add_themes_to_list(themes, user_themes);
  add_themes_to_list(themes, system_themes);
  std::sort(themes.begin(), themes.end(), themes_cmp);
  int ix;
  for (ix = 0; ix < static_cast<int>(themes.size()); ix += 1) {
    const char *file = themes[ix].c_str();
    iface_add_file(file, strrchr(file, '/') + 1, F_THEME);
  }
  return ix;
}

static char *dir_up(const char *path) {
  char *dir = xstrdup(path);
  char *slash = strrchr(dir, '/');
  if (slash == dir) *(slash + 1) = 0;
  else *slash = 0;
  return dir;
}

struct file_info {
  std::string file;
  std::unique_ptr<file_tags> tags;
  std::string title;
  int avg_bitrate = -1;
  int bitrate = -1;
  int rate = -1;
  int curr_time = -1;
  int total_time = -1;
  int channels = 1;
  int state = STATE_STOP;
  std::string block_file;
  int block_start = -1;
  int block_end = -1;
};

class UserInterface {
private:
    engine_event_queue *g_engine_eq = nullptr;
    plist playlist;
    plist queue;
    plist dir_plist;
    std::queue<Event> events;
    char cwd[PATH_MAX] = "";
    bool playlist_dirty = false;
    plist *engine_plist = nullptr;
    file_info curr_file;
    int silent_seek_pos = -1;
    time_t silent_seek_key_last = 0;
    time_t last_menu_move_time = 0;
#ifdef HAVE_SYS_INOTIFY_H
    int inotify_fd = -1;
    int inotify_wd = -1;
#endif

    void drain_engine_events() {
        engine_event_queue_flush(g_engine_eq, events);
    }

    void wait_and_drain_engine_events() {
        engine_event_queue_wait_flush(g_engine_eq, events);
    }

    int send_tags_request(const char *file, const int tags_sel) {
        if (file_type(file) == F_SOUND) {
            engine_request_file_tags(file, tags_sel);
            debug("Asking for tags for %s", file);
            return 1;
        }
        return 0;
    }

    void file_info_block_mark(int *marker) {
        if (curr_file.state == STATE_STOP) {
            iface_error("Cannot make block marks while stopped.");
        } else if (file_type(curr_file.file.c_str()) != F_SOUND) {
            iface_error("Cannot make block marks in non-audio files.");
        } else if (curr_file.block_file.empty()) {
            iface_error("Cannot make block marks in files of unknown duration.");
        } else {
            *marker = curr_file.curr_time;
            iface_set_block(curr_file.block_start, curr_file.block_end);
        }
    }

    void sync_bool_option(const char *name) {
        iface_set_option_state(name, options_get_bool(name));
    }

    void get_engine_options() {
        sync_bool_option("Shuffle");
        sync_bool_option("Repeat");
        sync_bool_option("AutoNext");
    }

    void update_mixer_value() {
        iface_set_mixer_value(MAX(audio_get_mixer(), 0));
    }

    void update_mixer_name() {
        iface_set_mixer_name(audio_get_mixer_channel_name().c_str());
        update_mixer_value();
    }

    void set_cwd(const char *path) {
        if (path[0] == '/') strcpy(cwd, "/");
        else if (!cwd[0]) {
            if (!getcwd(cwd, sizeof(cwd))) fatal("Can't get CWD: %s", xstrerror(errno).c_str());
        }
        resolve_path(cwd, sizeof(cwd), path);
    }

    void set_start_dir() {
        if (!getcwd(cwd, sizeof(cwd))) {
            if (errno == ERANGE) fatal("CWD is larger than PATH_MAX!");
            const char *home = get_home();
            if (strlen(home) >= sizeof(cwd)) fatal("Home directory path is longer than PATH_MAX!");
            strcpy(cwd, home);
        }
    }

    int read_last_dir() {
        FILE *dir_file = fopen(create_file_name("last_directory").c_str(), "r");
        if (!dir_file) return 0;
        int read_bytes = fread(cwd, sizeof(char), sizeof(cwd) - 1, dir_file);
        if (read_bytes == 0) {
            fclose(dir_file);
            return 0;
        }
        cwd[read_bytes] = 0;
        fclose(dir_file);
        return 1;
    }

    int get_tags_setting() {
        int needed_tags = 0;
        if (options_get_bool("ReadTags")) needed_tags |= TAGS_COMMENTS;
        if (strcasecmp(options_get_symb("ShowTime"), "no")) needed_tags |= TAGS_TIME;
        return needed_tags;
    }

    int ask_for_tags(const plist *p, const int tags_sel) {
        int req = 0;
        if (tags_sel != 0) {
            for (int i = 0; i < p->num; i++) {
                if (!plist_deleted(p, i) && (!p->items[i].tags || ~p->items[i].tags->filled & tags_sel)) {
                    std::string file = plist_get_file(p, i);
                    req += send_tags_request(file.c_str(), tags_sel);
                }
            }
        }
        return req;
    }

    void interface_message(const char *format, ...) {
        va_list va;
        va_start(va, format);
        std::string msg = format_msg_va(format, va);
        va_end(va);
        iface_message(msg.c_str());
    }

    void update_item_tags(plist *p, const int num, file_tags *tags) {
        file_tags *old_tags = plist_get_tags(p, num);
        if (old_tags) tags_update(tags, old_tags, 1);
        plist_set_tags(p, num, tags);
        if (!p->items[num].title_tags.empty()) p->items[num].title_tags.clear();
        make_tags_title(p, num);
        if (options_get_bool("ReadTags") && p->items[num].title_tags.empty()) {
            if (p->items[num].title_file.empty()) make_file_title(p, num, options_get_bool("HideFileExtension"));
        }
        if (old_tags) tags_free(old_tags);
    }

    void ev_file_tags(const tag_ev_response *data) {
        int n;
        file_tags *mutable_tags = data->tags.get();
        sanitise_string(mutable_tags->title);
        sanitise_string(mutable_tags->artist);
        sanitise_string(mutable_tags->album);

        if ((n = plist_find_fname(&dir_plist, data->file.c_str())) != -1) {
            update_item_tags(&dir_plist, n, data->tags.get());
            iface_update_item(IFACE_MENU_DIR, &dir_plist, n);
        }
        if ((n = plist_find_fname(&playlist, data->file.c_str())) != -1) {
            update_item_tags(&playlist, n, data->tags.get());
            iface_update_item(IFACE_MENU_PLIST, &playlist, n);
        }
        if (!curr_file.file.empty() && curr_file.file == data->file) {
            if (data->tags->time != -1) {
                curr_file.total_time = data->tags->time;
                iface_set_total_time(curr_file.total_time);
                if (file_type(curr_file.file.c_str()) == F_SOUND) {
                    if (curr_file.block_file.empty()) {
                        curr_file.block_file = curr_file.file;
                        curr_file.block_start = 0;
                        curr_file.block_end = curr_file.total_time;
                    }
                    iface_set_block(curr_file.block_start, curr_file.block_end);
                }
            }
            if (!data->tags->title.empty()) {
                curr_file.title = build_title(data->tags.get());
                iface_set_played_file_title(curr_file.title.c_str());
            }
            curr_file.tags.reset(tags_dup(data->tags.get()));
        }
    }

    void update_ctime() {
        curr_file.curr_time = audio_get_time();
        if (silent_seek_pos == -1) iface_set_curr_time(curr_file.curr_time);
    }

    void follow_curr_file() {
        if (!curr_file.file.empty() && file_type(curr_file.file.c_str()) == F_SOUND && last_menu_move_time <= time(nullptr) - 2) {
            if (plist_find_fname(&playlist, curr_file.file.c_str()) != -1) {
                iface_make_visible(IFACE_MENU_PLIST, curr_file.file.c_str());
            } else if (plist_find_fname(&dir_plist, curr_file.file.c_str()) != -1) {
                iface_make_visible(IFACE_MENU_DIR, curr_file.file.c_str());
            }
        }
    }

    void update_curr_file() {
        std::string file = audio_get_sname();
        if (file.empty() || curr_file.state == STATE_STOP) {
            curr_file = file_info{};
            iface_set_played_file(nullptr);
        } else if (curr_file.file.empty() || curr_file.file != file) {
            if (!curr_file.block_file.empty() && curr_file.block_file != file) {
                curr_file.block_file.clear();
            }
            iface_set_total_time(-1);
            iface_set_played_file(file.c_str());
            send_tags_request(file.c_str(), TAGS_COMMENTS | TAGS_TIME);
            curr_file.file = file;

            if (file.find('/') == std::string::npos) {
                curr_file.title = file;
            } else {
                if (options_get_bool("FileNamesIconv")) {
                    std::string iconv_str = files_iconv_str(file.c_str() + file.rfind('/') + 1);
                    curr_file.title = iconv_str;
                } else {
                    curr_file.title = file.substr(file.rfind('/') + 1);
                }
            }
            iface_set_played_file(file.c_str());
            iface_set_played_file_title(curr_file.title.c_str());
            silent_seek_pos = -1;
            iface_set_curr_time(curr_file.curr_time);
            if (options_get_bool("FollowPlayedFile")) follow_curr_file();
        }
    }

    void update_rate() {
        curr_file.rate = engine_get_rate();
        iface_set_rate(curr_file.rate);
    }

    void update_channels() {
        curr_file.channels = engine_get_channels() == 2 ? 2 : 1;
        iface_set_channels(curr_file.channels);
    }

    void update_bitrate() {
        curr_file.bitrate = engine_get_bitrate();
        iface_set_bitrate(curr_file.bitrate);
    }

    void update_state() {
        int old_state = curr_file.state;
        curr_file.state = audio_get_state();
        iface_set_state(curr_file.state);
        if (old_state != curr_file.state) silent_seek_pos = -1;
        update_curr_file();
        update_channels();
        update_bitrate();
        update_rate();
        update_ctime();
    }

    void event_queue_add(const plist_item *item) {
        if (plist_find_fname(&queue, item->file.c_str()) == -1) {
            plist_add_from_item(&queue, item);
            iface_set_files_in_queue(plist_count(&queue));
            iface_update_queue_position_last(&queue, &playlist, &dir_plist);
        }
    }

    void update_error(srv_error_ev *data) {
        if (!data->msg.empty()) iface_error(data->msg.c_str());
        if (!data->file.empty()) iface_mark_file_error(data->file.c_str());
    }

    void recv_engine_queue(plist *q) {
        plist *engine_q = engine_get_queue();
        if (!engine_q) return;
        for (int i = 0; i < engine_q->num; i++) {
            if (!plist_deleted(engine_q, i)) plist_add_from_item(q, &engine_q->items[i]);
        }
        plist_free(engine_q);
        delete engine_q;
    }

    void clear_playlist() {
        if (iface_in_plist_menu()) iface_switch_to_dir();
        plist_clear(&playlist);
        iface_clear_plist();
        interface_message("The playlist was cleared.");
        iface_set_status("");
        playlist_dirty = true;
    }

    void clear_queue() {
        iface_clear_queue_positions(&queue, &playlist, &dir_plist);
        plist_clear(&queue);
        iface_set_files_in_queue(0);
        interface_message("The queue was cleared.");
    }

    void event_queue_del(const char *file) {
        int item = plist_find_fname(&queue, file);
        if (item != -1) {
            plist_delete(&queue, item);
            if (plist_count(&queue) == 0 && queue.num >= QUEUE_CLEAR_THRESH) plist_clear(&queue);
            iface_set_files_in_queue(plist_count(&queue));
            iface_update_queue_positions(&queue, &playlist, &dir_plist, file);
        }
    }

    void swap_playlist_items(const char *file1, const char *file2) {
        plist_swap_files(&playlist, file1, file2);
        iface_swap_plist_items(file1, file2);
    }

    void event_queue_move(const move_ev_data *d) {
        plist_swap_files(&queue, d->from.c_str(), d->to.c_str());
    }

    void server_event(const int event, void *data) {
        switch (event) {
            case EV_CTIME: update_ctime(); break;
            case EV_STATE: update_state(); break;
            case EV_BITRATE: update_bitrate(); break;
            case EV_RATE: update_rate(); break;
            case EV_CHANNELS: update_channels(); break;
            case EV_SRV_ERROR: update_error(static_cast<srv_error_ev *>(data)); break;
            case EV_OPTIONS: get_engine_options(); break;
            case EV_STATUS_MSG: iface_set_status(static_cast<std::string *>(data)->c_str()); break;
            case EV_MIXER_CHANGE: update_mixer_name(); break;
            case EV_FILE_TAGS: ev_file_tags(static_cast<tag_ev_response *>(data)); break;
            case EV_AVG_BITRATE: curr_file.avg_bitrate = engine_get_avg_bitrate(); break;
            case EV_QUEUE_ADD: event_queue_add(static_cast<plist_item *>(data)); break;
            case EV_QUEUE_DEL: event_queue_del(static_cast<std::string *>(data)->c_str()); break;
            case EV_QUEUE_CLEAR: clear_queue(); break;
            case EV_QUEUE_MOVE: event_queue_move(static_cast<move_ev_data *>(data)); break;
            case EV_AUDIO_START: break;
            case EV_AUDIO_STOP: break;
            case EV_TAGS:
                if (!curr_file.file.empty()) send_tags_request(curr_file.file.c_str(), TAGS_COMMENTS);
                break;
            default:
                interface_fatal("Unknown event: 0x%02x!", event);
        }
        free_event_data(event, data);
    }

    void fill_tags(plist *p, const int tags_sel, const int no_iface) {
        int files = ask_for_tags(p, tags_sel);
        iface_set_status("Reading tags...");
        while (files && !user_wants_interrupt()) {
            int type;
            void *data;
            if (!no_iface && !events.empty()) {
                Event e = events.front();
                type = e.type;
                data = e.data;
                events.pop();
            } else {
                wait_and_drain_engine_events();
                if (events.empty()) continue;
                Event e = events.front();
                type = e.type;
                data = e.data;
                events.pop();
            }

            if (type == EV_FILE_TAGS) {
                tag_ev_response *ev = static_cast<tag_ev_response *>(data);
                if (plist_find_fname(p, ev->file.c_str()) != -1) {
                    if (ev->tags->filled & tags_sel) files--;
                }
                if (!no_iface) server_event(type, data);
                else free_event_data(type, data);
            } else if (no_iface) {
                abort();
            } else {
                server_event(type, data);
            }
        }
        iface_set_status("");
    }

    int go_to_dir(const char *dir, const int reload) {
        char last_dir[PATH_MAX];
        const char *new_dir = dir ? dir : cwd;
        int going_up = 0;
        std::vector<std::string> dirs, playlists;

        iface_set_status("Reading directory...");

        if (dir && is_subdir(dir, cwd)) {
            strcpy(last_dir, strrchr(cwd, '/') + 1);
            strcat(last_dir, "/");
            going_up = 1;
        }

        plist new_dir_plist;
        plist_init(&new_dir_plist);
        dirs.reserve(FILES_LIST_INIT_SIZE);
        playlists.reserve(FILES_LIST_INIT_SIZE);

        if (!read_directory(new_dir, dirs, playlists, &new_dir_plist)) {
            iface_set_status("");
            plist_free(&new_dir_plist);
            return 0;
        }

        plist_free(&dir_plist);
        dir_plist = std::move(new_dir_plist);

#ifdef HAVE_SYS_INOTIFY_H
        if (!reload && inotify_wd >= 0) {
            inotify_rm_watch(inotify_fd, inotify_wd);
        }
#endif

        if (dir) strcpy(cwd, dir);

        switch_titles_file(&dir_plist);
        plist_sort_fname(&dir_plist);
        std::sort(dirs.begin(), dirs.end(), sort_dirs_func);
        std::sort(playlists.begin(), playlists.end(), sort_strcmp_func);

        ask_for_tags(&dir_plist, get_tags_setting());

        if (reload) {
            iface_update_dir_content(IFACE_MENU_DIR, &dir_plist, dirs, playlists);
        } else {
            iface_set_dir_content(IFACE_MENU_DIR, &dir_plist, dirs, playlists);
#ifdef HAVE_SYS_INOTIFY_H
            if (inotify_fd >= 0) {
                inotify_wd = inotify_add_watch(inotify_fd, new_dir, IN_MODIFY | IN_CREATE | IN_DELETE);
            }
#endif
        }
        if (going_up) iface_set_curr_item_title(last_dir);

        iface_set_title(IFACE_MENU_DIR, cwd);
        iface_update_queue_positions(&queue, nullptr, &dir_plist, nullptr);

        if (iface_in_plist_menu()) iface_switch_to_dir();

        return 1;
    }

    void enter_first_dir() {
        static int first_run = 1;
        if (options_get_bool("StartInMusicDir")) {
            const char *music_dir = options_get_str("MusicDir");
            if (music_dir) {
                set_cwd(music_dir);
                if (first_run && file_type(music_dir) == F_PLAYLIST && plist_count(&playlist) == 0 && go_to_playlist(music_dir, false)) {
                    cwd[0] = 0;
                    first_run = 0;
                } else if (file_type(cwd) == F_DIR && go_to_dir(nullptr, 0)) {
                    first_run = 0;
                    return;
                }
            } else {
                iface_error("MusicDir is not set");
            }
        }

        if (!(read_last_dir() && go_to_dir(nullptr, 0))) {
            set_start_dir();
            if (!go_to_dir(nullptr, 0)) interface_fatal("Can't enter any directory!");
        }
        first_run = 0;
    }

    void toggle_menu() {
        if (iface_in_plist_menu()) {
            if (!cwd[0]) enter_first_dir();
            else iface_switch_to_dir();
        } else if (plist_count(&playlist)) {
            iface_switch_to_plist();
        } else {
            iface_error("The playlist is empty.");
        }
    }

    int go_to_playlist(const char *file, bool default_playlist) {
        if (plist_count(&playlist)) {
            iface_error("Please clear the playlist, because I'm not sure you want to do this.");
            return 0;
        }

        plist_clear(&playlist);
        iface_set_status("Loading playlist...");
        if (plist_load(&playlist, file, cwd)) {
            if (!default_playlist) toggle_menu();
            iface_set_dir_content(IFACE_MENU_PLIST, &playlist, {}, {});
            iface_update_queue_positions(&queue, &playlist, nullptr, nullptr);
            interface_message("Playlist loaded.");
        } else {
            interface_message("The playlist is empty");
            iface_set_status("");
            return 0;
        }
        return 1;
    }

    void use_engine_queue() {
        iface_set_status("Getting the queue...");
        recv_engine_queue(&queue);
        iface_set_files_in_queue(plist_count(&queue));
        iface_update_queue_positions(&queue, &playlist, &dir_plist, nullptr);
        iface_set_status("");
    }

    void process_dir_arg(const char *dir) {
        set_cwd(dir);
        if (!go_to_dir(nullptr, 0)) enter_first_dir();
    }

    void process_plist_arg(const char *file) {
        char path[PATH_MAX + 1];
        if (file[0] == '/') strcpy(path, "/");
        else if (!getcwd(path, sizeof(path))) interface_fatal("Can't get CWD: %s", xstrerror(errno).c_str());

        resolve_path(path, sizeof(path), file);
        char *slash = strrchr(path, '/');
        if (slash) *slash = 0;

        iface_set_status("Loading playlist...");
        plist_load(&playlist, file, path);
        iface_set_status("");
    }

    void process_multiple_args(const std::vector<std::string> &args) {
        char this_cwd[PATH_MAX];

        if (!getcwd(this_cwd, sizeof(cwd))) interface_fatal("Can't get CWD: %s", xstrerror(errno).c_str());

        for (const auto &a : args) {
            const char *arg = a.c_str();
            int dir = is_dir(arg);
            char path[2 * PATH_MAX];

            if (arg[0] == '/') strcpy(path, "/");
            else strcpy(path, this_cwd);
            resolve_path(path, sizeof(path), arg);

            if (dir == 1) {
                read_directory_recurr(path, &playlist);
            } else if (!dir && is_sound_file(path)) {
                if (plist_find_fname(&playlist, path) == -1) plist_add(&playlist, path);
            } else if (is_plist_file(path)) {
                char *plist_dir = xstrdup(path);
                char *slash = strrchr(plist_dir, '/');
                if (slash) *slash = 0;
                plist_load(&playlist, path, plist_dir);
                free(plist_dir);
            }
        }
    }

    void process_args(const std::vector<std::string> &args) {
        const char *arg = args[0].c_str();

        if (args.size() == 1 && is_dir(arg) == 1) {
            process_dir_arg(arg);
            return;
        }

        if (args.size() == 1 && is_plist_file(arg)) {
            process_plist_arg(arg);
        } else {
            process_multiple_args(args);
        }

        if (plist_count(&playlist)) {
            switch_titles_file(&playlist);
            ask_for_tags(&playlist, get_tags_setting());
            iface_set_dir_content(IFACE_MENU_PLIST, &playlist, {}, {});
            iface_update_queue_positions(&queue, &playlist, nullptr, nullptr);
            iface_switch_to_plist();
        } else {
            enter_first_dir();
        }
    }

    void load_playlist() {
        std::string plist_file = create_file_name(PLAYLIST_FILE);
        if (file_type(plist_file.c_str()) == F_PLAYLIST) go_to_playlist(plist_file.c_str(), true);
    }

    void do_resize() {
        iface_resize();
        want_resize_flag = false;
    }

    void go_dir_up() {
        char *dir = dir_up(cwd);
        go_to_dir(dir, 0);
        free(dir);
    }

    void send_playlist(plist *p, const int clear) {
        if (clear) audio_plist_clear();
        for (int i = 0; i < p->num; i++) {
            if (!plist_deleted(p, i)) audio_plist_add(p->items[i].file.c_str());
        }
    }

    void play_it(const char *file) {
        plist *curr_plist = iface_in_dir_menu() ? &dir_plist : &playlist;

        if (options_get_bool("ForceShufflePlaylistOnly")) {
            engine_set_option("Shuffle", !iface_in_dir_menu());
            sync_bool_option("Shuffle");
        }

        if (curr_plist == &dir_plist || curr_plist != engine_plist || playlist_dirty) {
            send_playlist(curr_plist, 1);
            engine_plist = curr_plist;
            playlist_dirty = false;
        }
        audio_play(file);
    }

    void go_file() {
        enum file_type type = iface_curritem_get_type();
        std::string file = iface_get_curr_file();
        if (file.empty()) return;

        if (type == F_SOUND) {
            play_it(file.c_str());
        } else if (type == F_DIR && iface_in_dir_menu()) {
            if (file == "..") go_dir_up();
            else go_to_dir(file.c_str(), 0);
        } else if (type == F_PLAYLIST) {
            go_to_playlist(file.c_str(), false);
        }
    }

    void switch_pause() {
        switch (curr_file.state) {
            case STATE_PLAY: audio_pause(); break;
            case STATE_PAUSE: audio_unpause(); break;
        }
    }

    void set_mixer(int val) {
        audio_set_mixer(CLAMP(0, val, 100));
    }

    void adjust_mixer(const int diff) {
        set_mixer(audio_get_mixer() + diff);
    }

    void add_dir_plist() {
        if (iface_in_plist_menu()) {
            iface_error("Can't add to the playlist a file from the playlist.");
            return;
        }

        std::string file = iface_get_curr_file();
        if (file.empty()) return;

        enum file_type type = iface_curritem_get_type();
        if (type != F_DIR && type != F_PLAYLIST) {
            iface_error("This is not a directory or a playlist.");
            return;
        }

        if (file == "..") {
            file = cwd;
        }

        iface_set_status("Reading directories...");
        plist p;
        plist_init(&p);

        if (type == F_DIR) {
            read_directory_recurr(file.c_str(), &p);
            plist_sort_fname(&p);
        } else {
            plist_load(&p, file.c_str(), cwd);
        }

        plist_remove_common_items(&p, &playlist);
        playlist_dirty = true;

        switch_titles_file(&p);
        ask_for_tags(&p, get_tags_setting());

        for (int i = 0; i < p.num; i++) {
            if (!plist_deleted(&p, i)) iface_add_to_plist(&p, i);
        }
        plist_cat(&playlist, &p);

        plist_free(&p);
    }

    void remove_file_from_playlist(const char *file) {
        int n = plist_find_fname(&playlist, file);
        if (n != -1) {
            plist_delete(&playlist, n);
            iface_del_plist_item(file);

            if (plist_count(&playlist) == 0) clear_playlist();

            if (engine_plist == &playlist) audio_plist_delete(file);
            else playlist_dirty = true;
        }
    }

    void remove_dead_entries_plist() {
        if (!iface_in_plist_menu()) {
            iface_error("Can't prune when not in the playlist.");
            return;
        }

        const char *file = nullptr;
        int i = 0;
        while ((file = plist_get_next_dead_entry(&playlist, &i)) != nullptr) {
            remove_file_from_playlist(file);
        }
    }

    void add_file_plist() {
        if (iface_in_plist_menu()) {
            iface_error("Can't add to the playlist a file from the playlist.");
            return;
        }

        if (iface_curritem_get_type() == F_DIR) {
            add_dir_plist();
            return;
        }

        std::string file = iface_get_curr_file();
        if (file.empty()) return;

        if (iface_curritem_get_type() != F_SOUND) {
            iface_error("You can only add a file using this command.");
            return;
        }

        if (plist_find_fname(&playlist, file.c_str()) == -1) {
            int added;
            plist_item *item = &dir_plist.items[plist_find_fname(&dir_plist, file.c_str())];

            added = plist_add_from_item(&playlist, item);
            iface_add_to_plist(&playlist, added);

            if (engine_plist == &playlist) audio_plist_add(file.c_str());
            else playlist_dirty = true;
        } else {
            iface_error("The file is already on the playlist.");
        }

        iface_menu_key(KEY_CMD_MENU_DOWN);
    }

    void queue_toggle_file() {
        std::string file = iface_get_curr_file();
        if (file.empty()) return;

        if (iface_curritem_get_type() != F_SOUND) {
            iface_error("You can only add a file using this command.");
            return;
        }

        if (plist_find_fname(&queue, file.c_str()) == -1) {
            engine_queue_add(file.c_str());
        } else {
            engine_queue_del(file.c_str());
        }

        iface_menu_key(KEY_CMD_MENU_DOWN);
    }

    void toggle_option(const char *name) {
        engine_set_option(name, !options_get_bool(name));
        sync_bool_option(name);
    }

    void toggle_show_time() {
        if (!strcasecmp(options_get_symb("ShowTime"), "yes")) {
            options_set_symb("ShowTime", "IfAvailable");
            iface_set_status("ShowTime: IfAvailable");
        } else if (!strcasecmp(options_get_symb("ShowTime"), "no")) {
            options_set_symb("ShowTime", "yes");
            iface_update_show_time();
            ask_for_tags(&dir_plist, TAGS_TIME);
            ask_for_tags(&playlist, TAGS_TIME);
            iface_set_status("ShowTime: yes");
        } else {
            options_set_symb("ShowTime", "no");
            iface_update_show_time();
            iface_set_status("ShowTime: no");
        }
    }

    void toggle_show_format() {
        bool show_format = !options_get_bool("ShowFormat");
        options_set_bool("ShowFormat", show_format);
        iface_set_status(show_format ? "ShowFormat: yes" : "ShowFormat: no");
        iface_update_show_format();
    }

    void reread_dir() {
        while (go_to_dir(nullptr, 1) == 0) go_dir_up();
    }

    void cmd_clear_playlist() { clear_playlist(); }
    void cmd_clear_queue() { engine_queue_clear(); }

    void go_to_music_dir() {
        const char *musicdir_optn = options_get_str("MusicDir");
        if (!musicdir_optn) {
            iface_error("MusicDir not defined");
            return;
        }

        char music_dir[PATH_MAX] = "/";
        resolve_path(music_dir, sizeof(music_dir), musicdir_optn);

        switch (file_type(music_dir)) {
            case F_DIR: go_to_dir(music_dir, 0); break;
            case F_PLAYLIST: go_to_playlist(music_dir, false); break;
            default: iface_error("MusicDir is neither a directory nor a playlist!");
        }
    }

    char *make_dir(const char *str) {
        char *dir = static_cast<char *>(xmalloc(sizeof(char) * PATH_MAX));
        dir[0] = 0;
        int add_slash = (strlen(str) > 1 && str[strlen(str) - 1] == '/');

        if (str[0] == '~') {
            const char *home = get_home();
            if (strnlen(home, PATH_MAX) == PATH_MAX) {
                free(dir);
                return nullptr;
            }
            strcpy(dir, home);
            if (!strcmp(str, "~")) add_slash = 1;
            str++;
        } else if (str[0] != '/') {
            strcpy(dir, cwd);
        } else {
            strcpy(dir, "/");
        }

        resolve_path(dir, PATH_MAX, str);
        if (add_slash && strlen(dir) < PATH_MAX) strcat(dir, "/");
        return dir;
    }

    void entry_key_go_dir(const iface_key *k) {
        if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\t') {
            char *entry_text = iface_entry_get_text();
            char *dir = make_dir(entry_text);
            free(entry_text);
            if (!dir) return;

            std::string complete_dir = find_match_dir(dir);
            if (!complete_dir.empty()) {
                free(dir);
                dir = xstrdup(complete_dir.c_str());
            }

            char buf[PATH_MAX];
            pathstrcpy(buf, dir);
            free(dir);
            iface_entry_set_text(buf);
        } else if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n') {
            char *entry_text = iface_entry_get_text();
            if (entry_text[0]) {
                char *dir = make_dir(entry_text);
                iface_entry_history_add();
                if (dir) {
                    if (dir[strlen(dir) - 1] == '/' && strcmp(dir, "/")) dir[strlen(dir) - 1] = 0;
                    go_to_dir(dir, 0);
                    free(dir);
                }
            }
            iface_entry_disable();
            free(entry_text);
        } else {
            iface_entry_handle_key(k);
        }
    }

    void entry_key_search(const iface_key *k) {
        if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n') {
            std::string file = iface_get_curr_file();
            char *text = iface_entry_get_text();
            iface_entry_disable();

            if (text[0]) {
                if (file == "..") {
                    char *up = dir_up(cwd);
                    file = up;
                    free(up);
                }
                if (file_type(file.c_str()) == F_DIR) go_to_dir(file.c_str(), 0);
                else if (file_type(file.c_str()) == F_PLAYLIST) go_to_playlist(file.c_str(), false);
                else play_it(file.c_str());
            }
            free(text);
        } else {
            iface_entry_handle_key(k);
        }
    }

    void save_playlist(const char *file) {
        iface_set_status("Saving the playlist...");
        if (options_get_bool("SavePlaylistTags")) {
            fill_tags(&playlist, TAGS_COMMENTS | TAGS_TIME, 0);
            if (user_wants_interrupt()) iface_set_status("Reading tags aborted");
        }

        if (plist_save(&playlist, file, (options_get_bool("SavePlaylistTags") && !user_wants_interrupt()))) {
            interface_message("Playlist saved");
        }
        iface_set_status("");
    }

    void entry_key_plist_save(const iface_key *k) {
        if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n') {
            char *text = iface_entry_get_text();
            iface_entry_disable();

            if (text[0]) {
                char *ext = ext_pos(text);
                if (!ext || strcmp(ext, "m3u")) {
                    char *tmp = static_cast<char *>(xmalloc((strlen(text) + 5) * sizeof(char)));
                    snprintf(tmp, strlen(text) + 5, "%s.m3u", text);
                    free(text);
                    text = tmp;
                }

                char *file = make_dir(text);
                if (file_exists(file)) {
                    iface_make_entry(ENTRY_PLIST_OVERWRITE);
                    iface_entry_set_file(file);
                } else {
                    save_playlist(file);
                    if (iface_in_dir_menu()) reread_dir();
                }
                free(file);
            }
            free(text);
        } else {
            iface_entry_handle_key(k);
        }
    }

    void entry_key_plist_overwrite(const iface_key *k) {
        if (k->type == IFACE_KEY_CHAR && toupper(k->key.ucs) == 'Y') {
            char *file = iface_entry_get_file();
            iface_entry_disable();
            save_playlist(file);
            if (iface_in_dir_menu()) reread_dir();
            free(file);
        } else if (k->type == IFACE_KEY_CHAR && toupper(k->key.ucs) == 'N') {
            iface_entry_disable();
            interface_message("Not overwriting.");
        }
    }

    void entry_key_user_query(const iface_key *k) {
        if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n') {
            char *entry_text = iface_entry_get_text();
            iface_entry_disable();
            iface_user_reply(entry_text);
            free(entry_text);
        } else {
            iface_entry_handle_key(k);
        }
    }

    void entry_key(const iface_key *k) {
        switch (iface_get_entry_type()) {
            case ENTRY_GO_DIR: entry_key_go_dir(k); break;
            case ENTRY_SEARCH: entry_key_search(k); break;
            case ENTRY_PLIST_SAVE: entry_key_plist_save(k); break;
            case ENTRY_PLIST_OVERWRITE: entry_key_plist_overwrite(k); break;
            case ENTRY_USER_QUERY: entry_key_user_query(k); break;
        }
    }

    void update_iface_menu(const iface_menu menu, const plist *p) {
        for (int i = 0; i < p->num; i++) {
            if (!plist_deleted(p, i)) iface_update_item(menu, p, i);
        }
    }

    void switch_read_tags() {
        if (options_get_bool("ReadTags")) {
            options_set_bool("ReadTags", false);
            switch_titles_file(&dir_plist);
            switch_titles_file(&playlist);
            iface_set_status("ReadTags: no");
        } else {
            options_set_bool("ReadTags", true);
            ask_for_tags(&dir_plist, TAGS_COMMENTS);
            ask_for_tags(&playlist, TAGS_COMMENTS);
            switch_titles_tags(&dir_plist);
            switch_titles_tags(&playlist);
            iface_set_status("ReadTags: yes");
        }
        update_iface_menu(IFACE_MENU_DIR, &dir_plist);
        update_iface_menu(IFACE_MENU_PLIST, &playlist);
    }

    void seek(const int sec) { audio_seek(sec); }
    void jump_to(const int sec) { engine_jump_to(sec); }
    void seek_to_percent(int percent) { engine_jump_to(-percent); }

    void delete_item() {
        if (!iface_in_plist_menu()) {
            iface_error("You can only delete an item from the playlist.");
            return;
        }
        std::string file = iface_get_curr_file();
        remove_file_from_playlist(file.c_str());
    }

    void go_to_playing_file() {
        if (!curr_file.file.empty()) {
            if (iface_in_plist_menu() && plist_find_fname(&playlist, curr_file.file.c_str()) != -1) {
                iface_select_file(curr_file.file.c_str());
            } else if (iface_in_dir_menu() && file_type(curr_file.file.c_str()) == F_SOUND) {
                if (plist_find_fname(&dir_plist, curr_file.file.c_str()) != -1) {
                    iface_select_file(curr_file.file.c_str());
                } else {
                    char *file = xstrdup(curr_file.file.c_str());
                    char *slash = strrchr(file, '/');
                    if (slash) *slash = 0;

                    if (file[0]) go_to_dir(file, 0);
                    else go_to_dir("/", 0);

                    iface_switch_to_dir();
                    free(file);
                    iface_select_file(curr_file.file.c_str());
                }
            }
        }
    }

    void seek_silent(const int sec) {
        if (curr_file.state == STATE_PLAY && !curr_file.file.empty()) {
            if (silent_seek_pos == -1) silent_seek_pos = curr_file.curr_time + sec;
            else silent_seek_pos += sec;

            silent_seek_pos = CLAMP(0, silent_seek_pos, curr_file.total_time);
            silent_seek_key_last = rounded_time();
            iface_set_curr_time(silent_seek_pos);
        }
    }

    void move_item(const int direction) {
        if (!iface_in_plist_menu()) {
            iface_error("You can move only playlist items.");
            return;
        }

        std::string file = iface_get_curr_file();
        if (file.empty()) return;

        int second = plist_find_fname(&playlist, file.c_str());
        if (direction == -1) second = plist_next(&playlist, second);
        else if (direction == 1) second = plist_prev(&playlist, second);

        if (second == -1) {
            return;
        }

        std::string second_file = plist_get_file(&playlist, second);
        swap_playlist_items(file.c_str(), second_file.c_str());

        if (engine_plist == &playlist) audio_plist_move(file.c_str(), second_file.c_str());
        else playlist_dirty = true;
    }

    void do_silent_seek() {
        time_t curr_time = time(nullptr);
        if (silent_seek_pos != -1 && silent_seek_key_last < curr_time) {
            seek(silent_seek_pos - curr_file.curr_time - 1);
            silent_seek_pos = -1;
            iface_set_curr_time(curr_file.curr_time);
        }
    }

    void cmd_next() {
        if (curr_file.state != STATE_STOP) {
            audio_next();
        } else if (plist_count(&playlist)) {
            if (engine_plist != &playlist || playlist_dirty) {
                send_playlist(&playlist, 1);
                engine_plist = &playlist;
                playlist_dirty = false;
            }
            audio_play("");
        }
    }

    void make_theme_menu() {
        iface_switch_to_theme_menu();
        if (add_themes_to_menu(create_file_name("themes").c_str(), SYSTEM_THEMES_DIR) == 0) {
            if (!cwd[0]) enter_first_dir();
            else iface_switch_to_dir();
            iface_error("No themes found.");
        } else {
            iface_update_theme_selection(get_current_theme());
        }
        iface_refresh();
    }

    void use_theme() {
        std::string file = iface_get_curr_file();
        if (file.empty()) return;
        themes_switch_theme(file.c_str());
        iface_update_attrs();
        iface_refresh();
    }

    void theme_menu_key(const iface_key *k) {
        if (!iface_key_is_resize(k)) {
            enum key_cmd cmd = get_key_cmd(CON_MENU, k);
            switch (cmd) {
                case KEY_CMD_GO: use_theme(); break;
                case KEY_CMD_MENU_DOWN:
                case KEY_CMD_MENU_UP:
                case KEY_CMD_MENU_NPAGE:
                case KEY_CMD_MENU_PPAGE:
                case KEY_CMD_MENU_FIRST:
                case KEY_CMD_MENU_LAST:
                    iface_menu_key(cmd);
                    break;
                default:
                    iface_switch_to_dir();
            }
        }
    }

    void go_to_fast_dir(const int num) {
        char option_name[20];
        snprintf(option_name, sizeof(option_name), "FastDir%d", num);
        if (options_get_str(option_name)) {
            char dir[PATH_MAX] = "/";
            resolve_path(dir, sizeof(dir), options_get_str(option_name));
            go_to_dir(dir, 0);
        } else {
            interface_message("%s is not defined", option_name);
        }
    }

    void toggle_playlist_full_paths() {
        bool new_val = !options_get_bool("PlaylistFullPaths");
        options_set_bool("PlaylistFullPaths", new_val);
        iface_set_status(new_val ? "PlaylistFullPaths: on" : "PlaylistFullPaths: off");
        update_iface_menu(IFACE_MENU_PLIST, &playlist);
    }

    void menu_key(const iface_key *k) {
        if (iface_in_help()) {
            iface_handle_help_key(k);
        } else if (iface_in_entry()) {
            entry_key(k);
        } else if (iface_in_theme_menu()) {
            theme_menu_key(k);
        } else if (!iface_key_is_resize(k)) {
            enum key_cmd cmd = get_key_cmd(CON_MENU, k);
            switch (cmd) {
                case KEY_CMD_QUIT: want_quit_flag = QUIT_APP; break;
                case KEY_CMD_GO: go_file(); break;
                case KEY_CMD_MENU_DOWN:
                case KEY_CMD_MENU_UP:
                case KEY_CMD_MENU_NPAGE:
                case KEY_CMD_MENU_PPAGE:
                case KEY_CMD_MENU_FIRST:
                case KEY_CMD_MENU_LAST:
                    iface_menu_key(cmd);
                    last_menu_move_time = time(nullptr);
                    break;
                case KEY_CMD_STOP: audio_stop(); break;
                case KEY_CMD_NEXT: cmd_next(); break;
                case KEY_CMD_PREVIOUS: audio_prev(); break;
                case KEY_CMD_PAUSE: switch_pause(); break;
                case KEY_CMD_TOGGLE_READ_TAGS: switch_read_tags(); break;
                case KEY_CMD_TOGGLE_SHUFFLE: toggle_option("Shuffle"); break;
                case KEY_CMD_TOGGLE_REPEAT: toggle_option("Repeat"); break;
                case KEY_CMD_TOGGLE_AUTO_NEXT: toggle_option("AutoNext"); break;
                case KEY_CMD_TOGGLE_MENU: toggle_menu(); break;
                case KEY_CMD_TOGGLE_PLAYLIST_FULL_PATHS: toggle_playlist_full_paths(); break;
                case KEY_CMD_PLIST_ADD_FILE: add_file_plist(); break;
                case KEY_CMD_PLIST_CLEAR: cmd_clear_playlist(); break;
                case KEY_CMD_PLIST_ADD_DIR: add_dir_plist(); break;
                case KEY_CMD_PLIST_REMOVE_DEAD_ENTRIES: remove_dead_entries_plist(); break;
                case KEY_CMD_MIXER_DEC_1: adjust_mixer(-1); break;
                case KEY_CMD_MIXER_DEC_5: adjust_mixer(-5); break;
                case KEY_CMD_MIXER_INC_5: adjust_mixer(+5); break;
                case KEY_CMD_MIXER_INC_1: adjust_mixer(+1); break;
                case KEY_CMD_SEEK_BACKWARD: seek(-options_get_int("SeekTime")); break;
                case KEY_CMD_SEEK_FORWARD: seek(options_get_int("SeekTime")); break;
                case KEY_CMD_SEEK_0: seek_to_percent(0 * 10); break;
                case KEY_CMD_SEEK_1: seek_to_percent(1 * 10); break;
                case KEY_CMD_SEEK_2: seek_to_percent(2 * 10); break;
                case KEY_CMD_SEEK_3: seek_to_percent(3 * 10); break;
                case KEY_CMD_SEEK_4: seek_to_percent(4 * 10); break;
                case KEY_CMD_SEEK_5: seek_to_percent(5 * 10); break;
                case KEY_CMD_SEEK_6: seek_to_percent(6 * 10); break;
                case KEY_CMD_SEEK_7: seek_to_percent(7 * 10); break;
                case KEY_CMD_SEEK_8: seek_to_percent(8 * 10); break;
                case KEY_CMD_SEEK_9: seek_to_percent(9 * 10); break;
                case KEY_CMD_HELP: iface_switch_to_help(); break;
                case KEY_CMD_HIDE_MESSAGE: iface_disable_message(); break;
                case KEY_CMD_REFRESH: iface_refresh(); break;
                case KEY_CMD_RELOAD: if (iface_in_dir_menu()) reread_dir(); break;
                case KEY_CMD_TOGGLE_SHOW_HIDDEN_FILES:
                    options_set_bool("ShowHiddenFiles", !options_get_bool("ShowHiddenFiles"));
                    if (iface_in_dir_menu()) reread_dir();
                    break;
                case KEY_CMD_GO_MUSIC_DIR: go_to_music_dir(); break;
                case KEY_CMD_PLIST_DEL: delete_item(); break;
                case KEY_CMD_MENU_SEARCH: iface_make_entry(ENTRY_SEARCH); break;
                case KEY_CMD_PLIST_SAVE:
                    if (plist_count(&playlist)) iface_make_entry(ENTRY_PLIST_SAVE);
                    else iface_error("The playlist is empty.");
                    break;
                case KEY_CMD_TOGGLE_SHOW_TIME: toggle_show_time(); break;
                case KEY_CMD_TOGGLE_SHOW_FORMAT: toggle_show_format(); break;
                case KEY_CMD_GO_TO_PLAYING_FILE: go_to_playing_file(); break;
                case KEY_CMD_GO_DIR: iface_make_entry(ENTRY_GO_DIR); break;
                case KEY_CMD_GO_DIR_UP: go_dir_up(); break;
                case KEY_CMD_WRONG: iface_error("Bad command"); break;
                case KEY_CMD_SEEK_FORWARD_5: seek_silent(options_get_int("SilentSeekTime")); break;
                case KEY_CMD_SEEK_BACKWARD_5: seek_silent(-options_get_int("SilentSeekTime")); break;
                case KEY_CMD_VOLUME_0: set_mixer(0); break;
                case KEY_CMD_VOLUME_10: set_mixer(10); break;
                case KEY_CMD_VOLUME_20: set_mixer(20); break;
                case KEY_CMD_VOLUME_30: set_mixer(30); break;
                case KEY_CMD_VOLUME_40: set_mixer(40); break;
                case KEY_CMD_VOLUME_50: set_mixer(50); break;
                case KEY_CMD_VOLUME_60: set_mixer(60); break;
                case KEY_CMD_VOLUME_70: set_mixer(70); break;
                case KEY_CMD_VOLUME_80: set_mixer(80); break;
                case KEY_CMD_VOLUME_90: set_mixer(90); break;
                case KEY_CMD_VOLUME_100: set_mixer(100); break;
                case KEY_CMD_MARK_START: file_info_block_mark(&curr_file.block_start); break;
                case KEY_CMD_MARK_END: file_info_block_mark(&curr_file.block_end); break;
                case KEY_CMD_FAST_DIR_1: go_to_fast_dir(1); break;
                case KEY_CMD_FAST_DIR_2: go_to_fast_dir(2); break;
                case KEY_CMD_FAST_DIR_3: go_to_fast_dir(3); break;
                case KEY_CMD_FAST_DIR_4: go_to_fast_dir(4); break;
                case KEY_CMD_FAST_DIR_5: go_to_fast_dir(5); break;
                case KEY_CMD_FAST_DIR_6: go_to_fast_dir(6); break;
                case KEY_CMD_FAST_DIR_7: go_to_fast_dir(7); break;
                case KEY_CMD_FAST_DIR_8: go_to_fast_dir(8); break;
                case KEY_CMD_FAST_DIR_9: go_to_fast_dir(9); break;
                case KEY_CMD_FAST_DIR_10: go_to_fast_dir(10); break;
                case KEY_CMD_TOGGLE_MIXER: engine_toggle_mixer_channel(); break;
                case KEY_CMD_TOGGLE_SOFTMIXER: engine_toggle_softmixer(); break;
                case KEY_CMD_TOGGLE_EQUALIZER: engine_toggle_equalizer(); break;
                case KEY_CMD_EQUALIZER_REFRESH: engine_equalizer_refresh(); break;
                case KEY_CMD_EQUALIZER_PREV: engine_equalizer_prev(); break;
                case KEY_CMD_EQUALIZER_NEXT: engine_equalizer_next(); break;
                case KEY_CMD_TOGGLE_MAKE_MONO: engine_toggle_make_mono(); break;
                case KEY_CMD_TOGGLE_LAYOUT: iface_toggle_layout(); break;
                case KEY_CMD_TOGGLE_PERCENT: iface_toggle_percent(); break;
                case KEY_CMD_PLIST_MOVE_UP: move_item(1); break;
                case KEY_CMD_PLIST_MOVE_DOWN: move_item(-1); break;
                case KEY_CMD_THEME_MENU: make_theme_menu(); break;
                case KEY_CMD_QUEUE_TOGGLE_FILE: queue_toggle_file(); break;
                case KEY_CMD_QUEUE_CLEAR: cmd_clear_queue(); break;
                default: abort();
            }
        }
    }

    void get_and_handle_event() {
        drain_engine_events();
    }

    void dequeue_events() {
        while (!events.empty()) {
            Event e = events.front();
            events.pop();
            server_event(e.type, e.data);
        }
    }

    void handle_interrupt() {
        if (iface_in_entry()) iface_entry_disable();
    }

    void save_curr_dir() {
        FILE *dir_file = fopen(create_file_name("last_directory").c_str(), "w");
        if (!dir_file) return;
        fprintf(dir_file, "%s", cwd);
        fclose(dir_file);
    }

    void save_playlist_in_moc() {
        std::string plist_file = create_file_name(PLAYLIST_FILE);
        if (plist_count(&playlist) && options_get_bool("SavePlaylist")) save_playlist(plist_file.c_str());
        else unlink(plist_file.c_str());
    }

public:
    UserInterface(engine_event_queue *eq, const std::vector<std::string> &args) {
        if (!setlocale(LC_CTYPE, "")) logit("Could not set locale!");

        g_engine_eq = eq;
        cwd[0] = '\0';
        playlist_dirty = false;
        engine_plist = nullptr;
        silent_seek_pos = -1;
        silent_seek_key_last = 0;
        last_menu_move_time = 0;

        plist_init(&dir_plist);
        plist_init(&playlist);
        plist_init(&queue);
        keys_init();
        windows_init();
        get_engine_options();
        update_mixer_name();

#ifdef HAVE_SYS_INOTIFY_H
        inotify_fd = inotify_init();
        inotify_wd = -1;
#endif

        xsignal(SIGQUIT, sig_quit);
        xsignal(SIGTERM, sig_quit);
        xsignal(SIGHUP, sig_quit);
        xsignal(SIGINT, sig_interrupt);

#ifdef SIGWINCH
        xsignal(SIGWINCH, sig_winch);
#endif

        if (!args.empty()) {
            process_args(args);
            if (plist_count(&playlist) == 0) load_playlist();
        } else {
            load_playlist();
            enter_first_dir();
        }

        use_engine_queue();
        update_state();
    }

    ~UserInterface() = default;

    void loop() {
        log_circular_start();

        while (want_quit_flag == NO_QUIT) {
            fd_set fds;
            int ret;
            struct timespec timeout = {1, 0};

            FD_ZERO(&fds);
            FD_SET(engine_event_queue_fd(g_engine_eq), &fds);
            FD_SET(STDIN_FILENO, &fds);
#ifdef HAVE_SYS_INOTIFY_H
            if (inotify_fd >= 0) FD_SET(inotify_fd, &fds);
#endif

            dequeue_events();
#ifdef HAVE_SYS_INOTIFY_H
            ret = pselect(MAX(engine_event_queue_fd(g_engine_eq), inotify_fd) + 1, &fds, nullptr, nullptr, &timeout, nullptr);
#else
            ret = pselect(engine_event_queue_fd(g_engine_eq) + 1, &fds, nullptr, nullptr, &timeout, nullptr);
#endif
            if (ret == -1 && want_quit_flag == NO_QUIT && errno != EINTR) {
                interface_fatal("pselect() failed: %s", xstrerror(errno).c_str());
            }

            iface_tick();

            if (ret == 0) do_silent_seek();

#ifdef SIGWINCH
            if (want_resize_flag) do_resize();
#endif

            if (ret > 0) {
                if (FD_ISSET(STDIN_FILENO, &fds)) {
                    struct iface_key k;
                    iface_get_key(&k);
                    clear_interrupt();
                    menu_key(&k);
                }

                if (want_quit_flag == NO_QUIT) {
                    if (FD_ISSET(engine_event_queue_fd(g_engine_eq), &fds)) get_and_handle_event();
                    do_silent_seek();
#ifdef HAVE_SYS_INOTIFY_H
                    if (FD_ISSET(inotify_fd, &fds)) {
                        char dummy[4096];
                        ret = read(inotify_fd, dummy, sizeof(dummy));
                        reread_dir();
                    }
#endif
                }
            } else if (user_wants_interrupt()) {
                handle_interrupt();
            }

            if (want_quit_flag == NO_QUIT) update_mixer_name();
        }

        log_circular_log();
        log_circular_stop();
    }

    void end() {
        save_curr_dir();
        save_playlist_in_moc();
        engine_quit();
        g_engine_eq = nullptr;

#ifdef HAVE_SYS_INOTIFY_H
        if (inotify_wd >= 0) inotify_rm_watch(inotify_fd, inotify_wd);
        if (inotify_fd >= 0) close(inotify_fd);
#endif

        windows_end();
        keys_cleanup();

        plist_free(&dir_plist);
        plist_free(&playlist);
        plist_free(&queue);

        while (!events.empty()) {
            free_event_data(events.front().type, events.front().data);
            events.pop();
        }
        log_close();
    }

    void show_error(const char *msg) {
        iface_error(msg);
    }
};

static std::unique_ptr<UserInterface> ui;

void init_interface(struct engine_event_queue *eq, const std::vector<std::string> &args) {
    ui = std::make_unique<UserInterface>(eq, args);
}

void interface_loop() {
    if (ui) ui->loop();
}

void interface_end() {
    if (ui) {
        ui->end();
        ui.reset();
    }
}

void interface_error(const char *msg) {
    if (ui) ui->show_error(msg);
}

void interface_fatal(const char *format, ...) {
    std::string msg;
    va_list va;

    va_start(va, format);
    msg = format_msg_va(format, va);
    va_end(va);

    windows_end();
    fatal("%s", msg.c_str());
}

// EOF
