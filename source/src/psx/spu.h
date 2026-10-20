/*
 * spu.h - the PS1 sound processor in software: 512 KB of sound RAM, 24
 * voices playing ADPCM samples with ADSR envelopes, noise, pitch
 * modulation and the reverb, driven through its hardware registers
 * (1F801C00h..1F801FFFh) and DMA channel 4, as Sony's LIBSPU/LIBSND (which
 * run recompiled) drive it.  The PC side takes 44.1 kHz stereo samples.
 */
#ifndef PSX_SPU_H
#define PSX_SPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_RAM_SIZE            0x80000
#define SPU_VOICES              24
#define SPU_SAMPLE_RATE         44100
#define SPU_REG_BASE            0x1F801C00u
#define SPU_REG_END             0x1F802000u

extern uint8_t g_spu_ram[SPU_RAM_SIZE];

void spu_reset(void);

/* a 16-bit register (offset from 1F801C00h) */
uint16_t spu_read16(uint32_t offset);
void spu_write16(uint32_t offset, uint16_t value);

/* DMA channel 4: the words go to / come from the transfer address */
void spu_dma_write(const void *data, uint32_t bytes);
void spu_dma_read(void *data, uint32_t bytes);

/* Mixes `frames` stereo samples (interleaved L, R) at 44.1 kHz. */
void spu_render(int16_t *out, int frames);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SPU_H */
