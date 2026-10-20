/*
 * hle_misc.c - small replacements, and the reset of all of them.
 */
#include "runtime/recomp.h"
#include "runtime/runtime.h"
#include "runtime/bios.h"
#include "runtime/kanji.h"

void hle_gpu_reset(void);
void hle_cd_reset(void);
void hle_press_reset(void);
void hle_mcrd_reset(void);

/* LIBGTE's InitGeom() patches the BIOS's exception handler for the GTE;
 * there is no BIOS code in memory to patch. */
void hle__patch_gte(void)
{
}

/* LIBSIO: the serial port's console (debugging output); nothing is attached */
void hle_AddSIO(void)
{
}

void hle_DelSIO(void)
{
}

/* LIBCARD: the BIOS's memory card driver (LIBMCRD is replaced as a whole) */
void hle_InitCARD(void)
{
}

void hle_StartCARD(void)
{
    RETURN(1);
}

void hle_StopCARD(void)
{
    RETURN(1);
}

void hle_reset_all(void)
{
    bios_reset();
    kanji_reset();
    hle_gpu_reset();
    hle_cd_reset();
    hle_press_reset();
    hle_mcrd_reset();
}
