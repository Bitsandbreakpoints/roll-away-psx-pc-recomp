/*
 * runtime.h - runs the recompiled executables: the modules (one per PS1
 * executable on the disc), calls by address, interrupts and callbacks.
 */
#ifndef RUNTIME_RUNTIME_H
#define RUNTIME_RUNTIME_H

#include <stdint.h>
#include "runtime/recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A PS1 executable whose code is recompiled into the port. */
typedef struct {
    const char *disc_name;              /* e.g. "MAIN.EXE" */
    const RtFunction *functions;        /* sorted by address */
    const int *function_count;
    const char *sha1;                   /* of the whole file (the code was made from it) */
} RtModule;

/* the executable whose code runs */
extern const RtModule *g_rt_module;

/* A host pointer for a PS1 address in RAM (any mirror) or the
 * scratchpad; NULL for anything else. */
void *rt_pointer(uint32_t address);
#define PTR(a)                  rt_pointer(a)

/* Arguments and result of a replaced (HLE) function: a0..a3, then the
 * stack (argument n >= 4 at sp + 4n). */
#define ARG(n)                  ((n) < 4 ? GPR[4 + (n)] : RD32(GPR[29] + 4u * (n)))
#define RETURN(v)               (GPR[2] = (uint32_t) (v))

/* Calls a function of the game (a callback) as an interrupt would: the
 * registers are saved and restored around it. */
void rt_invoke(uint32_t address, uint32_t a0);

/* Something is wrong: logs it, tells the player and stops the game. */
void rt_fatal(const char *format, ...);

/* Starts a PS1 executable from the disc ("cdrom:\MAIN.EXE;1" or
 * "/MAIN.EXE;1"), as the BIOS's LoadExec(): does not return. */
void rt_load_exec(const char *path, uint32_t stack, uint32_t stack_size);

/* Interrupts on/off (EnterCriticalSection ...). */
int rt_interrupts_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_RUNTIME_H */
