/*
 * resources.h - ids of the resources built into the exe (resources.rc).
 */
#ifndef RESOURCES_H
#define RESOURCES_H

/* the program's icon (the first: Explorer shows it): drawn by
 * tools/make_icon.py (our own) */
#define IDI_APP                 1

/* launcher background: drawn by tools/make_background.py (our own) */
#define IDR_LAUNCHER_BACKGROUND 101

/* the version shown in the exe's properties (Details): the release's */
#define RA_VERSION_MAJOR        2
#define RA_VERSION_MINOR        6
#define RA_VERSION_PATCH        0
#define RA_VERSION_TEXT         "2.6.0"

#endif /* RESOURCES_H */
