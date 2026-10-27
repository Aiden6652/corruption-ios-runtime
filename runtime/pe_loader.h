#ifndef PE_LOADER_H
#define PE_LOADER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// A contiguous guest memory segment to map into the emulator.
typedef struct {
    uint64_t va;     // guest virtual address
    uint64_t size;
    uint32_t perms;  // 1=R 2=W 4=X  (Unicorn UC_PROT_*)
    const uint8_t *data; // can be NULL for zero-filled (e.g. .bss)
} pe_segment_t;

// One imported symbol whose IAT slot we redirect to a guest trampoline.
typedef struct {
    uint64_t iat_va;       // guest VA of the IAT entry (write trampoline here)
    uint64_t tramp_va;     // guest VA of the trampoline the glue will hook
    char     dll[64];
    char     func[128];
} pe_import_t;

typedef struct {
    uint64_t       image_base;
    uint64_t       entry_va;     // AddressOfEntryPoint + image_base
    uint64_t       trampoline_base;
    pe_segment_t  *segments;
    size_t         n_segments;
    pe_import_t   *imports;
    size_t         n_imports;
    char           path[1024];
} pe_image_t;

// Callback the loader uses to allocate guest memory for trampolines.
// Returns a guest VA, or 0 on failure.
typedef uint64_t (*pe_alloc_fn)(void *ctx, uint64_t size);

// Load a PE (x86_64 only). Caller provides a guest allocator for trampolines.
// On success returns 0; ownership of segments/imports arrays is transferred to
// the caller (free with pe_image_free).
int  pe_load(const char *path, uint64_t trampoline_base,
             pe_alloc_fn alloc, void *alloc_ctx, pe_image_t *out);

void pe_image_free(pe_image_t *img);

// Emit a tiny trampoline at `va`: a single UD2 (0x0F 0x0B) so the emulator's
// code hook can intercept the call and dispatch to the host shim. Returns 0.
int  pe_emit_trampoline(uint8_t *guest_writable, uint64_t va, uint64_t tramp_va);

#ifdef __cplusplus
}
#endif

#endif // PE_LOADER_H
