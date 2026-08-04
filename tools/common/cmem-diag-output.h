#ifndef CMEM_DIAG_OUTPUT_H
#define CMEM_DIAG_OUTPUT_H

#include <stdbool.h>

void cmem_diag_output_text(const char *fmt, ...);
void cmem_diag_output_json(const char *fmt, ...);
void cmem_diag_output_error(const char *fmt, ...);
void cmem_diag_output_init(bool json, const char *output_file);

#endif
