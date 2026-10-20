/*
 * hle_press.c - LIBPRESS's MDEC functions (the movies), on the software
 * MDEC (psx/mdec.c).  The VLC decoding (DecDCTvlc2) runs recompiled.
 *
 * The game's movie player is Sony's: it gives a frame's run-length codes
 * to the MDEC (DecDCTin) and asks for the picture one column of
 * macroblocks at a time (DecDCTout); when a column is done the MDEC's DMA
 * interrupt calls the game's callback (DecDCToutCallback), which copies it
 * to VRAM and asks for the next column.  Here each DecDCTout() decodes
 * its column at once and calls the callback; a DecDCTout() made by the
 * callback is run right after it returns, as the interrupt would.
 */
#include "runtime/recomp.h"
#include "runtime/runtime.h"
#include "psx/mdec.h"
#include "psx/log.h"

#define MODE_24BIT              0x01    /* DecDCTin() mode bits */
#define MODE_SET_BIT15          0x02
#define CODE_COUNT_MASK         0xFFFF  /* the command word: 32-bit words that follow */
#define BYTES_PER_MACROBLOCK_24 (MDEC_MACROBLOCK * MDEC_MACROBLOCK * 3)
#define BYTES_PER_MACROBLOCK_15 (MDEC_MACROBLOCK * MDEC_MACROBLOCK * 2)

static int s_mode;
static uint32_t s_out_callback, s_in_callback_func;
static int s_in_callback;
static uint32_t s_pending_buf, s_pending_words;   /* DecDCTout() made by the callback */
static int s_pending;

void hle_press_reset(void)
{
    s_mode = 0;
    s_out_callback = s_in_callback_func = 0;
    s_in_callback = 0;
    s_pending = 0;
    mdec_start(NULL, 0);
}

void hle_DecDCTReset(void)
{
    mdec_start(NULL, 0);
    s_pending = 0;
}

/* DecDCTin(buf, mode): buf[0] is the MDEC command (word count), the codes follow */
void hle_DecDCTin(void)
{
    uint32_t buf = GPR[4];
    s_mode = (int) GPR[5];
    uint32_t words = RD32(buf) & CODE_COUNT_MASK;
    mdec_start((const uint16_t *) rt_pointer(buf + 4), (int) words * 2);
}

static void output(uint32_t buf, uint32_t words)
{
    int bytes = s_mode & MODE_24BIT ? BYTES_PER_MACROBLOCK_24 : BYTES_PER_MACROBLOCK_15;
    uint8_t *out = (uint8_t *) rt_pointer(buf);
    if (!out) {
        psx_log("DecDCTout: %08X is not in RAM", buf);
        return;
    }
    mdec_output(out, (int) (words * 4 / (uint32_t) bytes), s_mode & MODE_24BIT,
                s_mode & MODE_SET_BIT15);
}

/* DecDCTout(buf, words) */
void hle_DecDCTout(void)
{
    uint32_t buf = GPR[4], words = GPR[5];
    if (s_in_callback) {
        s_pending = 1;                  /* (run when the callback returns) */
        s_pending_buf = buf;
        s_pending_words = words;
        return;
    }
    for (;;) {
        output(buf, words);
        if (!s_out_callback) {
            return;
        }
        s_pending = 0;
        s_in_callback = 1;
        rt_invoke(s_out_callback, 0);
        s_in_callback = 0;
        if (!s_pending) {
            return;
        }
        buf = s_pending_buf;
        words = s_pending_words;
    }
}

void hle_DecDCTinSync(void)
{
    RETURN(0);
}

void hle_DecDCToutSync(void)
{
    RETURN(0);
}

void hle_DecDCTinCallback(void)
{
    uint32_t old = s_in_callback_func;
    s_in_callback_func = GPR[4];
    RETURN(old);
}

void hle_DecDCToutCallback(void)
{
    uint32_t old = s_out_callback;
    s_out_callback = GPR[4];
    RETURN(old);
}

/* DecDCTBufSize(bitstream): the first halfword of the frame's header */
void hle_DecDCTBufSize(void)
{
    RETURN(RD16(GPR[4]));
}

void hle_DecDCTGetEnv(void)
{
    RETURN(GPR[4]);
}

void hle_DecDCTPutEnv(void)
{
    RETURN(GPR[4]);
}
