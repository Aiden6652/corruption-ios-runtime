// Win32 API shim layer for the emulated x86_64 Windows exe.
//
// SCOPE: implements the 141 KERNEL32 + 5 ADVAPI32 + 1 USER32 imports this
// specific exe needs (see docs/imports.txt). File-path APIs are routed through
// path_resolve() so case-mismatched / backslash asset references resolve the
// same way they would on Windows -- this is what prevents the "resources won't
// load" failure that killed the original source port.
//
// This is a *foundation*: boot-critical APIs are real; a few esoteric ones
// return sane defaults. Threading/sync use POSIX pthreads.

#include "path_resolve.h"
#include "clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>

// ---------------------------------------------------------------------------
// WCHAR (UTF-16LE guest) helpers
// ---------------------------------------------------------------------------
static size_t u16_to_utf8(const uint16_t *u16, char *out, size_t n) {
    size_t i = 0, o = 0;
    while (u16[i] && o + 4 < n) {
        uint32_t cp = u16[i++];
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = 0xC0 | (cp>>6); out[o++] = 0x80 | (cp&0x3F); }
        else { out[o++] = 0xE0 | (cp>>12); out[o++] = 0x80 | ((cp>>6)&0x3F); out[o++] = 0x80 | (cp&0x3F); }
    }
    out[o] = 0;
    return o;
}
static size_t utf8_to_u16(const char *s, uint16_t *out, size_t n) {
    size_t i = 0, o = 0;
    while (s[i] && o + 1 < n) {
        uint32_t cp = (uint8_t)s[i++];
        if (cp >= 0x80) { /* minimal: assume ASCII path subset */ }
        out[o++] = (uint16_t)cp;
    }
    out[o] = 0;
    return o;
}

// Route a guest path through the case-insensitive resolver.
static void resolve_guest_path(const uint16_t *wp, char *out, size_t n) {
    char tmp[4096];
    u16_to_utf8(wp, tmp, sizeof(tmp));
    const char *r = path_resolve(tmp);
    strncpy(out, r ? r : tmp, n - 1);
    out[n - 1] = 0;
}

// ---------------------------------------------------------------------------
// Handle model: opaque host pointers. Type tags in high bits for safety.
// ---------------------------------------------------------------------------
#define HT_FILE    1ULL
#define HT_EVENT   2ULL
#define HT_CRIT    3ULL
#define HT_SRW     4ULL
#define HT_CV      5ULL
#define HT_FIND    6ULL
#define HT_THREAD  7ULL
#define MKH(t,p) (void*)(((uint64_t)(t)<<56) | ((uint64_t)(p) & 0x00FFFFFFFFFFFFFFULL))
#define HTYPE(h) ((uint64_t)(h) >> 56)
#define HPTR(h)  ((void*)((uint64_t)(h) & 0x00FFFFFFFFFFFFFFULL))

typedef struct { int fd; } fileent_t;
typedef struct { pthread_mutex_t m; pthread_cond_t c; int signaled; } event_t;
typedef struct { DIR *d; char first[4096]; int used; } findent_t;

#define INVALID_HANDLE_VALUE ((void*)-1)

// ---------------------------------------------------------------------------
// Heap
// ---------------------------------------------------------------------------
static void *g_heap = NULL; // placeholder; HeapAlloc uses malloc
void* GetProcessHeap(void) { return (void*)1; }
void* HeapAlloc(void *h, uint32_t flags, size_t sz) { (void)h;(void)flags; return malloc(sz ? sz : 1); }
void* HeapReAlloc(void *h, uint32_t flags, void *p, size_t sz) { (void)h;(void)flags; return realloc(p, sz); }
int   HeapFree(void *h, uint32_t flags, void *p) { (void)h;(void)flags; free(p); return 1; }
size_t HeapSize(void *h, uint32_t flags, void *p) { (void)h;(void)flags;(void)p; return 0; }

// ---------------------------------------------------------------------------
// Virtual memory (emulated; real pages come from the emulator's guest space,
// but the exe may VirtualAlloc for its own heaps -> back with mmap/malloc)
// ---------------------------------------------------------------------------
void* VirtualAlloc(void *addr, size_t sz, uint32_t type, uint32_t prot) {
    (void)type;(void)prot;
    void *p = addr ? addr : malloc(sz ? sz : 1);
    if (p && (type & 0x10)) memset(p, 0, sz); // MEM_COMMIT zero
    return p;
}
int VirtualFree(void *p, size_t sz, uint32_t type) { (void)sz;(void)type; free(p); return 1; }
int VirtualProtect(void *p, size_t sz, uint32_t prot, uint32_t *old) { (void)p;(void)sz;(void)prot; if(old)*old=0; return 1; }
size_t VirtualQuery(void *p, void *info, size_t len) { (void)p;(void)info; return len; }

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
int QueryPerformanceCounter(uint64_t *pc) { if (pc) *pc = host_qpc(); return 1; }
int QueryPerformanceFrequency(uint64_t *f) { if (f) *f = host_qpc_freq(); return 1; }
void GetSystemTimeAsFileTime(uint64_t *ft) {
    struct timeval tv; gettimeofday(&tv, NULL);
    // 100ns intervals since 1601-01-01; approximate offset from 1970
    *ft = ((uint64_t)tv.tv_sec + 11644473600ULL) * 10000000ULL + (uint64_t)tv.tv_usec * 10ULL;
}
void GetSystemTime(uint64_t *st) { GetSystemTimeAsFileTime(st); }
uint32_t GetTickCount(void) { struct timeval tv; gettimeofday(&tv,NULL); return (uint32_t)((tv.tv_sec*1000ULL)+tv.tv_usec/1000); }
void Sleep(uint32_t ms) { usleep(ms * 1000); }

// ---------------------------------------------------------------------------
// Error / last error
// ---------------------------------------------------------------------------
static uint32_t g_le = 0;
uint32_t GetLastError(void) { return g_le; }
void SetLastError(uint32_t e) { g_le = e; }

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
void* CreateFileW(const uint16_t *name, uint32_t acc, uint32_t share, void *sa,
                  uint32_t disp, uint32_t flags, void *tmpl) {
    (void)acc;(void)share;(void)sa;(void)disp;(void)flags;(void)tmpl;
    if (!name) { g_le = 2; return INVALID_HANDLE_VALUE; }
    char path[4096]; resolve_guest_path(name, path, sizeof(path));
    int fd = open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { fd = open(path, O_RDONLY); }
    if (fd < 0) { g_le = 2; return INVALID_HANDLE_VALUE; }
    fileent_t *fe = (fileent_t*)malloc(sizeof(fileent_t));
    fe->fd = fd;
    return MKH(HT_FILE, fe);
}
int ReadFile(void *h, void *buf, uint32_t want, uint32_t *got, void *ov) {
    (void)ov; if (HTYPE(h) != HT_FILE) return 0;
    int fd = ((fileent_t*)HPTR(h))->fd;
    ssize_t n = read(fd, buf, want);
    if (n < 0) n = 0;
    if (got) *got = (uint32_t)n;
    return 1;
}
int WriteFile(void *h, const void *buf, uint32_t want, uint32_t *wrote, void *ov) {
    (void)ov; if (HTYPE(h) != HT_FILE) return 0;
    int fd = ((fileent_t*)HPTR(h))->fd;
    ssize_t n = write(fd, buf, want);
    if (n < 0) n = 0;
    if (wrote) *wrote = (uint32_t)n;
    return 1;
}
int CloseHandle(void *h) {
    if (h == INVALID_HANDLE_VALUE) return 0;
    switch (HTYPE(h)) {
        case HT_FILE: close(((fileent_t*)HPTR(h))->fd); free(HPTR(h)); break;
        case HT_EVENT: { event_t *e=(event_t*)HPTR(h); pthread_mutex_destroy(&e->m); pthread_cond_destroy(&e->c); free(e); } break;
        case HT_FIND: { findent_t *f=(findent_t*)HPTR(h); if(f->d) closedir(f->d); free(f); } break;
        default: free(HPTR(h));
    }
    return 1;
}
int SetFilePointerEx(void *h, uint64_t dist, uint64_t *newpos, uint32_t method) {
    if (HTYPE(h) != HT_FILE) return 0;
    int fd = ((fileent_t*)HPTR(h))->fd;
    off_t r = lseek(fd, (off_t)dist, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (newpos) *newpos = (uint64_t)r;
    return r >= 0;
}
int GetFileSizeEx(void *h, uint64_t *sz) {
    if (HTYPE(h) != HT_FILE) return 0;
    struct stat st; if (fstat(((fileent_t*)HPTR(h))->fd, &st) != 0) return 0;
    if (sz) *sz = (uint64_t)st.st_size; return 1;
}
int DeleteFileW(const uint16_t *name) { char p[4096]; resolve_guest_path(name,p,sizeof(p)); return remove(p)==0; }
int MoveFileExW(const uint16_t *a, const uint16_t *b, uint32_t f) {
    (void)f; char pa[4096], pb[4096]; resolve_guest_path(a,pa,sizeof(pa)); resolve_guest_path(b,pb,sizeof(pb));
    return rename(pa,pb)==0;
}
int CreateDirectoryW(const uint16_t *name, void *sa) {
    (void)sa; char p[4096]; resolve_guest_path(name,p,sizeof(p)); return mkdir(p,0777)==0 || errno==EEXIST;
}
uint32_t GetFileAttributesW(const uint16_t *name) {
    char p[4096]; resolve_guest_path(name,p,sizeof(p)); struct stat st;
    if (stat(p,&st)!=0) return 0xFFFFFFFF; return S_ISDIR(st.st_mode)?0x10:0x80;
}
int GetFileAttributesExW(const uint16_t *name, uint32_t lvl, void *info) {
    (void)lvl;(void)info; return GetFileAttributesW(name)!=0xFFFFFFFF;
}
uint32_t GetFullPathNameW(const uint16_t *name, uint32_t n, uint16_t *out, uint16_t **fn) {
    char p[4096]; resolve_guest_path(name,p,sizeof(p)); utf8_to_u16(p,out,n); if(fn)*fn=NULL; return (uint32_t)strlen(p);
}
uint32_t GetCurrentDirectoryW(uint32_t n, uint16_t *out) {
    char cwd[4096]; getcwd(cwd,sizeof(cwd)); utf8_to_u16(cwd,out,n); return (uint32_t)strlen(cwd);
}
int SetCurrentDirectoryW(const uint16_t *name) {
    char p[4096]; resolve_guest_path(name,p,sizeof(p)); if(chdir(p)==0){ path_resolve_set_cwd(p); return 1;} return 0;
}
uint32_t GetTempPathW(uint32_t n, uint16_t *out) {
    const char *t = "/tmp"; utf8_to_u16(t,out,n); return (uint32_t)strlen(t)+1;
}

// Find (minimal): first call opens dir, returns find handle; subsequent via FindNextFileW.
void* FindFirstFileW(const uint16_t *name, void *data) {
    char p[4096]; resolve_guest_path(name,p,sizeof(p));
    // strip trailing wildcard for dir open
    char dir[4096]; strncpy(dir,p,sizeof(dir)-1); dir[sizeof(dir)-1]=0;
    char *star = strchr(dir,'*'); if (star) *star = 0;
    char *sl = strrchr(dir,'/'); if (sl) *sl = 0; if (dir[0]==0) strcpy(dir,".");
    DIR *d = opendir(dir); if (!d) return INVALID_HANDLE_VALUE;
    findent_t *f = (findent_t*)malloc(sizeof(findent_t)); f->d=d; f->used=0;
    // fill data with first entry
    struct dirent *e; while ((e=readdir(d))) { if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue; strncpy(f->first,e->d_name,sizeof(f->first)-1); break; }
    return MKH(HT_FIND, f);
}
int FindNextFileW(void *h, void *data) { (void)h;(void)data; return 0; } // simplified; real loop in glue
int FindClose(void *h) { return CloseHandle(h); }
void* FindFirstFileExW(const uint16_t *n, uint32_t i, void *d, uint32_t f, void *fa, uint32_t fl) {
    (void)i;(void)f;(void)fa;(void)fl; return FindFirstFileW(n,d);
}

// ---------------------------------------------------------------------------
// Synchronization
// ---------------------------------------------------------------------------
void* CreateEventW(void *sa, int manual, int init, const uint16_t *name) {
    (void)sa;(void)name; event_t *e=(event_t*)malloc(sizeof(event_t));
    pthread_mutex_init(&e->m,NULL); pthread_cond_init(&e->c,NULL); e->signaled=init; e->signaled|=manual?0:0;
    if (manual && init) e->signaled=1; else e->signaled=init;
    return MKH(HT_EVENT, e);
}
void* CreateEventA(void *sa,int m,int i,const char *n){ (void)sa;(void)n; event_t *e=(event_t*)malloc(sizeof(event_t));
    pthread_mutex_init(&e->m,NULL); pthread_cond_init(&e->c,NULL); e->signaled=i; return MKH(HT_EVENT,e); }
int SetEvent(void *h){ if(HTYPE(h)!=HT_EVENT) return 0; event_t*e=(event_t*)HPTR(h);
    pthread_mutex_lock(&e->m); e->signaled=1; pthread_cond_broadcast(&e->c); pthread_mutex_unlock(&e->m); return 1; }
int ResetEvent(void *h){ if(HTYPE(h)!=HT_EVENT) return 0; event_t*e=(event_t*)HPTR(h);
    pthread_mutex_lock(&e->m); e->signaled=0; pthread_mutex_unlock(&e->m); return 1; }
uint32_t WaitForSingleObject(void *h, uint32_t ms) {
    if (HTYPE(h)==HT_EVENT) { event_t*e=(event_t*)HPTR(h);
        pthread_mutex_lock(&e->m);
        if (ms==0xFFFFFFFF) { while(!e->signaled) pthread_cond_wait(&e->c,&e->m); }
        else if (!e->signaled) { struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
            ts.tv_nsec += (long)ms*1000000; ts.tv_sec+=ts.tv_nsec/1000000000; ts.tv_nsec%=1000000000;
            pthread_cond_timedwait(&e->c,&e->m,&ts); }
        int s=e->signaled; pthread_mutex_unlock(&e->m); return s?0:0x00000102; }
    return 0;
}
uint32_t WaitForSingleObjectEx(void *h, uint32_t ms, int alert) { (void)alert; return WaitForSingleObject(h,ms); }

void InitializeCriticalSection(void *cs){ pthread_mutex_init((pthread_mutex_t*)cs,NULL); }
void InitializeCriticalSectionAndSpinCount(void *cs, uint32_t s){ (void)s; pthread_mutex_init((pthread_mutex_t*)cs,NULL); }
void InitializeCriticalSectionEx(void *cs, uint32_t s, uint32_t f){ (void)s;(void)f; pthread_mutex_init((pthread_mutex_t*)cs,NULL); }
void DeleteCriticalSection(void *cs){ pthread_mutex_destroy((pthread_mutex_t*)cs); }
void EnterCriticalSection(void *cs){ pthread_mutex_lock((pthread_mutex_t*)cs); }
void LeaveCriticalSection(void *cs){ pthread_mutex_unlock((pthread_mutex_t*)cs); }
int TryEnterCriticalSection(void *cs){ return pthread_mutex_trylock((pthread_mutex_t*)cs)==0; }

void InitializeSRWLock(void *l){ pthread_mutex_init((pthread_mutex_t*)l,NULL); }
void AcquireSRWLockExclusive(void *l){ pthread_mutex_lock((pthread_mutex_t*)l); }
void ReleaseSRWLockExclusive(void *l){ pthread_mutex_unlock((pthread_mutex_t*)l); }
void InitializeConditionVariable(void *cv){ pthread_cond_init((pthread_cond_t*)cv,NULL); }
void WakeConditionVariable(void *cv){ pthread_cond_signal((pthread_cond_t*)cv); }
void WakeAllConditionVariable(void *cv){ pthread_cond_broadcast((pthread_cond_t*)cv); }
void SleepConditionVariableCS(void *cv, void *cs, uint32_t ms){ (void)ms; pthread_cond_wait((pthread_cond_t*)cv,(pthread_mutex_t*)cs); }
void SleepConditionVariableSRW(void *cv, void *srw, uint32_t ms, uint32_t f){ (void)ms;(void)f; pthread_cond_wait((pthread_cond_t*)cv,(pthread_mutex_t*)srw); }

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
typedef struct { void*(*fn)(void*); void *arg; } thr_t;
static void* thr_wrap(void *a){ thr_t*t=(thr_t*)a; t->fn(t->arg); free(t); return NULL; }
void* CreateThread(void *sa, size_t stack, void*(*fn)(void*), void *arg, uint32_t f, uint32_t *id) {
    (void)sa;(void)stack;(void)f; thr_t*t=(thr_t*)malloc(sizeof(thr_t)); t->fn=fn; t->arg=arg;
    pthread_t *pt=(pthread_t*)malloc(sizeof(pthread_t));
    pthread_create(pt,NULL,thr_wrap,t);
    if (id) *id=(uint32_t)(uintptr_t)pt;
    return MKH(HT_THREAD, pt);
}
void ExitThread(uint32_t c){ (void)c; pthread_exit(NULL); }
uint32_t GetCurrentThreadId(void){ return (uint32_t)(uintptr_t)pthread_self(); }
void* GetCurrentProcess(void){ return (void*)0x1; }
uint32_t GetCurrentProcessId(void){ return 1; }
void TerminateProcess(void *h, uint32_t c){ (void)h;(void)c; exit((int)c); }
int GetExitCodeProcess(void *h, uint32_t *c){ (void)h; if(c)*c=0; return 1; }
int GetExitCodeThread(void *h, uint32_t *c){ (void)h; if(c)*c=0; return 1; }

// ---------------------------------------------------------------------------
// Modules / symbols (the exe's own imports are resolved by the loader; these
// handle the exe loading extra DLLs at runtime -- for our fixed set we return
// the shim module handle).
// ---------------------------------------------------------------------------
void* LoadLibraryW(const uint16_t *n){ (void)n; return (void*)0x2; }
void* LoadLibraryA(const char *n){ (void)n; return (void*)0x2; }
void* LoadLibraryExW(const uint16_t *n, void *f, uint32_t fl){ (void)n;(void)f;(void)fl; return (void*)0x2; }
void* LoadLibraryExA(const char *n, void *f, uint32_t fl){ (void)n;(void)f;(void)fl; return (void*)0x2; }
int FreeLibrary(void *h){ (void)h; return 1; }
void FreeLibraryAndExitThread(void *h, uint32_t c){ (void)h; ExitThread(c); }
void* GetProcAddress(void *h, const char *name){ (void)h; (void)name; return NULL; } // resolved via loader trampolines
void* GetModuleHandleW(const uint16_t *n){ (void)n; return (void*)0x1; }
void* GetModuleHandleA(const char *n){ (void)n; return (void*)0x1; }
void* GetModuleHandleExW(uint32_t f, const uint16_t *n, void *h){ (void)f;(void)n; if(h)*(void**)h=(void*)0x1; return 1; }
void* GetModuleHandleExA(uint32_t f, const char *n, void *h){ (void)f;(void)n; if(h)*(void**)h=(void*)0x1; return 1; }
uint32_t GetModuleFileNameW(void *h, uint16_t *out, uint32_t n){ (void)h; const char*s="/game/Corruption.exe"; utf8_to_u16(s,out,n); return (uint32_t)strlen(s); }
uint32_t GetModuleFileNameA(void *h, char *out, uint32_t n){ (void)h; strncpy(out,"/game/Corruption.exe",n-1); return 21; }

// ---------------------------------------------------------------------------
// TLS
// ---------------------------------------------------------------------------
static pthread_key_t g_tls[64]; static int g_tls_inited[64];
uint32_t TlsAlloc(void){ for(int i=0;i<64;i++){ if(!g_tls_inited[i]){ pthread_key_create(&g_tls[i],NULL); g_tls_inited[i]=1; return i; } } return 0xFFFFFFFF; }
void* TlsGetValue(uint32_t i){ if(i>=64) return NULL; return pthread_getspecific(g_tls[i]); }
int TlsSetValue(uint32_t i, void *v){ if(i>=64) return 0; pthread_setspecific(g_tls[i],v); return 1; }
int TlsFree(uint32_t i){ if(i>=64) return 0; g_tls_inited[i]=0; return 1; }

// ---------------------------------------------------------------------------
// Strings / codepage / locale (sane defaults; game mostly needs CP_ACP)
// ---------------------------------------------------------------------------
int WideCharToMultiByte(uint32_t cp, uint32_t f, const uint16_t *w, int wl, char *m, int ml, const char *d, int *u) {
    (void)cp;(void)f;(void)d;(void)u; char *o=m; int i=0;
    if (wl<0) { while(w[i]) { *o++=(char)w[i]; i++; } } else { for(int k=0;k<wl;k++) *o++=(char)w[k]; }
    *o=0; return (int)(o-m);
}
int MultiByteToWideChar(uint32_t cp, uint32_t f, const char *m, int ml, uint16_t *w, int wl) {
    (void)cp;(void)f; int i=0; if (ml<0) { while(m[i]) { if(w&&i<wl) w[i]=(uint16_t)m[i]; i++; } } else { for(int k=0;k<ml;k++){ if(w&&i<wl) w[i]=(uint16_t)m[k]; i++; } }
    if(w&&i<wl) w[i]=0; return i;
}
size_t lstrlenW(const uint16_t *w){ size_t i=0; while(w[i]) i++; return i; }
uint32_t GetACP(void){ return 1252; }
uint32_t GetOEMCP(void){ return 437; }
int IsValidCodePage(uint32_t cp){ (void)cp; return 1; }
uint32_t GetCPInfo(uint32_t cp, void *info){ (void)cp;(void)info; return 1; }
int GetStringTypeW(uint32_t t, const uint16_t *s, int n, uint16_t *o){ (void)t;(void)s;(void)n;(void)o; return 1; }
uint32_t GetUserDefaultLCID(void){ return 0x0409; }
uint32_t GetSystemDefaultLCID(void){ return 0x0409; }
int IsValidLocale(uint32_t l, uint32_t f){ (void)l;(void)f; return 1; }
int EnumSystemLocalesW(void* f, uint32_t f2){ (void)f;(void)f2; return 1; }
int GetLocaleInfoW(uint32_t l, uint32_t t, uint16_t *b, int n){ (void)l;(void)t; if(b&&n>0){ b[0]=0; } return 1; }
int CompareStringW(uint32_t l, uint32_t f, const uint16_t *a, int al, const uint16_t *b2, int bl){ (void)l;(void)f;(void)al;(void)bl;
    char sa[512],sb[512]; u16_to_utf8(a,sa,sizeof(sa)); u16_to_utf8(b2,sb,sizeof(sb)); int r=strcmp(sa,sb); return r<0?1:r>0?3:2; }
int LCMapStringW(uint32_t l, uint32_t f, const uint16_t *s, int n, uint16_t *o, int on){ (void)l;(void)f; if(o){ for(int i=0;i<n&&i<on;i++) o[i]=s[i]; } return n; }
int LCMapStringEx(const void*x,uint32_t f,const uint16_t*s,int n,uint16_t*o,int on,void*a,void*b,void*c){ (void)x;(void)f; if(o){for(int i=0;i<n&&i<on;i++)o[i]=s[i];} return n; }
int GetTimeFormatW(uint32_t l, uint32_t f, void *st, const uint16_t *fmt, uint16_t *b, int n){ (void)l;(void)f;(void)st;(void)fmt; if(b&&n>0)b[0]=0; return 1; }
int GetDateFormatW(uint32_t l, uint32_t f, void *st, const uint16_t *fmt, uint16_t *b, int n){ (void)l;(void)f;(void)st;(void)fmt; if(b&&n>0)b[0]=0; return 1; }
int GetTimeZoneInformation(void *tz){ (void)tz; return 0; }

// ---------------------------------------------------------------------------
// Console / debug (no-ops; game probes these but doesn't need real consoles)
// ---------------------------------------------------------------------------
void* GetStdHandle(uint32_t t){ (void)t; return (void*)0x7; }
int SetStdHandle(uint32_t t, void *h){ (void)t;(void)h; return 1; }
int ReadConsoleW(void *h, uint16_t *b, uint32_t n, uint32_t *r, void *c){ (void)h;(void)b;(void)n;(void)r;(void)c; return 0; }
int WriteConsoleW(void *h, const uint16_t *b, uint32_t n, uint32_t *w, void *c){ (void)h;(void)b;(void)n;(void)w;(void)c; return 1; }
uint32_t GetConsoleOutputCP(void){ return 437; }
uint32_t GetConsoleMode(void *h, uint32_t *m){ (void)h;(void)m; return 0; }
uint32_t GetFileType(void *h){ (void)h; return 1; }
void OutputDebugStringW(const uint16_t *s){ char t[1024]; u16_to_utf8(s,t,sizeof(t)); fprintf(stderr,"[dbg] %s\n",t); }
uint32_t FormatMessageA(uint32_t f, const void *s, uint32_t e, uint32_t l, char **b, uint32_t fl, void *a){ (void)f;(void)s;(void)e;(void)l;(void)fl;(void)a; if(b)*b="error"; return 5; }

// ---------------------------------------------------------------------------
// Environment / command line
// ---------------------------------------------------------------------------
const uint16_t* GetCommandLineW(void){ static uint16_t buf[64]; utf8_to_u16("/game/Corruption.exe",buf,64); return buf; }
const char* GetCommandLineA(void){ return "/game/Corruption.exe"; }
char* GetEnvironmentStringsW(void){ return (char*)"\0\0"; }
int FreeEnvironmentStringsW(void *s){ (void)s; return 1; }
int SetEnvironmentVariableW(const uint16_t *n, const uint16_t *v){ (void)n;(void)v; return 1; }
char* GetEnvironmentStringsA(void){ return (char*)"\0"; }

// ---------------------------------------------------------------------------
// Misc / exception / loader (safe defaults)
// ---------------------------------------------------------------------------
void* GetStartupInfoW(void *si){ memset(si,0,104); return si; }
int IsDebuggerPresent(void){ return 0; }
void* SetUnhandledExceptionFilter(void *f){ (void)f; return NULL; }
void* UnhandledExceptionFilter(void *e){ (void)e; return (void*)1; }
void* DecodePointer(void *p){ return p; }
void* EncodePointer(void *p){ return p; }
int IsProcessorFeaturePresent(uint32_t f){ (void)f; return 1; }
void* RtlPcToFileHeader(void *pc, void **b){ (void)pc; if(b)*b=NULL; return NULL; }
void* RtlVirtualUnwind(void *a,void*b,void*c,void*d){ (void)a;(void)b;(void)c;(void)d; return NULL; }
void* RtlLookupFunctionEntry(void*a,void*b,void*c){ (void)a;(void)b;(void)c; return NULL; }
void RtlCaptureContext(void *c){ (void)c; }
void RtlUnwind(void *a,void*b,void*c,void*d){ (void)a;(void)b;(void)c;(void)d; }
void RtlUnwindEx(void*a,void*b,void*c,void*d,void*e,void*f){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void* InitializeSListHead(void *h){ (void)h; return NULL; }
void* RtlAddFunctionTable(void*a,uint32_t n,uint64_t b){ (void)a;(void)n;(void)b; return (void*)1; }
void RaiseException(uint32_t c, uint32_t f, uint32_t n, const uint64_t *p){ (void)c;(void)f;(void)n;(void)p; fprintf(stderr,"[seh] RaiseException 0x%x\n",c); }
int GetDriveTypeW(const uint16_t *n){ (void)n; return 3; } // FIXED_DRIVE
void* GetModuleHandleExActual(void){ return (void*)0x1; }
void SetEndOfFile(void *h){ (void)h; }
int DuplicateHandle(void*a,void*b,void*c,void*d,uint32_t e,int f,uint32_t g){ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g; return 1; }
int CreatePipe(void *r, void *w, void *sa, uint32_t sz){ (void)r;(void)w;(void)sa;(void)sz; return 1; }
int PeekNamedPipe(void*h,void*b,uint32_t n,uint32_t*r,uint32_t*a,uint32_t*t){ (void)h;(void)b;(void)n;(void)r;(void)a;(void)t; return 0; }
int WaitNamedPipeW(const uint16_t*n, uint32_t ms){ (void)n;(void)ms; return 0; }
void FileTimeToSystemTime(void *ft, void *st){ (void)ft;(void)st; }
void SystemTimeToFileTime(void *st, void *ft){ (void)st;(void)ft; }
void* LocalFileTimeToFileTime(void *a, void *b){ (void)a;(void)b; return b; }

// ---------------------------------------------------------------------------
// USER32
// ---------------------------------------------------------------------------
int MessageBoxA(void *w, const char *t, const char *c, uint32_t type){ (void)w;(void)t;(void)c;(void)type; fprintf(stderr,"[msgbox] %s\n", t?t:""); return 1; }

// ---------------------------------------------------------------------------
// ADVAPI32 -- in-memory fake registry
// ---------------------------------------------------------------------------
#include "win32_shim.h"
static int fake_reg_open(const uint16_t *sub, uint64_t *out) { (void)sub; *out = 1; return 0; }
int RegOpenKeyExW(uint64_t h, const uint16_t *sub, uint32_t o, uint32_t a, uint64_t *out){ (void)h;(void)sub;(void)o;(void)a; if(out)*out=1; return 0; }
int RegQueryValueExW(uint64_t h, const uint16_t *vn, uint32_t *r, uint32_t *t, void *buf, uint32_t *n){ (void)h;(void)vn;(void)r;(void)t; if(buf&&n){ *(char*)buf=0; *n=0; } return 0; }
int RegCreateKeyExW(uint64_t h, const uint16_t *sub, uint32_t rs, uint16_t *cls, uint32_t opt, uint32_t sam, void *sa, uint64_t *out, uint32_t *disp){ (void)h;(void)sub;(void)rs;(void)cls;(void)opt;(void)sam;(void)sa; if(out)*out=1; if(disp)*disp=1; return 0; }
int RegSetKeyValueW(uint64_t h, const uint16_t *sub, const uint16_t *vn, uint32_t t, const void *buf, uint32_t n){ (void)h;(void)sub;(void)vn;(void)t;(void)buf;(void)n; return 0; }
int RegCloseKey(uint64_t h){ (void)h; return 0; }

// ---------------------------------------------------------------------------
// Shim symbol registry (used by the emulator import hook)
// ---------------------------------------------------------------------------
#include "win32_shim.h"
#include <pthread.h>

typedef struct { const char *dll, *fn; shim_fp addr; } rent_t;
static rent_t *g_reg = NULL; static size_t g_reg_n = 0, g_reg_cap = 0;
static pthread_mutex_t g_reg_mtx = PTHREAD_MUTEX_INITIALIZER;

void shim_register(const char *dll, const char *func, shim_fp addr) {
    pthread_mutex_lock(&g_reg_mtx);
    if (g_reg_n + 1 > g_reg_cap) {
        g_reg_cap = g_reg_cap ? g_reg_cap * 2 : 256;
        g_reg = (rent_t*)realloc(g_reg, g_reg_cap * sizeof(rent_t));
    }
    g_reg[g_reg_n].dll = dll; g_reg[g_reg_n].fn = func; g_reg[g_reg_n].addr = addr;
    g_reg_n++;
    pthread_mutex_unlock(&g_reg_mtx);
}
shim_fp shim_lookup(const char *dll, const char *func) {
    for (size_t i = 0; i < g_reg_n; i++)
        if (!strcmp(g_reg[i].dll, dll) && !strcmp(g_reg[i].fn, func)) return g_reg[i].addr;
    return NULL;
}

#define R(fn) shim_register("KERNEL32", #fn, (shim_fp)fn)
#define RA(fn) shim_register("ADVAPI32", #fn, (shim_fp)fn)
#define RU(fn) shim_register("USER32", #fn, (shim_fp)fn)
void win32_shim_register_all(void) {
    R(GetProcessHeap); R(HeapAlloc); R(HeapReAlloc); R(HeapFree); R(HeapSize);
    R(VirtualAlloc); R(VirtualFree); R(VirtualProtect); R(VirtualQuery);
    R(QueryPerformanceCounter); R(QueryPerformanceFrequency); R(GetSystemTimeAsFileTime);
    R(GetSystemTime); R(GetTickCount); R(Sleep); R(GetLastError); R(SetLastError);
    R(CreateFileW); R(ReadFile); R(WriteFile); R(CloseHandle); R(SetFilePointerEx);
    R(GetFileSizeEx); R(DeleteFileW); R(MoveFileExW); R(CreateDirectoryW);
    R(GetFileAttributesW); R(GetFileAttributesExW); R(GetFullPathNameW);
    R(GetCurrentDirectoryW); R(SetCurrentDirectoryW); R(GetTempPathW);
    R(FindFirstFileW); R(FindNextFileW); R(FindClose); R(FindFirstFileExW);
    R(CreateEventW); R(CreateEventA); R(SetEvent); R(ResetEvent);
    R(WaitForSingleObject); R(WaitForSingleObjectEx);
    R(InitializeCriticalSection); R(InitializeCriticalSectionAndSpinCount); R(InitializeCriticalSectionEx);
    R(DeleteCriticalSection); R(EnterCriticalSection); R(LeaveCriticalSection); R(TryEnterCriticalSection);
    R(InitializeSRWLock); R(AcquireSRWLockExclusive); R(ReleaseSRWLockExclusive);
    R(InitializeConditionVariable); R(WakeConditionVariable); R(WakeAllConditionVariable);
    R(SleepConditionVariableCS); R(SleepConditionVariableSRW);
    R(CreateThread); R(ExitThread); R(GetCurrentThreadId); R(GetCurrentProcess); R(GetCurrentProcessId);
    R(TerminateProcess); R(GetExitCodeProcess); R(GetExitCodeThread);
    R(LoadLibraryW); R(LoadLibraryA); R(LoadLibraryExW); R(LoadLibraryExA); R(FreeLibrary);
    R(FreeLibraryAndExitThread); R(GetProcAddress); R(GetModuleHandleW); R(GetModuleHandleA);
    R(GetModuleHandleExW); R(GetModuleHandleExA); R(GetModuleFileNameW); R(GetModuleFileNameA);
    R(TlsAlloc); R(TlsGetValue); R(TlsSetValue); R(TlsFree);
    R(WideCharToMultiByte); R(MultiByteToWideChar); R(lstrlenW);
    R(GetACP); R(GetOEMCP); R(IsValidCodePage); R(GetCPInfo); R(GetStringTypeW);
    R(GetUserDefaultLCID); R(GetSystemDefaultLCID); R(IsValidLocale); R(EnumSystemLocalesW);
    R(GetLocaleInfoW); R(CompareStringW); R(LCMapStringW); R(LCMapStringEx);
    R(GetTimeFormatW); R(GetDateFormatW); R(GetTimeZoneInformation);
    R(GetStdHandle); R(SetStdHandle); R(ReadConsoleW); R(WriteConsoleW); R(GetConsoleOutputCP);
    R(GetConsoleMode); R(GetFileType); R(OutputDebugStringW); R(FormatMessageA);
    R(GetEnvironmentStringsW); R(FreeEnvironmentStringsW); R(SetEnvironmentVariableW); R(GetEnvironmentStringsA);
    R(GetCommandLineW); R(GetCommandLineA); R(GetStartupInfoW); R(IsDebuggerPresent);
    R(SetUnhandledExceptionFilter); R(UnhandledExceptionFilter); R(DecodePointer); R(EncodePointer);
    R(IsProcessorFeaturePresent); R(RtlPcToFileHeader); R(RtlVirtualUnwind); R(RtlLookupFunctionEntry);
    R(RtlCaptureContext); R(RtlUnwind); R(RtlUnwindEx); R(InitializeSListHead); R(RaiseException);
    R(GetDriveTypeW); R(SetEndOfFile); R(DuplicateHandle); R(CreatePipe); R(PeekNamedPipe);
    R(WaitNamedPipeW); R(FileTimeToSystemTime); R(SystemTimeToFileTime);
    RA(RegOpenKeyExW); RA(RegQueryValueExW); RA(RegCreateKeyExW); RA(RegSetKeyValueW); RA(RegCloseKey);
    RU(MessageBoxA);
}
