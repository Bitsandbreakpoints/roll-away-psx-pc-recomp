/*
 * io.c - the PS1's hardware registers (1F801000h..1F802FFFh) as far as
 * the recompiled code touches them directly.
 *
 * Most hardware is used through Sony's libraries, and the libraries for
 * the GPU, CD-ROM, controllers and interrupts are replaced (hle_*.c).
 * What still talks to the hardware is the sound (LIBSPU/LIBSND run
 * recompiled): the SPU registers and DMA channel 4, plus the interrupt
 * and timer registers around it.  DMA transfers finish at once.
 */
#include <string.h>
#include "runtime/recomp.h"
#include "runtime/runtime.h"
#include "runtime/memory.h"
#include "runtime/irq.h"
#include "psx/spu.h"
#include "psx/gpu.h"
#include "psx/log.h"

#define I_STAT                  0x1F801070u
#define I_MASK                  0x1F801074u
#define DMA_BASE                0x1F801080u
#define DMA_CHANNELS            7
#define DPCR                    0x1F8010F0u
#define DICR                    0x1F8010F4u
#define TIMER_BASE              0x1F801100u
#define TIMER_END               0x1F801130u
#define GPU_GP0                 0x1F801810u
#define GPU_GP1                 0x1F801814u
#define CD_BASE                 0x1F801800u
#define CD_END                  0x1F801804u

#define DMA_GPU                 2
#define DMA_SPU                 4
#define DMA_OTC                 6
#define CHCR_FROM_RAM           0x00000001u
#define CHCR_START              0x01000000u
#define CHCR_TRIGGER            0x10000000u
#define CHCR_SYNC(c)            (((c) >> 9) & 3)
#define DICR_FLAGS              0x7F000000u
#define DICR_MASTER             0x00800000u

#define GPUSTAT_READY           0x1C000000u     /* ready for commands, VRAM reads, DMA */

static uint32_t s_regs[0x2000 / 4];     /* everything as written */
static uint32_t s_dma[DMA_CHANNELS][3]; /* MADR, BCR, CHCR */
static uint32_t s_dicr;
static uint32_t s_istat, s_imask;

void io_reset(void)
{
    memset(s_regs, 0, sizeof(s_regs));
    memset(s_dma, 0, sizeof(s_dma));
    s_dicr = 0;
    s_istat = s_imask = 0;
}

static void log_once(const char *what, uint32_t address)
{
    static uint32_t seen[64];
    static int count;
    for (int i = 0; i < count; i++) {
        if (seen[i] == address) {
            return;
        }
    }
    if (count < 64) {
        seen[count++] = address;
    }
    psx_log("%s %08X (ra %08X)", what, address, GPR[31]);
}

/* ---- DMA ---------------------------------------------------------------- */

static void dma_done(int channel)
{
    s_dma[channel][2] &= ~(CHCR_START | CHCR_TRIGGER);
    if (s_dicr & (1u << (16 + channel))) {
        s_dicr |= 1u << (24 + channel);
    }
    /* (the interrupt library is replaced: a DMACallback() decides) */
    irq_dma_done(channel);
}

static void dma_run(int channel)
{
    uint32_t madr = s_dma[channel][0] & 0xFFFFFCu, bcr = s_dma[channel][1];
    uint32_t chcr = s_dma[channel][2];
    uint32_t words = CHCR_SYNC(chcr) == 0 ? (bcr & 0xFFFF ? bcr & 0xFFFF : 0x10000)
                                           : (bcr & 0xFFFF) * (bcr >> 16);
    switch (channel) {
    case DMA_SPU: {
        void *p = rt_pointer(RT_RAM_BASE | madr);
        if (!p || madr + words * 4 > RT_RAM_SIZE) {
            log_once("SPU DMA outside RAM:", madr);
            break;
        }
        if (chcr & CHCR_FROM_RAM) {
            spu_dma_write(p, words * 4);
        } else {
            spu_dma_read(p, words * 4);
        }
        break;
    }
    case DMA_GPU:
        if (CHCR_SYNC(chcr) == 2) {
            /* a linked list (an ordering table) */
            uint32_t a = madr;
            for (int guard = 0; guard < 0x100000; guard++) {
                uint32_t header = RD32(RT_RAM_BASE | a);
                for (uint32_t i = 0; i < header >> 24; i++) {
                    gpu_gp0(RD32(RT_RAM_BASE | (a + 4 + 4 * i)));
                }
                if ((header & 0xFFFFFF) == 0xFFFFFF || (header & 0x800000)) {
                    break;
                }
                a = header & 0x1FFFFC;
            }
        } else if (chcr & CHCR_FROM_RAM) {
            for (uint32_t i = 0; i < words; i++) {
                gpu_gp0(RD32(RT_RAM_BASE | ((madr + 4 * i) & (RT_RAM_SIZE - 1))));
            }
        } else {
            log_once("GPU DMA to RAM (VRAM read) is not supported:", madr);
        }
        break;
    case DMA_OTC: {
        uint32_t count = bcr & 0xFFFF ? bcr & 0xFFFF : 0x10000, a = madr;
        for (uint32_t i = 0; i < count; i++) {
            WR32(RT_RAM_BASE | a, i == count - 1 ? 0xFFFFFFu : (a - 4) & 0xFFFFFFu);
            a = (a - 4) & (RT_RAM_SIZE - 1);
        }
        break;
    }
    default:
        log_once("DMA on an unsupported channel, CHCR", DMA_BASE + channel * 16 + 8);
        break;
    }
    dma_done(channel);
}

/* ---- registers ---------------------------------------------------------- */

uint32_t io_read(uint32_t address, int size)
{
    if (address >= SPU_REG_BASE && address < SPU_REG_END) {
        uint32_t offset = address - SPU_REG_BASE;
        uint32_t v = spu_read16(offset & ~1u);
        if (size == 4) {
            v |= (uint32_t) spu_read16((offset & ~1u) + 2) << 16;
        } else if (size == 1 && (offset & 1)) {
            v >>= 8;
        }
        return size == 1 ? v & 0xFF : v;
    }
    uint32_t word = address & ~3u, shift = (address & 3) * 8, value;
    if (word == I_STAT) {
        value = s_istat;
    } else if (word == I_MASK) {
        value = s_imask;
    } else if (word >= DMA_BASE && word < DMA_BASE + DMA_CHANNELS * 16) {
        int reg = ((word - DMA_BASE) % 16) / 4;
        value = reg < 3 ? s_dma[(word - DMA_BASE) / 16][reg] : 0;
    } else if (word == DICR) {
        value = s_dicr | ((s_dicr & DICR_FLAGS) && (s_dicr & DICR_MASTER) ? 0x80000000u : 0);
    } else if (word >= TIMER_BASE && word < TIMER_END) {
        int timer = (word - TIMER_BASE) / 16, reg = ((word - TIMER_BASE) % 16) / 4;
        value = reg == 0 ? irq_timer_value(timer) : s_regs[(word - 0x1F801000u) / 4];
    } else if (word == GPU_GP1) {
        value = GPUSTAT_READY;
    } else if (word == GPU_GP0) {
        value = 0;
        log_once("GPUREAD", word);
    } else {
        if (word >= CD_BASE && word < CD_END) {
            log_once("CD-ROM register read", address);
        } else if (!(word >= 0x1F801000u && word < 0x1F801024u) && word != 0x1F801060u
                   && word != DPCR) {
            log_once("hardware register read", address);
        }
        value = s_regs[(word - 0x1F801000u) / 4];
    }
    value >>= shift;
    return size == 1 ? value & 0xFF : size == 2 ? value & 0xFFFF : value;
}

void io_write(uint32_t address, uint32_t value, int size)
{
    if (address >= SPU_REG_BASE && address < SPU_REG_END) {
        uint32_t offset = address - SPU_REG_BASE;
        spu_write16(offset, (uint16_t) value);
        if (size == 4) {
            spu_write16(offset + 2, (uint16_t) (value >> 16));
        }
        return;
    }
    uint32_t word = address & ~3u;
    if (size < 4) {
        uint32_t shift = (address & 3) * 8, mask = (size == 1 ? 0xFFu : 0xFFFFu) << shift;
        value = (s_regs[(word - 0x1F801000u) / 4] & ~mask) | ((value << shift) & mask);
    }
    s_regs[(word - 0x1F801000u) / 4] = value;
    if (word == I_STAT) {
        s_istat &= value;               /* (acknowledge: write 0 bits) */
    } else if (word == I_MASK) {
        s_imask = value;
    } else if (word >= DMA_BASE && word < DMA_BASE + DMA_CHANNELS * 16) {
        int channel = (word - DMA_BASE) / 16, reg = ((word - DMA_BASE) % 16) / 4;
        if (reg < 3) {
            s_dma[channel][reg] = value;
            if (reg == 2 && (value & CHCR_START)) {
                dma_run(channel);
            }
        }
    } else if (word == DICR) {
        s_dicr = (s_dicr & DICR_FLAGS & ~value) | (value & 0x00FF803Fu);
    } else if (word >= TIMER_BASE && word < TIMER_END) {
        int timer = (word - TIMER_BASE) / 16, reg = ((word - TIMER_BASE) % 16) / 4;
        irq_timer_write(timer, reg, value);
    } else if (word == GPU_GP0) {
        gpu_gp0(value);
    } else if (word == GPU_GP1) {
        log_once("GP1 command", value >> 24);
    } else if (word >= CD_BASE && word < CD_END) {
        log_once("CD-ROM register write", address);
    } else if (!(word >= 0x1F801000u && word < 0x1F801024u) && word != 0x1F801060u
               && word != DPCR) {
        log_once("hardware register write", address);
    }
}

void io_set_interrupt(uint32_t bits)
{
    s_istat |= bits;
}
