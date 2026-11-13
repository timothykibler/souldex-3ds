/* The ARM7's memory map for the port's ARM7 sound driver (port/runtime/snd.c): its I/O registers
 * (the 16 sound channels at 0x400-0x51F, SOUNDCNT, ...) are an array the port's mixer reads. */
#ifndef PORT_ARM7_MMAP_GLOBAL_H
#define PORT_ARM7_MMAP_GLOBAL_H
#include_next <nitro/hw/ARM7/mmap_global.h>
#undef HW_IOREG
#undef HW_REG_BASE
extern unsigned char port_a7_io[];
#define HW_IOREG ((unsigned long)port_a7_io)
#define HW_REG_BASE HW_IOREG
#endif
