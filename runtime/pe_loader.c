#include "pe_loader.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define PE32PLUS_MAGIC 0x20b
#define IMAGE_FILE_MACHINE_AMD64 0x8664

typedef struct {
    uint8_t  data[1024 * 1024 * 64]; // file buffer (max 64MB; our exe is ~38MB)
    size_t   len;
} filebuf_t;

static int read_file(const char *path, filebuf_t *fb) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fb->len = fread(fb->data, 1, sizeof(fb->data), f);
    fclose(f);
    return fb->len > 0 ? 0 : -1;
}

static uint64_t rva_to_off(const uint8_t *b, size_t len, uint64_t rva,
                           uint16_t numsec, uint64_t sec_table_off) {
    for (uint16_t i = 0; i < numsec; i++) {
        const uint8_t *o = b + sec_table_off + i * 40;
        uint32_t vsize = *(uint32_t *)(o + 8);
        uint32_t vaddr = *(uint32_t *)(o + 12);
        uint32_t rsz   = *(uint32_t *)(o + 16);
        uint32_t raw   = *(uint32_t *)(o + 20);
        uint32_t span  = vsize > rsz ? vsize : rsz;
        if (rva >= vaddr && rva < vaddr + span) {
            uint64_t off = raw + (rva - vaddr);
            if (off < len) return off;
        }
    }
    return 0;
}

int pe_load(const char *path, uint64_t trampoline_base,
            pe_alloc_fn alloc, void *alloc_ctx, pe_image_t *out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->path, path, sizeof(out->path) - 1);

    filebuf_t fb;
    if (read_file(path, &fb) != 0) { fprintf(stderr, "[pe] cannot open %s\n", path); return -1; }

    const uint8_t *b = fb.data;
    if (fb.len < 0x40 || b[0] != 'M' || b[1] != 'Z') { fprintf(stderr, "[pe] bad DOS magic\n"); return -1; }
    uint32_t e_lfanew = *(uint32_t *)(b + 0x3c);
    if (e_lfanew + 24 > fb.len || memcmp(b + e_lfanew, "PE\0\0", 4) != 0) { fprintf(stderr, "[pe] bad PE sig\n"); return -1; }

    uint16_t machine = *(uint16_t *)(b + e_lfanew + 4);
    if (machine != IMAGE_FILE_MACHINE_AMD64) { fprintf(stderr, "[pe] not x86_64 (machine=0x%x)\n", machine); return -1; }

    uint16_t numsec = *(uint16_t *)(b + e_lfanew + 6);
    uint16_t optsz  = *(uint16_t *)(b + e_lfanew + 20);
    uint64_t opt_off = e_lfanew + 24;
    uint16_t magic = *(uint16_t *)(b + opt_off);
    if (magic != PE32PLUS_MAGIC) { fprintf(stderr, "[pe] not PE32+ (magic=0x%x)\n", magic); return -1; }

    uint32_t image_base   = 0;
    uint32_t entry_rva    = *(uint32_t *)(b + opt_off + 16);
    uint64_t ib64        = *(uint64_t *)(b + opt_off + 24); // ImageBase (8 bytes in PE32+)
    image_base = (uint32_t)ib64;
    out->image_base = ib64;
    out->entry_va = ib64 + entry_rva;
    out->trampoline_base = trampoline_base;

    uint64_t sec_table_off = opt_off + optsz;
    // Data directories: import table is index 1, at opt_off + 0x70 (PE32+).
    uint32_t imp_rva = *(uint32_t *)(b + opt_off + 0x70);
    uint32_t imp_size= *(uint32_t *)(b + opt_off + 0x74);

    // --- segments from sections ---
    out->n_segments = numsec;
    out->segments = (pe_segment_t *)calloc(numsec, sizeof(pe_segment_t));
    for (uint16_t i = 0; i < numsec; i++) {
        const uint8_t *o = b + sec_table_off + i * 40;
        uint32_t vsize = *(uint32_t *)(o + 8);
        uint32_t vaddr = *(uint32_t *)(o + 12);
        uint32_t rsz   = *(uint32_t *)(o + 16);
        uint32_t raw   = *(uint32_t *)(o + 20);
        uint32_t chars = *(uint32_t *)(o + 36);
        uint32_t perms = 0;
        if (chars & 0x40000000) perms |= 1; // readable
        if (chars & 0x80000000) perms |= 2; // writable
        if (chars & 0x20000000) perms |= 4; // executable
        uint32_t span = vsize > rsz ? vsize : rsz;
        out->segments[i].va = ib64 + vaddr;
        out->segments[i].size = span;
        out->segments[i].perms = perms;
        // data: point into file buffer if raw data present
        if (rsz > 0 && raw + rsz <= fb.len) out->segments[i].data = b + raw;
        else out->segments[i].data = NULL; // zero-filled (e.g. .bss)
    }

    // --- imports ---
    if (imp_rva && imp_size) {
        uint64_t io = rva_to_off(b, fb.len, imp_rva, numsec, sec_table_off);
        // First pass count
        size_t n = 0;
        uint64_t cur = io;
        while (cur + 20 <= fb.len) {
            uint32_t oft = *(uint32_t *)(b + cur);
            uint32_t name_rva = *(uint32_t *)(b + cur + 12);
            if (name_rva == 0) break;
            uint64_t nm_off = rva_to_off(b, fb.len, name_rva, numsec, sec_table_off);
            if (!nm_off) break;
            // count thunks
            uint64_t to = rva_to_off(b, fb.len, oft, numsec, sec_table_off);
            uint64_t ft = rva_to_off(b, fb.len, *(uint32_t *)(b + cur + 16), numsec, sec_table_off);
            uint64_t arr = to ? to : ft;
            size_t k = 0;
            while (arr + k * 8 + 8 <= fb.len) {
                uint64_t t = *(uint64_t *)(b + arr + k * 8);
                if (t == 0) break;
                k++;
            }
            n += k;
            cur += 20;
        }
        out->imports = (pe_import_t *)calloc(n ? n : 1, sizeof(pe_import_t));
        out->n_imports = 0;
        cur = io;
        while (cur + 20 <= fb.len) {
            uint32_t oft = *(uint32_t *)(b + cur);
            uint32_t name_rva = *(uint32_t *)(b + cur + 12);
            if (name_rva == 0) break;
            uint64_t nm_off = rva_to_off(b, fb.len, name_rva, numsec, sec_table_off);
            if (!nm_off) break;
            const char *dll = (const char *)(b + nm_off);
            uint64_t to = rva_to_off(b, fb.len, oft, numsec, sec_table_off);
            uint64_t ft = rva_to_off(b, fb.len, *(uint32_t *)(b + cur + 16), numsec, sec_table_off);
            uint64_t arr = to ? to : ft;
            size_t k = 0;
            while (arr + k * 8 + 8 <= fb.len) {
                uint64_t t = *(uint64_t *)(b + arr + k * 8);
                if (t == 0) break;
                uint64_t name_off = 0;
                char func[128] = {0};
                if (!(t & 0x8000000000000000ULL)) {
                    name_off = rva_to_off(b, fb.len, t & 0x7fffffffffffffffULL, numsec, sec_table_off);
                    if (name_off) {
                        // IMAGE_IMPORT_BY_NAME: 2-byte hint + name
                        const char *s = (const char *)(b + name_off + 2);
                        strncpy(func, s, 127);
                    }
                } else {
                    snprintf(func, sizeof(func), "#%u", (uint32_t)(t & 0xffff));
                }
                pe_import_t *imp = &out->imports[out->n_imports++];
                strncpy(imp->dll, dll, 63);
                strncpy(imp->func, func, 127);
                imp->iat_va = ib64 + ft + k * 8;       // FirstThunk array VA
                imp->tramp_va = alloc ? alloc(alloc_ctx, 16) : 0;
                k++;
            }
            cur += 20;
        }
    }
    fprintf(stderr, "[pe] loaded base=%p entry=%p segs=%zu imports=%zu\n",
            (void *)out->image_base, (void *)out->entry_va,
            out->n_segments, out->n_imports);
    return 0;
}

void pe_image_free(pe_image_t *img) {
    free(img->segments);
    free(img->imports);
    img->segments = NULL;
    img->imports = NULL;
    img->n_segments = img->n_imports = 0;
}

int pe_emit_trampoline(uint8_t *guest_writable, uint64_t va, uint64_t tramp_va) {
    (void)va;
    // UD2 = 0x0F 0x0B. The emulator code-hook intercepts this and dispatches
    // to the host shim. Two bytes is enough.
    guest_writable[0] = 0x0F;
    guest_writable[1] = 0x0B;
    return 0;
}
