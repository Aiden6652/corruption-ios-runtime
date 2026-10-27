#ifndef CLOCK_H
#define CLOCK_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// High-resolution monotonic clock used to back Win32 QueryPerformanceCounter.
// On Apple platforms this is mach_absolute_time(); must be STABLE with no
// drift, because the emulated game treats this as its audio/timing authority.
uint64_t host_qpc(void);
uint64_t host_qpc_freq(void);
#ifdef __cplusplus
}
#endif
#endif
