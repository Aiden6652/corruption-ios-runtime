#include "clock.h"
#include <stdint.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
static mach_timebase_info_data_t g_tb;
static int g_inited = 0;
uint64_t host_qpc(void) {
    if (!g_inited) { mach_timebase_info(&g_tb); g_inited = 1; }
    return mach_absolute_time();
}
uint64_t host_qpc_freq(void) {
    if (!g_inited) { mach_timebase_info(&g_tb); g_inited = 1; }
    // mach_absolute_time ticks * (numer/denom) = nanoseconds
    return (uint64_t)g_tb.numer * 1000000000ULL / (uint64_t)g_tb.denom;
}
#elif defined(_WIN32)
#include <windows.h>
uint64_t host_qpc(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (uint64_t)c.QuadPart; }
uint64_t host_qpc_freq(void) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (uint64_t)f.QuadPart; }
#else
#include <time.h>
uint64_t host_qpc(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
uint64_t host_qpc_freq(void) { return 1000000000ULL; }
#endif
