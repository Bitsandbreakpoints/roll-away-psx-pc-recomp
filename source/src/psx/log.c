/*
 * log.c - the PC port's log file (roll_away.log next to the exe).
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include "psx/log.h"     /* (not psx.h: its RECT clashes with windows.h) */

#define MAX_STUBS               256

static FILE *s_log;
static const char *s_stubs_seen[MAX_STUBS];
static int s_stub_count;

static FILE *log_file(void)
{
    if (!s_log) {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
        while (n > 0 && path[n - 1] != L'\\') {
            n--;
        }
        wcscpy(path + n, L"roll_away.log");
        s_log = _wfopen(path, L"w");
    }
    return s_log;
}

void psx_log(const char *format, ...)
{
    FILE *f = log_file();
    va_list args;
    if (!f) {
        return;
    }
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fputc('\n', f);
    fflush(f);
}

void psx_stub(const char *function)
{
    /* __func__ is one string per function: compare the pointers */
    for (int i = 0; i < s_stub_count; i++) {
        if (s_stubs_seen[i] == function) {
            return;
        }
    }
    if (s_stub_count < MAX_STUBS) {
        s_stubs_seen[s_stub_count++] = function;
    }
    psx_log("not implemented yet: %s()", function);
}
