/*
 * memory.c - the PS1's memory at its own addresses.
 *
 * The PC port is a 32-bit process linked with --large-address-aware, so it
 * can map the 2 MB of main RAM at 0x80000000 and the scratchpad at
 * 0x1F800000: a PS1 address of RAM is a valid pointer, and the
 * recompiled code (recomp.h) accesses it directly.  What is left comes
 * here: the RAM mirrors (KUSEG, KSEG1), the scratchpad, the hardware
 * registers and the BIOS ROM area.
 */
#include <windows.h>
#include <string.h>
#include "runtime/recomp.h"
#include "runtime/memory.h"
#include "runtime/kanji.h"
#include "psx/log.h"

/* (VirtualAlloc works in pages: the scratchpad gets a whole one) */
#define PAGE_SIZE               0x1000u

#define PHYS_MASK               0x1FFFFFFFu
#define RAM_MIRRORS_END         0x00800000u     /* RAM is mirrored 4 times */
#define IO_START                0x1F801000u
#define IO_END                  0x1F803000u
#define EXPANSION1_START        0x1F000000u
#define EXPANSION1_END          0x1F800000u
#define BIOS_START              0x1FC00000u
#define BIOS_END                0x1FC80000u
#define CACHE_CONTROL           0xFFFE0130u

int psx_memory_init(void)
{
    void *ram = VirtualAlloc((void *) PSX_RAM_ADDRESS, PSX_RAM_SIZE, MEM_RESERVE | MEM_COMMIT,
                             PAGE_READWRITE);
    void *scratchpad = VirtualAlloc((void *) PSX_SCRATCHPAD_ADDRESS, PAGE_SIZE,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return ram == (void *) PSX_RAM_ADDRESS && scratchpad == (void *) PSX_SCRATCHPAD_ADDRESS;
}

void psx_memory_clear(void)
{
    memset((void *) PSX_RAM_ADDRESS, 0, PSX_RAM_SIZE);
    memset((void *) PSX_SCRATCHPAD_ADDRESS, 0, PSX_SCRATCHPAD_SIZE);
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

static void *host(uint32_t address)
{
    uint32_t phys = address & PHYS_MASK;
    if (phys < RAM_MIRRORS_END) {
        return (void *) (uintptr_t) (PSX_RAM_ADDRESS | (phys & (PSX_RAM_SIZE - 1)));
    }
    if (phys >= PSX_SCRATCHPAD_ADDRESS && phys < PSX_SCRATCHPAD_ADDRESS + PSX_SCRATCHPAD_SIZE) {
        return (void *) (uintptr_t) phys;
    }
    return NULL;
}

uint32_t mem_read(uint32_t address, int size)
{
    void *p = host(address);
    if (p) {
        return size == 1 ? *(uint8_t *) p : size == 2 ? *(uint16_t *) p : *(uint32_t *) p;
    }
    uint32_t phys = address & PHYS_MASK;
    if (phys >= IO_START && phys < IO_END) {
        return io_read(phys, size);
    }
    if (phys >= EXPANSION1_START && phys < EXPANSION1_END) {
        return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu;     /* nothing there */
    }
    if (phys >= BIOS_START && phys < BIOS_END) {
        uint32_t value;
        if (kanji_read(phys, &value, size)) {
            return value;               /* (the font, drawn by the PC) */
        }
        log_once("read of the BIOS ROM at", address);
        return 0;
    }
    log_once("read of unmapped", address);
    return 0;
}

void mem_write(uint32_t address, uint32_t value, int size)
{
    if (g_rt_ram_write_limit == 0 && address != CACHE_CONTROL) {
        return;                         /* (cache isolated: the BIOS's cache flush) */
    }
    void *p = host(address);
    if (p) {
        if (size == 1) {
            *(uint8_t *) p = (uint8_t) value;
        } else if (size == 2) {
            *(uint16_t *) p = (uint16_t) value;
        } else {
            *(uint32_t *) p = value;
        }
        return;
    }
    uint32_t phys = address & PHYS_MASK;
    if (phys >= IO_START && phys < IO_END) {
        io_write(phys, value, size);
        return;
    }
    if (address == CACHE_CONTROL || (phys >= EXPANSION1_START && phys < EXPANSION1_END)) {
        return;
    }
    log_once("write to unmapped", address);
}
