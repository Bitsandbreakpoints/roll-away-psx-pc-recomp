/*
 * runtime.c - runs the recompiled PS1 executables.
 *
 * The disc boots like on a PS1: SYSTEM.CNF names the executable
 * (SLUS_007.24, the whole game; no overlays).  The executable is loaded
 * from the player's disc into PS1 memory (its data, tables and text all come from there) and its
 * recompiled code (build/gen/, made from the same file) is run from the
 * entry point on the emulated registers.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "runtime/runtime.h"
#include "runtime/system.h"
#include "runtime/bios.h"
#include "runtime/irq.h"
#include "runtime/memory.h"
#include "disc/disc.h"
#include "disc/sha1.h"
#include "psx/log.h"

Cpu g_cpu;
uint32_t g_rt_ram_write_limit = RT_RAM_SIZE;
const RtModule *g_rt_module;

/* the executables on the disc whose code is in the port */
extern const RtFunction g_main_functions[];
extern const int g_main_function_count;
extern const char g_main_sha1[];

static const RtModule MODULES[] = {
    { "SLUS_007.24", g_main_functions, &g_main_function_count, g_main_sha1 },
};
#define MODULE_COUNT            (int) (sizeof(MODULES) / sizeof(MODULES[0]))

/* PS-X EXE header */
#define EXE_HEADER_SIZE         0x800
#define EXE_MAGIC               "PS-X EXE"
#define EXE_PC                  0x10
#define EXE_GP                  0x14
#define EXE_TEXT                0x18
#define EXE_TEXT_SIZE           0x1C
#define EXE_BSS                 0x28
#define EXE_BSS_SIZE            0x2C
#define EXE_STACK               0x30
#define EXE_STACK_SIZE          0x34
#define MAX_EXE_SIZE            0x200000

#define SR_ISOLATE_CACHE        0x10000u
#define SR_START                0x40000401u     /* COP2 usable, interrupts on (IEc, IM2) */
#define SYSCALL_ENTER_CRITICAL  1
#define SYSCALL_EXIT_CRITICAL   2
#define SR_INTERRUPTS           0x401u          /* IEc + IM2 */

static jmp_buf s_stop;                  /* leaves the game (psx_run_game) */
static jmp_buf s_exec;                  /* starts the loaded executable */
static int s_running;
static uint32_t s_entry;
static char s_fatal[512];

/* ---- calls by address -------------------------------------------------- */

static const RtFunction *find_function(uint32_t address)
{
    /* (compared as RAM offsets: the game is linked to KUSEG) */
    const RtFunction *f = g_rt_module->functions;
    int lo = 0, hi = *g_rt_module->function_count - 1;
    address &= RT_RAM_SIZE - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t at = f[mid].address & (RT_RAM_SIZE - 1);
        if (at == address) {
            return &f[mid];
        }
        if (at < address) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

void rt_call(uint32_t address)
{
    uint32_t phys = address & 0x1FFFFFFFu;
    if (phys == BIOS_A0 || phys == BIOS_B0 || phys == BIOS_C0) {
        bios_call(phys, GPR[9]);
        return;
    }
    if (phys < 0x800000u) {
        address = RT_RAM_BASE | (phys & (RT_RAM_SIZE - 1));    /* (KUSEG/KSEG1 mirrors) */
    }
    const RtFunction *f = g_rt_module ? find_function(address) : NULL;
    if (!f) {
        rt_fatal("The game called %08X (from %08X), where no recompiled function starts.",
                 address, GPR[31]);
    }
    f->func();
}

void rt_invoke(uint32_t address, uint32_t a0)
{
    Cpu saved = g_cpu;
    GPR[4] = a0;
    /* (below the interrupted code's stack, as the interrupt handler runs) */
    GPR[29] = (saved.r[29] - 0x400u) & ~7u;
    GPR[31] = 0;
    rt_call(address);
    g_cpu = saved;
}

/* ---- memory ------------------------------------------------------------ */

void *rt_pointer(uint32_t address)
{
    uint32_t phys = address & 0x1FFFFFFFu;
    if (phys < 0x800000u) {
        return (void *) (uintptr_t) (RT_RAM_BASE | (phys & (RT_RAM_SIZE - 1)));
    }
    if (phys >= PSX_SCRATCHPAD_ADDRESS && phys < PSX_SCRATCHPAD_ADDRESS + PSX_SCRATCHPAD_SIZE) {
        return (void *) (uintptr_t) phys;
    }
    return NULL;
}

void rt_lwc2(int reg, uint32_t address)
{
    void *p = rt_pointer(address);
    if (p) {
        gte_lwc2(reg, p);
    } else {
        uint32_t value = mem_read(address, 4);
        gte_lwc2(reg, &value);
    }
}

void rt_swc2(int reg, uint32_t address)
{
    void *p = rt_pointer(address);
    if (p) {
        gte_swc2(reg, p);
    } else {
        uint32_t value;
        gte_swc2(reg, &value);
        mem_write(address, value, 4);
    }
}

/* ---- coprocessor 0 and exceptions -------------------------------------- */

void rt_mtc0(int reg, uint32_t value)
{
    g_cpu.cop0[reg & 31] = value;
    if (reg == 12) {
        g_rt_ram_write_limit = (value & SR_ISOLATE_CACHE) ? 0 : RT_RAM_SIZE;
    }
}

void rt_rfe(void)
{
    uint32_t sr = g_cpu.cop0[12];
    g_cpu.cop0[12] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
}

int rt_interrupts_enabled(void)
{
    return (g_cpu.cop0[12] & SR_INTERRUPTS) == SR_INTERRUPTS;
}

/* (the BIOS's system calls: the critical sections of Sony's libraries) */
void rt_syscall(uint32_t pc)
{
    switch (GPR[4]) {
    case SYSCALL_ENTER_CRITICAL:
        GPR[2] = rt_interrupts_enabled();
        g_cpu.cop0[12] &= ~SR_INTERRUPTS;
        break;
    case SYSCALL_EXIT_CRITICAL:
        g_cpu.cop0[12] |= SR_INTERRUPTS;
        g_rt_poll_count = 0;            /* (what waited may come now) */
        break;
    default:
        psx_log("syscall(%u) at %08X", GPR[4], pc);
        break;
    }
}

void rt_break(uint32_t pc, uint32_t code)
{
    static uint32_t last;
    if (pc != last) {
        psx_log("break %u at %08X (ra %08X)", code, pc, GPR[31]);
        last = pc;
    }
}

void rt_bad_insn(uint32_t pc, uint32_t word)
{
    rt_fatal("Unknown instruction %08X at %08X.", word, pc);
}

void rt_hle_internal(uint32_t address)
{
    rt_fatal("Library-internal function %08X was called (from %08X); its library is "
             "replaced by the PC port.", address, GPR[31]);
}

void rt_fatal(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(s_fatal, sizeof(s_fatal), format, args);
    va_end(args);
    psx_log("FATAL: %s", s_fatal);
    psx_log("  pc-near ra %08X sp %08X a0 %08X a1 %08X v0 %08X", GPR[31], GPR[29], GPR[4],
            GPR[5], GPR[2]);
    if (s_running) {
        longjmp(s_stop, 1);
    }
}

const char *psx_fatal_message(void)
{
    return s_fatal[0] ? s_fatal : NULL;
}

/* ---- executables ------------------------------------------------------- */

static uint32_t get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
}

/* "cdrom:\MAIN.EXE;1" -> "MAIN.EXE" (the name without device, directories and version) */
static void base_name(const char *path, char *out, int size)
{
    const char *p = strchr(path, ':') ? strchr(path, ':') + 1 : path;
    for (const char *q = p; *q; q++) {
        if (*q == '\\' || *q == '/') {
            p = q + 1;
        }
    }
    int n = 0;
    while (p[n] && p[n] != ';' && n < size - 1) {
        out[n] = p[n] >= 'a' && p[n] <= 'z' ? (char) (p[n] - 32) : p[n];
        n++;
    }
    out[n] = 0;
}

/* Loads an executable from the disc and prepares the registers to start it. */
static int load_executable(const char *path, uint32_t stack, uint32_t stack_size)
{
    static uint8_t file[MAX_EXE_SIZE + EXE_HEADER_SIZE];
    char name[64], disc_path[80];
    uint32_t lba, size;
    const RtModule *module = NULL;

    base_name(path, name, sizeof(name));
    for (int i = 0; i < MODULE_COUNT; i++) {
        if (strcmp(MODULES[i].disc_name, name) == 0) {
            module = &MODULES[i];
        }
    }
    if (!module) {
        rt_fatal("The game started %s, which the port does not contain.", path);
    }
    snprintf(disc_path, sizeof(disc_path), "/%s;1", name);
    if (!disc_find_file(disc_path, &lba, &size) || size > sizeof(file) || size < EXE_HEADER_SIZE
        || !disc_read_sectors(lba, (size + DISC_SECTOR_DATA - 1) / DISC_SECTOR_DATA, file)) {
        rt_fatal("%s cannot be read from the disc.", name);
    }
    /* the code was recompiled from exactly this file */
    Sha1 ctx;
    uint8_t digest[20];
    char hex[41];
    sha1_init(&ctx);
    sha1_update(&ctx, file, size);
    sha1_final(&ctx, digest);
    for (int i = 0; i < 20; i++) {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }
    if (strcmp(hex, module->sha1) != 0) {
        rt_fatal("%s on this disc is not the one the port was made from (SHA1 %s).", name, hex);
    }
    if (memcmp(file, EXE_MAGIC, 8) != 0) {
        rt_fatal("%s is not a PS-X EXE.", name);
    }
    uint32_t text = get32(file + EXE_TEXT), text_size = get32(file + EXE_TEXT_SIZE);
    uint32_t bss = get32(file + EXE_BSS), bss_size = get32(file + EXE_BSS_SIZE);
    if (RAM_OFFSET(text) >= RT_RAM_SIZE || text_size > RT_RAM_SIZE - RAM_OFFSET(text)
        || text_size > size - EXE_HEADER_SIZE) {
        rt_fatal("%s: unexpected header.", name);
    }
    memcpy((void *) RAM_HOST(text), file + EXE_HEADER_SIZE, text_size);
    if (bss_size && RAM_OFFSET(bss) < RT_RAM_SIZE && bss_size <= RT_RAM_SIZE - RAM_OFFSET(bss)) {
        memset((void *) RAM_HOST(bss), 0, bss_size);
    }
    /* registers as the BIOS leaves them */
    if (!stack) {
        stack = get32(file + EXE_STACK);
        stack_size = get32(file + EXE_STACK_SIZE);
    }
    if (stack) {
        GPR[29] = GPR[30] = stack + stack_size;
    }
    GPR[28] = get32(file + EXE_GP);
    GPR[31] = 0;
    g_rt_module = module;
    s_entry = get32(file + EXE_PC);
    psx_log("loaded %s: %08X..%08X, entry %08X, sp %08X", name, text, text + text_size, s_entry,
            GPR[29]);
    return 1;
}

void rt_load_exec(const char *path, uint32_t stack, uint32_t stack_size)
{
    load_executable(path, stack, stack_size);
    longjmp(s_exec, 1);
}

/* the first executable: BOOT = cdrom:\SLUS_007.24;1 in SYSTEM.CNF */
static int boot_path(char *out, int size)
{
    char cnf[DISC_SECTOR_DATA + 1];
    uint32_t lba, length;
    if (!disc_find_file("/SYSTEM.CNF;1", &lba, &length) || !disc_read_sectors(lba, 1, cnf)) {
        return 0;
    }
    cnf[length < DISC_SECTOR_DATA ? length : DISC_SECTOR_DATA] = 0;
    const char *p = strstr(cnf, "BOOT");
    p = p ? strchr(p, '=') : NULL;
    if (!p) {
        return 0;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    int n = 0;
    while (p[n] && p[n] != '\r' && p[n] != '\n' && p[n] != ' ' && n < size - 1) {
        out[n] = p[n];
        n++;
    }
    out[n] = 0;
    return n > 0;
}

/* ---- running the game ---------------------------------------------------- */

static PsxFrameHook s_hook;

int psx_system_init(void)
{
    if (!psx_memory_init()) {
        psx_log("cannot map the PS1 memory at 0x80000000 / 0x1F800000");
        return 0;
    }
    return 1;
}

void psx_run_game(PsxFrameHook hook)
{
    char boot[64];
    s_hook = hook;
    s_fatal[0] = 0;
    psx_memory_clear();
    memset(&g_cpu, 0, sizeof(g_cpu));
    g_cpu.cop0[12] = SR_START;
    g_rt_ram_write_limit = RT_RAM_SIZE;
    g_rt_module = NULL;
    gte_reset();
    psx_devices_reset();
    psx_log("game started");
    s_running = 1;
    if (setjmp(s_stop) == 0) {
        if (!boot_path(boot, sizeof(boot))) {
            rt_fatal("SYSTEM.CNF cannot be read from the disc.");
        }
        load_executable(boot, 0, 0);
        setjmp(s_exec);                 /* (LoadExec() comes back here) */
        rt_call(s_entry);
        psx_log("the program returned");
    }
    s_running = 0;
    psx_devices_stop();
    psx_log("game stopped");
}

void psx_vblank(void)
{
    if (!s_hook()) {
        longjmp(s_stop, 1);
    }
}
