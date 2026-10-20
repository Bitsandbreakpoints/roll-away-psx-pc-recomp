/*
 * game_check.c - is the chosen disc the Roll Away (USA) the port was
 * made from?  The recompiled code belongs to exactly one version of each
 * executable, so their SHA1 must match (the rest of the disc is only data).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "disc/disc.h"
#include "disc/sha1.h"

extern const char g_main_sha1[];

static const struct {
    const char *path;
    const char *sha1;
} EXECUTABLES[] = {
    { "/SLUS_007.24;1", g_main_sha1 },
};

#define MAX_EXE_SIZE            0x200000

static int file_sha1(const char *path, char hex[41])
{
    uint32_t length;
    uint8_t digest[20];
    Sha1 ctx;
    uint8_t *data = (uint8_t *) malloc(MAX_EXE_SIZE);
    int ok = data && disc_read_file(path, data, MAX_EXE_SIZE, &length);
    if (ok) {
        sha1_init(&ctx);
        sha1_update(&ctx, data, length);
        sha1_final(&ctx, digest);
        for (int i = 0; i < 20; i++) {
            snprintf(hex + 2 * i, 3, "%02x", digest[i]);
        }
    }
    free(data);
    return ok;
}

int disc_check_executables(char *detail, int detail_size)
{
    char hex[41];
    for (size_t i = 0; i < sizeof(EXECUTABLES) / sizeof(EXECUTABLES[0]); i++) {
        if (!file_sha1(EXECUTABLES[i].path, hex)) {
            snprintf(detail, detail_size, "Roll Away, but %s cannot be read.",
                     EXECUTABLES[i].path + 1);
            return DISC_UNREADABLE;
        }
        if (strcmp(hex, EXECUTABLES[i].sha1) != 0) {
            snprintf(detail, detail_size,
                     "Roll Away, but another version than the port's (%s differs). "
                     "Only Roll Away (USA), SLUS-00724, is supported.", EXECUTABLES[i].path + 1);
            return DISC_OTHER_DUMP;
        }
    }
    snprintf(detail, detail_size, "Roll Away (USA) \xE2\x80\x93 verified.");
    return DISC_OK;
}
