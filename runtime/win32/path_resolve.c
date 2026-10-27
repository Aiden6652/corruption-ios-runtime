#include "path_resolve.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <tchar.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Very small open-addressing hash map: lowercased-normalized path -> real path.
// We keep both because we return the *real* (correct-case) path to the caller,
// but look up by the lowercased key (Windows semantics).
// ---------------------------------------------------------------------------

typedef struct {
    char *key;   // lowercased, normalized
    char *value; // real on-disk path (correct case)
    int   used;
} bucket_t;

#define INIT_CAP 65536
static bucket_t *g_map = NULL;
static size_t    g_cap = 0;
static size_t    g_count = 0;
static char      g_root[4096] = {0};
static char      g_cwd[4096]  = {0};
static char      g_scratch[8192] = {0};

static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + c;
    return h;
}

static void map_put(const char *key, const char *value) {
    if (!g_map) return;
    // grow
    if ((g_count + 1) * 4 >= g_cap * 3) {
        size_t ncap = g_cap ? g_cap * 2 : INIT_CAP;
        bucket_t *nm = (bucket_t *)calloc(ncap, sizeof(bucket_t));
        for (size_t i = 0; i < g_cap; i++) {
            if (g_map[i].used) {
                unsigned long idx = hash_str(g_map[i].key) % ncap;
                while (nm[idx].used) idx = (idx + 1) % ncap;
                nm[idx].key = g_map[i].key;
                nm[idx].value = g_map[i].value;
                nm[idx].used = 1;
            }
        }
        free(g_map);
        g_map = nm;
        g_cap = ncap;
    }
    unsigned long idx = hash_str(key) % g_cap;
    while (g_map[idx].used) idx = (idx + 1) % g_cap;
    g_map[idx].key = strdup(key);
    g_map[idx].value = strdup(value);
    g_map[idx].used = 1;
    g_count++;
}

// Returns real path if found, else NULL.
static const char *map_get(const char *key) {
    if (!g_map) return NULL;
    unsigned long idx = hash_str(key) % g_cap;
    while (g_map[idx].used) {
        if (strcmp(g_map[idx].key, key) == 0) return g_map[idx].value;
        idx = (idx + 1) % g_cap;
    }
    return NULL;
}

// Lowercase a path IN PLACE (ASCII; sufficient for asset filenames).
static void to_lower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

// Convert backslashes to forward slashes.
static void back_to_fwd(char *s) {
    for (; *s; s++) if (*s == '\\') *s = '/';
}

// Normalize: collapse multiple '/', strip trailing '/', handle "." and "..".
// Writes into out (must be large enough).
static void normalize(const char *in, char *out) {
    const char *p = in;
    char *o = out;
    // strip leading "./"
    // We process segment by segment onto a stack of segments in `out`.
    char segs[1024][256];
    int nseg = 0;
    char cur[256];
    int ci = 0;
    while (*p) {
        if (*p == '/') {
            if (ci > 0) {
                cur[ci] = 0;
                if (strcmp(cur, ".") == 0) { /* skip */ }
                else if (strcmp(cur, "..") == 0) { if (nseg > 0) nseg--; }
                else { if (nseg < 1024) { strncpy(segs[nseg], cur, 255); segs[nseg][255]=0; nseg++; } }
                ci = 0;
            }
            p++;
            continue;
        }
        if (ci < 255) cur[ci++] = *p;
        p++;
    }
    if (ci > 0) {
        cur[ci] = 0;
        if (strcmp(cur, ".") == 0) { /* skip */ }
        else if (strcmp(cur, "..") == 0) { if (nseg > 0) nseg--; }
        else { if (nseg < 1024) { strncpy(segs[nseg], cur, 255); segs[nseg][255]=0; nseg++; } }
    }
    out[0] = 0;
    for (int i = 0; i < nseg; i++) {
        strcat(out, segs[i]);
        if (i + 1 < nseg) strcat(out, "/");
    }
}

// Recursive index of a directory tree.
static void index_tree(const char *dir) {
#ifdef _WIN32
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char full[MAX_PATH];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        char norm[MAX_PATH];
        back_to_fwd(full);
        normalize(full, norm);
        char low[MAX_PATH];
        strncpy(low, norm, sizeof(low)-1); low[sizeof(low)-1]=0;
        to_lower(low);
        map_put(low, full);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) index_tree(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        char norm[4096];
        back_to_fwd(full);
        normalize(full, norm);
        char low[4096];
        strncpy(low, norm, sizeof(low)-1); low[sizeof(low)-1]=0;
        to_lower(low);
        map_put(low, full);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) index_tree(full);
    }
    closedir(d);
#endif
}

int path_resolve_init(const char *root_dir) {
    strncpy(g_root, root_dir, sizeof(g_root)-1);
    g_root[sizeof(g_root)-1] = 0;
    back_to_fwd(g_root);
    if (!g_cap) { g_cap = INIT_CAP; g_map = (bucket_t *)calloc(g_cap, sizeof(bucket_t)); }
    index_tree(g_root);
    strncpy(g_cwd, g_root, sizeof(g_cwd)-1);
    g_cwd[sizeof(g_cwd)-1] = 0;
    return 0;
}

void path_resolve_shutdown(void) {
    if (g_map) {
        for (size_t i = 0; i < g_cap; i++) {
            if (g_map[i].used) { free(g_map[i].key); free(g_map[i].value); }
        }
        free(g_map);
        g_map = NULL;
        g_cap = g_count = 0;
    }
}

void path_resolve_set_cwd(const char *cwd) {
    strncpy(g_cwd, cwd, sizeof(g_cwd)-1);
    g_cwd[sizeof(g_cwd)-1] = 0;
    back_to_fwd(g_cwd);
}

const char *path_resolve(const char *input) {
    if (!input) return NULL;
    char tmp[8192];
    strncpy(tmp, input, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = 0;
    back_to_fwd(tmp);
    // If relative, prepend cwd.
    char abs[8192];
    if (tmp[0] != '/' && !(tmp[0] && tmp[1] == ':')) {
        snprintf(abs, sizeof(abs), "%s/%s", g_cwd, tmp);
    } else {
        strncpy(abs, tmp, sizeof(abs)-1); abs[sizeof(abs)-1] = 0;
    }
    char norm[8192];
    normalize(abs, norm);
    char low[8192];
    strncpy(low, norm, sizeof(low)-1); low[sizeof(low)-1] = 0;
    to_lower(low);

    const char *hit = map_get(low);
    if (hit) {
        strncpy(g_scratch, hit, sizeof(g_scratch)-1);
        g_scratch[sizeof(g_scratch)-1] = 0;
        return g_scratch;
    }
    // Not found: return the normalized literal so the caller can attempt a
    // direct open and surface a clear error (instead of silently 404ing).
    strncpy(g_scratch, norm, sizeof(g_scratch)-1);
    g_scratch[sizeof(g_scratch)-1] = 0;
    return g_scratch;
}

size_t path_resolve_index_size(void) { return g_count; }

// ---------------------------------------------------------------------------
// Self test (compile with -DPATH_RESOLVE_TEST). Mirrors the algorithm that
// killed the original source-port: case-mismatched asset references.
// ---------------------------------------------------------------------------
#ifdef PATH_RESOLVE_TEST
int main(void) {
    // Synthetic tree: create temp dirs with mixed case, then resolve wrong-case
    // requests and assert they map to the real (correct-case) file.
    system("rm -rf /tmp/prtest && mkdir -p /tmp/prtest/assets/songs/Week1");
    system("echo x > /tmp/prtest/assets/songs/Week1/Icon.PNG");
    system("echo y > /tmp/prtest/assets/README.TXT");
    path_resolve_init("/tmp/prtest");
    const char *r1 = path_resolve("assets\\songs\\week1\\icon.png"); // backslash + wrong case
    const char *r2 = path_resolve("ASSETS/readme.txt");
    printf("index_size=%zu\n", path_resolve_index_size());
    printf("r1=%s\n", r1 ? r1 : "NULL");
    printf("r2=%s\n", r2 ? r2 : "NULL");
    int ok = r1 && strstr(r1, "Icon.PNG") && strstr(r1, "Week1") &&
             r2 && strstr(r2, "README.TXT");
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    path_resolve_shutdown();
    return ok ? 0 : 1;
}
#endif
