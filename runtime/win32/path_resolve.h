#ifndef PATH_RESOLVE_H
#define PATH_RESOLVE_H

// Case-insensitive path resolution layer.
//
// WHY THIS EXISTS:
// The target exe was built and tested on Windows, where the filesystem is
// case-INSENSITIVE and uses '\' as separator. FNF / Psych-Engine asset
// references are frequently case-mismatched (e.g. code asks for
// "songs/week1/icon.png" but the file on disk is "Icon.PNG"). On Windows this
// just works. On iOS/Linux the FS is case-sensitive, so a naive exact-match
// lookup 404s -> "native resources won't load". This layer reproduces Windows
// path semantics so the emulated exe sees exactly what it would on Windows.
//
// It builds an index of the bundled asset tree at init (real path -> lowercased
// normalized key) and resolves the exe's requests against that index.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the resolver with the root directory that contains the game's
// assets (typically the directory the exe + its "assets"/"data" folders live
// in). Recursively indexes every file. Call once at startup.
// Returns 0 on success, <0 on error.
int  path_resolve_init(const char *root_dir);

// Tear down and free all index memory.
void path_resolve_shutdown(void);

// Set the current working directory used to resolve relative paths.
void path_resolve_set_cwd(const char *cwd);

// Resolve a Windows-style path (may use '\', may be relative, may have mixed
// case) into a real on-disk path that actually exists.
//
// - Converts '\' -> '/'
// - Resolves against cwd if relative
// - Normalizes "." and ".."
// - Looks up case-insensitively (full-path match first, then per-segment
//   case-insensitive fallback so partially-mismatched paths still resolve)
//
// The returned pointer is owned by the resolver and valid until the next call
// (or shutdown). Returns NULL if no match is found (caller should then decide
// whether to fail or fall back to the literal path).
const char *path_resolve(const char *input);

// For diagnostics: how many files were indexed.
size_t path_resolve_index_size(void);

#ifdef __cplusplus
}
#endif

#endif // PATH_RESOLVE_H
