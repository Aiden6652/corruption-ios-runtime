#include "jit_mem.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#include <sys/mman.h>
#ifndef MAP_JIT
#define MAP_JIT 0x0
#endif
void *jit_alloc(size_t n) {
    // Round up to page size.
    long ps = sysconf(_SC_PAGESIZE);
    n = (n + ps - 1) & ~((size_t)ps - 1);
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
}
void jit_write_begin(void *p, size_t n) { (void)p; (void)n; pthread_jit_write_protect_np(0); }
void jit_write_end(void *p, size_t n)   { (void)p; (void)n; pthread_jit_write_protect_np(1); }
void jit_free(void *p, size_t n) {
    if (!p) return;
    long ps = sysconf(_SC_PAGESIZE);
    n = (n + ps - 1) & ~((size_t)ps - 1);
    munmap(p, n);
}
#elif defined(_WIN32)
#include <windows.h>
void *jit_alloc(size_t n) {
    return VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
void jit_write_begin(void *p, size_t n) { (void)n; DWORD o; VirtualProtect(p, n, PAGE_READWRITE, &o); }
void jit_write_end(void *p, size_t n)   { (void)n; DWORD o; VirtualProtect(p, n, PAGE_EXECUTE_READ, &o); }
void jit_free(void *p, size_t n) { (void)n; VirtualFree(p, 0, MEM_RELEASE); }
#else
#include <sys/mman.h>
void *jit_alloc(size_t n) {
    long ps = sysconf(_SC_PAGESIZE);
    n = (n + ps - 1) & ~((size_t)ps - 1);
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
void jit_write_begin(void *p, size_t n) { (void)p; (void)n; }
void jit_write_end(void *p, size_t n)   { (void)p; (void)n; }
void jit_free(void *p, size_t n) {
    if (!p) return;
    long ps = sysconf(_SC_PAGESIZE);
    n = (n + ps - 1) & ~((size_t)ps - 1);
    munmap(p, n);
}
#endif
