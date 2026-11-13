// The 3DS port's runtime: what the replacement SDK pieces share.
#ifndef PORT_H
#define PORT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void port_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

bool port_ds_mem_init(void);

// The player's own ROM (the game's files are read from it, never shipped).
bool port_rom_open(void);
void port_rom_read(uint32_t offset, void *dst, uint32_t len);
uint32_t port_rom_size(void);

// One displayed frame: waits for the 3DS's VBlank, then runs the game's VBlank interrupt handler.
void port_frame(void);
extern volatile uint32_t port_frame_count;

// Overlays (generated table, port/gen/overlays.c)
void port_overlays_init(void);
void port_overlay_start(uint32_t id);
uint32_t port_overlay_count(void);

// 3DS services (ctr.c, the one file that includes <3ds.h> besides main.c and ds_mem.c)
bool port_ctr_frame_wait(void);
__attribute__((noreturn)) void port_ctr_exit(int code);
uint64_t port_ctr_ticks(void);
void port_ctr_sleep_ns(int64_t ns);
void port_ctr_buttons(uint16_t *keyinput, uint16_t *xy);
bool port_ctr_touch(uint16_t *x, uint16_t *y);
void port_ctr_screens_init(void);
void port_ctr_present(const uint16_t *top, const uint16_t *bottom);
void port_render2d(uint16_t *top, uint16_t *bottom);

#endif
