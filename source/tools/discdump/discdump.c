/*
 * discdump.c - copies files out of the player's Roll Away disc image
 * (.bin/.cue, .chd, .iso) for the build: the recompiler (recomp/) reads
 * the executables from there.  Nothing of the disc is kept in the
 * project; the files land in build/.
 *
 *     discdump <disc image> <output folder> <file on the disc> ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "disc/disc.h"

#define MAX_FILE_SIZE           0x400000

/* (the build does not check the executables against recompiled code) */
int disc_check_executables(char *detail, int detail_size)
{
    snprintf(detail, detail_size, "not checked");
    return DISC_OK;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: discdump <disc image> <output folder> <file> ...\n");
        return 2;
    }
    if (!disc_open(argv[1])) {
        fprintf(stderr, "discdump: %s cannot be read as a PS1 disc image\n", argv[1]);
        return 1;
    }
    uint8_t *data = (uint8_t *) malloc(MAX_FILE_SIZE);
    for (int i = 3; i < argc; i++) {
        uint32_t length;
        char path[512];
        if (!data || !disc_read_file(argv[i], data, MAX_FILE_SIZE, &length)) {
            fprintf(stderr, "discdump: %s is not on the disc\n", argv[i]);
            return 1;
        }
        const char *name = argv[i];
        for (const char *p = argv[i]; *p; p++) {
            if (*p == '/' || *p == 92) {
                name = p + 1;
            }
        }
        snprintf(path, sizeof(path), "%s/%s", argv[2], name);
        char *semicolon = strrchr(path, ';');
        if (semicolon) {
            *semicolon = 0;
        }
        FILE *f = fopen(path, "wb");
        if (!f || fwrite(data, 1, length, f) != length) {
            fprintf(stderr, "discdump: cannot write %s\n", path);
            return 1;
        }
        fclose(f);
        printf("discdump: %s (%u bytes)\n", path, length);
    }
    free(data);
    disc_close();
    return 0;
}
