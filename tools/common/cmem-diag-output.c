#include "cmem-diag-output.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static bool g_json = false;
static FILE *g_output_file = NULL;

void cmem_diag_output_init(bool json, const char *output_file) {
    g_json = json;
    if (output_file) {
        g_output_file = fopen(output_file, "w");
        if (!g_output_file) {
            fprintf(stderr, "Failed to open output file: %s\n", output_file);
            g_output_file = stdout;
        }
    } else {
        g_output_file = stdout;
    }
}

void cmem_diag_output_text(const char *fmt, ...) {
    if (g_json) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(g_output_file, fmt, args);
    va_end(args);
    fprintf(g_output_file, "\n");
}

void cmem_diag_output_json(const char *fmt, ...) {
    if (!g_json) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(g_output_file, fmt, args);
    va_end(args);
    fprintf(g_output_file, "\n");
}

void cmem_diag_output_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}
