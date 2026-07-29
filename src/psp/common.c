#include "common.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Debug output lands in Azahar's log (and gdb) via svcOutputDebugString. */

static void debug_vout(const char *prefix, const char *fmt, va_list ap) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%s", prefix);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    svcOutputDebugString(buf, strlen(buf));
}

void fatal_error(const s8 *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    debug_vout("[SF3 FATAL] ", (const char *)fmt, ap);
    va_end(ap);
}

void not_implemented(const s8 *func) {
    char buf[256];
    snprintf(buf, sizeof(buf), "[SF3 STUB] %s", (const char *)func);
    svcOutputDebugString(buf, strlen(buf));
}

void debug_print(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    debug_vout("[SF3] ", fmt, ap);
    va_end(ap);
}
