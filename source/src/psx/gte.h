/*
 * gte.h - the PS1 Geometry Transformation Engine (coprocessor 2) in C.
 *
 * The whole register file and every command, so that Sony's library
 * routines, which are written in GTE instructions, can be translated
 * instruction by instruction: gte_mtc2/mfc2 (data registers), ctc2/cfc2
 * (control registers), lwc2/swc2 (memory) and gte_cop2() for a command.
 * The arithmetic, the saturation and the FLAG bits follow the "GTE"
 * chapter of psx-spx (nocash), so results are those of a PS1.
 */
#ifndef PSX_GTE_H
#define PSX_GTE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* data registers (mtc2/mfc2) */
enum GteData {
    GTE_VXY0, GTE_VZ0, GTE_VXY1, GTE_VZ1, GTE_VXY2, GTE_VZ2, GTE_RGBC, GTE_OTZ,
    GTE_IR0, GTE_IR1, GTE_IR2, GTE_IR3, GTE_SXY0, GTE_SXY1, GTE_SXY2, GTE_SXYP,
    GTE_SZ0, GTE_SZ1, GTE_SZ2, GTE_SZ3, GTE_RGB0, GTE_RGB1, GTE_RGB2, GTE_RES1,
    GTE_MAC0, GTE_MAC1, GTE_MAC2, GTE_MAC3, GTE_IRGB, GTE_ORGB, GTE_LZCS, GTE_LZCR
};

/* control registers (ctc2/cfc2) */
enum GteControl {
    GTE_RT11RT12, GTE_RT13RT21, GTE_RT22RT23, GTE_RT31RT32, GTE_RT33,
    GTE_TRX, GTE_TRY, GTE_TRZ,
    GTE_L11L12, GTE_L13L21, GTE_L22L23, GTE_L31L32, GTE_L33,
    GTE_RBK, GTE_GBK, GTE_BBK,
    GTE_LR1LR2, GTE_LR3LG1, GTE_LG2LG3, GTE_LB1LB2, GTE_LB3,
    GTE_RFC, GTE_GFC, GTE_BFC,
    GTE_OFX, GTE_OFY, GTE_H, GTE_DQA, GTE_DQB, GTE_ZSF3, GTE_ZSF4, GTE_FLAG
};

/* commands as the library writes them ("cop2 0x...") */
#define GTE_CMD_RTPS            0x0180001u
#define GTE_CMD_RTPT            0x0280030u
#define GTE_CMD_NCLIP           0x1400006u
#define GTE_CMD_AVSZ3           0x158002Du
#define GTE_CMD_AVSZ4           0x168002Eu
#define GTE_CMD_NCCS            0x108041Bu
#define GTE_CMD_NCCT            0x118043Fu
#define GTE_CMD_NCS             0x0C8041Eu
#define GTE_CMD_NCT             0x0D80420u
#define GTE_CMD_NCDS            0x0E80413u
#define GTE_CMD_NCDT            0x0F80416u

/* the fields of a command */
#define GTE_SF(sf)              ((uint32_t) (sf) << 19)
#define GTE_MX(mx)              ((uint32_t) (mx) << 17)     /* 0 RT, 1 LLM, 2 LCM */
#define GTE_V(v)                ((uint32_t) (v) << 15)      /* 0..2 V0..V2, 3 IR */
#define GTE_CV(cv)              ((uint32_t) (cv) << 13)     /* 0 TR, 1 BK, 2 FC, 3 none */
#define GTE_LM(lm)              ((uint32_t) (lm) << 10)
#define GTE_OP_MVMVA            0x12u
#define GTE_OP_SQR              0x28u
#define GTE_OP_OP               0x0Cu
#define GTE_OP_GPF              0x3Du
#define GTE_OP_GPL              0x3Eu
/* mvmva sf, mx, v, cv, lm (the assembler's operand order) */
#define GTE_CMD_MVMVA(sf, mx, v, cv, lm) \
    (GTE_SF(sf) | GTE_MX(mx) | GTE_V(v) | GTE_CV(cv) | GTE_LM(lm) | GTE_OP_MVMVA)

/* FLAG: bit 31 is set when any of these is */
#define GTE_FLAG_ERROR          0x7F87E000u
#define GTE_FLAG_ERROR_BIT      0x80000000u

/* Power-on state (all zero). */
void gte_reset(void);

void gte_mtc2(int reg, uint32_t value);
uint32_t gte_mfc2(int reg);
void gte_ctc2(int reg, uint32_t value);
uint32_t gte_cfc2(int reg);

/* lwc2 / swc2: a data register from / to memory (a 32-bit word) */
void gte_lwc2(int reg, const void *address);
void gte_swc2(int reg, void *address);

/* One GTE command (the 25-bit immediate of "cop2"). */
void gte_cop2(uint32_t command);

#ifdef __cplusplus
}
#endif

#endif /* PSX_GTE_H */
