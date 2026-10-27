// Bridging header: exposes the C app glue to Swift.
#ifndef Bridging_h
#define Bridging_h
#ifdef __cplusplus
extern "C" {
#endif
void app_set_paths(const char *exe, const char *assets);
int  run_emulator(void);
void app_deliver_frame(void *pixelBuffer);
#ifdef __cplusplus
}
#endif
#endif
