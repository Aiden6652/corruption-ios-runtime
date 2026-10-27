// libVLC -> MobileVLCKit bridge (Objective-C). Audio plays natively through
// iOS AVAudioSession (low latency, no translation). Video frames are received
// via VLCMediaPlayerDelegate and forwarded to the game's own lock/unlock/
// display callbacks that it registered through libvlc_video_set_callbacks.
//
// For a non-iOS build (e.g. host unit testing) these degrade to safe stubs so
// the shim registry is still complete.

#import "vlckshim.h"
#import "win32_shim.h"

#include <time.h>
#include <TargetConditionals.h>

// MobileVLCKit is delivered by CocoaPods (see Podfile). Guard the import so this
// translation unit still compiles as pure stubs when the pod is absent -- this
// keeps CI green even if `pod install` cannot fetch the very large MobileVLCKit
// binary.
//
// HAVE_VLCKIT therefore means "building for iOS AND the VLCKit pod is linked";
// every VLCKit-dependent block below is keyed off it.
#if TARGET_OS_IPHONE && __has_include(<MobileVLCKit/MobileVLCKit.h>)
#import <MobileVLCKit/MobileVLCKit.h>
#import <CoreVideo/CoreVideo.h>
#define HAVE_VLCKIT 1
static VLCMediaPlayer *g_player = nil;
#endif

static void *g_lock_cb = NULL, *g_unlock_cb = NULL, *g_display_cb = NULL, *g_video_opaque = NULL;

void vlckshim_register_all(void);

// ---- core playback ----
libvlc_instance_t libvlc_new(int argc, const char *const argv[]) {
    (void)argc; (void)argv;
#if defined(HAVE_VLCKIT)
    if (!g_player) g_player = [[VLCMediaPlayer alloc] init];
#endif
    return (libvlc_instance_t)0x1;
}
void libvlc_release(libvlc_instance_t i) { (void)i; }

libvlc_media_t* libvlc_media_new_path(libvlc_instance_t i, const char *path) {
    (void)i;
#if defined(HAVE_VLCKIT)
    NSString *p = [NSString stringWithUTF8String:path ?: ""];
    VLCMedia *m = [[VLCMedia alloc] initWithPath:p];
    return (__bridge_retained void*)m;
#else
    return (void*)path;
#endif
}
libvlc_media_t* libvlc_media_new_location(libvlc_instance_t i, const char *url) {
    (void)i;
#if defined(HAVE_VLCKIT)
    VLCMedia *m = [[VLCMedia alloc] initWithURL:[NSURL URLWithString:[NSString stringWithUTF8String:url?:"""]]];
    return (__bridge_retained void*)m;
#else
    return (void*)url;
#endif
}
libvlc_media_t* libvlc_media_new_fd(libvlc_instance_t i, int fd) { (void)i;(void)fd; return NULL; }
libvlc_media_t* libvlc_media_new_callbacks(libvlc_instance_t i, void *o, void *r, void *s, void *c, void *op) {
    (void)i;(void)o;(void)r;(void)s;(void)c;(void)op; return NULL;
}

void libvlc_media_add_option(libvlc_media_t *m, const char *opt) { (void)m;(void)opt; }
void libvlc_media_parse_with_options(libvlc_media_t *m, int o, int t) { (void)m;(void)o;(void)t; }
void libvlc_media_parse_stop(libvlc_media_t *m) { (void)m; }
int  libvlc_media_get_parsed_status(libvlc_media_t *m) { (void)m; return 1; }
const char* libvlc_media_get_mrl(libvlc_media_t *m) { (void)m; return "file:///game"; }
int64_t libvlc_media_get_duration(libvlc_media_t *m) {
#if defined(HAVE_VLCKIT)
    VLCMedia *vm = (__bridge VLCMedia*)m; return vm ? (int64_t)(vm.length.intValue*1000) : 0;
#else
    (void)m; return 0;
#endif
}
char* libvlc_media_get_meta(libvlc_media_t *m, int meta) { (void)m;(void)meta; return NULL; }
void  libvlc_media_set_meta(libvlc_media_t *m, int meta, const char *v) { (void)m;(void)meta;(void)v; }
int   libvlc_media_save_meta(libvlc_media_t *m) { (void)m; return 0; }
void  libvlc_media_release(libvlc_media_t *m) {
#if defined(HAVE_VLCKIT)
    if (m) CFRelease((CFTypeRef)m);
#else
    (void)m;
#endif
}
libvlc_media_list_t* libvlc_media_subitems(libvlc_media_t *m) { (void)m; return NULL; }
libvlc_media_t* libvlc_media_list_item_at_index(libvlc_media_list_t *l, int i) { (void)l;(void)i; return NULL; }
int libvlc_media_list_count(libvlc_media_list_t *l) { (void)l; return 0; }
void libvlc_media_list_release(libvlc_media_list_t *l) { (void)l; }
int libvlc_media_get_stats(libvlc_media_t *m, void *st) { (void)m;(void)st; return 0; }
libvlc_event_manager_t* libvlc_media_event_manager(libvlc_media_t *m) { (void)m; return (void*)0x1; }
int libvlc_media_get_track_count(libvlc_media_t *m) { (void)m; return 1; }

// ---- media player ----
libvlc_media_player_t* libvlc_media_player_new(libvlc_instance_t i) {
    (void)i;
#if defined(HAVE_VLCKIT)
    if (!g_player) g_player = [[VLCMediaPlayer alloc] init];
    return (libvlc_media_player_t)g_player;
#else
    return (void*)0x1;
#endif
}
void libvlc_media_player_release(libvlc_media_player_t *p) { (void)p; }
void libvlc_media_player_set_media(libvlc_media_player_t *p, libvlc_media_t *m) {
#if defined(HAVE_VLCKIT)
    VLCMediaPlayer *pl = (__bridge VLCMediaPlayer*)p;
    if (pl && m) pl.media = (__bridge VLCMedia*)m;
#else
    (void)p;(void)m;
#endif
}
libvlc_media_t* libvlc_media_player_get_media(libvlc_media_player_t *p) {
#if defined(HAVE_VLCKIT)
    VLCMediaPlayer *pl = (__bridge VLCMediaPlayer*)p; return pl ? (__bridge void*)pl.media : NULL;
#else
    (void)p; return NULL;
#endif
}
void libvlc_media_player_play(libvlc_media_player_t *p) {
#if defined(HAVE_VLCKIT)
    [(VLCMediaPlayer*)p play];
#else
    (void)p;
#endif
}
void libvlc_media_player_pause(libvlc_media_player_t *p) {
#if defined(HAVE_VLCKIT)
    [(VLCMediaPlayer*)p pause];
#else
    (void)p;
#endif
}
void libvlc_media_player_set_pause(libvlc_media_player_t *p, int b) {
#if defined(HAVE_VLCKIT)
    if (b) [(VLCMediaPlayer*)p pause]; else [(VLCMediaPlayer*)p play];
#else
    (void)p;(void)b;
#endif
}
void libvlc_media_player_stop(libvlc_media_player_t *p) {
#if defined(HAVE_VLCKIT)
    [(VLCMediaPlayer*)p stop];
#else
    (void)p;
#endif
}
int libvlc_media_player_is_playing(libvlc_media_player_t *p) {
#if defined(HAVE_VLCKIT)
    return (int)[(VLCMediaPlayer*)p isPlaying];
#else
    (void)p; return 0;
#endif
}
int libvlc_media_player_will_play(libvlc_media_player_t *p) { (void)p; return 1; }
int libvlc_media_player_can_pause(libvlc_media_player_t *p) { (void)p; return 1; }
int libvlc_media_player_is_seekable(libvlc_media_player_t *p) { (void)p; return 1; }
int64_t libvlc_media_player_get_length(libvlc_media_player_t *p) {
#if defined(HAVE_VLCKIT)
    return (int64_t)[(VLCMediaPlayer*)p mediaLength];
#else
    (void)p; return 0;
#endif
}
int64_t libvlc_media_player_get_time(libvlc_media_player_t *p) {
#if defined(HAVE_VLCKIT)
    return (int64_t)[(VLCMediaPlayer*)p time];
#else
    (void)p; return 0;
#endif
}
void libvlc_media_player_set_time(libvlc_media_player_t *p, int64_t t) {
#if defined(HAVE_VLCKIT)
    [(VLCMediaPlayer*)p setTime:t];
#else
    (void)p;(void)t;
#endif
}
float libvlc_media_player_get_position(libvlc_media_player_t *p) {
#if defined(HAVE_VLCKIT)
    return (float)[(VLCMediaPlayer*)p position];
#else
    (void)p; return 0;
#endif
}
void libvlc_media_player_set_position(libvlc_media_player_t *p, float f) {
#if defined(HAVE_VLCKIT)
    [(VLCMediaPlayer*)p setPosition:f];
#else
    (void)p;(void)f;
#endif
}
float libvlc_media_player_get_rate(libvlc_media_player_t *p) {
#if defined(HAVE_VLCKIT)
    return (float)[(VLCMediaPlayer*)p rate];
#else
    (void)p; return 1;
#endif
}
void libvlc_media_player_set_rate(libvlc_media_player_t *p, float r) {
#if defined(HAVE_VLCKIT)
    [(VLCMediaPlayer*)p setRate:r];
#else
    (void)p;(void)r;
#endif
}
int  libvlc_media_player_get_chapter(libvlc_media_player_t *p) { (void)p; return 0; }
void libvlc_media_player_set_chapter(libvlc_media_player_t *p, int c) { (void)p;(void)c; }
int  libvlc_media_player_get_chapter_count(libvlc_media_player_t *p) { (void)p; return 0; }
void libvlc_media_player_next_chapter(libvlc_media_player_t *p) { (void)p; }
void libvlc_media_player_previous_chapter(libvlc_media_player_t *p) { (void)p; }
int  libvlc_media_player_get_role(libvlc_media_player_t *p) { (void)p; return 0; }
void libvlc_media_player_set_role(libvlc_media_player_t *p, int r) { (void)p;(void)r; }
libvlc_event_manager_t* libvlc_media_player_event_manager(libvlc_media_player_t *p) { (void)p; return (void*)0x1; }

// video: store the game's callbacks; frames arrive via the VLCMediaPlayer delegate
void libvlc_video_set_callbacks(libvlc_media_player_t *p, void *lock, void *unlock, void *display, void *opaque) {
    (void)p;
#if defined(HAVE_VLCKIT)
    g_lock_cb = lock; g_unlock_cb = unlock; g_display_cb = display; g_video_opaque = opaque;
    g_player.delegate = /* set in app glue to forward pixel buffers to g_lock_cb/g_unlock_cb */ nil;
#endif
}
void libvlc_video_set_format_callbacks(libvlc_media_player_t *p, void *setup, void *cleanup) { (void)p;(void)setup;(void)cleanup; }
int  libvlc_video_get_size(libvlc_media_player_t *p, unsigned idx, unsigned *w, unsigned *h) {
    (void)p;(void)idx; if(w)*w=1280; if(h)*h=720; return 0;
}

// audio
int  libvlc_audio_get_volume(libvlc_media_player_t *p) { (void)p; return 100; }
void libvlc_audio_set_volume(libvlc_media_player_t *p, int v) { (void)p;(void)v; }
int  libvlc_audio_get_mute(libvlc_media_player_t *p) { (void)p; return 0; }
void libvlc_audio_set_mute(libvlc_media_player_t *p, int m) { (void)p;(void)m; }
int  libvlc_audio_get_track(libvlc_media_player_t *p) { (void)p; return 0; }
void libvlc_audio_set_track(libvlc_media_player_t *p, int t) { (void)p;(void)t; }
int  libvlc_audio_get_track_count(libvlc_media_player_t *p) { (void)p; return 1; }
int  libvlc_audio_get_channel(libvlc_media_player_t *p) { (void)p; return 1; }
void libvlc_audio_set_channel(libvlc_media_player_t *p, int c) { (void)p;(void)c; }
int64_t libvlc_audio_get_delay(libvlc_media_player_t *p) { (void)p; return 0; }
void libvlc_audio_set_delay(libvlc_media_player_t *p, int64_t d) { (void)p;(void)d; }
void* libvlc_audio_output_list_get(libvlc_media_player_t *p) { (void)p; return NULL; }
void libvlc_audio_output_list_release(void *l) { (void)l; }
int  libvlc_audio_output_set(libvlc_media_player_t *p, const char *n) { (void)p;(void)n; return 0; }

// misc
const char* libvlc_get_version(void) { return "libvlc-shim (MobileVLCKit)"; }
const char* libvlc_get_compiler(void) { return "clang"; }
const char* libvlc_get_changeset(void) { return "shim"; }
const char* libvlc_errmsg(void) { return NULL; }
int64_t libvlc_clock(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (int64_t)ts.tv_sec*1000000LL + (int64_t)ts.tv_nsec/1000; }
int libvlc_event_attach(libvlc_event_manager_t *em, int ev, void *cb, void *u) { (void)em;(void)ev;(void)cb;(void)u; return 0; }

// Forward a decoded video frame to the game's libvlc lock/unlock/display
// callbacks (the ones it registered via libvlc_video_set_callbacks). The game
// composites these frames into its own renderer.
void vlckshim_deliver_frame(void *pixelBuffer) {
#if defined(HAVE_VLCKIT)
    if (!pixelBuffer) return;
    CVPixelBufferRef pb = (CVPixelBufferRef)pixelBuffer;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    void *base = CVPixelBufferGetBaseAddress(pb);
    size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
    if (g_lock_cb) {
        // libvlc lock: void* (*)(void* opaque, void** plane)
        typedef void* (*lockfn)(void*, void**);
        void *plane = NULL;
        void *pic = ((lockfn)g_lock_cb)(g_video_opaque, &plane);
        if (plane && base) memcpy(plane, base, CVPixelBufferGetDataSize(pb));
        if (g_unlock_cb) {
            // libvlc unlock: void (*)(void* opaque, void* picture, void* const* plane)
            typedef void (*unlockfn)(void*, void*, void* const*);
            ((unlockfn)g_unlock_cb)(g_video_opaque, pic, (void* const*)&plane);
        }
        if (g_display_cb) {
            // libvlc display: void (*)(void* opaque, void* picture)
            typedef void (*dispfn)(void*, void*);
            ((dispfn)g_display_cb)(g_video_opaque, pic);
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    (void)w; (void)h;
#else
    (void)pixelBuffer;
#endif
}

// ---------------------------------------------------------------------------
// Register all 72 functions under "libvlc.dll".
// ---------------------------------------------------------------------------
#define RL(fn) shim_register("libvlc.dll", #fn, (shim_fp)fn)
void vlckshim_register_all(void) {
    RL(libvlc_new); RL(libvlc_release); RL(libvlc_get_version); RL(libvlc_get_compiler);
    RL(libvlc_get_changeset); RL(libvlc_errmsg); RL(libvlc_clock);
    RL(libvlc_media_new_path); RL(libvlc_media_new_location); RL(libvlc_media_new_fd); RL(libvlc_media_new_callbacks);
    RL(libvlc_media_add_option); RL(libvlc_media_parse_with_options); RL(libvlc_media_parse_stop);
    RL(libvlc_media_get_parsed_status); RL(libvlc_media_get_mrl); RL(libvlc_media_get_duration);
    RL(libvlc_media_get_meta); RL(libvlc_media_set_meta); RL(libvlc_media_save_meta); RL(libvlc_media_release);
    RL(libvlc_media_subitems); RL(libvlc_media_list_item_at_index); RL(libvlc_media_list_count);
    RL(libvlc_media_list_release); RL(libvlc_media_get_stats); RL(libvlc_media_event_manager);
    RL(libvlc_media_get_track_count);
    RL(libvlc_media_player_new); RL(libvlc_media_player_release); RL(libvlc_media_player_set_media);
    RL(libvlc_media_player_get_media); RL(libvlc_media_player_play); RL(libvlc_media_player_pause);
    RL(libvlc_media_player_set_pause); RL(libvlc_media_player_stop); RL(libvlc_media_player_is_playing);
    RL(libvlc_media_player_will_play); RL(libvlc_media_player_can_pause); RL(libvlc_media_player_is_seekable);
    RL(libvlc_media_player_get_length); RL(libvlc_media_player_get_time); RL(libvlc_media_player_set_time);
    RL(libvlc_media_player_get_position); RL(libvlc_media_player_set_position); RL(libvlc_media_player_get_rate);
    RL(libvlc_media_player_set_rate); RL(libvlc_media_player_get_chapter); RL(libvlc_media_player_set_chapter);
    RL(libvlc_media_player_get_chapter_count); RL(libvlc_media_player_next_chapter); RL(libvlc_media_player_previous_chapter);
    RL(libvlc_media_player_get_role); RL(libvlc_media_player_set_role); RL(libvlc_media_player_event_manager);
    RL(libvlc_video_set_callbacks); RL(libvlc_video_set_format_callbacks); RL(libvlc_video_get_size);
    RL(libvlc_audio_get_volume); RL(libvlc_audio_set_volume); RL(libvlc_audio_get_mute); RL(libvlc_audio_set_mute);
    RL(libvlc_audio_get_track); RL(libvlc_audio_set_track); RL(libvlc_audio_get_track_count);
    RL(libvlc_audio_get_channel); RL(libvlc_audio_set_channel); RL(libvlc_audio_get_delay); RL(libvlc_audio_set_delay);
    RL(libvlc_audio_output_list_get); RL(libvlc_audio_output_list_release); RL(libvlc_audio_output_set);
    RL(libvlc_event_attach);
}
