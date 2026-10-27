// iOS app glue: wires the emulator + shims into the app, runs on a real-time
// thread. Swift calls run_emulator(); frames are delivered back via
// vlckshim_deliver_frame() from the VLCMediaPlayer delegate.

#include "unicorn_glue.h"
#include "vlckshim.h"
#include "path_resolve.h"

#include <pthread.h>
#include <stdio.h>

static const char *g_exe = NULL;
static const char *g_assets = NULL;

void app_set_paths(const char *exe, const char *assets) {
    g_exe = exe; g_assets = assets;
}

// Run the emulator. Blocks until the guest exits. Call on a dedicated thread.
int run_emulator(void) {
    if (!g_exe || !g_assets) { fprintf(stderr, "[app] paths not set\n"); return -1; }
    emu_ctx_t *ctx = emu_create();
    if (!ctx) return -1;
    if (emu_load_exe(ctx, g_exe, g_assets, 0x70000000) != 0) { emu_destroy(ctx); return -1; }
    fprintf(stderr, "[app] warmup: translating static code...\n");
    emu_warmup(ctx, emu_entry_va(ctx), 2 * 1000 * 1000); // 2s AOT warmup
    fprintf(stderr, "[app] translated blocks=%zu, starting main loop\n", emu_translated_blocks(ctx));
    emu_run(ctx, emu_entry_va(ctx), 0, 0); // run until guest exits
    emu_destroy(ctx);
    return 0;
}

// Called by the Swift VLCMediaPlayer delegate for every decoded video frame.
// Forwards the CVPixelBuffer to the game's libvlc lock/unlock callbacks so it
// can composite the video into its own renderer.
void vlckshim_deliver_frame(void *pixelBuffer); // defined in vlckshim.m
void app_deliver_frame(void *pixelBuffer) {
    vlckshim_deliver_frame(pixelBuffer);
}
