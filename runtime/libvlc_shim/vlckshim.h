#ifndef VLCKSHIM_H
#define VLCKSHIM_H

// libVLC API shim. The exe imports 72 libvlc_* functions (see docs/imports.txt).
// These are bridged to iOS native MobileVLCKit so audio+video run NATIVE on
// arm64 -- we do NOT translate the x86_64 libvlc.dll. This is the single
// biggest latency win for the rhythm game.
//
// Declarations match the upstream libvlc ABI closely enough for the import
// bridge (handles are opaque pointers; scalars are int/uint64).

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* libvlc_instance_t;
typedef void* libvlc_media_player_t;
typedef void* libvlc_media_t;
typedef void* libvlc_media_list_t;
typedef void* libvlc_event_manager_t;

libvlc_instance_t     libvlc_new(int argc, const char *const argv[]);
void                  libvlc_release(libvlc_instance_t);
const char*           libvlc_get_version(void);
const char*           libvlc_get_compiler(void);
const char*           libvlc_get_changeset(void);
const char*           libvlc_errmsg(void);
int64_t               libvlc_clock(void);

libvlc_media_t*       libvlc_media_new_path(libvlc_instance_t, const char *path);
libvlc_media_t*       libvlc_media_new_location(libvlc_instance_t, const char *url);
libvlc_media_t*       libvlc_media_new_fd(libvlc_instance_t, int fd);
libvlc_media_t*       libvlc_media_new_callbacks(libvlc_instance_t, void *open_cb, void *read_cb,
                                                 void *seek_cb, void *close_cb, void *opaque);
void                  libvlc_media_add_option(libvlc_media_t*, const char *opt);
void                  libvlc_media_parse_with_options(libvlc_media_t*, int opts, int timeout);
void                  libvlc_media_parse_stop(libvlc_media_t*);
int                   libvlc_media_get_parsed_status(libvlc_media_t*);
const char*           libvlc_media_get_mrl(libvlc_media_t*);
int64_t               libvlc_media_get_duration(libvlc_media_t*);
char*                 libvlc_media_get_meta(libvlc_media_t*, int meta);
void                  libvlc_media_set_meta(libvlc_media_t*, int meta, const char *val);
int                   libvlc_media_save_meta(libvlc_media_t*);
void                  libvlc_media_release(libvlc_media_t*);
libvlc_media_list_t*  libvlc_media_subitems(libvlc_media_t*);
libvlc_media_t*       libvlc_media_list_item_at_index(libvlc_media_list_t*, int idx);
int                   libvlc_media_list_count(libvlc_media_list_t*);
void                  libvlc_media_list_release(libvlc_media_list_t*);
struct libvlc_media_stats_t;
int                   libvlc_media_get_stats(libvlc_media_t*, void *stats);
libvlc_event_manager_t* libvlc_media_event_manager(libvlc_media_t*);
int                   libvlc_media_get_track_count(libvlc_media_t*);

libvlc_media_player_t* libvlc_media_player_new(libvlc_instance_t);
void                  libvlc_media_player_release(libvlc_media_player_t*);
void                  libvlc_media_player_set_media(libvlc_media_player_t*, libvlc_media_t*);
libvlc_media_t*       libvlc_media_player_get_media(libvlc_media_player_t*);
void                  libvlc_media_player_play(libvlc_media_player_t*);
void                  libvlc_media_player_pause(libvlc_media_player_t*);
void                  libvlc_media_player_set_pause(libvlc_media_player_t*, int);
void                  libvlc_media_player_stop(libvlc_media_player_t*);
int                   libvlc_media_player_is_playing(libvlc_media_player_t*);
int                   libvlc_media_player_will_play(libvlc_media_player_t*);
int                   libvlc_media_player_can_pause(libvlc_media_player_t*);
int                   libvlc_media_player_is_seekable(libvlc_media_player_t*);
int64_t               libvlc_media_player_get_length(libvlc_media_player_t*);
int64_t               libvlc_media_player_get_time(libvlc_media_player_t*);
void                  libvlc_media_player_set_time(libvlc_media_player_t*, int64_t);
float                 libvlc_media_player_get_position(libvlc_media_player_t*);
void                  libvlc_media_player_set_position(libvlc_media_player_t*, float);
float                 libvlc_media_player_get_rate(libvlc_media_player_t*);
void                  libvlc_media_player_set_rate(libvlc_media_player_t*, float);
int                   libvlc_media_player_get_chapter(libvlc_media_player_t*);
void                  libvlc_media_player_set_chapter(libvlc_media_player_t*, int);
int                   libvlc_media_player_get_chapter_count(libvlc_media_player_t*);
void                  libvlc_media_player_next_chapter(libvlc_media_player_t*);
void                  libvlc_media_player_previous_chapter(libvlc_media_player_t*);
int                   libvlc_media_player_get_role(libvlc_media_player_t*);
void                  libvlc_media_player_set_role(libvlc_media_player_t*, int);
libvlc_event_manager_t* libvlc_media_player_event_manager(libvlc_media_player_t*);

// video: the game receives raw frames via callbacks and composites them
void libvlc_video_set_callbacks(libvlc_media_player_t*, void *lock, void *unlock, void *display, void *opaque);
void libvlc_video_set_format_callbacks(libvlc_media_player_t*, void *setup, void *cleanup);
int  libvlc_video_get_size(libvlc_media_player_t*, unsigned idx, unsigned *w, unsigned *h);

// audio
int  libvlc_audio_get_volume(libvlc_media_player_t*);
void libvlc_audio_set_volume(libvlc_media_player_t*, int);
int  libvlc_audio_get_mute(libvlc_media_player_t*);
void libvlc_audio_set_mute(libvlc_media_player_t*, int);
int  libvlc_audio_get_track(libvlc_media_player_t*);
void libvlc_audio_set_track(libvlc_media_player_t*, int);
int  libvlc_audio_get_track_count(libvlc_media_player_t*);
int  libvlc_audio_get_channel(libvlc_media_player_t*);
void libvlc_audio_set_channel(libvlc_media_player_t*, int);
int64_t libvlc_audio_get_delay(libvlc_media_player_t*);
void libvlc_audio_set_delay(libvlc_media_player_t*, int64_t);
void* libvlc_audio_output_list_get(libvlc_media_player_t*);
void libvlc_audio_output_list_release(void*);
int  libvlc_audio_output_set(libvlc_media_player_t*, const char *name);

int  libvlc_event_attach(libvlc_event_manager_t*, int event, void *cb, void *user);

#ifdef __cplusplus
}
#endif

#endif // VLCKSHIM_H
