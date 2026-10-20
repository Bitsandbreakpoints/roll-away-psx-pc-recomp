/*
 * hle_pad.c - the controllers: the BIOS's InitPAD()/StartPAD() (LIBAPI),
 * which Roll Away uses, and LIBPAD's PadInitDirect().
 *
 * As on the PS1, the replies of both controllers are written into the
 * game's buffers at every vertical blank: status
 * byte (0 = connected), type (0x41: digital pad) and the 16 button bits,
 * active low.  The buttons come from the PC side (psx_set_pad_reader():
 * keyboard / XInput after the settings).  Vibration is not supported.
 */
#include "runtime/recomp.h"
#include "runtime/runtime.h"
#include "runtime/system.h"
#include "runtime/irq.h"

#define PAD_PORTS               2
#define PAD_STATUS_OK           0x00
#define PAD_STATUS_NONE         0xFF
#define PAD_TYPE_DIGITAL        0x41    /* 4 bits: type, 4 bits: half-words of data */

static uint32_t s_buffers[PAD_PORTS];
static int s_running;
static PsxPadReader s_reader;

void psx_set_pad_reader(PsxPadReader reader)
{
    s_reader = reader;
}

void pad_reset(void)
{
    s_buffers[0] = s_buffers[1] = 0;
    s_running = 0;
}

static void write_reply(uint32_t buffer, int buttons)
{
    if (buttons < 0) {
        WR8(buffer, PAD_STATUS_NONE);
        WR8(buffer + 2, 0xFF);
        WR8(buffer + 3, 0xFF);
    } else {
        WR8(buffer, PAD_STATUS_OK);
        WR8(buffer + 1, PAD_TYPE_DIGITAL);
        WR8(buffer + 2, (uint8_t) ~buttons);
        WR8(buffer + 3, (uint8_t) (~buttons >> 8));
    }
}

/* at every vertical blank */
void pad_update(void)
{
    if (!s_running) {
        return;
    }
    for (int port = 0; port < PAD_PORTS; port++) {
        if (s_buffers[port]) {
            write_reply(s_buffers[port], s_reader ? s_reader(port) : -1);
        }
    }
}

void hle_PadInitDirect(void)
{
    s_buffers[0] = GPR[4];
    s_buffers[1] = GPR[5];
    for (int port = 0; port < PAD_PORTS; port++) {
        if (s_buffers[port]) {
            write_reply(s_buffers[port], -1);
        }
    }
    RETURN(1);
}

void hle_PadStartCom(void)
{
    s_running = 1;
    RETURN(1);
}

void hle_PadStopCom(void)
{
    s_running = 0;
    RETURN(1);
}

/* ---- the BIOS's controller functions (LIBAPI PAD.OBJ / PATCH.OBJ) ------- */

/* InitPAD(buf1, size1, buf2, size2): the same replies as PadInitDirect() */
void hle_InitPAD(void)
{
    s_buffers[0] = GPR[4];
    s_buffers[1] = GPR[6];
    for (int port = 0; port < PAD_PORTS; port++) {
        if (s_buffers[port]) {
            write_reply(s_buffers[port], -1);
        }
    }
    RETURN(1);
}

void hle_PAD_init(void)
{
    hle_InitPAD();
}

void hle_StartPAD(void)
{
    s_running = 1;
    RETURN(1);
}

/* (these patch the BIOS's code, which the port does not have) */
void hle_EnablePAD(void)
{
}

void hle_DisablePAD(void)
{
}

void hle__patch_pad(void)
{
}

void hle__remove_ChgclrPAD(void)
{
}

void hle_StopPAD(void)
{
    s_running = 0;
}

/* ---- the rest of LIBPAD: a digital pad, no actuators (no vibration) ------- */

#define PAD_STATE_DISCONNECTED  0       /* PadStateDiscon */
#define PAD_STATE_FIND_CTP1     2       /* PadStateFindCTP1: a pad without modes */

static int port_index(uint32_t port)
{
    return (port >> 4) & 1;
}

/* PadGetState(port) */
void hle_PadGetState(void)
{
    int connected = s_reader && s_reader(port_index(GPR[4])) >= 0;
    RETURN(connected ? PAD_STATE_FIND_CTP1 : PAD_STATE_DISCONNECTED);
}

void hle_PadInfoMode(void)
{
    RETURN(0);
}

void hle_PadInfoAct(void)
{
    RETURN(0);
}

void hle_PadInfoComb(void)
{
    RETURN(0);
}

void hle_PadSetActAlign(void)
{
    RETURN(0);
}

void hle_PadSetMainMode(void)
{
    RETURN(0);
}

void hle_PadSetAct(void)
{
}

void hle_PadChkVsync(void)
{
    RETURN(0);
}

void hle_PadChkMtap(void)
{
    RETURN(0);
}

void hle_PadEnableCom(void)
{
}
