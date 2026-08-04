#ifndef CMEM_INSPECT_H
#define CMEM_INSPECT_H

#include "cmem.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool success;
    char output[4096];
    size_t output_len;
    int exit_code;
    const char *error_msg;
} cmem_inspect_result_t;

cmem_inspect_result_t cmem_inspect_run(int argc, char **argv);
cmem_inspect_result_t cmem_inspect_leaks(memory_pool_t *pool, bool json);
cmem_inspect_result_t cmem_inspect_audit(memory_pool_t *pool);
cmem_inspect_result_t cmem_inspect_stats(memory_pool_t *pool, bool json);
cmem_inspect_result_t cmem_inspect_tree(memory_pool_t *pool, bool json);

#endif
