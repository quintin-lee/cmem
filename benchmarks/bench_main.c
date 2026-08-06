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
#define BENCH_SIZE_TLSF 4096                  /* TLSF-class element size for batch benchmark */
#define BENCH_BATCH_SCALE_MAX_ELEMS 256       /* largest batch in scaling matrix */
#define BENCH_BATCH_OS_SIZE (5 * 1024 * 1024) /* OS-tier element size */
#define BENCH_BATCH_OS_ELEMS 8                /* elements per OS-tier batch */
#define BENCH_BATCH_OS_ITERS 200              /* OS-tier batch rounds */
#define BENCH_BATCH_MIXED_SIZE_MID 512        /* middle-size element in mixed rounds */
#define BENCH_BATCH_MIXED_ITERS 5000          /* mixed-size rounds */
#define BENCH_BATCH_THREAD_ALLOCS 100000      /* per-thread allocs in MT batch bench */
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
void bench_batch_scaling(void);
void bench_batch_multithreaded(void);
void bench_batch_free_path(void);
void bench_batch_mixed_sizes(void);
void bench_batch_os_tier(void);
void bench_batch_fastpath(void);

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
    bench_batch_scaling();
    bench_batch_multithreaded();
    bench_batch_free_path();
    bench_batch_mixed_sizes();
    bench_batch_os_tier();
    bench_batch_fastpath();
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

    /* TLSF-class size (4 KB) on the same pool */
    start = get_time_sec();
    for (int batch_idx = 0; batch_idx < BENCH_BATCH_COUNT; batch_idx++) {
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            ptrs[i] = mp_alloc(pool, BENCH_SIZE_TLSF);
            bench_escape(ptrs[i]);
        }
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            mp_free(pool, ptrs[i]);
        }
    }
    double time_tlsf_single = get_time_sec() - start;

    start = get_time_sec();
    for (int batch_idx = 0; batch_idx < BENCH_BATCH_COUNT; batch_idx++) {
        size_t got = mp_alloc_batch(pool, BENCH_SIZE_TLSF, ptrs, BENCH_BATCH_ELEMS);
        if (got != BENCH_BATCH_ELEMS) {
            fprintf(
                stderr, "mp_alloc_batch(4KB) returned %zu (expected %d)\n", got, BENCH_BATCH_ELEMS);
        }
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            bench_escape(ptrs[i]);
        }
        mp_free_batch(pool, ptrs, BENCH_BATCH_ELEMS);
    }
    double time_tlsf_batch = get_time_sec() - start;
    printf("  TLSF 4KB single : %.4f sec (%.2f Mops/sec)\n",
           time_tlsf_single,
           (total_ops / time_tlsf_single) / MILLION_OPS);
    printf("  TLSF 4KB batch  : %.4f sec (%.2f Mops/sec)\n",
           time_tlsf_batch,
           (total_ops / time_tlsf_batch) / MILLION_OPS);
    printf("  TLSF 4KB speedup: %.2fx\n", time_tlsf_single / time_tlsf_batch);

    /* THREAD_SAFE pool (lock-bound): batch should amortize pool_lock */
    memory_pool_t *ts_pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_SAFE);
    bench_warmup(ts_pool);
    start = get_time_sec();
    for (int batch_idx = 0; batch_idx < BENCH_BATCH_COUNT; batch_idx++) {
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            ptrs[i] = mp_alloc(ts_pool, BENCH_SIZE_FIXED);
            bench_escape(ptrs[i]);
        }
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            mp_free(ts_pool, ptrs[i]);
        }
    }
    double time_ts_single = get_time_sec() - start;

    start = get_time_sec();
    for (int batch_idx = 0; batch_idx < BENCH_BATCH_COUNT; batch_idx++) {
        size_t got = mp_alloc_batch(ts_pool, BENCH_SIZE_FIXED, ptrs, BENCH_BATCH_ELEMS);
        if (got != BENCH_BATCH_ELEMS) {
            fprintf(
                stderr, "mp_alloc_batch(TS) returned %zu (expected %d)\n", got, BENCH_BATCH_ELEMS);
        }
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            bench_escape(ptrs[i]);
        }
        mp_free_batch(ts_pool, ptrs, BENCH_BATCH_ELEMS);
    }
    double time_ts_batch = get_time_sec() - start;
    printf("  TS single loops : %.4f sec (%.2f Mops/sec)\n",
           time_ts_single,
           (total_ops / time_ts_single) / MILLION_OPS);
    printf("  TS batch APIs   : %.4f sec (%.2f Mops/sec)\n",
           time_ts_batch,
           (total_ops / time_ts_batch) / MILLION_OPS);
    printf("  TS batch speedup: %.2fx\n", time_ts_single / time_ts_batch);

    mp_destroy(ts_pool);

    mp_destroy(pool);
    free(ptrs);
}

/* Batch size scaling matrix: slab (32B) and TLSF (4KB) x 4/16/64/256 elems. */
void bench_batch_scaling(void)
{
    printf("\n--- Benchmark 8b: Batch Size Scaling (32B slab / 4KB TLSF) ---\n");
    static const size_t elem_variants[] = {4, 16, 64, 256};
    static const size_t size_variants[] = {BENCH_SIZE_FIXED, BENCH_SIZE_TLSF};
    static const char *size_names[] = {"32B", "4KB"};
    void **ptrs = (void **)malloc(sizeof(void *) * BENCH_BATCH_SCALE_MAX_ELEMS);
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    bench_warmup(pool);

    for (size_t si = 0; si < 2; si++) {
        size_t esize = size_variants[si];
        printf("  %s:\n", size_names[si]);
        printf("    elems | single Mops | batch Mops | speedup | malloc Mops\n");
        for (size_t vi = 0; vi < 4; vi++) {
            size_t elems = elem_variants[vi];
            double start = get_time_sec();
            for (int b = 0; b < BENCH_BATCH_COUNT; b++) {
                for (size_t i = 0; i < elems; i++) {
                    ptrs[i] = mp_alloc(pool, esize);
                    bench_escape(ptrs[i]);
                }
                for (size_t i = 0; i < elems; i++) {
                    mp_free(pool, ptrs[i]);
                }
            }
            double time_single = get_time_sec() - start;

            start = get_time_sec();
            for (int b = 0; b < BENCH_BATCH_COUNT; b++) {
                size_t got = mp_alloc_batch(pool, esize, ptrs, elems);
                if (got != elems) {
                    fprintf(stderr, "scaling batch returned %zu (expected %zu)\n", got, elems);
                }
                for (size_t i = 0; i < elems; i++) {
                    bench_escape(ptrs[i]);
                }
                mp_free_batch(pool, ptrs, elems);
            }
            double time_batch = get_time_sec() - start;

            start = get_time_sec();
            for (int b = 0; b < BENCH_BATCH_COUNT; b++) {
                for (size_t i = 0; i < elems; i++) {
                    ptrs[i] = malloc(esize);
                    bench_escape(ptrs[i]);
                }
                for (size_t i = 0; i < elems; i++) {
                    free(ptrs[i]);
                }
            }
            double time_malloc = get_time_sec() - start;

            double total_ops = (double)BENCH_BATCH_COUNT * (double)elems;
            printf("    %4zu  | %10.2f  | %9.2f  | %6.2fx  | %9.2f\n",
                   elems,
                   (total_ops / time_single) / MILLION_OPS,
                   (total_ops / time_batch) / MILLION_OPS,
                   time_single / time_batch,
                   (total_ops / time_malloc) / MILLION_OPS);
        }
    }

    mp_destroy(pool);
    free(ptrs);
}

typedef struct {
    memory_pool_t *pool;
    int alloc_count;
    int batch_elems;
} bench_batch_arg_t;

static void *bench_batch_thread_func(void *arg)
{
    bench_batch_arg_t *ta = (bench_batch_arg_t *)arg;
    if (ta->batch_elems > 0) {
        void **ptrs = (void **)malloc(sizeof(void *) * (size_t)ta->batch_elems);
        if (!ptrs) {
            return NULL;
        }
        int rounds = ta->alloc_count / ta->batch_elems;
        for (int r = 0; r < rounds; r++) {
            size_t got = mp_alloc_batch(ta->pool, BENCH_SIZE_FIXED, ptrs, ta->batch_elems);
            if (got != (size_t)ta->batch_elems) {
                fprintf(stderr, "MT batch returned %zu (expected %d)\n", got, ta->batch_elems);
            }
            for (int j = 0; j < ta->batch_elems; j++) {
                bench_escape(ptrs[j]);
            }
            mp_free_batch(ta->pool, ptrs, ta->batch_elems);
        }
        free(ptrs);
    } else {
        for (int i = 0; i < ta->alloc_count; i++) {
            void *ptr = mp_alloc(ta->pool, BENCH_SIZE_FIXED);
            bench_escape(ptr);
            mp_free(ta->pool, ptr);
        }
    }
    return NULL;
}

static double
bench_batch_run(memory_pool_t *pool, int threads, int allocs_per_thread, int batch_elems)
{
    pthread_t *threads_arr = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)threads);
    bench_batch_arg_t *args =
        (bench_batch_arg_t *)malloc(sizeof(bench_batch_arg_t) * (size_t)threads);
    if (!threads_arr || !args) {
        free(threads_arr);
        free(args);
        return -1.0;
    }
    for (int i = 0; i < threads; i++) {
        args[i].pool = pool;
        args[i].alloc_count = allocs_per_thread;
        args[i].batch_elems = batch_elems;
        pthread_create(&threads_arr[i], NULL, bench_batch_thread_func, &args[i]);
    }
    double start = get_time_sec();
    for (int i = 0; i < threads; i++) {
        pthread_join(threads_arr[i], NULL);
    }
    double time = get_time_sec() - start;
    free(threads_arr);
    free(args);
    return (double)threads * (double)allocs_per_thread / time / MILLION_OPS;
}

/* Multi-thread scaling on a lock-bound THREAD_SAFE pool: batch vs single. */
void bench_batch_multithreaded(void)
{
    printf("\n--- Benchmark 8c: Multi-threaded Batch (TS pool, 1/2/4/8 threads, 32B) ---\n");
    static const int thread_counts[] = {1, 2, 4, 8};
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_SAFE);
    bench_warmup(pool);
    printf("  threads | single Mops | batch Mops | speedup | single eff | batch eff\n");
    double single_1 = bench_batch_run(pool, 1, BENCH_BATCH_THREAD_ALLOCS, 0);
    double batch_1 = bench_batch_run(pool, 1, BENCH_BATCH_THREAD_ALLOCS, BENCH_BATCH_ELEMS);
    for (size_t ti = 0; ti < 4; ti++) {
        int tc = thread_counts[ti];
        double m_single = bench_batch_run(pool, tc, BENCH_BATCH_THREAD_ALLOCS, 0);
        double m_batch = bench_batch_run(pool, tc, BENCH_BATCH_THREAD_ALLOCS, BENCH_BATCH_ELEMS);
        printf("  %5d  | %10.2f  | %9.2f  | %6.2fx  | %9.2f  | %8.2f\n",
               tc,
               m_single,
               m_batch,
               m_batch / m_single,
               m_single / single_1,
               m_batch / batch_1);
    }
    mp_destroy(pool);
}

/* Free-path: mp_free_batch vs per-element mp_free (alloc side untimed). */
void bench_batch_free_path(void)
{
    printf("\n--- Benchmark 8d: Free Path (mp_free_batch vs per-elem mp_free, 32B) ---\n");
    void **ptrs = (void **)malloc(sizeof(void *) * BENCH_BATCH_ELEMS);
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    bench_warmup(pool);

    double start = get_time_sec();
    for (int b = 0; b < BENCH_BATCH_COUNT; b++) {
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            ptrs[i] = mp_alloc(pool, BENCH_SIZE_FIXED);
        }
        mp_free_batch(pool, ptrs, BENCH_BATCH_ELEMS);
    }
    double time_batch = get_time_sec() - start;

    start = get_time_sec();
    for (int b = 0; b < BENCH_BATCH_COUNT; b++) {
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            ptrs[i] = mp_alloc(pool, BENCH_SIZE_FIXED);
        }
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            mp_free(pool, ptrs[i]);
        }
    }
    double time_single = get_time_sec() - start;

    size_t total_ops = (size_t)BENCH_BATCH_COUNT * (size_t)BENCH_BATCH_ELEMS;
    printf("  free batch  : %.2f Mops/sec\n", (total_ops / time_batch) / MILLION_OPS);
    printf("  free single : %.2f Mops/sec\n", (total_ops / time_single) / MILLION_OPS);
    printf("  free speedup: %.2fx\n", time_single / time_batch);

    /* Thread-scaled free path on a THREAD_SAFE pool (alloc+free timed together):
     * the batch free's single aggregated pool write lock vs per-element locks. */
    printf("  -- THREAD_SAFE pool, 1/2/4/8 threads, batch vs single --\n");
    static const int free_thread_counts[] = {1, 2, 4, 8};
    memory_pool_t *ts_pool = mp_create(32 * 1024 * 1024, MP_FLAG_THREAD_SAFE);
    bench_warmup(ts_pool);
    printf("  threads | single Mops | batch Mops | speedup | single eff | batch eff\n");
    double ts_single_1 = bench_batch_run(ts_pool, 1, BENCH_BATCH_THREAD_ALLOCS, 0);
    double ts_batch_1 = bench_batch_run(ts_pool, 1, BENCH_BATCH_THREAD_ALLOCS, BENCH_BATCH_ELEMS);
    for (size_t ti = 0; ti < 4; ti++) {
        int tc = free_thread_counts[ti];
        double m_single = bench_batch_run(ts_pool, tc, BENCH_BATCH_THREAD_ALLOCS, 0);
        double m_batch = bench_batch_run(ts_pool, tc, BENCH_BATCH_THREAD_ALLOCS, BENCH_BATCH_ELEMS);
        printf("  %5d  | %10.2f  | %9.2f  | %6.2fx  | %9.2f  | %8.2f\n",
               tc,
               m_single,
               m_batch,
               m_batch / m_single,
               m_single / ts_single_1,
               m_batch / ts_batch_1);
    }
    mp_destroy(ts_pool);

    mp_destroy(pool);
    free(ptrs);
}

/* Mixed-size round: 32B x64 + 512B x16 + 4KB x4, batch vs per-element. */
void bench_batch_mixed_sizes(void)
{
    printf("\n--- Benchmark 8e: Mixed-Size Batch (32B x64 + 512B x16 + 4KB x4/round) ---\n");
    void **ptrs = (void **)malloc(sizeof(void *) * 64);
    memory_pool_t *pool = mp_create(64 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    bench_warmup(pool);

    double start = get_time_sec();
    for (int r = 0; r < BENCH_BATCH_MIXED_ITERS; r++) {
        size_t got = mp_alloc_batch(pool, BENCH_SIZE_FIXED, ptrs, 64);
        if (got != 64) {
            fprintf(stderr, "mixed 32B batch returned %zu (expected 64)\n", got);
        }
        mp_free_batch(pool, ptrs, 64);
        got = mp_alloc_batch(pool, BENCH_BATCH_MIXED_SIZE_MID, ptrs, 16);
        if (got != 16) {
            fprintf(stderr, "mixed 512B batch returned %zu (expected 16)\n", got);
        }
        mp_free_batch(pool, ptrs, 16);
        got = mp_alloc_batch(pool, BENCH_SIZE_TLSF, ptrs, 4);
        if (got != 4) {
            fprintf(stderr, "mixed 4KB batch returned %zu (expected 4)\n", got);
        }
        mp_free_batch(pool, ptrs, 4);
    }
    double time_batch = get_time_sec() - start;

    start = get_time_sec();
    for (int r = 0; r < BENCH_BATCH_MIXED_ITERS; r++) {
        for (int i = 0; i < 64; i++) {
            void *p = mp_alloc(pool, BENCH_SIZE_FIXED);
            bench_escape(p);
            mp_free(pool, p);
        }
        for (int i = 0; i < 16; i++) {
            void *p = mp_alloc(pool, BENCH_BATCH_MIXED_SIZE_MID);
            bench_escape(p);
            mp_free(pool, p);
        }
        for (int i = 0; i < 4; i++) {
            void *p = mp_alloc(pool, BENCH_SIZE_TLSF);
            bench_escape(p);
            mp_free(pool, p);
        }
    }
    double time_single = get_time_sec() - start;

    size_t total_ops = (size_t)BENCH_BATCH_MIXED_ITERS * (64 + 16 + 4);
    printf("  mixed batch  : %.2f Mops/sec\n", (total_ops / time_batch) / MILLION_OPS);
    printf("  mixed single : %.2f Mops/sec\n", (total_ops / time_single) / MILLION_OPS);
    printf("  mixed speedup: %.2fx\n", time_single / time_batch);

    mp_destroy(pool);
    free(ptrs);
}

/* OS-tier: 5 MB elements x8 per batch, 200 rounds, single vs batch. */
void bench_batch_os_tier(void)
{
    printf("\n--- Benchmark 8f: OS-Tier Batch (5MB elems x8, 200 rounds) ---\n");
    void **ptrs = (void **)malloc(sizeof(void *) * BENCH_BATCH_OS_ELEMS);
    memory_pool_t *pool = mp_create(64 * 1024 * 1024, MP_FLAG_THREAD_LOCAL_CACHE);
    bench_warmup(pool);

    double start = get_time_sec();
    for (int r = 0; r < BENCH_BATCH_OS_ITERS; r++) {
        for (int i = 0; i < BENCH_BATCH_OS_ELEMS; i++) {
            ptrs[i] = mp_alloc(pool, BENCH_BATCH_OS_SIZE);
            bench_escape(ptrs[i]);
        }
        for (int i = 0; i < BENCH_BATCH_OS_ELEMS; i++) {
            mp_free(pool, ptrs[i]);
        }
    }
    double time_single = get_time_sec() - start;

    start = get_time_sec();
    for (int r = 0; r < BENCH_BATCH_OS_ITERS; r++) {
        size_t got = mp_alloc_batch(pool, BENCH_BATCH_OS_SIZE, ptrs, BENCH_BATCH_OS_ELEMS);
        if (got != BENCH_BATCH_OS_ELEMS) {
            fprintf(stderr, "OS batch returned %zu (expected %d)\n", got, BENCH_BATCH_OS_ELEMS);
        }
        for (int i = 0; i < BENCH_BATCH_OS_ELEMS; i++) {
            bench_escape(ptrs[i]);
        }
        mp_free_batch(pool, ptrs, BENCH_BATCH_OS_ELEMS);
    }
    double time_batch = get_time_sec() - start;

    size_t total_ops = (size_t)BENCH_BATCH_OS_ITERS * (size_t)BENCH_BATCH_OS_ELEMS;
    printf("  OS single : %.2f Mops/sec\n", (total_ops / time_single) / MILLION_OPS);
    printf("  OS batch  : %.2f Mops/sec\n", (total_ops / time_batch) / MILLION_OPS);
    printf("  OS speedup: %.2fx\n", time_single / time_batch);

    mp_destroy(pool);
    free(ptrs);
}

/* FAST_PATH pool: 32B single vs batch (skips metadata/active-list). */
void bench_batch_fastpath(void)
{
    printf("\n--- Benchmark 8g: FAST_PATH Pool (32B single vs batch) ---\n");
    void **ptrs = (void **)malloc(sizeof(void *) * BENCH_BATCH_ELEMS);
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_FAST_PATH);
    bench_warmup(pool);

    double start = get_time_sec();
    for (int b = 0; b < BENCH_BATCH_COUNT; b++) {
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            ptrs[i] = mp_alloc(pool, BENCH_SIZE_FIXED);
            bench_escape(ptrs[i]);
        }
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            mp_free(pool, ptrs[i]);
        }
    }
    double time_single = get_time_sec() - start;

    start = get_time_sec();
    for (int b = 0; b < BENCH_BATCH_COUNT; b++) {
        size_t got = mp_alloc_batch(pool, BENCH_SIZE_FIXED, ptrs, BENCH_BATCH_ELEMS);
        if (got != BENCH_BATCH_ELEMS) {
            fprintf(stderr, "FAST batch returned %zu (expected %d)\n", got, BENCH_BATCH_ELEMS);
        }
        for (int i = 0; i < BENCH_BATCH_ELEMS; i++) {
            bench_escape(ptrs[i]);
        }
        mp_free_batch(pool, ptrs, BENCH_BATCH_ELEMS);
    }
    double time_batch = get_time_sec() - start;

    size_t total_ops = (size_t)BENCH_BATCH_COUNT * (size_t)BENCH_BATCH_ELEMS;
    printf("  FAST single : %.2f Mops/sec\n", (total_ops / time_single) / MILLION_OPS);
    printf("  FAST batch  : %.2f Mops/sec\n", (total_ops / time_batch) / MILLION_OPS);
    printf("  FAST speedup: %.2fx\n", time_single / time_batch);

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
