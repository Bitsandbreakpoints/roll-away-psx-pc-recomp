/*
 * disc.h - the player's own disc image (.bin/.cue, .chd or .iso) and its
 * ISO9660 file system.  Nothing of the game is shipped with the port: all
 * data is read from here.
 */
#ifndef DISC_DISC_H
#define DISC_DISC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISC_SECTOR_DATA        2048    /* user data of a Mode 1 / Mode 2 Form 1 sector */
#define DISC_RAW_SECTOR         2352    /* a whole sector: sync, header, subheader, data */

/* disc_check() result */
enum DiscStatus {
    DISC_NONE = 0,                  /* nothing chosen */
    DISC_UNREADABLE,                /* not a disc image we can read */
    DISC_WRONG_GAME,                /* a PS1 disc, but not a version of the game */
    DISC_OTHER_DUMP,                /* the game, but not the release the port's code is made from */
    DISC_OK                         /* the executables match the recompiled code */
};

/* Opens a .bin/.cue/.chd/.iso; FALSE if it cannot be read. */
int disc_open(const char *path);
void disc_close(void);

/* Reads `count` sectors of user data starting at `lba`. */
int disc_read_sectors(uint32_t lba, uint32_t count, void *out);

/* Reads a whole sector (2352 bytes: the header and the XA subheader
 * too, for streaming); FALSE for an .iso image, which has no subheaders. */
int disc_read_raw(uint32_t lba, uint8_t out[DISC_RAW_SECTOR]);

/* Finds a file ("\\DATA\\GAME.PAK;1", case and ";1" optional). */
int disc_find_file(const char *path, uint32_t *lba, uint32_t *size);

/* Opens the image and checks that it is a version of the game (Roll Away
 * (USA), Kula World (Europe), KulaQuest (Japan)) with the executable the
 * port was recompiled from.  `detail` gets a short
 * explanation (UTF-8). */
int disc_check(const char *path, char *detail, int detail_size);

/* (game_check.c) the second half of disc_check(): the opened disc's
 * executables against the recompiled code; a DiscStatus */
int disc_check_executables(char *detail, int detail_size);

/* (game_check.c) the release disc_check_executables() last recognised
 * (also when its executable is not the port's): a DiscVersion */
enum DiscVersion {
    DISC_VERSION_NONE = -1,
    DISC_VERSION_USA,               /* Roll Away */
    DISC_VERSION_EUROPE,            /* Kula World */
    DISC_VERSION_JAPAN              /* KulaQuest */
};
int disc_version(void);

/* A file of the opened disc, whole; FALSE if it is missing or larger than `size`. */
int disc_read_file(const char *path, void *out, uint32_t size, uint32_t *length);

/* Total sectors of the opened image. */
uint32_t disc_sector_count(void);

#ifdef __cplusplus
}
#endif

#endif /* DISC_DISC_H */
