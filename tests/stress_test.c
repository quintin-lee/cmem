/**
 * @file stress_test.c
 * @brief Long-running high-concurrency stress test for cmem.
 *
 * Runs N threads for a configurable duration, performing mixed-size
 * allocations/frees while periodically reporting pool pressure and RSS.
 */

#ifdef _WIN32
static inline int cmem_rand_r(unsigned int *seed)
{
    return (int)((*seed = (1103515245u * (*seed) + 12345u)) >> 16) & 0x7FFF;
}
#define rand_r(seed) cmem_rand_r(seed)
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "../include/cmem.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// NOLINTBEGIN(readability-magic-numbers, clang-analyzer-optin.core.EnumCastOutOfRange)
// Test vectors, sizes, and flag combinations in this suite intentionally use literal values.

/* ========================================================================== */
/*  Configuration                                                             */
/* ========================================================================== */

#ifndef STRESS_THREADS
#define STRESS_THREADS 8
#endif

#ifndef STRESS_DURATION_SEC
#define STRESS_DURATION_SEC (24 * 3600)
#endif

#ifndef STRESS_REPORT_INTERVAL_SEC
#define STRESS_REPORT_INTERVAL_SEC 10
#endif

#ifndef STRESS_WORKING_SET_PER_THREAD
#define STRESS_WORKING_SET_PER_THREAD 2048
#endif

#ifndef STRESS_MAX_ALLOC_SIZE
#define STRESS_MAX_ALLOC_SIZE (4 * 1024 * 1024)
#endif

/* ========================================================================== */
/*  RSS Helper                                                                 */
/* ========================================================================== */

static size_t get_rss_kb()
{
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp) {
        return 0;
    }

    char line[256];
    size_t rss = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            char *pos = line + 6;
            while (*pos == ' ' || *pos == '\t') {
                pos++;
            }
            rss = (size_t)strtoul(pos, NULL, 10);
            break;
        }
    }
    (void)fclose(fp);
    return rss;
}

/* ========================================================================== */
/*  Per-Thread Context                                                        */
/* ========================================================================== */

typedef struct {
    int thread_id;
    memory_pool_t *pool;
    volatile uint64_t alloc_count;
    volatile uint64_t free_count;
    volatile uint64_t fail_count;
    void **slots;
    int slot_count;
    int slot_capacity;
} thread_ctx_t;

/* ========================================================================== */
/*  Worker Thread                                                             */
/* ========================================================================== */

static volatile sig_atomic_t g_stop = 0;

static void *stress_worker(void *arg)
{
    thread_ctx_t *ctx = (thread_ctx_t *)arg;

    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)ctx->thread_id;

    while (!g_stop) {

        size_t sz = 32 + (rand_r(&seed) % (STRESS_MAX_ALLOC_SIZE - 32));
        void *ptr = mp_alloc(ctx->pool, sz);

        if (!ptr) {
            ctx->fail_count++;
            continue;
        }

        if (ctx->slot_count < ctx->slot_capacity) {
            ctx->slots[ctx->slot_count++] = ptr;
        } else {
            int idx = rand_r(&seed) % ctx->slot_capacity;
            void *old = ctx->slots[idx];
            mp_free(ctx->pool, old);
            ctx->slots[idx] = ptr;
        }

        ctx->alloc_count++;

        if (rand_r(&seed) % 4 == 0 && ctx->slot_count > 0) {
            int idx = rand_r(&seed) % ctx->slot_count;
            mp_free(ctx->pool, ctx->slots[idx]);
            ctx->slots[idx] = ctx->slots[--ctx->slot_count];
            ctx->free_count++;
        }
    }

    return NULL;
}

/* ========================================================================== */
/*  Main                                                                      */
/* ========================================================================== */

int main()
{
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    printf("================ CMEM LONG-RUN STRESS TEST ================\n");
    printf("Threads            : %d\n", STRESS_THREADS);
    printf("Duration           : %d seconds\n", STRESS_DURATION_SEC);
    printf("Report interval    : %d seconds\n", STRESS_REPORT_INTERVAL_SEC);
    printf("Working set/thread : %d objects\n", STRESS_WORKING_SET_PER_THREAD);
    printf("Max alloc size     : %d bytes\n", STRESS_MAX_ALLOC_SIZE);
    printf("\n");
    memory_pool_t *pool =
        mp_create(64 * 1024 * 1024, MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE);
    assert(pool != NULL);

    mp_set_memory_limit(pool, (size_t)4 * 1024 * 1024 * 1024);

    pthread_t threads[STRESS_THREADS];
    thread_ctx_t contexts[STRESS_THREADS];

    for (int i = 0; i < STRESS_THREADS; i++) {
        contexts[i].thread_id = i;
        contexts[i].pool = pool;
        contexts[i].alloc_count = 0;
        contexts[i].free_count = 0;
        contexts[i].fail_count = 0;
        contexts[i].slot_capacity = STRESS_WORKING_SET_PER_THREAD;
        contexts[i].slot_count = 0;
        contexts[i].slots = (void **)calloc((size_t)contexts[i].slot_capacity, sizeof(void *));
        assert(contexts[i].slots != NULL);

        int rc = pthread_create(&threads[i], NULL, stress_worker, &contexts[i]);
        assert(rc == 0);
    }

    uint64_t last_total_allocs = 0;
    time_t start_time = time(NULL);

    while (1) {
        sleep(STRESS_REPORT_INTERVAL_SEC);

        time_t now = time(NULL);
        if (now - start_time >= STRESS_DURATION_SEC) {
            printf("\n[STRESS] Test duration reached. Shutting down...\n");
            g_stop = 1;
            break;
        }

        double pressure = mp_pressure(pool);
        size_t rss_kb = get_rss_kb();
        size_t resident = mp_resident(pool);

        uint64_t total_allocs = 0;
        uint64_t total_frees = 0;
        uint64_t total_fails = 0;
        for (int i = 0; i < STRESS_THREADS; i++) {
            total_allocs += contexts[i].alloc_count;
            total_frees += contexts[i].free_count;
            total_fails += contexts[i].fail_count;
        }

        uint64_t delta_allocs = total_allocs - last_total_allocs;
        double qps = (double)delta_allocs / (double)STRESS_REPORT_INTERVAL_SEC;

        printf("[STRESS] T=%lds | Pressure=%.2f%% | RSS=%zu MB | Resident=%zu MB | "
               "QPS=%.0f | Allocs=%" PRIu64 " | Frees=%" PRIu64 " | Fails=%" PRIu64 " | "
               "Active=%" PRIu64 "\n",
               (long)(now - start_time),
               pressure,
               rss_kb / 1024,
               resident / (1024 * 1024),
               qps,
               total_allocs,
               total_frees,
               total_fails,
               total_allocs - total_frees);

        last_total_allocs = total_allocs;

        if (pressure > 95.0) {
            printf("[STRESS] WARNING: Pressure exceeded 95%%!\n");
        }
    }

    for (int i = 0; i < STRESS_THREADS; i++) {
        pthread_join(threads[i], NULL);
        free((void *)contexts[i].slots);
    }

    // mp_check_leaks(pool); // Disabled for long-run stress; enable for leak debugging
    mp_destroy(pool);

    printf("\n[STRESS] Long-run stress test completed successfully.\n");
    return 0;
}
// NOLINTEND(readability-magic-numbers, clang-analyzer-optin.core.EnumCastOutOfRange)
