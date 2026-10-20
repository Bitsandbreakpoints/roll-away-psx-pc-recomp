/*
 * bios.c - the PS1 BIOS functions that the game calls, in C.
 *
 * Sony's libraries call the BIOS through three vectors (A0h, B0h, C0h)
 * with the function number in t1.  The PC port has no BIOS: the C library
 * functions work on PS1 memory, the heap is a simple first-fit allocator
 * in the area InitHeap() gives, events always "happen", and LoadExec()
 * starts the next recompiled executable.  Unknown functions are logged.
 */
#include <stdio.h>
#include <string.h>
#include "runtime/recomp.h"
#include "runtime/runtime.h"
#include "runtime/bios.h"
#include "runtime/kanji.h"
#include "psx/gpu.h"
#include "psx/log.h"

#define EVENT_HANDLE_BASE       0xF1000000u
#define HEAP_USED               1u

static uint32_t s_rand_seed;
static uint32_t s_heap_start, s_heap_end;
static char s_line[256];                /* printf / putchar output until a newline */
static int s_line_length;
static uint32_t s_next_event;

void bios_reset(void)
{
    s_rand_seed = 0;
    s_heap_start = s_heap_end = 0;
    s_line_length = 0;
    s_next_event = 0;
}

/* ---- console output (to the log) -------------------------------------- */

static void out_char(char c)
{
    if (c == '\n' || s_line_length == (int) sizeof(s_line) - 1) {
        s_line[s_line_length] = 0;
        psx_log("PS1: %s", s_line);
        s_line_length = 0;
        if (c == '\n') {
            return;
        }
    }
    s_line[s_line_length++] = c;
}

static void out_string(const char *s)
{
    while (*s) {
        out_char(*s++);
    }
}

/* printf with the PS1's arguments: a1, a2, a3, then the stack */
static void bios_printf(uint32_t format, int first_arg)
{
    int arg = first_arg;
    char spec[16], buf[128];
    for (uint32_t p = format;; p++) {
        char c = (char) RD8(p);
        if (!c) {
            break;
        }
        if (c != '%') {
            out_char(c);
            continue;
        }
        int n = 0;
        spec[n++] = '%';
        for (;;) {
            c = (char) RD8(++p);
            if (!c || n >= (int) sizeof(spec) - 2) {
                break;
            }
            if (c == 'l' || c == 'h') {
                continue;               /* (all 32 bits) */
            }
            spec[n++] = c;
            if (strchr("diouxXcsp%", c)) {
                break;
            }
        }
        spec[n] = 0;
        if (!c) {
            break;
        }
        if (c == '%') {
            out_char('%');
        } else if (c == 's') {
            char s[128];
            uint32_t a = ARG(arg);
            arg++;
            int k = 0;
            while (a && k < (int) sizeof(s) - 1 && RD8(a + k)) {
                s[k] = (char) RD8(a + k);
                k++;
            }
            s[k] = 0;
            snprintf(buf, sizeof(buf), spec, s);
            out_string(buf);
        } else {
            snprintf(buf, sizeof(buf), spec, ARG(arg));
            arg++;
            out_string(buf);
        }
    }
}

/* ---- memory and strings -------------------------------------------------- */

static uint32_t str_length(uint32_t s)
{
    uint32_t n = 0;
    while (RD8(s + n)) {
        n++;
    }
    return n;
}

static void copy(uint32_t dst, uint32_t src, uint32_t n)
{
    if (dst > src && dst < src + n) {
        while (n--) {
            WR8(dst + n, RD8(src + n));
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            WR8(dst + i, RD8(src + i));
        }
    }
}

static int compare(uint32_t a, uint32_t b, uint32_t n, int strings)
{
    for (uint32_t i = 0; i < n; i++) {
        int x = (int) RD8(a + i), y = (int) RD8(b + i);
        if (x != y) {
            return x - y;
        }
        if (strings && !x) {
            return 0;
        }
    }
    return 0;
}

static uint32_t find_char(uint32_t s, int c, int last)
{
    uint32_t found = 0;
    for (;; s++) {
        int x = (int) RD8(s);
        if (x == (c & 0xFF)) {
            found = s;
            if (!last) {
                return s;
            }
        }
        if (!x) {
            return found;
        }
    }
}

static int in_set(uint32_t set, int c)
{
    for (; RD8(set); set++) {
        if ((int) RD8(set) == c) {
            return 1;
        }
    }
    return 0;
}

static int32_t parse_int(uint32_t s, uint32_t *end, int base)
{
    int sign = 1;
    int32_t v = 0;
    while (RD8(s) == ' ' || RD8(s) == '\t') {
        s++;
    }
    if (RD8(s) == '-' || RD8(s) == '+') {
        sign = RD8(s) == '-' ? -1 : 1;
        s++;
    }
    if ((base == 0 || base == 16) && RD8(s) == '0' && (RD8(s + 1) | 0x20) == 'x') {
        base = 16;
        s += 2;
    } else if (base == 0) {
        base = RD8(s) == '0' ? 8 : 10;
    }
    for (;; s++) {
        int c = (int) RD8(s), d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') {
            d = (c | 0x20) - 'a' + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        v = v * base + d;
    }
    if (end) {
        *end = s;
    }
    return v * sign;
}

/* ---- heap (InitHeap / malloc / free) --------------------------------------- */

/* blocks: an 8-byte header (size including the header | HEAP_USED), then the data */
static void heap_init(uint32_t start, uint32_t size)
{
    s_heap_start = (start + 7) & ~7u;
    s_heap_end = (start + size) & ~7u;
    if (s_heap_end > s_heap_start + 8) {
        WR32(s_heap_start, s_heap_end - s_heap_start);
    }
}

static uint32_t heap_alloc(uint32_t n)
{
    n = ((n + 7) & ~7u) + 8;
    for (uint32_t b = s_heap_start; b && b < s_heap_end;) {
        uint32_t header = RD32(b), size = header & ~7u;
        if (!size) {
            break;
        }
        if (!(header & HEAP_USED)) {
            /* join the free blocks that follow */
            while (b + size < s_heap_end && !(RD32(b + size) & HEAP_USED) && (RD32(b + size) & ~7u)) {
                size += RD32(b + size) & ~7u;
            }
            WR32(b, size);
            if (size >= n) {
                if (size - n >= 16) {
                    WR32(b + n, size - n);
                    size = n;
                }
                WR32(b, size | HEAP_USED);
                return b + 8;
            }
        }
        b += size;
    }
    psx_log("malloc(%u): out of memory", n - 8);
    return 0;
}

static void heap_free(uint32_t p)
{
    if (p >= s_heap_start + 8 && p < s_heap_end) {
        WR32(p - 8, RD32(p - 8) & ~HEAP_USED);
    }
}

static void log_unknown(uint32_t table, uint32_t function)
{
    static uint32_t seen[64];
    static int count;
    uint32_t key = table << 8 | function;
    for (int i = 0; i < count; i++) {
        if (seen[i] == key) {
            return;
        }
    }
    if (count < 64) {
        seen[count++] = key;
    }
    psx_log("BIOS %02Xh:%02Xh not implemented (ra %08X)", table, function, GPR[31]);
}

static void read_path(uint32_t a, char *out, int size)
{
    int n = 0;
    while (n < size - 1 && RD8(a + n)) {
        out[n] = (char) RD8(a + n);
        n++;
    }
    out[n] = 0;
}

/* ---- A0h: the C library ------------------------------------------------------ */

static void string_function(uint32_t f, uint32_t a0, uint32_t a1, uint32_t a2)
{
    uint32_t end, n = 0;
    switch (f) {
    case 0x0C: case 0x0D:                                   /* strtoul, strtol */
        RETURN(parse_int(a0, &end, (int) a2));
        if (a1) {
            WR32(a1, end);
        }
        break;
    case 0x10: case 0x11:                                   /* atoi, atol */
        RETURN(parse_int(a0, NULL, 10));
        break;
    case 0x15:                                              /* strcat */
        copy(a0 + str_length(a0), a1, str_length(a1) + 1);
        RETURN(a0);
        break;
    case 0x16:                                              /* strncat */
        end = a0 + str_length(a0);
        for (; n < a2 && RD8(a1 + n); n++) {
            WR8(end + n, RD8(a1 + n));
        }
        WR8(end + n, 0);
        RETURN(a0);
        break;
    case 0x17:                                              /* strcmp */
        RETURN(compare(a0, a1, 0xFFFFFFFFu, 1));
        break;
    case 0x18:                                              /* strncmp */
        RETURN(compare(a0, a1, a2, 1));
        break;
    case 0x19:                                              /* strcpy */
        copy(a0, a1, str_length(a1) + 1);
        RETURN(a0);
        break;
    case 0x1A:                                              /* strncpy */
        for (; n < a2 && RD8(a1 + n); n++) {
            WR8(a0 + n, RD8(a1 + n));
        }
        for (; n < a2; n++) {
            WR8(a0 + n, 0);
        }
        RETURN(a0);
        break;
    case 0x1B:                                              /* strlen */
        RETURN(str_length(a0));
        break;
    case 0x1C: case 0x1E:                                   /* index, strchr */
        RETURN(find_char(a0, (int) a1, 0));
        break;
    case 0x1D: case 0x1F:                                   /* rindex, strrchr */
        RETURN(find_char(a0, (int) a1, 1));
        break;
    case 0x20:                                              /* strpbrk */
        end = a0;
        while (RD8(end) && !in_set(a1, (int) RD8(end))) {
            end++;
        }
        RETURN(RD8(end) ? end : 0);
        break;
    case 0x21: case 0x22:                                   /* strspn, strcspn */
        while (RD8(a0 + n) && in_set(a1, (int) RD8(a0 + n)) == (f == 0x21)) {
            n++;
        }
        RETURN(n);
        break;
    case 0x24:                                              /* strstr */
        n = str_length(a1);
        RETURN(0);
        for (uint32_t s = a0;; s++) {
            if (compare(s, a1, n, 0) == 0) {
                RETURN(s);
                break;
            }
            if (!RD8(s)) {
                break;
            }
        }
        break;
    }
}

static void call_a0(uint32_t f)
{
    uint32_t a0 = GPR[4], a1 = GPR[5], a2 = GPR[6];
    char path[64];
    if ((f >= 0x0C && f <= 0x11 && f != 0x0E && f != 0x0F) || (f >= 0x15 && f <= 0x22)
        || f == 0x24) {
        string_function(f, a0, a1, a2);
        return;
    }
    switch (f) {
    case 0x0E: case 0x0F:                                   /* abs, labs */
        RETURN((int32_t) a0 < 0 ? -a0 : a0);
        break;
    case 0x13: case 0x14:                                   /* setjmp, longjmp */
        rt_fatal("BIOS setjmp/longjmp are not supported (ra %08X).", GPR[31]);
        break;
    case 0x25:                                              /* toupper */
        RETURN(a0 >= 'a' && a0 <= 'z' ? a0 - 32 : a0);
        break;
    case 0x26:                                              /* tolower */
        RETURN(a0 >= 'A' && a0 <= 'Z' ? a0 + 32 : a0);
        break;
    case 0x27:                                              /* bcopy(src, dst, n) */
        copy(a1, a0, a2);
        break;
    case 0x28:                                              /* bzero */
        for (uint32_t i = 0; i < a1; i++) {
            WR8(a0 + i, 0);
        }
        break;
    case 0x29: case 0x2D:                                   /* bcmp, memcmp */
        RETURN(compare(a0, a1, a2, 0));
        break;
    case 0x2A: case 0x2C:                                   /* memcpy, memmove */
        copy(a0, a1, a2);
        RETURN(a0);
        break;
    case 0x2B:                                              /* memset */
        for (uint32_t i = 0; i < a2; i++) {
            WR8(a0 + i, a1);
        }
        RETURN(a0);
        break;
    case 0x2E:                                              /* memchr */
        RETURN(0);
        for (uint32_t i = 0; i < a2; i++) {
            if (RD8(a0 + i) == (a1 & 0xFF)) {
                RETURN(a0 + i);
                break;
            }
        }
        break;
    case 0x2F:                                              /* rand */
        s_rand_seed = s_rand_seed * 0x41C64E6Du + 0x3039u;
        RETURN((s_rand_seed >> 16) & 0x7FFF);
        break;
    case 0x30:                                              /* srand */
        s_rand_seed = a0;
        break;
    case 0x33:                                              /* malloc */
        RETURN(heap_alloc(a0));
        break;
    case 0x34:                                              /* free */
        heap_free(a0);
        break;
    case 0x37: {                                            /* calloc */
        uint32_t p = heap_alloc(a0 * a1);
        for (uint32_t i = 0; p && i < a0 * a1; i++) {
            WR8(p + i, 0);
        }
        RETURN(p);
        break;
    }
    case 0x38: {                                            /* realloc */
        uint32_t p = heap_alloc(a1);
        if (p && a0) {
            uint32_t old = (RD32(a0 - 8) & ~7u) - 8;
            copy(p, a0, old < a1 ? old : a1);
            heap_free(a0);
        }
        RETURN(p);
        break;
    }
    case 0x39:                                              /* InitHeap */
        heap_init(a0, a1);
        break;
    case 0x3C:                                              /* putchar */
        out_char((char) a0);
        break;
    case 0x3E:                                              /* puts */
        for (uint32_t s = a0; RD8(s); s++) {
            out_char((char) RD8(s));
        }
        out_char('\n');
        break;
    case 0x3F:                                              /* printf */
        bios_printf(a0, 1);
        break;
    case 0x44:                                              /* FlushCache */
        break;
    case 0x49:                                              /* GPU_cw */
        gpu_gp0(a0);
        break;
    case 0x51:                                              /* LoadExec */
        read_path(a0, path, sizeof(path));
        psx_log("LoadExec(%s, %08X, %08X)", path, a1, a2);
        rt_load_exec(path, a1, a2);
        break;
    case 0x70: case 0x71: case 0x72: case 0x9F:             /* _bu_init, _96_init, _96_remove, SetMem */
        break;
    default:
        log_unknown(BIOS_A0, f);
        RETURN(0);
        break;
    }
}

/* ---- B0h: events, threads, exceptions, files ---------------------------------- */

static void call_b0(uint32_t f)
{
    switch (f) {
    case 0x07:                                              /* DeliverEvent */
    case 0x20:                                              /* UnDeliverEvent */
        break;
    case 0x08:                                              /* OpenEvent */
        RETURN(EVENT_HANDLE_BASE | (s_next_event++ & 0xFFFF));
        break;
    case 0x09:                                              /* CloseEvent */
    case 0x0A:                                              /* WaitEvent (always happened) */
    case 0x0B:                                              /* TestEvent */
    case 0x0C:                                              /* EnableEvent */
    case 0x0D:                                              /* DisableEvent */
        RETURN(1);
        break;
    case 0x0E: case 0x0F: case 0x10:                        /* OpenTh, CloseTh, ChangeTh */
        rt_fatal("BIOS threads are not supported (B0h:%02Xh, ra %08X).", f, GPR[31]);
        break;
    case 0x17:                                              /* ReturnFromException */
        rt_fatal("ReturnFromException outside an exception (ra %08X).", GPR[31]);
        break;
    case 0x18: case 0x19:                                   /* Set{Default,Custom}ExitFromException */
    case 0x5B:                                              /* ChangeClearPad */
        RETURN(0);
        break;
    case 0x35:                                              /* write(fd, buf, n) */
        if (GPR[4] == 1) {
            for (uint32_t i = 0; i < GPR[6]; i++) {
                out_char((char) RD8(GPR[5] + i));
            }
        }
        RETURN(GPR[6]);
        break;
    case 0x3D:                                              /* putchar */
        out_char((char) GPR[4]);
        break;
    case 0x3F:                                              /* puts */
        for (uint32_t s = GPR[4]; RD8(s); s++) {
            out_char((char) RD8(s));
        }
        out_char('\n');
        break;
    case 0x51:                                              /* Krom2RawAdd */
        RETURN(kanji_block_address((uint16_t) GPR[4]));
        break;
    default:
        log_unknown(BIOS_B0, f);
        RETURN(0);
        break;
    }
}

/* ---- C0h: kernel ------------------------------------------------------------- */

static void call_c0(uint32_t f)
{
    switch (f) {
    case 0x02: case 0x03:                                   /* SysEnqIntRP, SysDeqIntRP */
    case 0x0A:                                              /* ChangeClearRCnt */
        RETURN(0);
        break;
    default:
        log_unknown(BIOS_C0, f);
        RETURN(0);
        break;
    }
}

void bios_call(uint32_t table, uint32_t function)
{
    function &= 0xFF;
    if (table == BIOS_A0) {
        call_a0(function);
    } else if (table == BIOS_B0) {
        call_b0(function);
    } else {
        call_c0(function);
    }
}
