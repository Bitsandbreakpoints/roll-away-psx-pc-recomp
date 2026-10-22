/*
 * game_check.c - is the chosen disc a version of the game whose code is in
 * the port: Roll Away (USA), Kula World (Europe) or KulaQuest (Japan)?
 * SYSTEM.CNF names the version's executable; the recompiled code belongs to
 * exactly one release of it, so its SHA1 must match (the rest of the disc
 * is only data).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "disc/disc.h"
#include "disc/sha1.h"
#include "runtime/runtime.h"

/* every version of the game, in the order of enum DiscVersion (a build
 * may contain fewer: configure.py) */
static const struct {
    const char *exe, *title;
} VERSIONS[] = {
    { "SLUS_007.24", "Roll Away (USA)" },
    { "SCES_010.00", "Kula World (Europe)" },
    { "SCPS_100.64", "KulaQuest (Japan)" },
};

#define MAX_EXE_SIZE            0x200000

static int s_version = DISC_VERSION_NONE;      /* (an index of VERSIONS) */

int disc_version(void)
{
    return s_version;
}

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

/* the executable SYSTEM.CNF boots, e.g. "SLUS_007.24" (BOOT = cdrom:\SLUS_007.24;1) */
static int boot_name(char *out, int size)
{
    char cnf[DISC_SECTOR_DATA + 1];
    uint32_t lba, length;
    if (!disc_find_file("/SYSTEM.CNF;1", &lba, &length) || !disc_read_sectors(lba, 1, cnf)) {
        return 0;
    }
    cnf[length < DISC_SECTOR_DATA ? length : DISC_SECTOR_DATA] = 0;
    const char *p = strstr(cnf, "BOOT");
    p = p ? strchr(p, '=') : NULL;
    if (!p) {
        return 0;
    }
    for (p++; *p == ' ' || *p == '\t'; p++) {
    }
    const char *colon = strchr(p, ':');
    if (colon && colon - p < 8) {
        p = colon + 1;
    }
    while (*p == '\\' || *p == '/') {
        p++;
    }
    int n = 0;
    while (p[n] && p[n] != ';' && p[n] != '\r' && p[n] != '\n' && p[n] != ' ' && n < size - 1) {
        out[n] = p[n];
        n++;
    }
    out[n] = 0;
    return n > 0;
}

int disc_check_executables(char *detail, int detail_size)
{
    char name[64], path[80], hex[41];
    s_version = DISC_VERSION_NONE;
    if (!boot_name(name, sizeof(name))) {
        snprintf(detail, detail_size, "Not a PS1 game disc (SYSTEM.CNF cannot be read).");
        return DISC_WRONG_GAME;
    }
    const char *title = NULL;
    for (size_t i = 0; i < sizeof(VERSIONS) / sizeof(VERSIONS[0]); i++) {
        if (strcmp(VERSIONS[i].exe, name) == 0) {
            title = VERSIONS[i].title;
            s_version = (int) i;
        }
    }
    const RtModule *module = NULL;
    for (int i = 0; i < g_rt_module_count; i++) {
        if (strcmp(g_rt_modules[i].disc_name, name) == 0) {
            module = &g_rt_modules[i];
        }
    }
    if (!title) {
        snprintf(detail, detail_size,
                 "This is not Roll Away (USA), Kula World (Europe) or KulaQuest (Japan).");
        return DISC_WRONG_GAME;
    }
    if (!module) {
        snprintf(detail, detail_size, "%s, but this build of the port has no code for it.", title);
        return DISC_OTHER_DUMP;
    }
    snprintf(path, sizeof(path), "/%s;1", name);
    if (!file_sha1(path, hex)) {
        snprintf(detail, detail_size, "%s, but %s cannot be read.", title, name);
        return DISC_UNREADABLE;
    }
    if (strcmp(hex, module->sha1) != 0) {
        snprintf(detail, detail_size,
                 "%s, but another release than the port's (%s differs).", title, name);
        return DISC_OTHER_DUMP;
    }
    snprintf(detail, detail_size, "%s \xE2\x80\x93 verified.", title);
    return DISC_OK;
}
