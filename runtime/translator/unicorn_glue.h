#ifndef UNICORN_GLUE_H
#define UNICORN_GLUE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Emulator context for running the x86_64 Windows exe on iOS (arm64) via
// Unicorn (TCG-based x86_64 -> host translation, with JIT + an AOT warmup pass).
typedef struct emu_ctx emu_ctx_t;

emu_ctx_t *emu_create(void);

// Load the exe, map its segments, patch the IAT to point at guest trampolines,
// and wire each trampoline to the host shim resolved by (dll, func) name.
// asset_root is the directory whose tree is indexed for case-insensitive
// path resolution (path_resolve_init is called here).
int  emu_load_exe(emu_ctx_t *ctx, const char *exe_path,
                  const char *asset_root, uint64_t tramp_base);

// Guest entry point (AddressOfEntryPoint + image base), valid after a
// successful emu_load_exe. Provided as an accessor so the context struct can
// stay opaque to callers.
uint64_t emu_entry_va(emu_ctx_t *ctx);

// Run guest code starting at `entry` until it reaches `until_va` (e.g. a known
// idle/menu hook) or `timeout_us` elapses. Returns 0 on stop-VA hit, 1 on
// timeout, <0 on error. Re-entrant: call again to continue after a yield.
int  emu_run(emu_ctx_t *ctx, uint64_t entry, uint64_t until_va, uint64_t timeout_us);

// AOT warmup: force translation of the static code reachable from entry by
// running it (muted) for `warmup_us`. After this, the translation cache holds
// all engine-init / asset-parse code, so the first real song has no JIT hitch.
int  emu_warmup(emu_ctx_t *ctx, uint64_t entry, uint64_t warmup_us);

// Stats for latency tuning.
size_t emu_translated_blocks(emu_ctx_t *ctx);
size_t emu_trampoline_calls(emu_ctx_t *ctx);

void emu_destroy(emu_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif // UNICORN_GLUE_H
