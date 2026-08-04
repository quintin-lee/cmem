/**
 * @file bench_main.c
 * @brief High-Throughput Performance Benchmark comparing system malloc vs cmem.
 */

#include "cmem.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SMALL_ALLOC_COUNT 1000000
#define MEDIUM_ALLOC_COUNT 100000
#define SMALL_ALLOC_SPREAD 224
#define MEDIUM_ALLOC_SPREAD 63488
#define ARENA_RESET_ALLOCS 500
#define ARENA_RESET_ROUNDS 1000
#define NSEC_PER_SEC 1e9
#define MILLION_OPS 1e6

/**
 * @brief Retrieves the current monotonic time in seconds.
 * @return Current time in seconds as a double.
 */
static double get_time_sec()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / NSEC_PER_SEC;
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

        size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
        ptrs[i] = malloc(sz);
    }
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        free(ptrs[i]);
    }
    double time_sys = get_time_sec() - start_sys;

    // 2. cmem Benchmark
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    double start_mp = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {

        size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
        ptrs[i] = mp_alloc(pool, sz);
    }
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        mp_free(pool, ptrs[i]);
    }
    double time_mp = get_time_sec() - start_mp;

    printf("  System Malloc Time : %.4f sec (%.2f Mops/sec)\n",
           time_sys,
           (SMALL_ALLOC_COUNT / time_sys) / MILLION_OPS);
    printf("  cmem Time          : %.4f sec (%.2f Mops/sec)\n",
           time_mp,
           (SMALL_ALLOC_COUNT / time_mp) / MILLION_OPS);
    printf("  Speedup            : %.2fx faster!\n", time_sys / time_mp);

    mp_destroy(pool);
    free((void *)ptrs);
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

        size_t sz = 1024 + (i % MEDIUM_ALLOC_SPREAD);
        ptrs[i] = malloc(sz);
    }
    for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
        free(ptrs[i]);
    }
    double time_sys = get_time_sec() - start_sys;

    memory_pool_t *pool = mp_create(64 * 1024 * 1024, MP_FLAG_DEFAULT);
    double start_mp = get_time_sec();
    for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {

        size_t sz = 1024 + (i % MEDIUM_ALLOC_SPREAD);
        ptrs[i] = mp_alloc(pool, sz);
    }
    for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
        mp_free(pool, ptrs[i]);
    }
    double time_mp = get_time_sec() - start_mp;

    printf("  System Malloc Time : %.4f sec\n", time_sys);
    printf("  cmem Time          : %.4f sec\n", time_mp);
    printf("  Speedup            : %.2fx faster!\n", time_sys / time_mp);

    mp_destroy(pool);
    free((void *)ptrs);
}

/**
 * @brief Benchmarks arena reset performance comparing individual free loop vs mp_reset batch reset.
 * Runs 1000 rounds of 500 allocations to measure the speedup of batch reset.
 */
void bench_arena_reset()
{
    printf("\n--- Benchmark 3: Fast Arena Reset (mp_reset x %d rounds of %d allocs) ---\n",
           ARENA_RESET_ROUNDS,
           ARENA_RESET_ALLOCS);
    memory_pool_t *pool = mp_create(8 * 1024 * 1024, MP_FLAG_DEFAULT);

    void *ptrs[ARENA_RESET_ALLOCS];

    // Standard Free loop
    double start_loop = get_time_sec();
    for (int round = 0; round < ARENA_RESET_ROUNDS; round++) {
        for (int i = 0; i < ARENA_RESET_ALLOCS; i++) {
            ptrs[i] = mp_alloc(pool, 128 + i * 8);
        }
        for (int i = 0; i < ARENA_RESET_ALLOCS; i++) {
            mp_free(pool, ptrs[i]);
        }
    }
    double time_loop = get_time_sec() - start_loop;

    // mp_reset Batch Free
    double start_reset = get_time_sec();
    for (int round = 0; round < ARENA_RESET_ROUNDS; round++) {
        for (int i = 0; i < ARENA_RESET_ALLOCS; i++) {
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
 * @brief Benchmarks arena workload (game/render style) comparing individual free vs arena reset.
 * Simulates per-frame allocation patterns.
 */
void bench_arena_workload(int rounds, int allocs_per_round);

/**
 * @brief Benchmarks multithreaded allocations comparing thread count scaling.
 * Measures throughput for N threads with M allocations each.
 */
void bench_multithreaded(int thread_count, int allocs_per_thread);

/**
 * @brief Entry point for cmem performance benchmarks.
 * Runs small alloc, medium alloc, arena reset, multithreaded, and arena workload benchmarks.
 * @return 0 on success.
 */
int main()
{
    printf("================ CMEM PERFORMANCE BENCHMARK ================\n");
    bench_small_allocs();
    bench_medium_allocs();
    bench_arena_reset();
    bench_multithreaded(4, MEDIUM_ALLOC_COUNT);
    bench_arena_workload(ARENA_RESET_ROUNDS, ARENA_RESET_ALLOCS);
    return 0;
}

/**
 * @brief Benchmarks multithreaded allocations comparing thread count scaling.
 * Measures throughput for N threads with M allocations each.
 */
typedef struct {
    memory_pool_t *pool;
    int thread_id;
    int alloc_count;
    double elapsed;
} bench_thread_arg_t;

static void *bench_thread_func(void *arg)
{
    bench_thread_arg_t *ta = (bench_thread_arg_t *)arg;
    void **ptrs = (void **)malloc(sizeof(void *) * ta->alloc_count);
    if (!ptrs) {
        return NULL;
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < ta->alloc_count; i++) {
        size_t sz = 32 + (i % 256);
        ptrs[i] = mp_alloc(ta->pool, sz);
    }
    for (int i = 0; i < ta->alloc_count; i++) {
        mp_free(ta->pool, ptrs[i]);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    ta->elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / NSEC_PER_SEC;

    free(ptrs);
    return NULL;
}

void bench_multithreaded(int thread_count, int allocs_per_thread)
{
    printf("\n--- Benchmark 4: Multithreaded Allocations (%d threads x %d ops) ---\n",
           thread_count,
           allocs_per_thread);

    memory_pool_t *pool =
        mp_create(64 * 1024 * 1024, MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE);

    bench_thread_arg_t *args =
        (bench_thread_arg_t *)malloc(sizeof(bench_thread_arg_t) * thread_count);
    pthread_t *threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
    if (!args || !threads) {
        fprintf(stderr, "Failed to allocate benchmark resources\n");
        mp_destroy(pool);
        free(args);
        free(threads);
        return;
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < thread_count; i++) {
        args[i].pool = pool;
        args[i].thread_id = i;
        args[i].alloc_count = allocs_per_thread;
        pthread_create(&threads[i], NULL, bench_thread_func, &args[i]);
    }

    for (int i = 0; i < thread_count; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double total_time = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / NSEC_PER_SEC;

    printf("  Total Time: %.4f sec\n", total_time);
    printf("  Throughput: %.2f Mops/sec\n",
           (thread_count * allocs_per_thread) / total_time / MILLION_OPS);

    for (int i = 0; i < thread_count; i++) {
        printf("  Thread %d: %.4f sec\n", i, args[i].elapsed);
    }

    mp_destroy(pool);
    free(args);
    free(threads);
}

/**
 * @brief Benchmarks arena workload (game/render style) comparing individual free vs arena reset.
 * Simulates per-frame allocation patterns.
 */
void bench_arena_workload(int rounds, int allocs_per_round)
{
    printf("\n--- Benchmark 5: Arena Workload (Game/Render style) ---\n");

    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_DEFAULT);
    void **ptrs = (void **)malloc(sizeof(void *) * allocs_per_round);
    if (!ptrs) {
        fprintf(stderr, "Failed to allocate benchmark resources\n");
        return;
    }

    // Method 1: Individual free
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int frame = 0; frame < rounds; frame++) {
        for (int i = 0; i < allocs_per_round; i++) {
            ptrs[i] = mp_alloc(pool, 64 + (i * 8));
        }
        for (int i = 0; i < allocs_per_round; i++) {
            mp_free(pool, ptrs[i]);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double time_individual =
        (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / NSEC_PER_SEC;

    // Method 2: Arena reset
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int frame = 0; frame < rounds; frame++) {
        for (int i = 0; i < allocs_per_round; i++) {
            mp_alloc(pool, 64 + (i * 8));
        }
        mp_reset(pool);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double time_reset = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / NSEC_PER_SEC;

    printf("  Individual Free:  %.4f sec\n", time_individual);
    printf("  Arena Reset:      %.4f sec\n", time_reset);
    printf("  Speedup:          %.2fx\n", time_individual / time_reset);

    mp_destroy(pool);
    free(ptrs);
}
