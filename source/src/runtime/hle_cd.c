/*
 * hle_cd.c - LIBCD: the CD-ROM drive, read from the player's disc image
 * (src/disc/).
 *
 * Plain reads (CdRead) finish at once: the sectors are copied into PS1
 * memory and CdReadSync() reports completion.
 *
 * Continuous reading - CdlReadS / CdlReadN for the XA music, CdRead2()
 * for the movies - goes on in real time as on the console: 150 sectors a
 * second at double speed, 75 at single speed, counted from when it
 * started.  The sectors due are read at every vertical blank (cd_tick())
 * and whenever the game asks for a movie frame (StGetNext()):
 *  - XA audio sectors (in real-time mode, CdlModeRT) go to the XA decoder
 *    (psx/xa.c), as the drive plays them itself; with CdlModeSF only the
 *    file and channel of CdlSetfilter;
 *  - movie sectors (32-byte STR header + 2016 bytes) are put together into
 *    whole frames, which StGetNext() hands out in the game's ring buffer
 *    (after the D PC port's stream.c, same author).
 * CdlGetlocL / CdlGetlocP report the sector read last, so the game can
 * loop its music before the end of a track.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "runtime/recomp.h"
#include "runtime/runtime.h"
#include "runtime/irq.h"
#include "disc/disc.h"
#include "runtime/custom_level.h"
#include "psx/xa.h"
#include "psx/log.h"

/* sectors before LBA 0: the 2 second pregap (00:02:00) */
#define PREGAP_SECTORS          150
#define SECTORS_PER_SECOND      75

/* CdControl() commands */
#define CDL_NOP                 0x01
#define CDL_SETLOC              0x02
#define CDL_PLAY                0x03
#define CDL_READN               0x06
#define CDL_STANDBY             0x07
#define CDL_STOP                0x08
#define CDL_PAUSE               0x09
#define CDL_INIT                0x0A
#define CDL_MUTE                0x0B
#define CDL_DEMUTE              0x0C
#define CDL_SETFILTER           0x0D
#define CDL_SETMODE             0x0E
#define CDL_GETPARAM            0x0F
#define CDL_GETLOCL             0x10
#define CDL_GETLOCP             0x11
#define CDL_GETTN               0x13
#define CDL_GETTD               0x14
#define CDL_SEEKL               0x15
#define CDL_SEEKP               0x16
#define CDL_READS               0x1B

/* CdlSetmode bits */
#define MODE_SPEED              0x80    /* double speed */
#define MODE_RT                 0x40    /* XA audio sectors are played */
#define MODE_SIZE_MASK          0x30    /* 2340-byte sectors */
#define MODE_SF                 0x08    /* only the file/channel of CdlSetfilter */

/* status byte */
#define STAT_MOTOR_ON           0x02
#define STAT_READING            0x20

/* CdSync() / CdReady() results */
#define CDL_NO_INTR             0
#define CDL_COMPLETE            2

/* CdlFILE: CdlLOC pos (4 bytes), u_long size, char name[16] */
#define FILE_POS                0
#define FILE_SIZE               4
#define FILE_NAME               8
#define FILE_NAME_SIZE          16

/* a whole sector: the header (minute, second, sector, mode) and the XA
 * subheader (file, channel, submode, coding) */
#define SECTOR_HEADER           12
#define SECTOR_SUBHEADER        16
#define SUBHEADER_FILE          16
#define SUBHEADER_CHANNEL       17
#define SUBHEADER_SUBMODE       18
#define SUBMODE_VIDEO           0x02
#define SUBMODE_AUDIO           0x04
#define SUBMODE_DATA            0x08
#define SECTOR_DATA             24

/* a movie sector: the STR header, then the frame's data */
#define STR_HEADER_SIZE         32
#define STR_DATA_SIZE           (DISC_SECTOR_DATA - STR_HEADER_SIZE)
#define STR_SECTOR_ID           0x0160
#define MAX_FRAME_SECTORS       32
#define FRAME_QUEUE             3       /* complete frames waiting for the game */
#define RING_FRAME_DATA         64      /* where a frame's data starts in the ring */

#define SECTORS_PER_SECOND_DOUBLE   150
#define MAX_CATCH_UP_SECTORS        75  /* after a pause, do not read more at once */
#define PAUSE_US                    250000  /* no tick for this long: the game was paused */
#define FRAME_US                    16683
#define US_PER_SECOND               1000000ull

/* the STR sector header (the game reads it from the ring: StGetNext) */
typedef struct {
    uint16_t id;
    uint16_t type;
    uint16_t sector_index;
    uint16_t sector_count;
    uint32_t frame_number;
    uint32_t frame_size;
    uint16_t width, height;
    uint8_t rest[STR_HEADER_SIZE - 20];
} StrHeader;

typedef struct {
    StrHeader header;
    int sectors_seen;
    uint8_t data[MAX_FRAME_SECTORS * STR_DATA_SIZE];
} Frame;

int g_cd_trace;                         /* (debugging: RA_CD_TRACE logs the commands) */
static int s_position;                  /* LBA of CdlSetloc */
static int s_mode;
static int s_filter_file, s_filter_channel;
static int s_muted;
static uint32_t s_sync_callback, s_ready_callback, s_read_callback;

/* continuous reading */
static int s_reading;
static int s_next_lba;
static uint64_t s_read_start_us;
static uint32_t s_sectors_read;
static uint64_t s_last_advance_us;
static int s_last_lba;
static uint8_t s_last_header[8];        /* CdlGetlocL: header + subheader */

/* the movies (St*) */
static uint32_t s_ring, s_ring_bytes;   /* StSetRing() */
static Frame s_assembling;
static int s_assembling_active;
static Frame s_queue[FRAME_QUEUE];
static int s_queue_read, s_queue_count;

static int bcd_to_int(uint8_t bcd)
{
    return (bcd >> 4) * 10 + (bcd & 0xF);
}

static uint8_t int_to_bcd(int value)
{
    return (uint8_t) ((value / 10) << 4 | value % 10);
}

static int pos_to_int(uint32_t p)
{
    return (bcd_to_int((uint8_t) RD8(p)) * 60 + bcd_to_int((uint8_t) RD8(p + 1)))
               * SECTORS_PER_SECOND
           + bcd_to_int((uint8_t) RD8(p + 2)) - PREGAP_SECTORS;
}

static void int_to_pos(int lba, uint32_t p)
{
    lba += PREGAP_SECTORS;
    WR8(p, int_to_bcd(lba / (SECTORS_PER_SECOND * 60)));
    WR8(p + 1, int_to_bcd(lba / SECTORS_PER_SECOND % 60));
    WR8(p + 2, int_to_bcd(lba % SECTORS_PER_SECOND));
}

static void clear_frames(void)
{
    s_queue_count = 0;
    s_assembling_active = 0;
}

void hle_cd_reset(void)
{
    g_cd_trace = GetEnvironmentVariableA("RA_CD_TRACE", NULL, 0) != 0;
    s_position = 0;
    s_mode = 0;
    s_filter_file = s_filter_channel = 0;
    s_muted = 0;
    s_sync_callback = s_ready_callback = s_read_callback = 0;
    s_reading = 0;
    s_last_lba = 0;
    memset(s_last_header, 0, sizeof(s_last_header));
    s_ring = s_ring_bytes = 0;
    clear_frames();
    xa_reset();
}

/* ---- continuous reading -------------------------------------------------- */

static int sectors_per_second(void)
{
    return s_mode & MODE_SPEED ? SECTORS_PER_SECOND_DOUBLE : SECTORS_PER_SECOND;
}

static void start_reading(int lba)
{
    s_reading = 1;
    s_next_lba = lba;
    s_sectors_read = 0;
    s_read_start_us = s_last_advance_us = irq_time_us();
    clear_frames();
    xa_reset();
}

static void stop_reading(void)
{
    s_reading = 0;
    xa_reset();
}

/* A movie sector: into the frame being put together. */
static void video_sector(const uint8_t *sector)
{
    const StrHeader *h = (const StrHeader *) (sector + SECTOR_DATA);
    if (h->id != STR_SECTOR_ID || h->sector_count == 0 || h->sector_count > MAX_FRAME_SECTORS
        || h->sector_index >= h->sector_count) {
        return;
    }
    if (!s_assembling_active || s_assembling.header.frame_number != h->frame_number) {
        memset(&s_assembling, 0, sizeof(s_assembling));
        s_assembling.header = *h;
        s_assembling_active = 1;
    }
    memcpy(s_assembling.data + h->sector_index * STR_DATA_SIZE,
           sector + SECTOR_DATA + STR_HEADER_SIZE, STR_DATA_SIZE);
    if (++s_assembling.sectors_seen < h->sector_count) {
        return;
    }
    s_assembling_active = 0;
    if (s_queue_count == FRAME_QUEUE) {
        return;                         /* the game is behind: the frame is lost */
    }
    s_queue[(s_queue_read + s_queue_count) % FRAME_QUEUE] = s_assembling;
    s_queue_count++;
}

static int audio_wanted(const uint8_t *sector)
{
    if (!(s_mode & MODE_RT) || s_muted) {
        return 0;
    }
    return !(s_mode & MODE_SF) || (sector[SUBHEADER_FILE] == s_filter_file
                                   && sector[SUBHEADER_CHANNEL] == s_filter_channel);
}

/* Reads the sectors due by now. */
static void advance(void)
{
    uint8_t sector[DISC_RAW_SECTOR];
    if (!s_reading) {
        return;
    }
    /* (the drive's clock stands still while the game is paused: F1 menu) */
    uint64_t now = irq_time_us();
    if (now - s_last_advance_us > PAUSE_US) {
        s_read_start_us += now - s_last_advance_us - FRAME_US;
    }
    s_last_advance_us = now;
    uint32_t due = (uint32_t) ((now - s_read_start_us) * sectors_per_second()
                               / US_PER_SECOND);
    if (due > s_sectors_read + MAX_CATCH_UP_SECTORS) {
        s_sectors_read = due - MAX_CATCH_UP_SECTORS;    /* (the game was paused) */
    }
    while (s_reading && s_sectors_read < due) {
        s_sectors_read++;
        if (!disc_read_raw((uint32_t) s_next_lba, sector)) {
            psx_log("cd: reading stopped at the end of the disc (%d)", s_next_lba);
            s_reading = 0;
            return;
        }
        s_last_lba = s_next_lba++;
        memcpy(s_last_header, sector + SECTOR_HEADER, 4);
        memcpy(s_last_header + 4, sector + SECTOR_SUBHEADER, 4);
        uint8_t submode = sector[SUBHEADER_SUBMODE];
        if (submode & SUBMODE_AUDIO) {
            if (audio_wanted(sector)) {
                xa_decode_sector(sector);
            }
        } else if (s_ring && (submode & (SUBMODE_VIDEO | SUBMODE_DATA))) {
            video_sector(sector);
        }
    }
}

/* every vertical blank (irq.c) */
void cd_tick(void)
{
    advance();
}

/* The drive's XA sound, added to the SPU's (irq.c): volume 0..256. */
void cd_mix_audio(int16_t *out, int frames, int volume_left, int volume_right)
{
    xa_mix(out, frames, volume_left, volume_right);
}

/* ---- CdControl ------------------------------------------------------------ */

static void control(void)
{
    uint32_t com = GPR[4] & 0xFF, param = GPR[5], result = GPR[6];
    if (g_cd_trace) {
        psx_log("CdControl(%02X, %02X %02X %02X %02X) ra %08X", com, param ? RD8(param) : 0,
                param ? RD8(param + 1) : 0, param ? RD8(param + 2) : 0,
                param ? RD8(param + 3) : 0, GPR[31]);
    }
    switch (com) {
    case CDL_SETLOC:
        if (param) {
            s_position = pos_to_int(param);
        }
        break;
    case CDL_SETMODE:
        if (param) {
            s_mode = RD8(param);
        }
        break;
    case CDL_SETFILTER:
        if (param) {
            s_filter_file = RD8(param);
            s_filter_channel = RD8(param + 1);
        }
        break;
    case CDL_READN:
    case CDL_READS:
        if (param) {
            s_position = pos_to_int(param);
        }
        start_reading(s_position);
        psx_log("cd: reading from %d (mode %02X, filter %d/%d)", s_position, s_mode,
                s_filter_file, s_filter_channel);
        break;
    case CDL_SEEKL:
    case CDL_SEEKP:
        if (param) {
            s_position = pos_to_int(param);     /* (the game seeks without CdlSetloc) */
        }
        stop_reading();
        s_last_lba = s_position;
        break;
    case CDL_STOP:
    case CDL_PAUSE:
    case CDL_STANDBY:
        stop_reading();
        break;
    case CDL_INIT:
        stop_reading();
        s_mode = 0;
        s_muted = 0;
        break;
    case CDL_MUTE:
        s_muted = 1;
        xa_reset();
        break;
    case CDL_DEMUTE:
        s_muted = 0;
        break;
    case CDL_GETLOCL:
        if (result) {
            for (int i = 0; i < 8; i++) {
                WR8(result + i, s_last_header[i]);
            }
        }
        break;
    case CDL_GETLOCP:
        if (result) {
            WR8(result, 1);             /* track 1, index 1 */
            WR8(result + 1, 1);
            int_to_pos(s_last_lba - PREGAP_SECTORS, result + 2);    /* (relative) */
            int_to_pos(s_last_lba, result + 5);
        }
        break;
    case CDL_GETTN:
        if (result) {
            WR8(result + 1, 1);
            WR8(result + 2, 1);         /* one track */
        }
        break;
    case CDL_GETTD:
        if (result) {
            int end = (int) disc_sector_count();
            WR8(result + 1, int_to_bcd((end + PREGAP_SECTORS) / (SECTORS_PER_SECOND * 60)));
            WR8(result + 2, int_to_bcd((end + PREGAP_SECTORS) / SECTORS_PER_SECOND % 60));
        }
        break;
    case CDL_NOP:
    case CDL_GETPARAM:
        break;
    case CDL_PLAY:
        psx_log("CdControl: CdlPlay (CD audio) ignored: the disc has no audio tracks");
        break;
    default:
        psx_log("CdControl(%u) ignored", com);
        break;
    }
    if (result && com != CDL_GETLOCL && com != CDL_GETLOCP && com != CDL_GETTN
        && com != CDL_GETTD) {
        WR8(result, STAT_MOTOR_ON | (s_reading ? STAT_READING : 0));
    }
    RETURN(1);
}

void hle_CdControl(void)
{
    control();
}

void hle_CdControlF(void)
{
    control();
}

void hle_CdControlB(void)
{
    control();
}

/* ---- the rest of LIBCD ------------------------------------------------------ */

void hle_CdInit(void)
{
    hle_cd_reset();
    RETURN(1);
}

void hle_CdReset(void)
{
    stop_reading();
    s_mode = 0;
    RETURN(1);
}

void hle_CdFlush(void)
{
}

void hle_CdSetDebug(void)
{
    RETURN(0);
}

void hle_CdStatus(void)
{
    RETURN(STAT_MOTOR_ON | (s_reading ? STAT_READING : 0));
}

void hle_CdMode(void)
{
    RETURN(s_mode);
}

void hle_CdLastCom(void)
{
    RETURN(0);
}

void hle_CdLastPos(void)
{
    RETURN(0);
}

void hle_CdMix(void)
{
    RETURN(1);
}

/* CdSync(): every command finishes at once */
void hle_CdSync(void)
{
    if (GPR[5]) {
        WR8(GPR[5], STAT_MOTOR_ON | (s_reading ? STAT_READING : 0));
    }
    RETURN(CDL_COMPLETE);
}

/* CdReady(): no sector waits for the game (the movies come through St*) */
void hle_CdReady(void)
{
    RETURN(CDL_NO_INTR);
}

static uint32_t swap(uint32_t *slot, uint32_t value)
{
    uint32_t old = *slot;
    *slot = value;
    return old;
}

void hle_CdSyncCallback(void)
{
    RETURN(swap(&s_sync_callback, GPR[4]));
}

void hle_CdReadyCallback(void)
{
    RETURN(swap(&s_ready_callback, GPR[4]));
}

void hle_CdReadCallback(void)
{
    RETURN(swap(&s_read_callback, GPR[4]));
}

void hle_CdDataCallback(void)
{
    RETURN(0);
}

void hle_CdDataSync(void)
{
    RETURN(0);
}

void hle_CdReadMode(void)
{
    RETURN(0);
}

void hle_CdGetSector(void)
{
    psx_log("CdGetSector(%08X, %u): no sector is buffered", GPR[4], GPR[5]);
    RETURN(0);
}

void hle_CdGetSector2(void)
{
    psx_log("CdGetSector2(%08X, %u): no sector is buffered", GPR[4], GPR[5]);
    RETURN(0);
}

/* CdRead(sectors, buf, mode): the sectors from CdlSetloc on, at once */
void hle_CdRead(void)
{
    int sectors = (int) GPR[4];
    uint32_t buf = GPR[5];
    int mode = (int) GPR[6];
    stop_reading();                     /* (the drive reads data now: the music stops) */
    s_mode = mode;
    if (mode & MODE_SIZE_MASK) {
        psx_log("CdRead: mode %02X (2340-byte sectors) is not supported", mode);
    }
    for (int i = 0; i < sectors; i++) {
        void *p = rt_pointer(buf + (uint32_t) i * DISC_SECTOR_DATA);
        if (!p || (!custom_level_read_sector((uint32_t) s_position, p)
                   && !disc_read_sectors((uint32_t) s_position, 1, p))) {
            psx_log("CdRead: cannot read sector %d to %08X", s_position, buf);
            RETURN(0);
            return;
        }
        s_last_lba = s_position++;
    }
    RETURN(1);
}

void hle_CdReadSync(void)
{
    if (GPR[5]) {
        WR8(GPR[5], STAT_MOTOR_ON);
    }
    RETURN(0);                          /* no sectors left: done */
}

void hle_CdReadBreak(void)
{
    RETURN(0);
}

/* CdRead2(mode): continuous reading from CdlSetloc on (the movies) */
void hle_CdRead2(void)
{
    s_mode = (int) (GPR[4] & 0xFF);
    start_reading(s_position);
    psx_log("CdRead2(%02X) from %d", s_mode, s_position);
    RETURN(1);
}

void hle_CdSearchFile(void)
{
    uint32_t fp = GPR[4], name_address = GPR[5];
    char name[64];
    uint32_t lba, size;
    int n = 0;
    while (n < (int) sizeof(name) - 1) {
        char c = (char) RD8(name_address + n);
        if (!c) {
            break;
        }
        name[n++] = c;
    }
    name[n] = 0;
    if (!disc_find_file(name, &lba, &size)) {
        psx_log("CdSearchFile: %s not found", name);
        RETURN(0);
        return;
    }
    int_to_pos((int) lba, fp + FILE_POS);
    WR8(fp + FILE_POS + 3, 0);
    WR32(fp + FILE_SIZE, size);
    /* the name without the directories */
    const char *base = name;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == 92) {
            base = p + 1;
        }
    }
    for (int i = 0; i < FILE_NAME_SIZE; i++) {
        WR8(fp + FILE_NAME + i, i < (int) strlen(base) ? (uint8_t) base[i] : 0);
    }
    RETURN(fp);
}

/* LIBDS's callbacks (linked in, not used by the game) */
void hle_DsSyncCallback(void)
{
    RETURN(0);
}

void hle_DsReadyCallback(void)
{
    RETURN(0);
}

void hle_DsStartCallback(void)
{
    RETURN(0);
}

void hle_DsDataCallback(void)
{
    RETURN(0);
}

/* ---- LIBCD's streaming functions (the movies) ------------------------------ */

/* StSetRing(ring, sectors) */
void hle_StSetRing(void)
{
    s_ring = GPR[4];
    s_ring_bytes = GPR[5] * DISC_SECTOR_DATA;
    clear_frames();
}

void hle_StUnSetRing(void)
{
    s_ring = s_ring_bytes = 0;
    clear_frames();
}

void hle_StClearRing(void)
{
    clear_frames();
}

/* StSetStream(mode, start frame, end frame, func1, func2) */
void hle_StSetStream(void)
{
    clear_frames();
}

void hle_StSetMask(void)
{
}

/* StGetNext(&addr, &header): 0 and the oldest whole frame, or 1 (none yet) */
void hle_StGetNext(void)
{
    uint32_t addr_out = GPR[4], header_out = GPR[5];
    advance();
    if (s_queue_count == 0) {
        if (s_reading) {
            irq_idle();                 /* (the next sectors are on their way) */
        }
        RETURN(1);
        return;
    }
    Frame *f = &s_queue[s_queue_read];
    uint32_t bytes = f->header.sector_count * STR_DATA_SIZE;
    s_queue_read = (s_queue_read + 1) % FRAME_QUEUE;
    s_queue_count--;
    uint8_t *ring = (uint8_t *) rt_pointer(s_ring);
    if (!ring || RING_FRAME_DATA + bytes > s_ring_bytes) {
        psx_log("StGetNext: no ring buffer or the frame does not fit");
        RETURN(1);
        return;
    }
    memcpy(ring, &f->header, sizeof(StrHeader));
    memcpy(ring + RING_FRAME_DATA, f->data, bytes);
    WR32(header_out, s_ring);
    WR32(addr_out, s_ring + RING_FRAME_DATA);
    RETURN(0);
}

void hle_StFreeRing(void)
{
    RETURN(0);
}

/* StGetBackloc(&loc): where reading would go on */
void hle_StGetBackloc(void)
{
    int_to_pos(s_reading ? s_next_lba : s_position, GPR[4]);
    RETURN(0);
}

/* (LIBCD's own sector interrupt and its helpers: the reading above does it) */
void hle_StCdInterrupt(void)
{
}

void hle_data_ready_callback(void)
{
}

void hle_init_ring_status(void)
{
}
