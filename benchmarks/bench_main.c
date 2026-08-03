/**
 * @file bench_main.c
 * @brief High-Throughput Performance Benchmark comparing system malloc vs cmem.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "cmem.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

#define SMALL_ALLOC_COUNT 1000000
#define MEDIUM_ALLOC_COUNT 100000

/**
 * @brief Retrieves the current monotonic time in seconds.
 * @return Current time in seconds as a double.
 */
static double get_time_sec()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/**
 * @brief Benchmarks small object allocations (32-256 bytes) comparing system malloc vs cmem.
 * Measures throughput in Mops/sec for 1,000,000 operations.
 */
void bench_small_allocs()
{
    printf("\n--- Benchmark 1: Small Allocations (32-256 Bytes x %d ops) ---\n", SMALL_ALLOC_COUNT);
    void **ptrs = (void **)malloc(sizeof(void *) * SMALL_ALLOC_COUNT);

    // 1. System Malloc Benchmark
    double start_sys = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        size_t sz = 32 + (i % 224);
        ptrs[i]   = malloc(sz);
    }
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        free(ptrs[i]);
    }
    double time_sys = get_time_sec() - start_sys;

    // 2. cmem Benchmark
    memory_pool_t *pool     = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    double         start_mp = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        size_t sz = 32 + (i % 224);
        ptrs[i]   = mp_alloc(pool, sz);
    }
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        mp_free(pool, ptrs[i]);
    }
    double time_mp = get_time_sec() - start_mp;

    printf("  System Malloc Time : %.4f sec (%.2f Mops/sec)\n",
           time_sys,
           (SMALL_ALLOC_COUNT / time_sys) / 1e6);
    printf("  cmem Time          : %.4f sec (%.2f Mops/sec)\n",
           time_mp,
           (SMALL_ALLOC_COUNT / time_mp) / 1e6);
    printf("  Speedup            : %.2fx faster!\n", time_sys / time_mp);

    mp_destroy(pool);
    free(ptrs);
}

/**
 * @brief Benchmarks medium object allocations (1KB-64KB) comparing system malloc vs cmem.
 * Measures throughput for 100,000 operations.
 */
void bench_medium_allocs()
{
    printf("\n--- Benchmark 2: Medium Dynamic Allocations (1KB-64KB x %d ops) ---\n",
           MEDIUM_ALLOC_COUNT);
    void **ptrs = (void **)malloc(sizeof(void *) * MEDIUM_ALLOC_COUNT);

    double start_sys = get_time_sec();
    for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
        size_t sz = 1024 + (i % 63488);
        ptrs[i]   = malloc(sz);
    }
    for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
        free(ptrs[i]);
    }
    double time_sys = get_time_sec() - start_sys;

    memory_pool_t *pool     = mp_create(64 * 1024 * 1024, MP_FLAG_DEFAULT);
    double         start_mp = get_time_sec();
    for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
        size_t sz = 1024 + (i % 63488);
        ptrs[i]   = mp_alloc(pool, sz);
    }
    for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
        mp_free(pool, ptrs[i]);
    }
    double time_mp = get_time_sec() - start_mp;

    printf("  System Malloc Time : %.4f sec\n", time_sys);
    printf("  cmem Time          : %.4f sec\n", time_mp);
    printf("  Speedup            : %.2fx faster!\n", time_sys / time_mp);

    mp_destroy(pool);
    free(ptrs);
}

/**
 * @brief Benchmarks arena reset performance comparing individual free loop vs mp_reset batch reset.
 * Runs 1000 rounds of 500 allocations to measure the speedup of batch reset.
 */
void bench_arena_reset()
{
    printf("\n--- Benchmark 3: Fast Arena Reset (mp_reset x 1000 rounds of 500 allocs) ---\n");
    memory_pool_t *pool = mp_create(8 * 1024 * 1024, MP_FLAG_DEFAULT);
    void          *ptrs[500];

    // Standard Free loop
    double start_loop = get_time_sec();
    for (int r = 0; r < 1000; r++) {
        for (int i = 0; i < 500; i++) {
            ptrs[i] = mp_alloc(pool, 128 + i * 8);
        }
        for (int i = 0; i < 500; i++) {
            mp_free(pool, ptrs[i]);
        }
    }
    double time_loop = get_time_sec() - start_loop;

    // mp_reset Batch Free
    double start_reset = get_time_sec();
    for (int r = 0; r < 1000; r++) {
        for (int i = 0; i < 500; i++) {
            mp_alloc(pool, 128 + i * 8);
        }
        mp_reset(pool);
    }
    double time_reset = get_time_sec() - start_reset;

    printf("  Individual Free Loop Time : %.5f sec\n", time_loop);
    printf("  Arena mp_reset Batch Time : %.5f sec\n", time_reset);
    printf("  Speedup                   : %.2fx faster!\n", time_loop / time_reset);

    mp_destroy(pool);
}

/**
 * @brief Entry point for cmem performance benchmarks.
 * Runs small alloc, medium alloc, and arena reset benchmarks.
 * @return 0 on success.
 */
int main()
{
    printf("================ CMEM PERFORMANCE BENCHMARK ================\n");
    bench_small_allocs();
    bench_medium_allocs();
    bench_arena_reset();
    return 0;
}
