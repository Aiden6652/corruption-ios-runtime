#ifndef WIN32_SHIM_H
#define WIN32_SHIM_H

// Shim symbol registry. The emulator's import hook looks up a host function by
// (dll, func) name at runtime and bridges to it. All Win32/libVLC shim
// functions register themselves here so the glue never needs per-function glue
// code.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*shim_fp)(void);

// Register a host shim function under its Windows DLL + function name.
void shim_register(const char *dll, const char *func, shim_fp addr);

// Look up a previously registered shim. Returns NULL if not implemented.
shim_fp shim_lookup(const char *dll, const char *func);

// Call once at startup to populate the registry from win32_shim.c and
// libvlc_shim.c.
void win32_shim_register_all(void);
void vlckshim_register_all(void);

#ifdef __cplusplus
}
#endif

#endif // WIN32_SHIM_H
