// A streaming JSON writer to stdout: open/close objects and arrays, keyed or bare values, commas and
// escaping handled. One document or one line at a time; nothing is buffered beyond stdio's.
#pragma once
#include <stdio.h>
#include <string.h>

static int jdepth;
static int jcomma[64];

static void jsep(const char *key) {
    if (jdepth > 0 && jcomma[jdepth]) fputc(',', stdout);
    if (jdepth > 0) jcomma[jdepth] = 1;
    if (key) printf("\"%s\":", key);
}

static void jstrraw(const char *s) {
    fputc('"', stdout);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 0x20) printf("\\u%04x", *p);
        else fputc(*p, stdout);
    }
    fputc('"', stdout);
}

static void jo(const char *key) { jsep(key); fputc('{', stdout); jcomma[++jdepth] = 0; }
static void ja(const char *key) { jsep(key); fputc('[', stdout); jcomma[++jdepth] = 0; }
static void jc(void) { jdepth--; }
static void jeo(void) { fputc('}', stdout); jc(); }
static void jea(void) { fputc(']', stdout); jc(); }
static void js(const char *key, const char *v) { jsep(key); jstrraw(v); }
static void ju(const char *key, unsigned long long v) { jsep(key); printf("%llu", v); }
static void ji(const char *key, long long v) { jsep(key); printf("%lld", v); }
static void jb(const char *key, int v) { jsep(key); fputs(v ? "true" : "false", stdout); }
static void jf(const char *key, double v) { jsep(key); printf("%.3f", v); }
// Ends a top-level document or line: a newline and a flush, so a reader never waits on a buffer.
static void jline(void) { fputc('\n', stdout); fflush(stdout); }
