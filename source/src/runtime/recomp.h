/*
 * recomp.h - what the recompiled code (build/gen/, recomp/recomp.py)
 * needs: the emulated R3000A registers, PS1 memory access and the
 * helpers for instructions that are not plain C operators.
 *
 * Main RAM lies at its own address (0x80000000, a 32-bit process, see
 * memory.c), so a KSEG0 address is a valid pointer.  Roll Away is linked
 * to the KUSEG mirror (0x00010000...): RAM_HOST() turns either address
 * into the pointer, so loads and stores to RAM are a mask, a compare and
 * a plain access.  Everything else (mirrors, the
 * scratchpad, hardware registers) goes through mem_read() / mem_write().
 */
#ifndef RUNTIME_RECOMP_H
#define RUNTIME_RECOMP_H

#include <stdint.h>
#include "psx/gte.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t r[32];             /* r[0] is always 0 (never written) */
    uint32_t hi, lo;
    uint32_t cop0[32];          /* 12 SR, 13 CAUSE, 14 EPC */
} Cpu;

extern Cpu g_cpu;
#define GPR                     g_cpu.r

/* one translated function of an executable */
typedef struct {
    uint32_t address;
    void (*func)(void);
} RtFunction;

/* ---- memory ------------------------------------------------------------ */

#define RT_RAM_BASE             0x80000000u
#define RT_RAM_SIZE             0x200000u

/* KUSEG (0x00000000) and KSEG0 (0x80000000) addresses of main RAM */
#define RAM_OFFSET(a)           ((a) & 0x7FFFFFFFu)
#define RAM_HOST(a)             ((uintptr_t) ((a) | RT_RAM_BASE))

/* RT_RAM_SIZE, or 0 while the cache is isolated (writes are dropped) */
extern uint32_t g_rt_ram_write_limit;

uint32_t mem_read(uint32_t address, int size);
void mem_write(uint32_t address, uint32_t value, int size);

static inline uint32_t RD8(uint32_t a)
{
    if (RAM_OFFSET(a) < RT_RAM_SIZE) {
        return *(const uint8_t *) RAM_HOST(a);
    }
    return mem_read(a, 1);
}

static inline uint32_t RD16(uint32_t a)
{
    if (RAM_OFFSET(a) < RT_RAM_SIZE) {
        return *(const uint16_t *) RAM_HOST(a);
    }
    return mem_read(a, 2);
}

static inline uint32_t RD32(uint32_t a)
{
    if (RAM_OFFSET(a) < RT_RAM_SIZE) {
        return *(const uint32_t *) RAM_HOST(a);
    }
    return mem_read(a, 4);
}

static inline void WR8(uint32_t a, uint32_t v)
{
    if (RAM_OFFSET(a) < g_rt_ram_write_limit) {
        *(uint8_t *) RAM_HOST(a) = (uint8_t) v;
    } else {
        mem_write(a, v, 1);
    }
}

static inline void WR16(uint32_t a, uint32_t v)
{
    if (RAM_OFFSET(a) < g_rt_ram_write_limit) {
        *(uint16_t *) RAM_HOST(a) = (uint16_t) v;
    } else {
        mem_write(a, v, 2);
    }
}

static inline void WR32(uint32_t a, uint32_t v)
{
    if (RAM_OFFSET(a) < g_rt_ram_write_limit) {
        *(uint32_t *) RAM_HOST(a) = v;
    } else {
        mem_write(a, v, 4);
    }
}

/* unaligned word halves (little endian) */
static inline uint32_t rt_lwl(uint32_t rt, uint32_t a)
{
    uint32_t shift = (a & 3) * 8, w = RD32(a & ~3u);
    return (rt & (0x00FFFFFFu >> shift)) | (w << (24 - shift));
}

static inline uint32_t rt_lwr(uint32_t rt, uint32_t a)
{
    uint32_t shift = (a & 3) * 8, w = RD32(a & ~3u);
    return (rt & (0xFFFFFF00u << (24 - shift))) | (w >> shift);
}

static inline void rt_swl(uint32_t a, uint32_t rt)
{
    uint32_t shift = (a & 3) * 8, w = RD32(a & ~3u);
    WR32(a & ~3u, (w & (0xFFFFFF00u << shift)) | (rt >> (24 - shift)));
}

static inline void rt_swr(uint32_t a, uint32_t rt)
{
    uint32_t shift = (a & 3) * 8, w = RD32(a & ~3u);
    WR32(a & ~3u, (w & (0x00FFFFFFu >> (24 - shift))) | (rt << shift));
}

/* ---- multiply / divide (HI/LO; division by 0 as the R3000 does it) ---- */

static inline void rt_mult(uint32_t a, uint32_t b)
{
    int64_t p = (int64_t) (int32_t) a * (int32_t) b;
    g_cpu.lo = (uint32_t) p;
    g_cpu.hi = (uint32_t) ((uint64_t) p >> 32);
}

static inline void rt_multu(uint32_t a, uint32_t b)
{
    uint64_t p = (uint64_t) a * b;
    g_cpu.lo = (uint32_t) p;
    g_cpu.hi = (uint32_t) (p >> 32);
}

static inline void rt_div(uint32_t a, uint32_t b)
{
    int32_t n = (int32_t) a, d = (int32_t) b;
    if (d == 0) {
        g_cpu.hi = a;
        g_cpu.lo = n >= 0 ? 0xFFFFFFFFu : 1u;
    } else if (n == INT32_MIN && d == -1) {
        g_cpu.hi = 0;
        g_cpu.lo = 0x80000000u;
    } else {
        g_cpu.hi = (uint32_t) (n % d);
        g_cpu.lo = (uint32_t) (n / d);
    }
}

static inline void rt_divu(uint32_t a, uint32_t b)
{
    if (b == 0) {
        g_cpu.hi = a;
        g_cpu.lo = 0xFFFFFFFFu;
    } else {
        g_cpu.hi = a % b;
        g_cpu.lo = a / b;
    }
}

/* ---- the rest (runtime.c) ---------------------------------------------- */

/* GTE loads/stores (the address is a PS1 address) */
void rt_lwc2(int reg, uint32_t address);
void rt_swc2(int reg, uint32_t address);

void rt_mtc0(int reg, uint32_t value);
void rt_rfe(void);
void rt_syscall(uint32_t pc);
void rt_break(uint32_t pc, uint32_t code);
void rt_bad_insn(uint32_t pc, uint32_t word);
/* a function of a replaced library object that should never run */
void rt_hle_internal(uint32_t address);

/* Calls the function at a PS1 address (jalr, jr through a register,
 * callbacks); the BIOS vectors A0h/B0h/C0h are the BIOS functions. */
void rt_call(uint32_t address);

/* Interrupts: the recompiled code checks for them on backward jumps
 * (loops), as often as the hardware would interrupt it. */
extern int g_rt_poll_count;
void rt_poll(void);
#define RT_POLL()               do { if (--g_rt_poll_count < 0) rt_poll(); } while (0)

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_RECOMP_H */
