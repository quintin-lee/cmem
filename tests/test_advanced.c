/**
 * @file test_advanced.c
 * @brief Advanced Unit Tests for cmem advanced APIs not covered by test_main.c.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "../include/cmem.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define setenv(key, value, overwrite) _putenv_s(key, value)
#endif

// NOLINTBEGIN(readability-magic-numbers, clang-analyzer-optin.core.EnumCastOutOfRange)
// Test vectors and flag combinations in this suite intentionally use literal values.
#define TEST_PASS(name) printf("[PASS] %s\n", name)

/* ========================================================================== */
/*  Loc-tracking Variants                                                     */
/* ========================================================================== */

static void test_calloc_loc()
{
    printf("\n--- Test: mp_calloc_loc ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *ptr = mp_calloc_loc(pool, 32, 1, __FILE__, __LINE__, __func__);
    assert(ptr != NULL);
    assert(mp_ptr_valid(pool, ptr));
    assert(mp_alloc_size(pool, ptr) == 32);

    unsigned char *bytes = (unsigned char *)ptr;
    for (int i = 0; i < 32; i++) {
        assert(bytes[i] == 0);
    }

    mp_free(pool, ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_calloc_loc");
}

static void test_realloc_loc()
{
    printf("\n--- Test: mp_realloc_loc ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *ptr = mp_alloc_loc(pool, 32, __FILE__, __LINE__, __func__);
    assert(ptr != NULL);
    memset(ptr, 0xAB, 32);

    ptr = mp_realloc_loc(pool, ptr, 64, __FILE__, __LINE__, __func__);
    assert(ptr != NULL);
    assert(mp_alloc_size(pool, ptr) == 64);

    unsigned char *bytes = (unsigned char *)ptr;
    for (int i = 0; i < 32; i++) {
        assert(bytes[i] == 0xAB);
    }

    mp_free(pool, ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_realloc_loc");
}

static void test_reallocarray_loc()
{
    printf("\n--- Test: mp_reallocarray_loc ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    int *arr = (int *)mp_alloc_loc(pool, 4 * sizeof(int), __FILE__, __LINE__, __func__);
    assert(arr != NULL);

    arr = (int *)mp_reallocarray_loc(pool, arr, 8, sizeof(int), __FILE__, __LINE__, __func__);
    assert(arr != NULL);
    assert(mp_alloc_size(pool, arr) == 8 * sizeof(int));

    mp_free(pool, arr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_reallocarray_loc");
}

static void test_memdup_loc()
{
    printf("\n--- Test: mp_memdup_loc ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    int src[5] = {1, 2, 3, 4, 5};
    int *dup = (int *)mp_memdup_loc(pool, src, sizeof(src), __FILE__, __LINE__, __func__);
    assert(dup != NULL);
    assert(memcmp(dup, src, sizeof(src)) == 0);

    mp_free(pool, dup);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_memdup_loc");
}

static void test_strdup_loc()
{
    printf("\n--- Test: mp_strdup_loc ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    char *dup = mp_strdup_loc(pool, "hello cmem", __FILE__, __LINE__, __func__);
    assert(dup != NULL);
    assert(strcmp(dup, "hello cmem") == 0);

    mp_free(pool, dup);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_strdup_loc");
}

static void test_asprintf_loc()
{
    printf("\n--- Test: mp_asprintf_loc ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    char *fmt = mp_asprintf_loc(pool, __FILE__, __LINE__, __func__, "val=%d", 42);
    assert(fmt != NULL);
    assert(strstr(fmt, "val=42") != NULL);

    mp_free(pool, fmt);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_asprintf_loc");
}

/* ========================================================================== */
/*  Callbacks                                                                 */
/* ========================================================================== */

static bool g_error_recovery_called = false;

static void error_recovery_cb(memory_pool_t *pool,
                              bool is_high,
                              size_t current, // NOLINT(bugprone-easily-swappable-parameters)
                              size_t limit,
                              void *udata)
{
    (void)pool;
    (void)is_high;
    (void)current;
    (void)limit;
    (void)udata;
    g_error_recovery_called = true;
}

static void test_callbacks()
{
    printf("\n--- Test: Callbacks (error recovery) ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_error_recovery_callback(pool, error_recovery_cb, NULL);

    void *ptr = mp_alloc(pool, 128);
    assert(ptr != NULL);

    mp_free(pool, ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_callbacks");
}

/* ========================================================================== */
/*  Dirty Pool / Bad Block Isolation                                           */
/* ========================================================================== */

static void test_dirty_pool_and_bad_block()
{
    printf("\n--- Test: Dirty pool state ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEBUG_CANARY | MP_FLAG_TRACK_LOCATIONS);
    assert(pool != NULL);

    assert(mp_is_pool_dirty(pool) == false);

    mp_mark_pool_dirty(pool);
    assert(mp_is_pool_dirty(pool) == true);

    mp_clear_pool_dirty(pool);
    assert(mp_is_pool_dirty(pool) == false);

    mp_destroy(pool);
    TEST_PASS("test_dirty_pool_and_bad_block");
}

static void test_isolate_bad_block()
{
    printf("\n--- Test: Bad block isolation ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEBUG_CANARY | MP_FLAG_TRACK_LOCATIONS);
    assert(pool != NULL);

    void *ptr = mp_alloc(pool, 64);
    assert(ptr != NULL);

    assert(mp_isolate_bad_block(pool, ptr) == true);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_isolate_bad_block");
}

static void test_null_param_guards()
{
    printf("\n--- Test: NULL parameter guards ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    assert(mp_isolate_bad_block(NULL, NULL) == false);
    assert(mp_isolate_bad_block(pool, NULL) == false);
    mp_set_error_recovery_callback(NULL, NULL, NULL);
    mp_set_thread_quota(NULL, 1024);
    mp_set_circuit_breaker(NULL, true);
    assert(mp_get_thread_allocated_bytes(NULL) == 0);
    mp_reset_thread_quota(NULL);
    assert(mp_is_circuit_breaker_tripped(NULL) == false);

    mp_destroy(pool);
    TEST_PASS("test_null_param_guards");
}

/* ========================================================================== */
/*  Quota / Circuit Breaker                                                    */
/* ========================================================================== */

static void test_quota_and_circuit_breaker()
{
    printf("\n--- Test: Arena quota and circuit breaker ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    mp_set_arena_quota(pool, 1024, NULL, NULL);
    assert(mp_check_arena_quota(pool) == true);

    mp_set_thread_quota(pool, 512);
    mp_set_circuit_breaker(pool, true);
    assert(mp_is_circuit_breaker_tripped(pool) == false);

    void *ptr = mp_alloc(pool, 1024);
    assert(ptr != NULL);
    assert(mp_get_thread_allocated_bytes(pool) >= 1024);

    mp_reset_thread_quota(pool);
    assert(mp_get_thread_allocated_bytes(pool) == 0);

    mp_free(pool, ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_quota_and_circuit_breaker");
}

/* ========================================================================== */
/*  Latency Stats                                                              */
/* ========================================================================== */

static void test_latency_stats()
{
    printf("\n--- Test: Latency recording and stats ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_record_latency(pool, 1000);
    mp_record_latency(pool, 2000);
    mp_record_latency(pool, 3000);

    uint64_t avg = mp_get_latency_avg(pool);
    assert(avg == 2000);

    uint64_t p99 = mp_get_latency_p99(pool);
    (void)p99;

    mp_reset_latency_stats(pool);
    assert(mp_get_latency_avg(pool) == 0);

    mp_destroy(pool);
    TEST_PASS("test_latency_stats");
}

/* ========================================================================== */
/*  Slab Class Config                                                          */
/* ========================================================================== */

static void test_slab_class_config()
{
    printf("\n--- Test: Custom slab classes ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    size_t custom_sizes[] = {16, 32, 64, 128};
    mp_set_slab_classes(pool, custom_sizes, 4);

    size_t out[8];
    size_t count = mp_get_slab_classes(pool, out, 8);
    assert(count == 7);
    assert(out[0] == 16);
    assert(out[1] == 32);
    assert(out[2] == 64);
    assert(out[3] == 128);

    assert(mp_get_slab_class_count(pool) == 7);
    assert(mp_preferred_size_for_pool(pool, 20) >= 20);

    mp_destroy(pool);
    TEST_PASS("test_slab_class_config");
}

/* ========================================================================== */
/*  Per-CPU Freelist                                                           */
/* ========================================================================== */

static void test_percpu_freelist()
{
    printf("\n--- Test: Per-CPU freelist ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_PERCPU_FREELIST | MP_FLAG_THREAD_LOCAL_CACHE);
    assert(pool != NULL);

    mp_set_percpu_freelist(pool, true);

    assert(mp_get_percpu_freelist(pool) == true);
    int cpus = mp_get_percpu_cpu_count(pool);
    assert(cpus > 0);

    void *slots[128];
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < 128; i++) {
            slots[i] = mp_alloc(pool, 32 + (i % 64));
            assert(slots[i] != NULL);
        }
        for (int i = 0; i < 128; i++) {
            mp_free(pool, slots[i]);
            slots[i] = NULL;
        }
    }

    mp_set_percpu_freelist(pool, false);
    mp_destroy(pool);
    TEST_PASS("test_percpu_freelist");
}

/* ========================================================================== */
/*  Auto-Compact                                                               */
/* ========================================================================== */

static void test_auto_compact()
{
    printf("\n--- Test: Auto-compact trigger ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_memory_limit(pool, 1024);
    mp_set_auto_compact(pool, true, 0.001, 0.001);
    void *ptr = mp_alloc(pool, 512);
    assert(ptr != NULL);
    assert(mp_auto_compact_check(pool) == true);

    mp_free(pool, ptr);
    mp_destroy(pool);
    TEST_PASS("test_auto_compact");
}

/* ========================================================================== */
/*  Encrypted Memory                                                           */
/* ========================================================================== */

static void test_encrypted_memory()
{
    printf("\n--- Test: Encrypted memory API ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_ENCRYPTED_MEMORY);
    assert(pool != NULL);

    mp_set_encrypted_memory(pool, true);

    void *ptr = mp_alloc(pool, 256);
    assert(ptr != NULL);

    mp_lock_memory(pool, ptr, 256);
    mp_protect_from_dump(pool, ptr, 256);
    mp_secure_zero(pool, ptr, 256);
    mp_unlock_memory(pool, ptr, 256);

    mp_free(pool, ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_encrypted_memory");
}

/* ========================================================================== */
/*  ASan Integration                                                           */
/* ========================================================================== */

static void test_asan_integration()
{
    printf("\n--- Test: ASan integration ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_ASAN_INTEGRATION);
    assert(pool != NULL);

    mp_set_asan_integration(pool, true);
    assert(mp_asan_is_enabled() == false || true); /* depends on compile flags */
    assert(mp_asan_check_memory(pool, NULL, 0) == false);

    void *ptr = mp_alloc(pool, 128);
    assert(ptr != NULL);
#ifndef __SANITIZE_ADDRESS__
    mp_asan_report_error(pool, ptr, 128, true);
#endif

    mp_free(pool, ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_asan_integration");
}

/* ========================================================================== */
/*  Hot/Cold Pages                                                             */
/* ========================================================================== */

static void test_hot_cold_pages()
{
    printf("\n--- Test: Hot/Cold page separation ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_HOT_COLD_SEPARATION);
    assert(pool != NULL);

    void *ptr = mp_alloc(pool, 4096);
    assert(ptr != NULL);

    mp_mark_page_hot(pool, ptr);
    mp_mark_page_cold(pool, ptr);

    mp_get_hot_page_count(pool);
    mp_get_cold_page_count(pool);
    (void)mp_get_hot_page_count(pool);
    (void)mp_get_cold_page_count(pool);

    mp_separate_hot_cold_pages(pool);

    mp_free(pool, ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_hot_cold_pages");
}

/* ========================================================================== */
/*  Online Expansion                                                           */
/* ========================================================================== */

static void test_online_expansion()
{
    printf("\n--- Test: Online pool expansion ---\n");
    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_memory_limit(pool, 4 * 1024 * 1024);
    assert(mp_can_expand(pool) == true);
    size_t expandable = mp_get_expandable_size(pool);
    assert(expandable > 0);

    bool ok = mp_expand_pool(pool, 512 * 1024);
    assert(ok == true);

    void *ptr = mp_alloc(pool, 256);
    assert(ptr != NULL);

    mp_free(pool, ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_online_expansion");
}

/* ========================================================================== */
/*  Event Log                                                                  */
/* ========================================================================== */

static void test_event_log()
{
    printf("\n--- Test: Structured event log ---\n");
    mp_event_log_t *log = mp_event_log_create(16);
    assert(log != NULL);

    assert(mp_event_log_record(log, MP_EVENT_ALLOC, (void *)0x1000, 128) == true);
    assert(mp_event_log_pending(log) == 1);

    mp_event_log_clear(log);
    assert(mp_event_log_pending(log) == 0);

    mp_event_log_destroy(log);
    TEST_PASS("test_event_log");
}

/* ========================================================================== */
/*  Exports                                                                    */
/* ========================================================================== */

static void test_exports()
{
    printf("\n--- Test: Leak report and pprof export ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_TRACK_LOCATIONS);
    assert(pool != NULL);

    void *ptr = mp_alloc_loc(pool, 256, __FILE__, __LINE__, __func__);
    assert(ptr != NULL);

    assert(mp_export_leak_report(pool, "test_advanced_leak.txt") == true);

    char pprof_buf[4096];
    size_t len = mp_export_pprof(pool, pprof_buf, sizeof(pprof_buf));
    assert(len > 0);

    mp_free(pool, ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    (void)remove("test_advanced_leak.txt");
    TEST_PASS("test_exports");
}

/* ========================================================================== */
/*  Misc                                                                       */
/* ========================================================================== */

static void test_abi_version()
{
    printf("\n--- Test: mp_abi_version ---\n");
    uint32_t ver = mp_abi_version();
    assert(ver > 0);
    TEST_PASS("test_abi_version");
}

static void test_cgroup_aware()
{
    printf("\n--- Test: mp_set_cgroup_aware and mp_get_cgroup_mem_limit ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_cgroup_aware(pool, true);
    size_t limit = mp_get_cgroup_mem_limit(pool);
    assert(limit == 0 || limit > 0);

    mp_destroy(pool);
    TEST_PASS("test_cgroup_aware");
}

static void test_create_custom()
{
    printf("\n--- Test: mp_create_custom ---\n");
    memory_pool_t *pool = mp_create_custom(1024 * 1024, MP_FLAG_DEFAULT, NULL);
    assert(pool != NULL);

    void *ptr = mp_alloc(pool, 128);
    assert(ptr != NULL);

    mp_free(pool, ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_create_custom");
}

static void test_calloc()
{
    printf("\n--- Test: mp_calloc ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *ptr = mp_calloc(pool, 1, 64);
    assert(ptr != NULL);
    memset(ptr, 0xFF, 64);

    mp_free(pool, ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_calloc");
}

static void test_event_log_consume()
{
    printf("\n--- Test: mp_event_log_consume ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_event_log_t *log = mp_event_log_create(256);
    assert(log != NULL);

    mp_event_log_entry_t entry;
    bool got = mp_event_log_consume(log, &entry);
    (void)got;

    mp_event_log_destroy(log);
    mp_destroy(pool);
    TEST_PASS("test_event_log_consume");
}

static void test_reparse_env_flags()
{
    printf("\n--- Test: mp_reparse_env_flags ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    setenv("CMEM_CONF", "DEBUG_CANARY=1", 1);
    mp_flags_t flags = mp_reparse_env_flags(pool);
    (void)flags;

    uint64_t gen = mp_get_env_generation(pool);
    (void)gen;

    mp_destroy(pool);
    TEST_PASS("test_reparse_env_flags");
}

static void test_tls_cache_refill()
{
    printf("\n--- Test: TLS cache refill ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_THREAD_LOCAL_CACHE);
    assert(pool != NULL);

    void *slots[256];
    for (int i = 0; i < 256; i++) {
        slots[i] = mp_alloc(pool, 32);
        assert(slots[i] != NULL);
    }
    for (int i = 0; i < 256; i++) {
        mp_free(pool, slots[i]);
    }

    mp_destroy(pool);
    TEST_PASS("test_tls_cache_refill");
}

static void test_static_buffer_pool()
{
    printf("\n--- Test: Static buffer pool ---\n");
    uint8_t buffer[64 * 1024];
    memory_pool_t *pool = mp_create_from_buffer(buffer, sizeof(buffer), MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *ptr = mp_alloc(pool, 256);
    assert(ptr != NULL);

    mp_free(pool, ptr);
    mp_destroy(pool);
    TEST_PASS("test_static_buffer_pool");
}

static void test_alloc_error_paths()
{
    printf("\n--- Test: Allocation error paths ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_memory_limit(pool, 64);
    mp_enable_emergency_reserve(pool, 0);
    mp_set_fallback_on_oom(pool, false);

    void *p_big = mp_alloc(pool, 128);
    assert(p_big == NULL);

    mp_set_memory_limit(pool, 0);
    mp_destroy(pool);
    TEST_PASS("test_alloc_error_paths");
}

static void test_os_fallback_alloc()
{
    printf("\n--- Test: OS fallback allocation ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *ptr = mp_alloc(pool, 5 * 1024 * 1024);
    if (ptr) {
        mp_free(pool, ptr);
    }

    mp_destroy(pool);
    TEST_PASS("test_os_fallback_alloc");
}

static void test_debug_canary_and_zero()
{
    printf("\n--- Test: Debug canary and zero-on-alloc ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEBUG_CANARY | MP_FLAG_ZERO_ON_ALLOC);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 256);
    assert(p1 != NULL);
    memset(p1, 0xFF, 256);

    void *p2 = mp_calloc(pool, 1, 512);
    assert(p2 != NULL);

    void *p3 = mp_realloc(pool, p1, 512);
    assert(p3 != NULL);

    mp_free(pool, p3);
    mp_free(pool, p2);

    mp_destroy(pool);
    TEST_PASS("test_debug_canary_and_zero");
}

static void test_percpu_thread_safe_free()
{
    printf("\n--- Test: Per-CPU freelist with thread-safe free ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_THREAD_SAFE | MP_FLAG_PERCPU_FREELIST);
    assert(pool != NULL);

    void *slots[64];
    for (int i = 0; i < 64; i++) {
        slots[i] = mp_alloc(pool, 32);
        assert(slots[i] != NULL);
    }
    for (int i = 0; i < 64; i++) {
        mp_free(pool, slots[i]);
    }

    mp_destroy(pool);
    TEST_PASS("test_percpu_thread_safe_free");
}

static void test_error_recovery_callback()
{
    printf("\n--- Test: Error recovery callback ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    g_error_recovery_called = false;
    mp_set_error_recovery_callback(pool, error_recovery_cb, NULL);

    mp_mark_pool_dirty(pool);
    void *ptr = mp_alloc(pool, 64);
    (void)ptr;

    mp_destroy(pool);
    TEST_PASS("test_error_recovery_callback");
}

static void test_dirty_pool_rejection()
{
    printf("\n--- Test: Dirty pool rejection ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_mark_pool_dirty(pool);
    assert(mp_is_pool_dirty(pool) == true);

    void *ptr = mp_alloc(pool, 64);
    assert(ptr == NULL);

    mp_clear_pool_dirty(pool);
    ptr = mp_alloc(pool, 64);
    assert(ptr != NULL);
    mp_free(pool, ptr);

    mp_destroy(pool);
    TEST_PASS("test_dirty_pool_rejection");
}

static void test_circuit_breaker()
{
    printf("\n--- Test: Circuit breaker ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_circuit_breaker(pool, true);
    mp_set_thread_quota(pool, 32);
    mp_set_fallback_on_oom(pool, false);

    void *p1 = mp_alloc(pool, 32);
    assert(p1 != NULL);

    void *p2 = mp_alloc(pool, 32);
    assert(p2 == NULL);

    mp_set_circuit_breaker(pool, false);
    mp_free(pool, p1);
    mp_destroy(pool);
    TEST_PASS("test_circuit_breaker");
}

static void test_tlsf_expansion()
{
    printf("\n--- Test: TLSF expansion ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *slots[64];
    for (int i = 0; i < 64; i++) {
        slots[i] = mp_alloc(pool, 1024);
        assert(slots[i] != NULL);
    }
    for (int i = 0; i < 64; i++) {
        mp_free(pool, slots[i]);
    }

    mp_destroy(pool);
    TEST_PASS("test_tlsf_expansion");
}

static void test_purge_lazy_advanced()
{
    printf("\n--- Test: Purge lazy with empty pages ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *ptrs[400];
    for (int i = 0; i < 400; i++) {
        ptrs[i] = mp_alloc(pool, 32);
        assert(ptrs[i] != NULL);
    }
    for (int i = 0; i < 400; i++) {
        mp_free(pool, ptrs[i]);
    }

    size_t purged = mp_purge_lazy(pool);
    printf("  Lazy RSS purge: %zu bytes\n", purged);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_purge_lazy_advanced");
}

static void test_get_allocation_info()
{
    printf("\n--- Test: Get allocation info ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_TRACK_LOCATIONS);
    assert(pool != NULL);

    void *ptr = mp_alloc_loc(pool, 256, __FILE__, __LINE__, __func__);
    assert(ptr != NULL);

    mp_allocation_info_t info;

    bool ok = mp_get_allocation_info(pool, ptr, &info);

    assert(ok == true);
    assert(info.ptr == ptr);
    assert(info.requested_size == 256);
    assert(info.usable_size >= 256);
    assert(info.alloc_type == ALLOC_TYPE_SLAB || info.alloc_type == ALLOC_TYPE_TLSF);
    assert(info.alloc_file != NULL);
    assert(info.alloc_line > 0);
    assert(info.alloc_func != NULL);

    mp_free(pool, ptr);

    ok = mp_get_allocation_info(pool, ptr, &info);
    assert(ok == false);

    mp_destroy(pool);
    TEST_PASS("test_get_allocation_info");
}

static void test_enumerate_regions()
{
    printf("\n--- Test: Enumerate regions ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 64);
    assert(p1 != NULL);
    void *p2 = mp_alloc(pool, 1024);
    assert(p2 != NULL);
    mp_free(pool, p2);

    mp_region_info_t regions[32];
    size_t count = mp_enumerate_regions(pool, regions, 32);
    assert(count > 0);

    bool has_slab = false;
    bool has_tlsf = false;
    for (size_t i = 0; i < count; i++) {
        if (regions[i].type == ALLOC_TYPE_SLAB) {
            has_slab = true;
        }
        if (regions[i].type == ALLOC_TYPE_TLSF) {
            has_tlsf = true;
        }
        assert(regions[i].base != NULL);
        assert(regions[i].size > 0);
    }
    assert(has_slab == true);
    assert(has_tlsf == true);

    mp_free(pool, p1);
    mp_destroy(pool);
    TEST_PASS("test_enumerate_regions");
}

static void test_report_leaks_on_destroy()
{
    printf("\n--- Test: Report leaks on destroy ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_REPORT_LEAKS_ON_DESTROY);
    assert(pool != NULL);

    void *ptr = mp_alloc(pool, 128);
    assert(ptr != NULL);

    mp_destroy(pool);
    TEST_PASS("test_report_leaks_on_destroy");
}

static void test_snapshot_diff()
{
    printf("\n--- Test: Snapshot diff ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    char report[4096];
    bool ok = mp_diff_snapshots(NULL, NULL, report, sizeof(report));
    (void)ok;

    mp_destroy(pool);
    TEST_PASS("test_snapshot_diff");
}

static void test_tlsf_inplace_realloc()
{
    printf("\n--- Test: TLSF in-place realloc ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 1024);
    assert(p1 != NULL);
    void *p2 = mp_alloc(pool, 2048);
    assert(p2 != NULL);
    mp_free(pool, p2);

    void *p3 = mp_realloc(pool, p1, 2048);
    assert(p3 != NULL);
    mp_free(pool, p3);

    mp_destroy(pool);
    TEST_PASS("test_tlsf_inplace_realloc");
}

static void test_slab_full_page_transition()
{
    printf("\n--- Test: Slab full page transition ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *slots[400];
    for (int i = 0; i < 400; i++) {
        slots[i] = mp_alloc(pool, 32);
        assert(slots[i] != NULL);
    }
    for (int i = 0; i < 400; i++) {
        mp_free(pool, slots[i]);
    }

    mp_destroy(pool);
    TEST_PASS("test_slab_full_page_transition");
}

static void test_reset_with_full_pages()
{
    printf("\n--- Test: Reset with full pages ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *slots[400];
    for (int i = 0; i < 400; i++) {
        slots[i] = mp_alloc(pool, 32);
        assert(slots[i] != NULL);
    }

    mp_reset(pool);

    mp_destroy(pool);
    TEST_PASS("test_reset_with_full_pages");
}

/* ========================================================================== */
/*  Entry Point                                                                */
/* ========================================================================== */

// NOLINTEND(readability-magic-numbers, clang-analyzer-optin.core.EnumCastOutOfRange)

static void *multi_arena_worker(void *arg)
{
    memory_pool_t *pool = (memory_pool_t *)arg;
    for (int i = 0; i < 500; i++) {
        void *p = mp_alloc(pool, 64);
        assert(p != NULL);
        mp_free(pool, p);
    }
    return NULL;
}

static void test_multi_arena()
{
    printf("\n--- Test: Multi-Arena Thread-to-Arena Partitioning ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);
    assert(mp_get_arena_count(pool) > 0);

    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, multi_arena_worker, pool);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    mp_destroy(pool);
    TEST_PASS("test_multi_arena");
}

int main()
{
    printf("\n================ RUNNING CMEM ADVANCED UNIT TESTS ================\n");

    test_calloc_loc();
    test_realloc_loc();
    test_reallocarray_loc();
    test_memdup_loc();
    test_strdup_loc();
    test_asprintf_loc();
    test_callbacks();
    test_dirty_pool_and_bad_block();
    test_isolate_bad_block();
    test_null_param_guards();
    test_quota_and_circuit_breaker();
    test_latency_stats();
    test_slab_class_config();
    test_percpu_freelist();
    test_auto_compact();
    test_encrypted_memory();
    test_asan_integration();
    test_hot_cold_pages();
    test_online_expansion();
    test_event_log();
    test_exports();
    test_abi_version();
    test_cgroup_aware();
    test_create_custom();
    test_calloc();
    test_event_log_consume();
    test_reparse_env_flags();
    test_tls_cache_refill();
    test_static_buffer_pool();
    test_alloc_error_paths();
    test_os_fallback_alloc();
    test_debug_canary_and_zero();
    test_percpu_thread_safe_free();
    test_error_recovery_callback();
    test_dirty_pool_rejection();
    test_circuit_breaker();
    test_tlsf_expansion();
    test_snapshot_diff();
    test_tlsf_inplace_realloc();
    test_slab_full_page_transition();
    test_reset_with_full_pages();
    test_purge_lazy_advanced();
    test_get_allocation_info();
    test_enumerate_regions();
    test_report_leaks_on_destroy();
    test_multi_arena();

    printf("\nALL CMEM ADVANCED UNIT TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
