#ifndef PORT_H
#define PORT_H
#include <stdint.h>
#include <stddef.h>

#define PORT_ARENA_SIZE (8 * 1024 * 1024) // display lists read the top address byte as a segment number, so everything they point at must stay under 16 MB
#define PORT_SLOTS 8

void port_fatal(const char *why);
void port_log(const char *fmt, ...);

// scene_driver.c
void port_boot(void);
int  port_step(int draw);   // one sim frame. Returns 1 while a match runs.
void port_match_begin(void);
void port_match_park(void);
uint32_t port_sim_frame(void);

// sched.c, os.c
void port_sched_drain(void);
void *port_gfx_malloc(size_t size, uint32_t align);
void port_rom_set(const uint8_t *image, uint32_t size);

// gfx
void port_gfx_run(void *display_list);
// audio
struct SYTaskAudioTag;
int  port_audio_queued_samples(void);

// net
void port_pad_for_frame(int slot, uint16_t *buttons, int8_t *x, int8_t *y);

// snapshot.c
void port_snapshot_boot_take(void);
void port_snapshot_boot_restore(void);
#endif
