#ifndef JIT_MEM_H
#define JIT_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// JIT memory allocator with proper W^X on Apple platforms.
//
// On non-jailbroken iOS you CANNOT have a page that is both writable and
// executable. The approved pattern is:
//   1. mmap with MAP_JIT (PROT_READ|PROT_WRITE)
//   2. jit_write_begin()  -> pthread_jit_write_protect_np(0)  (allow writes)
//   3. write translated code
//   4. jit_write_end()    -> pthread_jit_write_protect_np(1)  (now executable)
//
// This is exactly how UTM / Dolphin / PPSSPP run on stock iOS.

void *jit_alloc(size_t n);
void  jit_write_begin(void *p, size_t n);
void  jit_write_end(void *p, size_t n);
void  jit_free(void *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif // JIT_MEM_H
