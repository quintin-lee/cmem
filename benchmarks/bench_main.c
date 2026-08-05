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
#define BENCH_COMPRESS_ITERS 2000
#define BENCH_MOPS_THRESHOLD 0.01
#define BENCH_KOPS_FACTOR 1000.0
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
#define BENCH_WARMUP_ITERS 10000

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
    for (int i = 0; i < BENCH_WARMUP_ITERS; i++) {
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

static int double_cmp(const void *lhs, const void *rhs)
{
    double da = *(const double *)lhs;
    double db = *(const double *)rhs;
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

    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    bench_warmup(pool);

    double sys_times[BENCH_MEDIAN_RUNS];
    for (int run = 0; run < BENCH_MEDIAN_RUNS; run++) {
        double start = get_time_sec();
        for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
            size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
            void *ptr = malloc(sz);
            bench_escape(ptr);
            free(ptr);
        }
        sys_times[run] = get_time_sec() - start;
    }
    double time_sys = get_median_time(sys_times, BENCH_MEDIAN_RUNS);

    double mp_times[BENCH_MEDIAN_RUNS];
    for (int run = 0; run < BENCH_MEDIAN_RUNS; run++) {
        double start = get_time_sec();
        for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
            size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
            void *ptr = mp_alloc(pool, sz);
            bench_escape(ptr);
            mp_free(pool, ptr);
        }
        mp_times[run] = get_time_sec() - start;
    }
    double time_mp = get_median_time(mp_times, BENCH_MEDIAN_RUNS);

    printf("  System Malloc Time : %.4f sec (%.2f Mops/sec)\n",
           time_sys,
           (SMALL_ALLOC_COUNT / time_sys) / MILLION_OPS);
    printf("  cmem Time          : %.4f sec (%.2f Mops/sec)\n",
           time_mp,
           (SMALL_ALLOC_COUNT / time_mp) / MILLION_OPS);
    if (time_sys >= time_mp) {
        printf("  Speedup            : %.2fx faster!\n", time_sys / time_mp);
    } else {
        printf("  Ratio              : %.2fx (%.2fx slower than malloc)\n",
               time_sys / time_mp,
               time_mp / time_sys);
    }

    mp_destroy(pool);
}

void bench_medium_allocs()
{
    printf("\n--- Benchmark 2: Medium Dynamic Allocations (1KB-64KB x %d ops) ---\n",
           MEDIUM_ALLOC_COUNT);
    void **ptrs = (void **)malloc(sizeof(void *) * MEDIUM_ALLOC_COUNT);

    memory_pool_t *pool = mp_create(64 * 1024 * 1024, MP_FLAG_DEFAULT);
    bench_warmup(pool);

    double sys_times[BENCH_MEDIAN_RUNS];
    for (int run = 0; run < BENCH_MEDIAN_RUNS; run++) {
        double start = get_time_sec();
        for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
            size_t sz = 1024 + (i % MEDIUM_ALLOC_SPREAD);
            ptrs[i] = malloc(sz);
            bench_escape(ptrs[i]);
        }
        for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
            free(ptrs[i]);
        }
        sys_times[run] = get_time_sec() - start;
    }
    double time_sys = get_median_time(sys_times, BENCH_MEDIAN_RUNS);

    double mp_times[BENCH_MEDIAN_RUNS];
    for (int run = 0; run < BENCH_MEDIAN_RUNS; run++) {
        double start = get_time_sec();
        for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
            size_t sz = 1024 + (i % MEDIUM_ALLOC_SPREAD);
            ptrs[i] = mp_alloc(pool, sz);
            bench_escape(ptrs[i]);
        }
        for (int i = 0; i < MEDIUM_ALLOC_COUNT; i++) {
            mp_free(pool, ptrs[i]);
        }
        mp_times[run] = get_time_sec() - start;
    }
    double time_mp = get_median_time(mp_times, BENCH_MEDIAN_RUNS);

    printf("  System Malloc Time : %.4f sec\n", time_sys);
    printf("  cmem Time          : %.4f sec\n", time_mp);
    if (time_sys >= time_mp) {
        printf("  Speedup            : %.2fx faster!\n", time_sys / time_mp);
    } else {
        printf("  Ratio              : %.2fx (%.2fx slower than malloc)\n",
               time_sys / time_mp,
               time_mp / time_sys);
    }

    mp_destroy(pool);
    free((void *)ptrs);
}

void bench_arena_reset()
{
    printf("\n--- Benchmark 3: Fast Arena Reset (mp_reset x %d rounds of %d allocs) ---\n",
           ARENA_RESET_ROUNDS,
           ARENA_RESET_ALLOCS);
    memory_pool_t *pool = mp_create(8 * 1024 * 1024, MP_FLAG_DEFAULT);
    bench_warmup(pool);

    void *ptrs[ARENA_RESET_ALLOCS];

    double loop_times[BENCH_MEDIAN_RUNS];
    for (int run = 0; run < BENCH_MEDIAN_RUNS; run++) {
        double start = get_time_sec();
        for (int round = 0; round < ARENA_RESET_ROUNDS; round++) {
            for (int i = 0; i < ARENA_RESET_ALLOCS; i++) {
                ptrs[i] = mp_alloc(pool, 128 + i * 8);
                bench_escape(ptrs[i]);
            }
            for (int i = 0; i < ARENA_RESET_ALLOCS; i++) {
                mp_free(pool, ptrs[i]);
            }
        }
        loop_times[run] = get_time_sec() - start;
    }
    double time_loop = get_median_time(loop_times, BENCH_MEDIAN_RUNS);

    double reset_times[BENCH_MEDIAN_RUNS];
    for (int run = 0; run < BENCH_MEDIAN_RUNS; run++) {
        double start = get_time_sec();
        for (int round = 0; round < ARENA_RESET_ROUNDS; round++) {
            for (int i = 0; i < ARENA_RESET_ALLOCS; i++) {
                void *ptr = mp_alloc(pool, 128 + i * 8);
                bench_escape(ptr);
            }
            mp_reset(pool);
        }
        reset_times[run] = get_time_sec() - start;
    }
    double time_reset = get_median_time(reset_times, BENCH_MEDIAN_RUNS);

    printf("  Individual Free Loop Time : %.5f sec\n", time_loop);
    printf("  Arena mp_reset Batch Time : %.5f sec\n", time_reset);
    printf("  Speedup                   : %.2fx faster!\n", time_loop / time_reset);

    mp_destroy(pool);
}

void bench_arena_workload(int rounds, int allocs_per_round);
void bench_multithreaded(int thread_count, int allocs_per_thread);
void bench_fast_path(void);
void bench_size_distribution(void);
void bench_batch_apis(void);
void bench_thread_scaling(void);
void bench_compressed_storage(void);
void bench_allocation_patterns(void);

/**
 * @brief Entry point for cmem performance benchmarks.
 * @return 0 on success.
 */
int main(void)
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
        bench_escape(ptrs[i]);
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
    bench_warmup(pool);

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

    double run_times[BENCH_MEDIAN_RUNS];
    for (int run = 0; run < BENCH_MEDIAN_RUNS; run++) {
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
        run_times[run] = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / NSEC_PER_SEC;
    }
    double total_time = get_median_time(run_times, BENCH_MEDIAN_RUNS);

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

void bench_arena_workload(int rounds, int allocs_per_round)
{
    printf("\n--- Benchmark 5: Arena Workload (Game/Render style) ---\n");

    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_DEFAULT);
    bench_warmup(pool);
    void **ptrs = (void **)malloc(sizeof(void *) * allocs_per_round);
    if (!ptrs) {
        fprintf(stderr, "Failed to allocate benchmark resources\n");
        mp_destroy(pool);
        return;
    }

    double ind_times[BENCH_MEDIAN_RUNS];
    for (int run = 0; run < BENCH_MEDIAN_RUNS; run++) {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (int frame = 0; frame < rounds; frame++) {
            for (int i = 0; i < allocs_per_round; i++) {
                ptrs[i] = mp_alloc(pool, 64 + (i * 8));
                bench_escape(ptrs[i]);
            }
            for (int i = 0; i < allocs_per_round; i++) {
                mp_free(pool, ptrs[i]);
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        ind_times[run] = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / NSEC_PER_SEC;
    }
    double time_individual = get_median_time(ind_times, BENCH_MEDIAN_RUNS);

    double reset_times[BENCH_MEDIAN_RUNS];
    for (int run = 0; run < BENCH_MEDIAN_RUNS; run++) {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (int frame = 0; frame < rounds; frame++) {
            for (int i = 0; i < allocs_per_round; i++) {
                void *ptr = mp_alloc(pool, 64 + (i * 8));
                bench_escape(ptr);
            }
            mp_reset(pool);
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        reset_times[run] =
            (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / NSEC_PER_SEC;
    }
    double time_reset = get_median_time(reset_times, BENCH_MEDIAN_RUNS);

    printf("  Individual Free:  %.4f sec\n", time_individual);
    printf("  Arena Reset:      %.4f sec\n", time_reset);
    printf("  Speedup:          %.2fx\n", time_individual / time_reset);

    mp_destroy(pool);
    free(ptrs);
}

void bench_fast_path()
{
    printf("\n--- Benchmark 6: FAST_PATH (interleaved %d x 32-256B) ---\n", BENCH_FAST_PATH_ITERS);

    memory_pool_t *plain_pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    bench_warmup(plain_pool);

    double plain_times[BENCH_MEDIAN_RUNS];
    for (int run = 0; run < BENCH_MEDIAN_RUNS; run++) {
        double start = get_time_sec();
        for (int i = 0; i < BENCH_FAST_PATH_ITERS; i++) {
            size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
            void *ptr = mp_alloc(plain_pool, sz);
            bench_escape(ptr);
            mp_free(plain_pool, ptr);
        }
        plain_times[run] = get_time_sec() - start;
    }
    double time_plain = get_median_time(plain_times, BENCH_MEDIAN_RUNS);
    mp_destroy(plain_pool);

    memory_pool_t *fast_pool =
        mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE | MP_FLAG_FAST_PATH);
    bench_warmup(fast_pool);

    double fast_times[BENCH_MEDIAN_RUNS];
    for (int run = 0; run < BENCH_MEDIAN_RUNS; run++) {
        double start = get_time_sec();
        for (int i = 0; i < BENCH_FAST_PATH_ITERS; i++) {
            size_t sz = 32 + (i % SMALL_ALLOC_SPREAD);
            void *ptr = mp_alloc_fast(fast_pool, sz);
            bench_escape(ptr);
            mp_free_fast(fast_pool, ptr);
        }
        fast_times[run] = get_time_sec() - start;
    }
    double time_fast = get_median_time(fast_times, BENCH_MEDIAN_RUNS);
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

    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    bench_warmup(pool);

    /* (a) fixed 32-byte — single slab class */
    double start = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        void *ptr = malloc(BENCH_SIZE_FIXED);
        bench_escape(ptr);
        free(ptr);
    }
    double sys_fixed = get_time_sec() - start;

    start = get_time_sec();
    for (int i = 0; i < SMALL_ALLOC_COUNT; i++) {
        void *ptr = mp_alloc(pool, BENCH_SIZE_FIXED);
        bench_escape(ptr);
        mp_free(pool, ptr);
    }
    double cmem_fixed = get_time_sec() - start;
    printf("  Fixed 32B        : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (SMALL_ALLOC_COUNT / sys_fixed) / MILLION_OPS,
           (SMALL_ALLOC_COUNT / cmem_fixed) / MILLION_OPS,
           sys_fixed / cmem_fixed);

    /* (b) irregular mixed sizes 16-240 */
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
        bench_escape(ptr);
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
        bench_escape(ptr);
        mp_free(pool, ptr);
    }
    double cmem_large = get_time_sec() - start;
    printf("  Large 4-20KB     : malloc %.2f | cmem %.2f Mops/sec (%.2fx)\n",
           (MEDIUM_ALLOC_COUNT / sys_large) / MILLION_OPS,
           (MEDIUM_ALLOC_COUNT / cmem_large) / MILLION_OPS,
           sys_large / cmem_large);

    mp_destroy(pool);
}

void bench_batch_apis()
{
    printf("\n--- Benchmark 8: Batch APIs (64 elems x %d batches, 32B) ---\n", BENCH_BATCH_COUNT);

    void **ptrs = (void **)malloc(sizeof(void *) * BENCH_BATCH_ELEMS);
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    bench_warmup(pool);

    /* single alloc/free loops */
    double start = get_time_sec();
    for (int batch_idx = 0; batch_idx < BENCH_BATCH_COUNT; batch_idx++) {
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            ptrs[i] = mp_alloc(pool, BENCH_SIZE_FIXED);
            bench_escape(ptrs[i]);
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
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            bench_escape(ptrs[i]);
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
        bench_escape(ptr);
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
        bench_warmup(pool);
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

void bench_compressed_storage()
{
    printf("\n--- Benchmark 10: Compressed Storage (4KB blocks x %d) ---\n", BENCH_COMPRESS_ITERS);

    memory_pool_t *pool = mp_create(64 * 1024 * 1024, MP_FLAG_DEFAULT);
    bench_warmup(pool);

    compressed_handle_t *handles =
        (compressed_handle_t *)calloc(BENCH_COMPRESS_ITERS, sizeof(compressed_handle_t));
    if (!handles) {
        fprintf(stderr, "Benchmark 10: handles allocation failed\n");
        mp_destroy(pool);
        return;
    }

    void *data = mp_alloc(pool, BENCH_COMPRESS_BLOCK_SIZE);
    char *bytes = (char *)data;
    static const char pattern[] = "{\"id\":12345,\"name\":\"cmem_tiered_memory_pool\",\"status\":"
                                  "\"active\",\"tags\":[\"high_performance\",\"c11\"]}\n";
    size_t pat_len = sizeof(pattern) - 1;
    for (int i = 0; i < BENCH_COMPRESS_BLOCK_SIZE; i++) {
        bytes[i] = pattern[i % pat_len];
    }

    double start = get_time_sec();
    size_t total_in = 0;
    for (int i = 0; i < BENCH_COMPRESS_ITERS; i++) {
        handles[i] = mp_compress_block(pool, data, BENCH_COMPRESS_BLOCK_SIZE);
        if (handles[i] != 0) {
            total_in += BENCH_COMPRESS_BLOCK_SIZE;
        }
    }
    double time_compress = get_time_sec() - start;

    size_t total_out = 0;
    size_t comp_budget = 0;
    size_t comp_blocks = 0;
    mp_get_compressed_stats(pool, &total_out, &comp_budget, &comp_blocks);

    start = get_time_sec();
    for (int i = 0; i < BENCH_COMPRESS_ITERS; i++) {
        if (handles[i] != 0) {
            void *out_buf = mp_decompress_block(pool, handles[i]);
            if (out_buf) {
                bench_escape(out_buf);
                mp_free(pool, out_buf);
            }
        }
    }
    double time_decompress = get_time_sec() - start;

    double compress_mops =
        (time_compress > 0) ? ((double)BENCH_COMPRESS_ITERS / time_compress) / MILLION_OPS : 0.0;
    double decompress_mops = (time_decompress > 0)
                                 ? ((double)BENCH_COMPRESS_ITERS / time_decompress) / MILLION_OPS
                                 : 0.0;
    double compress_mbps =
        (time_compress > 0) ? (((double)total_in / time_compress) / BENCH_MB_FACTOR) : 0.0;
    double decompress_mbps =
        (time_decompress > 0) ? (((double)total_in / time_decompress) / BENCH_MB_FACTOR) : 0.0;

    if (compress_mops < BENCH_MOPS_THRESHOLD) {
        printf("  Compress   : %.2f MB/s (%.2f Kops/sec)\n",
               compress_mbps,
               compress_mops * BENCH_KOPS_FACTOR);
    } else {
        printf("  Compress   : %.2f MB/s (%.2f Mops/sec)\n", compress_mbps, compress_mops);
    }
    if (decompress_mops < BENCH_MOPS_THRESHOLD) {
        printf("  Decompress : %.2f MB/s (%.2f Kops/sec)\n",
               decompress_mbps,
               decompress_mops * BENCH_KOPS_FACTOR);
    } else {
        printf("  Decompress : %.2f MB/s (%.2f Mops/sec)\n", decompress_mbps, decompress_mops);
    }
    printf("  Compressed : %zu bytes stored for %zu input (ratio %.2f:1)\n",
           total_out,
           total_in,
           total_out > 0 ? (double)total_in / (double)total_out : 0.0);

    for (int i = 0; i < BENCH_COMPRESS_ITERS; i++) {
        if (handles[i] != 0) {
            mp_free_compressed(pool, handles[i]);
        }
    }
    free(handles);
    mp_free(pool, data);
    mp_destroy(pool);
}

void bench_allocation_patterns()
{
    printf("\n--- Benchmark 11: Allocation Patterns (%d ops, 32-256B) ---\n", BENCH_PATTERN_OPS);

    void **ptrs = (void **)malloc(sizeof(void *) * BENCH_PATTERN_OPS);
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    bench_warmup(pool);
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
        bench_escape(ptr);
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
        bench_escape(ptrs[i]);
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
        bench_escape(ptr);
        mp_free(pool, ptr);
    }
    double cmem_c = get_time_sec() - start;

    /* (d) live set: allocate all, free 75% (timed), free rest after */
    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        ptrs[i] = malloc(32 + (i % SMALL_ALLOC_SPREAD));
        bench_escape(ptrs[i]);
    }
    for (int i = 0; i < BENCH_PATTERN_OPS - (int)live_count; i++) {
        free(ptrs[i]);
    }
    double sys_d = get_time_sec() - start;
    for (int i = BENCH_PATTERN_OPS - (int)live_count; i < BENCH_PATTERN_OPS; i++) {
        free(ptrs[i]);
    }

    start = get_time_sec();
    for (int i = 0; i < BENCH_PATTERN_OPS; i++) {
        ptrs[i] = mp_alloc(pool, 32 + (i % SMALL_ALLOC_SPREAD));
        bench_escape(ptrs[i]);
    }
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
