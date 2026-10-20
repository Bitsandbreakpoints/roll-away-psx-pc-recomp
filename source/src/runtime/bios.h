/*
 * bios.h - the PS1 BIOS functions that the game calls (A0h/B0h/C0h
 * tables): C library, heap, events, LoadExec ...  No BIOS ROM is needed.
 */
#ifndef RUNTIME_BIOS_H
#define RUNTIME_BIOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BIOS_A0                 0xA0u
#define BIOS_B0                 0xB0u
#define BIOS_C0                 0xC0u

void bios_reset(void);

/* A call through a BIOS vector: `table` A0h/B0h/C0h, `function` in t1. */
void bios_call(uint32_t table, uint32_t function);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_BIOS_H */
