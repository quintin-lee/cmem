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

#define BENCH_FAST_PATH_ITERS 1000000
#define BENCH_SIZE_FIXED 32
#define BENCH_SIZE_MIXED_SPREAD 224
#define BENCH_SIZE_LARGE_BASE 4096
#define BENCH_SIZE_LARGE_SPREAD 16384
#define BENCH_BATCH_ELEMS 64
#define BENCH_BATCH_COUNT 10000
#define BENCH_THREAD_COUNTS 4
#define BENCH_COMPRESS_BLOCK_SIZE 4096
#define BENCH_COMPRESS_ITERS 10000
#define BENCH_PATTERN_OPS 200000
#define BENCH_PATTERN_LIVE_KEEP 25
#define BENCH_SIZE_MIXED_STEP 7
#define BENCH_SIZE_LARGE_STEP 13
#define BENCH_NSEC_PER_OP 1e9
#define BENCH_MB_FACTOR (1024.0 * 1024.0)
#define BENCH_XORSHIFT_SEED 12345u
#define XORSHIFT_A 13
#define XORSHIFT_B 17
#define XORSHIFT_C 5

#define BENCH_MEDIAN_RUNS 5

/**
 * @brief Inline compiler memory barrier and memory touch write to prevent DCE and simulate real
 * access.
 */
static inline void bench_escape(void *ptr)
{
    if (ptr) {
        ((volatile char *)ptr)[0] = 1;
    }
    __asm__ __volatile__("" : : "r"(ptr) : "memory");
}

static void bench_warmup(memory_pool_t *pool)
{
    for (int i = 0; i < 10000; i++) {
        void *sys_p = malloc(64);
        bench_escape(sys_p);
        free(sys_p);

        if (pool) {
            void *mp_p = mp_alloc(pool, 64);
            bench_escape(mp_p);
            mp_free(pool, mp_p);
        }
    }
}

static int double_cmp(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double get_median_time(double times[], int count)
{
    qsort(times, count, sizeof(double), double_cmp);
    return times[count / 2];
}

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

    // 1. System Malloc Benchmark (interleaved alloc/free — realistic)
    double start_sys = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
        void *ptr = malloc(sz);
        bench_escape(ptr);
        free(ptr);
    }
    double time_sys = get_time_sec() - start_sys;

    // 2. cmem Benchmark (interleaved alloc/free — realistic)
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    double start_mp = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
        void *ptr = mp_alloc(pool, sz);
        mp_free(pool, ptr);
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
        bench_escape(ptrs[i]);
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
 * @brief Benchmarks FAST_PATH flag impact on small interleaved allocations.
 */
void bench_fast_path();

/**
 * @brief Benchmarks size-class distribution (fixed, mixed, large TLSF).
 */
void bench_size_distribution();

/**
 * @brief Benchmarks batch allocation APIs vs single alloc/free loops.
 */
void bench_batch_apis();

/**
 * @brief Benchmarks thread-count scaling for single vs multi-arena pools.
 */
void bench_thread_scaling();

/**
 * @brief Benchmarks compressed-storage compress/decompress throughput.
 */
void bench_compressed_storage();

/**
 * @brief Benchmarks allocation patterns: interleaved, batch, random, live-set.
 */
void bench_allocation_patterns();

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
    bench_fast_path();
    bench_size_distribution();
    bench_batch_apis();
    bench_thread_scaling();
    bench_compressed_storage();
    bench_allocation_patterns();
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

/**
 * @brief Compares MP_FLAG_THREAD_LOCAL_CACHE with and without MP_FLAG_FAST_PATH
 * on the same interleaved 32-256 byte workload as Benchmark 1.
 */
void bench_fast_path()
{
    printf("\n--- Benchmark 6: FAST_PATH (interleaved %d x 32-256B) ---\n", BENCH_FAST_PATH_ITERS);

    memory_pool_t *plain_pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    double start = get_time_sec();
    for (int i = 0; i < BENCH_FAST_PATH_ITERS; i++) {
        size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
        void *ptr = mp_alloc(plain_pool, sz);
        mp_free(plain_pool, ptr);
    }
    double time_plain = get_time_sec() - start;
    mp_destroy(plain_pool);

    memory_pool_t *fast_pool =
        mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE | MP_FLAG_FAST_PATH);
    start = get_time_sec();
    for (int i = 0; i < BENCH_FAST_PATH_ITERS; i++) {
        size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
        void *ptr = mp_alloc(fast_pool, sz);
        mp_free(fast_pool, ptr);
    }
    double time_fast = get_time_sec() - start;
    mp_destroy(fast_pool);

    printf("  THREAD_LOCAL_CACHE        : %.4f sec (%.2f Mops/sec)\n",
           time_plain,
           (BENCH_FAST_PATH_ITERS / time_plain) / MILLION_OPS);
    printf("  +MP_FLAG_FAST_PATH        : %.4f sec (%.2f Mops/sec)\n",
           time_fast,
           (BENCH_FAST_PATH_ITERS / time_fast) / MILLION_OPS);
    printf("  Speedup                   : %.2fx\n", time_plain / time_fast);
}

/**
 * @brief Compares cmem vs system malloc across three size distributions:
 * fixed 32-byte, irregular mixed 16-240 byte, and large TLSF 4-20KB.
 */
void bench_size_distribution()
{
    printf("\n--- Benchmark 7: Size Distributions (1M interleaved each) ---\n");

    /* (a) fixed 32-byte — single slab class */
    double start = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        void *ptr = malloc(BENCH_SIZE_FIXED);
        bench_escape(ptr);
        free(ptr);
    }
    double sys_fixed = get_time_sec() - start;
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    start = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        void *ptr = mp_alloc(pool, BENCH_SIZE_FIXED);
        mp_free(pool, ptr);
    }
    double cmem_fixed = get_time_sec() - start;
    printf("  Fixed 32B        : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (SMALL_ALLOC_COUNT / sys_fixed) / MILLION_OPS,
           (SMALL_ALLOC_COUNT / cmem_fixed) / MILLION_OPS,
           sys_fixed / cmem_fixed);

    /* (b) irregular mixed sizes 16-240 (i*7 avoids power-of-2 alignment) */
    start = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        size_t sz = 16 + (i * BENCH_SIZE_MIXED_STEP) % BENCH_SIZE_MIXED_SPREAD;
        void *ptr = malloc(sz);
        bench_escape(ptr);
        free(ptr);
    }
    double sys_mixed = get_time_sec() - start;
    start = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        size_t sz = 16 + (i * BENCH_SIZE_MIXED_STEP) % BENCH_SIZE_MIXED_SPREAD;
        void *ptr = mp_alloc(pool, sz);
        mp_free(pool, ptr);
    }
    double cmem_mixed = get_time_sec() - start;
    printf("  Mixed 16-240B    : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (SMALL_ALLOC_COUNT / sys_mixed) / MILLION_OPS,
           (SMALL_ALLOC_COUNT / cmem_mixed) / MILLION_OPS,
           sys_mixed / cmem_mixed);

    /* (c) large 4-20KB — TLSF path */
    start = get_time_sec();
    for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
        size_t sz = BENCH_SIZE_LARGE_BASE + (i * BENCH_SIZE_LARGE_STEP) % BENCH_SIZE_LARGE_SPREAD;
        void *ptr = malloc(sz);
        bench_escape(ptr);
        free(ptr);
    }
    double sys_large = get_time_sec() - start;
    start = get_time_sec();
    for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
        size_t sz = BENCH_SIZE_LARGE_BASE + (i * BENCH_SIZE_LARGE_STEP) % BENCH_SIZE_LARGE_SPREAD;
        void *ptr = mp_alloc(pool, sz);
        mp_free(pool, ptr);
    }
    double cmem_large = get_time_sec() - start;
    printf("  Large 4-20KB     : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (MEDIUM_ALLOC_COUNT / sys_large) / MILLION_OPS,
           (MEDIUM_ALLOC_COUNT / cmem_large) / MILLION_OPS,
           sys_large / cmem_large);

    mp_destroy(pool);
}

/**
 * @brief Compares mp_alloc_batch/mp_free_batch against individual alloc/free
 * loops for a fixed 32-byte block, 64 elements per batch, 10,000 batches.
 */
void bench_batch_apis()
{
    printf("\n--- Benchmark 8: Batch APIs (64 elems x %d batches, 32B) ---\n", BENCH_BATCH_COUNT);

    void **ptrs = (void **)malloc(sizeof(void *) * BENCH_BATCH_ELEMS);
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);

    /* single alloc/free loops */
    double start = get_time_sec();
    for (int batch_idx = 0; batch_idx < BENCH_BATCH_COUNT; batch_idx++) {
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            ptrs[i] = mp_alloc(pool, BENCH_SIZE_FIXED);
        }
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            mp_free(pool, ptrs[i]);
        }
    }
    double time_single = get_time_sec() - start;

    /* batch APIs */
    start = get_time_sec();
    for (int batch_idx = 0; batch_idx < BENCH_BATCH_COUNT; batch_idx++) {
        size_t got = mp_alloc_batch(pool, BENCH_SIZE_FIXED, ptrs, BENCH_BATCH_ELEMS);
        if (got != BENCH_BATCH_ELEMS) {
            fprintf(stderr, "mp_alloc_batch returned %zu (expected %d)\n", got, BENCH_BATCH_ELEMS);
        }
        mp_free_batch(pool, ptrs, BENCH_BATCH_ELEMS);
    }
    double time_batch = get_time_sec() - start;

    size_t total_ops = (size_t)BENCH_BATCH_COUNT * (size_t)BENCH_BATCH_ELEMS;
    printf("  Single loops : %.4f sec (%.2f Mops/sec, %.2f ns/op)\n",
           time_single,
           (total_ops / time_single) / MILLION_OPS,
           (time_single / total_ops) * BENCH_NSEC_PER_OP);
    printf("  Batch APIs   : %.4f sec (%.2f Mops/sec, %.2f ns/op)\n",
           time_batch,
           (total_ops / time_batch) / MILLION_OPS,
           (time_batch / total_ops) * BENCH_NSEC_PER_OP);
    printf("  Batch speedup: %.2fx\n", time_single / time_batch);

    mp_destroy(pool);
    free(ptrs);
}

typedef struct {
    memory_pool_t *pool;
    int alloc_count;
} bench_scaling_arg_t;

static void *bench_scaling_thread_func(void *arg)
{
    bench_scaling_arg_t *ta = (bench_scaling_arg_t *)arg;
    for (int i = 0; i < ta->alloc_count; i++) {
        size_t sz = 32 + (i % 256);
        void *ptr = mp_alloc(ta->pool, sz);
        mp_free(ta->pool, ptr);
    }
    return NULL;
}

static double bench_scaling_run(memory_pool_t *pool, int threads, int allocs_per_thread)
{
    pthread_t *threads_arr = (pthread_t *)malloc(sizeof(pthread_t) * threads);
    bench_scaling_arg_t *args =
        (bench_scaling_arg_t *)malloc(sizeof(bench_scaling_arg_t) * threads);
    if (!threads_arr || !args) {
        free(threads_arr);
        free(args);
        return -1.0;
    }
    for (int i = 0; i < threads; i++) {
        args[i].pool = pool;
        args[i].alloc_count = allocs_per_thread;
        pthread_create(&threads_arr[i], NULL, bench_scaling_thread_func, &args[i]);
    }
    double start = get_time_sec();
    for (int i = 0; i < threads; i++) {
        pthread_join(threads_arr[i], NULL);
    }
    double time = get_time_sec() - start;
    free(threads_arr);
    free(args);
    return (double)threads * allocs_per_thread / time / MILLION_OPS;
}

/**
 * @brief Measures throughput scaling across {1,2,4,8} threads for a
 * single shared pool vs a multi-arena pool, reporting Mops/sec and
 * scaling efficiency (x-thread rate / 1-thread rate).
 */
void bench_thread_scaling()
{
    printf("\n--- Benchmark 9: Thread Scaling (1-8 threads x 100k interleaved) ---\n");

    int thread_counts[BENCH_THREAD_COUNTS];
    thread_counts[0] = 1;
    thread_counts[1] = 2;
    thread_counts[2] = 4;
    thread_counts[3] = 8;
    const int allocs_per_thread = 100000;

    printf("  Mode       | Threads | Mops/sec | Efficiency\n");
    printf("  -----------+---------+----------+-----------\n");

    for (int mode_idx = 0; mode_idx < 2; mode_idx++) {
        const char *mode_name = (mode_idx == 0) ? "single      " : "multi-arena ";
        mp_flags_t flags = MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE;
        if (mode_idx == 1) {
            flags |= MP_FLAG_MULTI_ARENA;
        }
        memory_pool_t *pool = mp_create(128 * 1024 * 1024, flags);
        double base = 0.0;
        for (int thread_idx = 0; thread_idx < BENCH_THREAD_COUNTS; thread_idx++) {
            int nthreads = thread_counts[thread_idx];
            double mops = bench_scaling_run(pool, nthreads, allocs_per_thread);
            if (mops < 0.0) {
                fprintf(stderr, "Thread scaling run failed\n");
                break;
            }
            if (thread_idx == 0) {
                base = mops;
            }
            printf("  %s | %d       | %.2f     | %.2fx\n", mode_name, nthreads, mops, mops / base);
        }
        mp_destroy(pool);
    }
}

/**
 * @brief Measures compressed-storage throughput: compress 4KB repetitive
 * blocks, decompress a stored handle, and report the compression ratio.
 */
void bench_compressed_storage()
{
    printf("\n--- Benchmark 10: Compressed Storage (4KB blocks x %d) ---\n", BENCH_COMPRESS_ITERS);

    memory_pool_t *pool = mp_create(64 * 1024 * 1024, MP_FLAG_DEFAULT);
    void *data = mp_alloc(pool, BENCH_COMPRESS_BLOCK_SIZE);
    char *bytes = (char *)data;
    for (int i = 0; i < BENCH_COMPRESS_BLOCK_SIZE; i++) {
        bytes[i] = (char)('a' + (i % 4));
    }

    /* compress path: mp_compress_block TRANSFERS ownership of data on
     * success, so each iteration must re-allocate and refill the buffer. */
    double start = get_time_sec();
    size_t total_in = 0;
    size_t total_out = 0;
    for (int i = 0; i < BENCH_COMPRESS_ITERS; i++) {
        compressed_handle_t comp_handle = mp_compress_block(pool, data, BENCH_COMPRESS_BLOCK_SIZE);
        if (comp_handle != 0) {
            mp_free_compressed(pool, comp_handle);
            total_in += BENCH_COMPRESS_BLOCK_SIZE;
            total_out += BENCH_COMPRESS_BLOCK_SIZE;
        }
        data = mp_alloc(pool, BENCH_COMPRESS_BLOCK_SIZE);
        bytes = (char *)data;
        for (int j = 0; j < BENCH_COMPRESS_BLOCK_SIZE; j++) {
            bytes[j] = (char)('a' + (j % 4));
        }
    }
    double time_compress = get_time_sec() - start;

    /* decompress path — one stored handle, decompressed repeatedly */
    compressed_handle_t handle = mp_compress_block(pool, data, BENCH_COMPRESS_BLOCK_SIZE);
    if (handle == 0) {
        fprintf(stderr, "Benchmark 10: initial compress failed\n");
        mp_free(pool, data);
        mp_destroy(pool);
        return;
    }
    size_t comp_used = 0;
    size_t comp_budget = 0;
    size_t comp_blocks = 0;
    if (mp_get_compressed_stats(pool, &comp_used, &comp_budget, &comp_blocks)) {
        total_out = comp_used;
    }
    start = get_time_sec();
    void *out_buf = NULL;
    for (int i = 0; i < BENCH_COMPRESS_ITERS; i++) {
        out_buf = mp_decompress_block(pool, handle);
        if (out_buf) {
            mp_free(pool, out_buf);
        }
    }
    double time_decompress = get_time_sec() - start;

    printf("  Compress   : %.2f MB/s (%.2f Mops/sec)\n",
           ((double)total_in / time_compress) / BENCH_MB_FACTOR,
           ((double)total_in / time_compress) / MILLION_OPS);
    printf("  Decompress : %.2f MB/s (%.2f Mops/sec)\n",
           ((double)total_in / time_decompress) / BENCH_MB_FACTOR,
           ((double)total_in / time_decompress) / MILLION_OPS);
    printf("  Compressed : %zu bytes stored for %zu input (ratio %.2f:1)\n",
           total_out,
           total_in,
           total_in > 0 ? (double)total_in / (double)total_out : 0.0);

    mp_free_compressed(pool, handle);
    mp_free(pool, data);
    mp_destroy(pool);
}

/**
 * @brief Compares cmem vs system malloc across four allocation patterns:
 * interleaved, batch alloc-then-free, random-size, and live-set.
 */
void bench_allocation_patterns()
{
    printf("\n--- Benchmark 11: Allocation Patterns (%d ops, 32-256B) ---\n", BENCH_PATTERN_OPS);

    void **ptrs = (void **)malloc(sizeof(void *) * BENCH_PATTERN_OPS);
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    const size_t live_count = BENCH_PATTERN_OPS * BENCH_PATTERN_LIVE_KEEP / 100;

    /* (a) interleaved */
    double start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
        void *ptr = malloc(sz);
        bench_escape(ptr);
        free(ptr);
    }
    double sys_a = get_time_sec() - start;
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
        void *ptr = mp_alloc(pool, sz);
        mp_free(pool, ptr);
    }
    double cmem_a = get_time_sec() - start;

    /* (b) batch alloc-then-free */
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        ptrs[i] = malloc(32 + (i % SMALL_ALLOC_SPREAD));
        bench_escape(ptrs[i]);
    }
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        free(ptrs[i]);
    }
    double sys_b = get_time_sec() - start;
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        ptrs[i] = mp_alloc(pool, 32 + (i % SMALL_ALLOC_SPREAD));
    }
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        mp_free(pool, ptrs[i]);
    }
    double cmem_b = get_time_sec() - start;

    /* (c) random sizes via xorshift */
    uint32_t seed = BENCH_XORSHIFT_SEED;
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        seed ^= seed << XORSHIFT_A;
        seed ^= seed >> XORSHIFT_B;
        seed ^= seed << XORSHIFT_C;
        size_t sz = 32 + (seed % SMALL_ALLOC_SPREAD);
        void *ptr = malloc(sz);
        bench_escape(ptr);
        free(ptr);
    }
    double sys_c = get_time_sec() - start;
    seed = BENCH_XORSHIFT_SEED;
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        seed ^= seed << XORSHIFT_A;
        seed ^= seed >> XORSHIFT_B;
        seed ^= seed << XORSHIFT_C;
        size_t sz = 32 + (seed % SMALL_ALLOC_SPREAD);
        void *ptr = mp_alloc(pool, sz);
        mp_free(pool, ptr);
    }
    double cmem_c = get_time_sec() - start;

    /* (d) live set: allocate all, free 75% (timed), free rest after */
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        ptrs[i] = malloc(32 + (i % SMALL_ALLOC_SPREAD));
        bench_escape(ptrs[i]);
    }
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS - (int)live_count; i++) {
        free(ptrs[i]);
    }
    double sys_d = get_time_sec() - start;
    for (int i = BENCH_PATTERN_OPS - (int)live_count; i < BENCH_PATTERN_OPS; i++) {
        free(ptrs[i]);
    }

    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        ptrs[i] = mp_alloc(pool, 32 + (i % SMALL_ALLOC_SPREAD));
    }
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS - (int)live_count; i++) {
        mp_free(pool, ptrs[i]);
    }
    double cmem_d = get_time_sec() - start;
    for (int i = BENCH_PATTERN_OPS - (int)live_count; i < BENCH_PATTERN_OPS; i++) {
        mp_free(pool, ptrs[i]);
    }

    printf("  Interleaved   : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (BENCH_PATTERN_OPS / sys_a) / MILLION_OPS,
           (BENCH_PATTERN_OPS / cmem_a) / MILLION_OPS,
           sys_a / cmem_a);
    printf("  Batch         : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (BENCH_PATTERN_OPS / sys_b) / MILLION_OPS,
           (BENCH_PATTERN_OPS / cmem_b) / MILLION_OPS,
           sys_b / cmem_b);
    printf("  Random sizes  : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (BENCH_PATTERN_OPS / sys_c) / MILLION_OPS,
           (BENCH_PATTERN_OPS / cmem_c) / MILLION_OPS,
           sys_c / cmem_c);
    printf("  Live set 25%%  : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (BENCH_PATTERN_OPS / sys_d) / MILLION_OPS,
           (BENCH_PATTERN_OPS / cmem_d) / MILLION_OPS,
           sys_d / cmem_d);

    mp_destroy(pool);
    free(ptrs);
}
