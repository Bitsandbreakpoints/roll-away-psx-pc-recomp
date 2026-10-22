/*
 * disc.cpp - reading the player's disc image: raw 2352-byte sectors (.bin,
 * also through a .cue), 2048-byte ones (.iso), and compressed MAME CHD
 * images (libchdr); ISO9660 directories; the check that it is a version of
 * the game (game_check.c compares its executable with the recompiled ones).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "disc/disc.h"
#include "disc/sha1.h"
#include "libchdr/chd.h"
#include "libchdr/cdrom.h"

#define RAW_SECTOR              2352
#define PVD_LBA                 16

enum ImageKind {
    KIND_NONE,
    KIND_RAW,                       /* .bin: 2352-byte sectors */
    KIND_ISO,                       /* .iso: 2048-byte sectors */
    KIND_CHD                        /* CHD: frames of 2352 + 96 bytes of subcode */
};

static int s_kind = KIND_NONE;
static uint32_t s_data_offset;          /* user data within a raw sector */
static uint32_t s_sectors;

/* .bin / .iso */
static HANDLE s_file = INVALID_HANDLE_VALUE;

/* .chd */
static FILE *s_chd_stdio;
static chd_file *s_chd;
static uint8_t *s_hunk;                 /* the last hunk read */
static uint32_t s_hunk_number;
static uint32_t s_frames_per_hunk;

static void utf8_to_wide(const char *in, wchar_t *out, int size)
{
    MultiByteToWideChar(CP_UTF8, 0, in, -1, out, size);
}

static HANDLE open_utf8(const char *path)
{
    wchar_t wpath[MAX_PATH];
    utf8_to_wide(path, wpath, MAX_PATH);
    return CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, NULL);
}

static int read_at(uint64_t offset, void *out, uint32_t size)
{
    LARGE_INTEGER pos;
    DWORD got = 0;
    pos.QuadPart = (LONGLONG) offset;
    return SetFilePointerEx(s_file, pos, NULL, FILE_BEGIN)
           && ReadFile(s_file, out, size, &got, NULL) && got == size;
}

static int has_extension(const char *path, const char *ext)
{
    size_t n = strlen(path), e = strlen(ext);
    return n > e && _stricmp(path + n - e, ext) == 0;
}

/* A .cue: the image is the first FILE "name" BINARY next to it. */
static int bin_from_cue(const char *cue, char *bin, int size)
{
    HANDLE f = open_utf8(cue);
    char text[4096];
    DWORD got = 0;
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ReadFile(f, text, sizeof(text) - 1, &got, NULL);
    CloseHandle(f);
    text[got] = '\0';
    const char *p = strstr(text, "FILE \"");
    if (!p) {
        return 0;
    }
    p += 6;
    const char *end = strchr(p, '"');
    if (!end) {
        return 0;
    }
    const char *slash = strrchr(cue, '\\');
    const char *slash2 = strrchr(cue, '/');
    if (!slash || (slash2 && slash2 > slash)) {
        slash = slash2;
    }
    int dir_len = slash ? (int) (slash - cue + 1) : 0;
    int name_len = (int) (end - p);
    if (dir_len + name_len + 1 > size) {
        return 0;
    }
    memcpy(bin, cue, dir_len);
    memcpy(bin + dir_len, p, name_len);
    bin[dir_len + name_len] = '\0';
    return 1;
}

static int open_raw_or_iso(const char *path)
{
    LARGE_INTEGER size;
    uint8_t sector[RAW_SECTOR];
    static const uint8_t SYNC[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };

    s_file = open_utf8(path);
    if (s_file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    GetFileSizeEx(s_file, &size);
    if (size.QuadPart % RAW_SECTOR == 0 && size.QuadPart >= (PVD_LBA + 1) * RAW_SECTOR
        && read_at((uint64_t) PVD_LBA * RAW_SECTOR, sector, RAW_SECTOR)
        && memcmp(sector, SYNC, sizeof(SYNC)) == 0) {
        s_kind = KIND_RAW;
        s_data_offset = sector[15] == 2 ? 24 : 16;      /* Mode 2 Form 1 / Mode 1 */
        s_sectors = (uint32_t) (size.QuadPart / RAW_SECTOR);
    } else if (size.QuadPart % DISC_SECTOR_DATA == 0) {
        s_kind = KIND_ISO;
        s_data_offset = 0;
        s_sectors = (uint32_t) (size.QuadPart / DISC_SECTOR_DATA);
    } else {
        return 0;
    }
    return 1;
}

/* First track of a CD CHD: its type decides where the user data is. */
static int open_chd(const char *path)
{
    wchar_t wpath[MAX_PATH];
    char meta[256];
    uint32_t len;
    int track = 0, frames = 0;
    char type[32] = "";

    utf8_to_wide(path, wpath, MAX_PATH);
    s_chd_stdio = _wfopen(wpath, L"rb");
    if (!s_chd_stdio || chd_open_file(s_chd_stdio, CHD_OPEN_READ, NULL, &s_chd) != CHDERR_NONE) {
        return 0;
    }
    const chd_header *header = chd_get_header(s_chd);
    if (header->hunkbytes % CD_FRAME_SIZE != 0) {
        return 0;                       /* not a CD image */
    }
    if (chd_get_metadata(s_chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof(meta) - 1, &len, NULL,
                         NULL) == CHDERR_NONE
        || chd_get_metadata(s_chd, CDROM_TRACK_METADATA_TAG, 0, meta, sizeof(meta) - 1, &len,
                            NULL, NULL) == CHDERR_NONE) {
        meta[len < sizeof(meta) ? len : sizeof(meta) - 1] = '\0';
        sscanf(meta, "TRACK:%d TYPE:%31s SUBTYPE:%*s FRAMES:%d", &track, type, &frames);
    }
    if (strcmp(type, "MODE2_RAW") == 0) {
        s_data_offset = 24;
    } else if (strcmp(type, "MODE1_RAW") == 0) {
        s_data_offset = 16;
    } else if (strcmp(type, "MODE1") == 0 || strcmp(type, "MODE2_FORM1") == 0) {
        s_data_offset = 0;              /* stored as 2048-byte user data */
    } else {
        return 0;                       /* audio or unknown */
    }
    s_frames_per_hunk = header->hunkbytes / CD_FRAME_SIZE;
    s_sectors = frames > 0 ? (uint32_t) frames : (uint32_t) (header->logicalbytes / CD_FRAME_SIZE);
    s_hunk = (uint8_t *) malloc(header->hunkbytes);
    s_hunk_number = 0xFFFFFFFF;
    s_kind = KIND_CHD;
    return s_hunk != NULL;
}

int disc_open(const char *path)
{
    char bin[MAX_PATH * 3];
    disc_close();
    if (has_extension(path, ".cue")) {
        if (!bin_from_cue(path, bin, sizeof(bin))) {
            return 0;
        }
        path = bin;
    }
    if (has_extension(path, ".chd") ? open_chd(path) : open_raw_or_iso(path)) {
        return 1;
    }
    disc_close();
    return 0;
}

void disc_close(void)
{
    if (s_file != INVALID_HANDLE_VALUE) {
        CloseHandle(s_file);
        s_file = INVALID_HANDLE_VALUE;
    }
    if (s_chd) {
        chd_close(s_chd);
        s_chd = NULL;
    }
    if (s_chd_stdio) {
        fclose(s_chd_stdio);
        s_chd_stdio = NULL;
    }
    free(s_hunk);
    s_hunk = NULL;
    s_kind = KIND_NONE;
    s_sectors = 0;
}

uint32_t disc_sector_count(void)
{
    return s_sectors;
}

/* The CHD frame (2352 bytes + subcode) of a sector. */
static const uint8_t *chd_frame(uint32_t lba)
{
    uint32_t hunk = lba / s_frames_per_hunk;
    if (hunk != s_hunk_number) {
        if (chd_read(s_chd, hunk, s_hunk) != CHDERR_NONE) {
            s_hunk_number = 0xFFFFFFFF;
            return NULL;
        }
        s_hunk_number = hunk;
    }
    return s_hunk + (lba % s_frames_per_hunk) * CD_FRAME_SIZE;
}

int disc_read_sectors(uint32_t lba, uint32_t count, void *out)
{
    uint8_t *dst = (uint8_t *) out;
    uint8_t raw[RAW_SECTOR];

    if (s_kind == KIND_NONE || lba + count > s_sectors) {
        return 0;
    }
    if (s_kind == KIND_ISO) {
        return read_at((uint64_t) lba * DISC_SECTOR_DATA, dst, count * DISC_SECTOR_DATA);
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *sector;
        if (s_kind == KIND_CHD) {
            sector = chd_frame(lba + i);
            if (!sector) {
                return 0;
            }
        } else {
            if (!read_at((uint64_t) (lba + i) * RAW_SECTOR, raw, RAW_SECTOR)) {
                return 0;
            }
            sector = raw;
        }
        memcpy(dst + i * DISC_SECTOR_DATA, sector + s_data_offset, DISC_SECTOR_DATA);
    }
    return 1;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
}

/* Compares a path component with an ISO name ("NAME.EXT;1"). */
static int name_matches(const char *want, int want_len, const uint8_t *name, int name_len)
{
    int i;
    for (i = 0; i < want_len; i++) {
        if (i >= name_len || toupper((unsigned char) want[i]) != toupper(name[i])) {
            return 0;
        }
    }
    return i == name_len || name[i] == ';';
}

/* Looks for `want` in the directory at lba/size. */
static int find_in_dir(uint32_t lba, uint32_t size, const char *want, int want_len,
                       uint32_t *out_lba, uint32_t *out_size, int *is_dir)
{
    uint8_t sector[DISC_SECTOR_DATA];
    for (uint32_t done = 0; done < size; done += DISC_SECTOR_DATA, lba++) {
        if (!disc_read_sectors(lba, 1, sector)) {
            return 0;
        }
        for (uint32_t pos = 0; pos < DISC_SECTOR_DATA;) {
            const uint8_t *rec = sector + pos;
            if (rec[0] == 0) {
                break;                  /* rest of the sector is unused */
            }
            if (name_matches(want, want_len, rec + 33, rec[32])) {
                *out_lba = le32(rec + 2);
                *out_size = le32(rec + 10);
                *is_dir = (rec[25] & 2) != 0;
                return 1;
            }
            pos += rec[0];
        }
    }
    return 0;
}

int disc_find_file(const char *path, uint32_t *lba, uint32_t *size)
{
    uint8_t pvd[DISC_SECTOR_DATA];
    uint32_t dir_lba, dir_size;
    int is_dir = 1;

    if (!disc_read_sectors(PVD_LBA, 1, pvd) || memcmp(pvd + 1, "CD001", 5) != 0) {
        return 0;
    }
    dir_lba = le32(pvd + 156 + 2);
    dir_size = le32(pvd + 156 + 10);
    while (*path == '\\' || *path == '/') {
        path++;
    }
    while (*path) {
        const char *end = path;
        while (*end && *end != '\\' && *end != '/' && *end != ';') {
            end++;
        }
        if (!is_dir || !find_in_dir(dir_lba, dir_size, path, (int) (end - path), &dir_lba,
                                    &dir_size, &is_dir)) {
            return 0;
        }
        if (*end == ';') {
            while (*end && *end != '\\' && *end != '/') {
                end++;
            }
        }
        path = *end ? end + 1 : end;
    }
    *lba = dir_lba;
    *size = dir_size;
    return !is_dir;
}

int disc_check(const char *path, char *detail, int detail_size)
{
    uint32_t lba, size;
    char cnf[DISC_SECTOR_DATA + 1];
    if (!path || !*path) {
        snprintf(detail, detail_size, "No disc chosen.");
        return DISC_NONE;
    }
    if (!disc_open(path)) {
        snprintf(detail, detail_size, "The file cannot be read as a PS1 disc.");
        return DISC_UNREADABLE;
    }
    if (!disc_find_file("SYSTEM.CNF;1", &lba, &size) || !disc_read_sectors(lba, 1, cnf)) {
        snprintf(detail, detail_size, "Not a PS1 disc (SYSTEM.CNF is missing).");
        return DISC_WRONG_GAME;
    }
    /* which version, and is the port's code made from its executable? */
    return disc_check_executables(detail, detail_size);
}

int disc_read_file(const char *path, void *out, uint32_t size, uint32_t *length)
{
    uint32_t lba, file_size;
    static uint8_t sector[DISC_SECTOR_DATA];
    if (!disc_find_file(path, &lba, &file_size) || file_size > size) {
        return 0;
    }
    uint32_t whole = file_size / DISC_SECTOR_DATA;
    if (whole && !disc_read_sectors(lba, whole, out)) {
        return 0;
    }
    if (file_size % DISC_SECTOR_DATA) {
        if (!disc_read_sectors(lba + whole, 1, sector)) {
            return 0;
        }
        memcpy((uint8_t *) out + whole * DISC_SECTOR_DATA, sector, file_size % DISC_SECTOR_DATA);
    }
    if (length) {
        *length = file_size;
    }
    return 1;
}

int disc_read_raw(uint32_t lba, uint8_t out[DISC_RAW_SECTOR])
{
    if (s_kind == KIND_NONE || s_kind == KIND_ISO || lba >= s_sectors) {
        return 0;                       /* (an .iso has no subheaders: no XA, no movies) */
    }
    if (s_kind == KIND_CHD) {
        const uint8_t *sector = chd_frame(lba);
        if (!sector) {
            return 0;
        }
        memcpy(out, sector, DISC_RAW_SECTOR);
        return 1;
    }
    return read_at((uint64_t) lba * RAW_SECTOR, out, RAW_SECTOR);
}
