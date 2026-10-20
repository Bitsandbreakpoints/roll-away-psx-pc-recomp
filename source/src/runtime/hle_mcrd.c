/*
 * hle_mcrd.c - LIBMCRD: the memory card in slot 1, kept in a 128 KB card
 * image file next to the exe (roll_away_card1.mcd, the raw format
 * DuckStation and other emulators use).  Slot 2 has no card.  After the
 * American Pool PC port's libmcrd.c (same author).
 *
 * Card layout (psx-spx): 16 blocks of 8 KB, each 64 frames of 128 bytes.
 * Block 0 holds the header frame ("MC"), 15 directory frames (one per
 * data block: state, file size, next block, file name) and the broken
 * sector list; blocks 1-15 hold the files.  Every system frame ends with
 * the XOR of its first 127 bytes.
 *
 * Everything finishes at once.  As on the console, an asynchronous call
 * (MemCardExist/Accept/ReadFile/WriteFile) made while the previous one is
 * still "busy" (not yet collected by MemCardSync) is refused, once.  A
 * missing file counts as a new formatted card and is created by the first
 * write; a file that is not a card image counts as unformatted.
 */
#include <string.h>
#include "runtime/recomp.h"
#include "runtime/runtime.h"
#include "platform/files.h"
#include "psx/log.h"

#define CARD_FILE               "roll_away_card1.mcd"
#define CARD_SIZE               0x20000
#define FRAME_SIZE              128
#define BLOCK_SIZE              0x2000
#define DIR_FRAMES              15      /* frames 1-15 of block 0 */
#define BROKEN_FRAMES_START     16
#define BROKEN_FRAMES           20
#define WRITE_TEST_FRAME        63
#define NAME_LENGTH             20
#define NO_BLOCK                0xFFFF

/* directory frame: block states */
#define STATE_FREE              0xA0
#define STATE_FIRST             0x51
#define STATE_MIDDLE            0x52
#define STATE_LAST              0x53
#define STATE_DELETED_MASK      0xF0    /* A1h-A3h: deleted, also free */
#define STATE_DELETED           0xA0    /* | 1..3 */

#define CHANNEL_PORT_1          0x00

/* LIBMCRD's results */
enum {
    MC_ERR_NONE = 0,
    MC_ERR_CARD_NOT_EXIST = 1,
    MC_ERR_CARD_INVALID = 2,
    MC_ERR_NEW_CARD = 3,
    MC_ERR_NOT_FORMAT = 4,
    MC_ERR_FILE_NOT_EXIST = 5,
    MC_ERR_ALREADY_EXIST = 6,
    MC_ERR_BLOCK_FULL = 7,
};

/* MemCardSync() command numbers */
enum {
    MC_FUNC_NONE = 0,
    MC_FUNC_EXIST = 1,
    MC_FUNC_ACCEPT = 2,
    MC_FUNC_READ_FILE = 3,
    MC_FUNC_WRITE_FILE = 4,
};

/* struct DIRENTRY: char name[20]; long attr, size; DIRENTRY *next; long head; char system[4] */
#define DIRENTRY_SIZE           40
#define DIRENTRY_ATTR           20
#define DIRENTRY_SIZE_FIELD     24
#define DIRENTRY_HEAD           32

typedef struct {
    uint32_t state;
    uint32_t size;              /* bytes, in the first block of a file */
    uint16_t next;              /* next data block (0..14), or NO_BLOCK */
    char name[NAME_LENGTH + 1];
    uint8_t unused[FRAME_SIZE - 0x1F - 1];
    uint8_t checksum;
} DirFrame;

static uint8_t s_card[CARD_SIZE];
static int s_card_formatted;
static char s_path[1024];

static int s_command;           /* the asynchronous call not yet synced */
static int32_t s_result;
static int s_busy;              /* refuse one call while s_command is busy */

void hle_mcrd_reset(void)
{
    s_command = MC_FUNC_NONE;
    s_busy = 0;
    if (!s_path[0]) {
        file_exe_relative(CARD_FILE, s_path, sizeof(s_path));
    }
}

/* ---- the card image ----------------------------------------------------- */

static uint8_t *frame(int block, int number)
{
    return s_card + block * BLOCK_SIZE + number * FRAME_SIZE;
}

static DirFrame *dir_frame(int data_block)
{
    return (DirFrame *) frame(0, 1 + data_block);
}

static uint8_t *block_data(int data_block)
{
    return s_card + (1 + data_block) * BLOCK_SIZE;
}

static void set_checksum(uint8_t *f)
{
    uint8_t x = 0;
    for (int i = 0; i < FRAME_SIZE - 1; i++) {
        x ^= f[i];
    }
    f[FRAME_SIZE - 1] = x;
}

static void format_card(void)
{
    memset(s_card, 0, sizeof(s_card));
    uint8_t *header = frame(0, 0);
    header[0] = 'M';
    header[1] = 'C';
    set_checksum(header);
    for (int i = 0; i < DIR_FRAMES; i++) {
        DirFrame *d = dir_frame(i);
        d->state = STATE_FREE;
        d->next = NO_BLOCK;
        set_checksum((uint8_t *) d);
    }
    for (int i = 0; i < BROKEN_FRAMES; i++) {
        uint8_t *f = frame(0, BROKEN_FRAMES_START + i);
        memset(f, 0xFF, 4);             /* no broken sector */
        f[8] = f[9] = 0xFF;
        set_checksum(f);
    }
    memcpy(frame(0, WRITE_TEST_FRAME), header, FRAME_SIZE);
    s_card_formatted = 1;
}

/* Reads the card from the file (each time the game checks the card). */
static void load_card(void)
{
    int n = file_read(s_path, s_card, CARD_SIZE);
    if (n < 0) {
        format_card();                  /* no file yet: a new card */
    } else {
        s_card_formatted = n == CARD_SIZE && s_card[0] == 'M' && s_card[1] == 'C';
    }
}

static int save_card(void)
{
    if (!file_write(s_path, s_card, CARD_SIZE)) {
        psx_log("memory card: cannot write %s", s_path);
        return 0;
    }
    return 1;
}

static int block_is_free(int data_block)
{
    return (dir_frame(data_block)->state & STATE_DELETED_MASK) == STATE_FREE;
}

/* a file name from PS1 memory */
static void read_name(uint32_t address, char out[NAME_LENGTH + 1])
{
    int i = 0;
    for (; i < NAME_LENGTH; i++) {
        out[i] = (char) RD8(address + i);
        if (!out[i]) {
            break;
        }
    }
    for (; i <= NAME_LENGTH; i++) {
        out[i] = 0;
    }
}

/* pattern with '?' (any character) and '*' (the rest), as in LIBMCRD */
static int name_matches(const char *pattern, const char *name)
{
    for (int i = 0; i < NAME_LENGTH; i++) {
        if (pattern[i] == '*') {
            return 1;
        }
        if (pattern[i] != '?' && pattern[i] != name[i]) {
            return 0;
        }
        if (pattern[i] == 0) {
            return 1;
        }
    }
    return 1;
}

/* first data block of the file, or -1 */
static int find_file(const char *name)
{
    for (int i = 0; i < DIR_FRAMES; i++) {
        DirFrame *d = dir_frame(i);
        if (d->state == STATE_FIRST && strncmp(d->name, name, NAME_LENGTH) == 0) {
            return i;
        }
    }
    return -1;
}

/* the error for a channel, or MC_ERR_NONE if the card can be used */
static int check_card(uint32_t chan)
{
    if (chan != CHANNEL_PORT_1) {
        return MC_ERR_CARD_NOT_EXIST;
    }
    return s_card_formatted ? MC_ERR_NONE : MC_ERR_NOT_FORMAT;
}

/* Copies between PS1 memory at `adrs` and the file's blocks (ofs/bytes in bytes). */
static int transfer(uint32_t chan, const char *name, uint32_t adrs, int32_t ofs, int32_t bytes,
                    int write)
{
    int error = check_card(chan);
    if (error != MC_ERR_NONE) {
        return error;
    }
    int block = find_file(name);
    if (block < 0) {
        return MC_ERR_FILE_NOT_EXIST;
    }
    while (bytes > 0 && block >= 0) {
        if (ofs < BLOCK_SIZE) {
            int32_t n = BLOCK_SIZE - ofs;
            if (n > bytes) {
                n = bytes;
            }
            for (int32_t i = 0; i < n; i++) {
                if (write) {
                    block_data(block)[ofs + i] = (uint8_t) RD8(adrs + (uint32_t) i);
                } else {
                    WR8(adrs + (uint32_t) i, block_data(block)[ofs + i]);
                }
            }
            adrs += (uint32_t) n;
            bytes -= n;
            ofs = 0;
        } else {
            ofs -= BLOCK_SIZE;
        }
        uint16_t next = dir_frame(block)->next;
        block = next == NO_BLOCK ? -1 : next;
    }
    if (bytes > 0) {
        return MC_ERR_CARD_INVALID;     /* past the end of the file */
    }
    if (write && !save_card()) {
        return MC_ERR_CARD_INVALID;
    }
    return MC_ERR_NONE;
}

/* ---- asynchronous calls ---------------------------------------------------- */

/* A call made while the previous one is busy: refused, and the busy one
 * finishes (its result waits for MemCardSync). */
static int refuse_if_busy(void)
{
    if (s_busy) {
        s_busy = 0;
        RETURN(0);
        return 1;
    }
    return 0;
}

/* Registers a call that has finished with `result`. */
static void start_command(int command, int result)
{
    s_command = command;
    s_result = result;
    s_busy = 1;
    RETURN(1);
}

void hle_MemCardInit(void)
{
    s_command = MC_FUNC_NONE;
    s_busy = 0;
}

void hle_MemCardEnd(void)
{
}

void hle_MemCardStart(void)
{
}

void hle_MemCardStop(void)
{
}

void hle_MemCardClose(void)
{
}

void hle_MemCardCallback(void)
{
    RETURN(0);
}

void hle_MemCardOpen(void)
{
    RETURN(MC_ERR_NONE);
}

/* MemCardExist(chan) */
void hle_MemCardExist(void)
{
    if (!refuse_if_busy()) {
        start_command(MC_FUNC_EXIST, GPR[4] == CHANNEL_PORT_1 ? MC_ERR_NONE
                                                               : MC_ERR_CARD_NOT_EXIST);
    }
}

/* MemCardAccept(chan) */
void hle_MemCardAccept(void)
{
    if (refuse_if_busy()) {
        return;
    }
    if (GPR[4] == CHANNEL_PORT_1) {
        load_card();
    }
    start_command(MC_FUNC_ACCEPT, check_card(GPR[4]));
}

/* MemCardReadFile(chan, file, adrs, ofs, bytes) */
void hle_MemCardReadFile(void)
{
    char name[NAME_LENGTH + 1];
    if (refuse_if_busy()) {
        return;
    }
    read_name(GPR[5], name);
    start_command(MC_FUNC_READ_FILE, transfer(GPR[4], name, GPR[6], (int32_t) GPR[7],
                                              (int32_t) ARG(4), 0));
}

/* MemCardWriteFile(chan, file, adrs, ofs, bytes) */
void hle_MemCardWriteFile(void)
{
    char name[NAME_LENGTH + 1];
    if (refuse_if_busy()) {
        return;
    }
    read_name(GPR[5], name);
    start_command(MC_FUNC_WRITE_FILE, transfer(GPR[4], name, GPR[6], (int32_t) GPR[7],
                                               (int32_t) ARG(4), 1));
}

void hle_MemCardReadData(void)
{
    RETURN(0);
}

void hle_MemCardWriteData(void)
{
    RETURN(0);
}

/* MemCardSync(mode, *cmds, *result): 1 and the registered call's result,
 * -1 if there is none (the calls have always finished) */
void hle_MemCardSync(void)
{
    if (s_command == MC_FUNC_NONE) {
        RETURN(-1);
        return;
    }
    if (GPR[5]) {
        WR32(GPR[5], (uint32_t) s_command);
    }
    if (GPR[6]) {
        WR32(GPR[6], (uint32_t) s_result);
    }
    s_command = MC_FUNC_NONE;
    s_busy = 0;
    RETURN(1);
}

/* ---- synchronous calls ------------------------------------------------------- */

/* MemCardGetDirentry(chan, name, dir, *files, ofs, max) */
void hle_MemCardGetDirentry(void)
{
    char pattern[NAME_LENGTH + 1];
    uint32_t dir = GPR[6], files = GPR[7];
    int32_t ofs = (int32_t) ARG(4), max = (int32_t) ARG(5);
    int error = check_card(GPR[4]);
    if (error != MC_ERR_NONE) {
        RETURN(error);
        return;
    }
    read_name(GPR[5], pattern);
    int32_t found = 0;
    for (int i = 0; i < DIR_FRAMES; i++) {
        DirFrame *d = dir_frame(i);
        if (d->state != STATE_FIRST || !name_matches(pattern, d->name)) {
            continue;
        }
        if (found >= ofs && found - ofs < max) {
            uint32_t e = dir + (uint32_t) (found - ofs) * DIRENTRY_SIZE;
            for (int k = 0; k < DIRENTRY_SIZE; k++) {
                WR8(e + k, k < NAME_LENGTH ? (uint8_t) d->name[k] : 0);
            }
            WR32(e + DIRENTRY_ATTR, d->state);
            WR32(e + DIRENTRY_SIZE_FIELD, d->size);
            WR32(e + DIRENTRY_HEAD, (uint32_t) i);
        }
        found++;
    }
    found = found > ofs ? found - ofs : 0;
    if (files) {
        WR32(files, (uint32_t) (found < max ? found : max));
    }
    RETURN(MC_ERR_NONE);
}

/* MemCardCreateFile(chan, file, blocks) */
void hle_MemCardCreateFile(void)
{
    char name[NAME_LENGTH + 1];
    int32_t blocks = (int32_t) GPR[6];
    int error = check_card(GPR[4]);
    if (error != MC_ERR_NONE) {
        RETURN(error);
        return;
    }
    read_name(GPR[5], name);
    if (find_file(name) >= 0) {
        RETURN(MC_ERR_ALREADY_EXIST);
        return;
    }
    int list[DIR_FRAMES];
    int count = 0;
    for (int i = 0; i < DIR_FRAMES && count < blocks; i++) {
        if (block_is_free(i)) {
            list[count++] = i;
        }
    }
    if (blocks < 1 || count < blocks) {
        RETURN(MC_ERR_BLOCK_FULL);
        return;
    }
    for (int i = 0; i < count; i++) {
        DirFrame *d = dir_frame(list[i]);
        memset(d, 0, FRAME_SIZE);
        d->state = i == 0 ? STATE_FIRST : i == count - 1 ? STATE_LAST : STATE_MIDDLE;
        d->next = i == count - 1 ? NO_BLOCK : (uint16_t) list[i + 1];
        if (i == 0) {
            d->size = (uint32_t) (blocks * BLOCK_SIZE);
            memcpy(d->name, name, NAME_LENGTH);
        }
        set_checksum((uint8_t *) d);
        memset(block_data(list[i]), 0, BLOCK_SIZE);
    }
    RETURN(save_card() ? MC_ERR_NONE : MC_ERR_CARD_INVALID);
}

/* MemCardDeleteFile(chan, file) */
void hle_MemCardDeleteFile(void)
{
    char name[NAME_LENGTH + 1];
    int error = check_card(GPR[4]);
    if (error != MC_ERR_NONE) {
        RETURN(error);
        return;
    }
    read_name(GPR[5], name);
    int block = find_file(name);
    if (block < 0) {
        RETURN(MC_ERR_FILE_NOT_EXIST);
        return;
    }
    while (block >= 0) {
        DirFrame *d = dir_frame(block);
        uint16_t next = d->next;
        d->state = STATE_DELETED | (d->state & 0x0F);
        set_checksum((uint8_t *) d);
        block = next == NO_BLOCK ? -1 : next;
    }
    RETURN(save_card() ? MC_ERR_NONE : MC_ERR_CARD_INVALID);
}

/* MemCardFormat(chan) */
void hle_MemCardFormat(void)
{
    if (GPR[4] != CHANNEL_PORT_1) {
        RETURN(MC_ERR_CARD_NOT_EXIST);
        return;
    }
    format_card();
    RETURN(save_card() ? MC_ERR_NONE : MC_ERR_CARD_INVALID);
}

/* MemCardUnformat(chan) */
void hle_MemCardUnformat(void)
{
    if (GPR[4] != CHANNEL_PORT_1) {
        RETURN(MC_ERR_CARD_NOT_EXIST);
        return;
    }
    memset(s_card, 0, sizeof(s_card));
    s_card_formatted = 0;
    RETURN(save_card() ? MC_ERR_NONE : MC_ERR_CARD_INVALID);
}
