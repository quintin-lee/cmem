/**
 * @file bench_main.c
 * @brief High-Throughput Performance Benchmark: System Malloc vs Antigravity Memory Pool.
 */

#include "../include/memory_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

#define NUM_OPERATIONS 1000000

static double get_time_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void bench_small_allocations() {
    printf("--- Benchmark 1: Small Allocations (32-256 Bytes x %d ops) ---\n", NUM_OPERATIONS);

    void** ptrs = (void**)malloc(sizeof(void*) * NUM_OPERATIONS);

    // 1. Standard System Malloc
    double t0 = get_time_sec();
    for (int i = 0; i < NUM_OPERATIONS; i++) {
        size_t sz = 32 + (i % 224);
        ptrs[i] = malloc(sz);
    }
    for (int i = 0; i < NUM_OPERATIONS; i++) {
        free(ptrs[i]);
    }
    double t_system = get_time_sec() - t0;

    // 2. Custom Memory Pool (Slab Tier)
    memory_pool_t* pool = mp_create(4 * 1024 * 1024, MP_FLAG_DEFAULT);
    t0 = get_time_sec();
    for (int i = 0; i < NUM_OPERATIONS; i++) {
        size_t sz = 32 + (i % 224);
        ptrs[i] = mp_alloc(pool, sz);
    }
    for (int i = 0; i < NUM_OPERATIONS; i++) {
        mp_free(pool, ptrs[i]);
    }
    double t_pool = get_time_sec() - t0;

    printf("  System Malloc Time : %.4f sec (%.2f Mops/sec)\n", t_system, (NUM_OPERATIONS * 2) / (t_system * 1e6));
    printf("  Memory Pool Time   : %.4f sec (%.2f Mops/sec)\n", t_pool, (NUM_OPERATIONS * 2) / (t_pool * 1e6));
    printf("  Speedup            : %.2fx faster!\n\n", t_system / t_pool);

    mp_destroy(pool);
    free(ptrs);
}

void bench_medium_allocations() {
    printf("--- Benchmark 2: Medium Dynamic Allocations (1KB-64KB x %d ops) ---\n", NUM_OPERATIONS / 10);
    int ops = NUM_OPERATIONS / 10;
    void** ptrs = (void**)malloc(sizeof(void*) * ops);

    // 1. Standard System Malloc
    double t0 = get_time_sec();
    for (int i = 0; i < ops; i++) {
        size_t sz = 1024 + (i % 63488);
        ptrs[i] = malloc(sz);
    }
    for (int i = 0; i < ops; i++) {
        free(ptrs[i]);
    }
    double t_system = get_time_sec() - t0;

    // 2. Custom Memory Pool (TLSF Tier)
    memory_pool_t* pool = mp_create(16 * 1024 * 1024, MP_FLAG_DEFAULT);
    t0 = get_time_sec();
    for (int i = 0; i < ops; i++) {
        size_t sz = 1024 + (i % 63488);
        ptrs[i] = mp_alloc(pool, sz);
    }
    for (int i = 0; i < ops; i++) {
        mp_free(pool, ptrs[i]);
    }
    double t_pool = get_time_sec() - t0;

    printf("  System Malloc Time : %.4f sec\n", t_system);
    printf("  Memory Pool Time   : %.4f sec\n", t_pool);
    printf("  Speedup            : %.2fx faster!\n\n", t_system / t_pool);

    mp_destroy(pool);
    free(ptrs);
}

int main() {
    printf("================ MEMORY POOL PERFORMANCE BENCHMARK ================\n\n");
    bench_small_allocations();
    bench_medium_allocations();
    return 0;
}
