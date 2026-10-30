#ifndef NVQ_DRIVER_VERSION_H
#define NVQ_DRIVER_VERSION_H
#include <ctype.h>
#include <stddef.h>
#include <string.h>

// /proc's open modules can say "Kernel Module for x86_64 VERSION";
// proprietary modules put VERSION immediately after "Kernel Module".
// Read a complete numeric dotted version token, never the intervening words.
static inline void kernel_version(const char *line, char *out, size_t size) {
    if (!size) return;
    out[0] = 0;
    const char *p = strstr(line, "Kernel Module");
    if (!p) return;
    p += strlen("Kernel Module");
    while (*p && *p != '\n') {
        while (*p == ' ' || *p == '\t') ++p;
        size_t n = strcspn(p, " \t\r\n");
        if (!n) return;
        unsigned dots = 0, digits = 0;
        size_t i = 0;
        for (; i < n; ++i) {
            if (isdigit((unsigned char)p[i])) ++digits;
            else if (p[i] == '.' && digits) { ++dots; digits = 0; }
            else break;
        }
        if (i == n && digits && (dots == 1 || dots == 2)) {
            if (n >= size) return;
            memcpy(out, p, n);
            out[n] = 0;
            return;
        }
        p += n;
    }
}
#endif
