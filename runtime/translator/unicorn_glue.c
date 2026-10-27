// Unicorn-based x86_64 -> host (arm64) translator with:
//   * import thunking (guest CALL [IAT] -> host shim via code hook)
//   * AOT warmup pass (pre-translate static code so the first song doesn't hitch)
//   * JIT fallback (Unicorn lazily translates any block not warmed)
//
// This file is the bridge between the PE loader, the Win32/libVLC shims, and
// the CPU emulator. It does NOT contain x86 decoding -- Unicorn does.

#include "unicorn_glue.h"
#include "pe_loader.h"
#include "win32_shim.h"
#include "path_resolve.h"
#include "vlckshim.h"       // provides vlckshim_register_all + libvlc_* bridges

#include <unicorn/unicorn.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>   // struct timeval / gettimeofday

struct emu_ctx {
    uc_engine *uc;
    pe_image_t img;
    uint64_t   tramp_base;
    size_t     n_imp;
    size_t     blocks;
    size_t     calls;
    uint64_t   last_pc;
};

// Installed once (in emu_load_exe) after the trampoline region exists; see below.
static void install_hook(emu_ctx_t *c);

emu_ctx_t *emu_create(void) {
    emu_ctx_t *c = (emu_ctx_t*)calloc(1, sizeof(*c));
    uc_err e = uc_open(UC_ARCH_X86, UC_MODE_64, &c->uc);
    if (e != UC_ERR_OK) { fprintf(stderr, "[emu] uc_open failed: %s\n", uc_strerror(e)); free(c); return NULL; }
    return c;
}

// guest allocator for trampolines (returns contiguous guest VAs)
static uint64_t tramp_alloc(void *ctx, uint64_t size) {
    emu_ctx_t *c = (emu_ctx_t*)ctx;
    uint64_t base = c->tramp_base;
    c->tramp_base += size; // caller asks 16 per trampoline
    return base;
}

int emu_load_exe(emu_ctx_t *ctx, const char *exe_path,
                 const char *asset_root, uint64_t tramp_base) {
    ctx->tramp_base = tramp_base;
    path_resolve_init(asset_root);
    win32_shim_register_all();
    vlckshim_register_all();

    if (pe_load(exe_path, tramp_base, tramp_alloc, ctx, &ctx->img) != 0) return -1;
    ctx->n_imp = ctx->img.n_imports;
    // The import hook locates trampolines via img.trampoline_base. The loader
    // itself leaves it untouched, so set it here or no import is ever hooked.
    ctx->img.trampoline_base = tramp_base;

    // Map PE segments into the emulator.
    for (size_t i = 0; i < ctx->img.n_segments; i++) {
        pe_segment_t *s = &ctx->img.segments[i];
        uint32_t perms = 0;
        if (s->perms & 1) perms |= UC_PROT_READ;
        if (s->perms & 2) perms |= UC_PROT_WRITE;
        if (s->perms & 4) perms |= UC_PROT_EXEC;
        uc_err e = uc_mem_map(ctx->uc, s->va, s->size, perms ? perms : UC_PROT_READ);
        if (e != UC_ERR_OK) { fprintf(stderr, "[emu] map seg %zu failed: %s\n", i, uc_strerror(e)); return -1; }
        if (s->data) uc_mem_write(ctx->uc, s->va, s->data, s->size);
    }

    // Trampoline region (RWX) + patch IAT.
    uint64_t tramp_region = tramp_base;
    uint64_t tramp_size = (ctx->img.n_imports + 1) * 16;
    uc_mem_map(ctx->uc, tramp_region, tramp_size, UC_PROT_READ | UC_PROT_WRITE | UC_PROT_EXEC);

    uint8_t ud2[2] = {0x0F, 0x0B};
    for (size_t i = 0; i < ctx->img.n_imports; i++) {
        pe_import_t *imp = &ctx->img.imports[i];
        uint64_t tv = tramp_base + i * 16;
        uc_mem_write(ctx->uc, tv, ud2, 2);              // trampoline body
        uint64_t le = tv;                               // IAT slot value (guest VA)
        uc_mem_write(ctx->uc, imp->iat_va, &le, 8);     // redirect import to trampoline
    }
    install_hook(ctx); // install once; persists for warmup + run
    return 0;
}

// --- import bridge hook ---
static void hook_code(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    emu_ctx_t *c = (emu_ctx_t*)user_data;
    // Is this a trampoline? (tramps are 16-byte aligned after tramp_region)
    uint64_t region = c->img.trampoline_base;
    uint64_t span = (c->img.n_imports + 1) * 16;
    if (address >= region && address < region + span && ((address - region) % 16) == 0) {
        size_t idx = (address - region) / 16;
        if (idx < c->img.n_imports) {
            pe_import_t *imp = &c->img.imports[idx];
            uint64_t rcx, rdx, r8, r9, rsp, rax;
            uc_reg_read(uc, UC_X86_REG_RCX, &rcx);
            uc_reg_read(uc, UC_X86_REG_RDX, &rdx);
            uc_reg_read(uc, UC_X86_REG_R8,  &r8);
            uc_reg_read(uc, UC_X86_REG_R9,  &r9);
            uc_reg_read(uc, UC_X86_REG_RSP, &rsp);
            uint64_t retaddr; uc_mem_read(uc, rsp, &retaddr, 8);
            uint64_t stk[4];  uc_mem_read(uc, rsp + 0x20, stk, 32);

            c->calls++;
            shim_fp fp = shim_lookup(imp->dll, imp->func);
            typedef uint64_t (*F)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
            if (fp) {
                rax = ((F)fp)(rcx, rdx, r8, r9, stk[0], stk[1], stk[2], stk[3]);
            } else {
                fprintf(stderr, "[emu] unimplemented import %s!%s\n", imp->dll, imp->func);
                rax = 0;
            }
            uc_reg_write(uc, UC_X86_REG_RAX, &rax);
            uint64_t npc = retaddr;  uc_reg_write(uc, UC_X86_REG_RIP, &npc);
            uint64_t nrsp = rsp + 8; uc_reg_write(uc, UC_X86_REG_RSP, &nrsp);
            uc_emu_stop(uc); // break out so the outer loop resumes at retaddr
            return;
        }
    }
    c->blocks++;
}

static void install_hook(emu_ctx_t *c) {
    uc_hook h;
    // Only hook the trampoline region (each import's UD2). Hooking every guest
    // instruction would be catastrophically slow for a real-time rhythm game, and
    // installing per-emu_run would stack duplicate hooks and double-dispatch.
    uint64_t region = c->img.trampoline_base;
    uint64_t span   = (c->img.n_imports + 1) * 16;
    uc_hook_add(c->uc, &h, UC_HOOK_CODE, (void*)hook_code, c, region, region + span);
}

int emu_run(emu_ctx_t *ctx, uint64_t entry, uint64_t until_va, uint64_t timeout_us) {
    // Set Windows EXE entry registers: rcx=hInstance, rdx=0, r8=cmdline, r9=1
    uint64_t r = ctx->img.image_base; uc_reg_write(ctx->uc, UC_X86_REG_RCX, &r);
    uint64_t z = 0; uc_reg_write(ctx->uc, UC_X86_REG_RDX, &z);
    uc_reg_write(ctx->uc, UC_X86_REG_R8,  &z);
    uc_reg_write(ctx->uc, UC_X86_REG_R9,  &z); uc_reg_write(ctx->uc, UC_X86_REG_R9, &z); // nShowCmd=1 below
    uint64_t one = 1; uc_reg_write(ctx->uc, UC_X86_REG_R9, &one);

    uint64_t pc = entry;
    struct timeval t0; gettimeofday(&t0, NULL);
    int guard = 0;
    while (guard++ < 100000) {
        uc_err e = uc_emu_start(ctx->uc, pc, until_va ? until_va : 0, 0, 0);
        uc_reg_read(ctx->uc, UC_X86_REG_RIP, &pc);
        if (until_va && pc == until_va) return 0;
        if (e == UC_ERR_OK && !until_va) return 0;
        // stopped by trampoline hook -> continue
        struct timeval t1; gettimeofday(&t1, NULL);
        uint64_t el = (t1.tv_sec - t0.tv_sec) * 1000000ULL + (t1.tv_usec - t0.tv_usec);
        if (timeout_us && el > timeout_us) return 1;
    }
    return -1;
}

int emu_warmup(emu_ctx_t *ctx, uint64_t entry, uint64_t warmup_us) {
    // Run the engine init (muted) to populate Unicorn's translation cache.
    return emu_run(ctx, entry, 0, warmup_us);
}

uint64_t emu_entry_va(emu_ctx_t *ctx) { return ctx ? ctx->img.entry_va : 0; }

size_t emu_translated_blocks(emu_ctx_t *ctx) { return ctx->blocks; }
size_t emu_trampoline_calls(emu_ctx_t *ctx) { return ctx->calls; }

void emu_destroy(emu_ctx_t *ctx) {
    if (!ctx) return;
    uc_close(ctx->uc);
    pe_image_free(&ctx->img);
    path_resolve_shutdown();
    free(ctx);
}
