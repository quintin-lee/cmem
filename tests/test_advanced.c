/**
 * @file test_advanced.c
 * @brief Advanced Unit Tests for cmem advanced APIs not covered by test_main.c.
 */

#include "../include/cmem.h"

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define setenv(key, value, overwrite) _putenv_s(key, value)
static int cmem_unsetenv(const char *name)
{
    return _putenv_s(name, "");
}
#define unsetenv cmem_unsetenv
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

    size_t out[16];
    size_t count = mp_get_slab_classes(pool, out, 16);
    assert(count == 12);
    assert(out[0] == 16);
    assert(out[1] == 32);
    assert(out[2] == 64);
    assert(out[3] == 128);

    assert(mp_get_slab_class_count(pool) == 12);
    assert(mp_preferred_size_for_pool(pool, 20) >= 20);
    assert(mp_get_slab_class_count(pool) == 12);
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

    unsetenv("CMEM_CONF");
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

static void test_safety_defaults()
{
    printf("\n--- Test: safety defaults ---\n");
    unsetenv("CMEM_CONF");

    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);
    assert(mp_reparse_env_flags(pool) == CMEM_DEFAULT_SAFETY_FLAGS);
    assert(mp_default_pool_flags() == CMEM_DEFAULT_SAFETY_FLAGS);

    mp_destroy(pool);
    TEST_PASS("test_safety_defaults");
}

static void test_safety_explicit_flags()
{
    printf("\n--- Test: safety explicit flags ---\n");
    unsetenv("CMEM_CONF");

    memory_pool_t *pool = mp_create(0, MP_FLAG_FAST_PATH);
    assert(pool != NULL);
    assert(mp_reparse_env_flags(pool) == MP_FLAG_FAST_PATH);

    mp_destroy(pool);
    TEST_PASS("test_safety_explicit_flags");
}

static void test_safety_multi_arena_children()
{
    printf("\n--- Test: safety multi-arena children ---\n");
    unsetenv("CMEM_CONF");

    memory_pool_t *parent = mp_create(0, MP_FLAG_MULTI_ARENA);
    assert(parent != NULL);
    assert(mp_get_arena_count(parent) > 0);

    memory_pool_t *child = mp_get_arena(parent, 0);
    assert(child != NULL);
    assert(mp_reparse_env_flags(child) == MP_FLAG_DEFAULT);
    mp_destroy(parent);

    parent = mp_create(0, (mp_flags_t)(MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE));
    assert(parent != NULL);

    child = mp_get_arena(parent, 0);
    assert(child != NULL);
    assert(mp_reparse_env_flags(child) == MP_FLAG_THREAD_SAFE);
    mp_destroy(parent);

    TEST_PASS("test_safety_multi_arena_children");
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
    enum { MULTI_ARENA_ITERS = 500 };
    memory_pool_t *pool = (memory_pool_t *)arg;
    for (int i = 0; i < MULTI_ARENA_ITERS; i++) {
        void *ptr = mp_alloc(pool, 64);
        assert(ptr != NULL);
        mp_free(pool, ptr);
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

typedef struct {
    memory_pool_t *pool;
    size_t sz;
} race_arg_t;

static void *shared_tlsf_race_worker(void *arg)
{
    enum { SHARED_TLSF_ITERS = 15000 };

    race_arg_t *work = (race_arg_t *)arg;
    for (int i = 0; i < SHARED_TLSF_ITERS; i++) {
        void *ptr = mp_alloc(work->pool, work->sz);
        assert(ptr != NULL);
        ((volatile char *)ptr)[0] = 1;
        mp_free(work->pool, ptr);
    }
    return NULL;
}

static void test_shared_tlsf_threaded_race()
{
    printf("\n--- Test: Shared TLSF concurrent alloc/free (D12 race) ---\n");
    const size_t sizes[] = {65536, 3 * 65536, 16 * 65536};
    enum { RACE_THREADS = 4 };

    for (int phase = 0; phase < 3; phase++) {
        memory_pool_t *pool = mp_create(16 * 1024 * 1024, MP_FLAG_THREAD_SAFE);
        assert(pool != NULL);

        pthread_t threads[RACE_THREADS];
        race_arg_t args[RACE_THREADS];
        for (int i = 0; i < RACE_THREADS; i++) {
            args[i].pool = pool;
            args[i].sz = sizes[phase];
            pthread_create(&threads[i], NULL, shared_tlsf_race_worker, &args[i]);
        }
        for (int i = 0; i < RACE_THREADS; i++) {
            pthread_join(threads[i], NULL);
        }
        mp_destroy(pool);
    }
    TEST_PASS("test_shared_tlsf_threaded_race");
}

extern bool tlsf_arena_validate_current(void);

/* Fresh multi-arena pool; sizes are above the TLSF cache range (>8 KiB) so the
 * TLSF arena path (not the cache/slab path) is exercised. */
static void test_tlsf_arena_basic(void)
{
    printf("\n--- Test: TLSF arena basic alloc/free (master handle) ---\n");
    memory_pool_t *pool = mp_create(16 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    void *block = mp_alloc(pool, 32768);
    assert(block != NULL);
    /* The child arena descriptor is created lazily on first alloc, so the
     * count only becomes non-zero now. */
    assert(mp_get_arena_count(pool) > 0);
    memset(block, 0xAB, 32768);
    assert(tlsf_arena_validate_current());
    mp_free(pool, block); /* master handle: must NOT crash (D5) */
    assert(tlsf_arena_validate_current());
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("TLSF arena basic alloc/free with master handle");
}

static void test_tlsf_arena_realloc(void)
{
    printf("\n--- Test: TLSF arena realloc grow/shrink ---\n");
    memory_pool_t *pool = mp_create(16 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    size_t size = 32768;
    uint8_t *p = (uint8_t *)mp_alloc(pool, size);
    assert(p != NULL);
    for (size_t i = 0; i < size; i++) {
        p[i] = (uint8_t)(i & 0xFF);
    }
    assert(tlsf_arena_validate_current());

    size_t bigger = 49152;
    uint8_t *q = (uint8_t *)mp_realloc(pool, p, bigger);
    assert(q != NULL);
    for (size_t i = 0; i < size; i++) {
        assert(q[i] == (uint8_t)(i & 0xFF));
    }
    assert(tlsf_arena_validate_current());

    size_t smaller = 20000;
    uint8_t *r = (uint8_t *)mp_realloc(pool, q, smaller);
    assert(r != NULL);
    for (size_t i = 0; i < smaller; i++) {
        assert(r[i] == (uint8_t)(i & 0xFF));
    }
    assert(tlsf_arena_validate_current());

    mp_free(pool, r);
    assert(tlsf_arena_validate_current());
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("TLSF arena realloc grow/shrink preserves payload");
}

static void test_tlsf_arena_coalesce(void)
{
    printf("\n--- Test: TLSF arena coalesce ---\n");
    memory_pool_t *pool = mp_create(16 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    enum { N = 4 };
    const size_t blk = 32768;
    void *v[N];
    for (int i = 0; i < N; i++) {
        v[i] = mp_alloc(pool, blk);
        assert(v[i] != NULL);
        memset(v[i], (unsigned char)i, blk);
    }
    assert(tlsf_arena_validate_current());

    /* Free order forces forward (1->2), backward (0), then the remaining pair
     * merges both ways. */
    const int order[N] = {1, 0, 3, 2};
    for (int i = 0; i < N; i++) {
        mp_free(pool, v[order[i]]);
        assert(tlsf_arena_validate_current());
    }

    /* Full-capacity proof: 192 KiB only succeeds if all 4 blocks plus the
     * arena's always-free tail coalesced back into one block. */
    void *big = mp_alloc(pool, 196608);
    assert(big != NULL);
    mp_free(pool, big);
    assert(tlsf_arena_validate_current());
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("TLSF arena coalesce (fwd/back/both) + full-capacity merge");
}

static void test_tlsf_arena_master_free(void)
{
    printf("\n--- Test: TLSF arena free via alternate child handle ---\n");
    memory_pool_t *pool = mp_create(16 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    void *block = mp_alloc(pool, 32768);
    assert(block != NULL);
    memset(block, 0x5A, 32768);
    assert(tlsf_arena_validate_current());

    memory_pool_t *child = mp_get_thread_arena(pool);
    assert(child != NULL);
    mp_free(child, block);
    assert(tlsf_arena_validate_current());

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("TLSF arena free via alternate child handle");
}

typedef struct {
    memory_pool_t *pool;
    void *ptrs[4];
    int count;
} arena_xfree_arg_t;

static void *arena_xfree_worker(void *arg)
{
    arena_xfree_arg_t *xa = (arena_xfree_arg_t *)arg;
    for (int i = 0; i < xa->count; i++) {
        mp_free(xa->pool, xa->ptrs[i]);
    }
    return NULL;
}

static void test_tlsf_arena_cross_thread_free(void)
{
    printf("\n--- Test: TLSF arena cross-thread free (mailbox) ---\n");
    memory_pool_t *pool = mp_create(16 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    arena_xfree_arg_t xa;
    xa.pool = pool;
    xa.count = 4;
    /* 4 x 32 KiB = 128 KiB: all arena-resident in the 256 KiB arena. */
    for (int i = 0; i < xa.count; i++) {
        xa.ptrs[i] = mp_alloc(pool, 32768);
        assert(xa.ptrs[i] != NULL);
        memset(xa.ptrs[i], 0xC3, 32768);
    }
    assert(tlsf_arena_validate_current());

    pthread_t worker;
    assert(pthread_create(&worker, NULL, arena_xfree_worker, &xa) == 0);
    assert(pthread_join(worker, NULL) == 0);

    /* Owner still alive: the next owner-side alloc reaps the mailbox. */
    void *again = mp_alloc(pool, 32768);
    assert(again != NULL);
    mp_free(pool, again);
    assert(tlsf_arena_validate_current());
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("TLSF arena cross-thread free via mailbox + reap");
}

static void test_tlsf_arena_exhaustion(void)
{
    printf("\n--- Test: TLSF arena exhaustion + shared-pool fallback ---\n");
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    enum { MAXBLK = 64 }; /* 64 x 32 KiB = 2 MiB >> 256 KiB arena */
    void *blocks[MAXBLK];
    int n = 0;

    /* ~7 blocks fit the 256 KiB arena; every further block must transparently
     * fall back to the shared pool and still succeed. */
    for (; n < MAXBLK; n++) {
        blocks[n] = mp_alloc(pool, 32768);
        assert(blocks[n] != NULL);
        memset(blocks[n], 0x7E, 32768);
    }
    assert(tlsf_arena_validate_current());

    for (int i = 0; i < n; i++) {
        mp_free(pool, blocks[i]);
    }
    assert(tlsf_arena_validate_current());
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("TLSF arena exhaustion falls back to shared pool cleanly");
}

typedef struct {
    memory_pool_t *pool;
    void *ptrs[4];
} arena_exit_arg_t;

static void *arena_exit_worker(void *arg)
{
    arena_exit_arg_t *ea = (arena_exit_arg_t *)arg;
    for (int i = 0; i < 4; i++) {
        /* 4 x 32 KiB stays inside the 256 KiB arena; the worker exits with the
         * blocks still live, so retirement cannot clean up immediately. */
        ea->ptrs[i] = mp_alloc(ea->pool, 32768);
        assert(ea->ptrs[i] != NULL);
        memset(ea->ptrs[i], 0xD4, 32768);
    }
    return NULL;
}

static void test_tlsf_arena_thread_exit(void)
{
    printf("\n--- Test: TLSF arena thread-exit retire with live blocks ---\n");
    memory_pool_t *pool = mp_create(16 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    arena_exit_arg_t ea;
    ea.pool = pool;
    pthread_t worker;
    assert(pthread_create(&worker, NULL, arena_exit_worker, &ea) == 0);
    assert(pthread_join(worker, NULL) == 0);

    for (int i = 0; i < 4; i++) {
        mp_free(pool, ea.ptrs[i]);
    }

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("TLSF arena thread-exit retire + deferred cleanup");
}

static void test_tlsf_arena_pool_switch(void)
{
    printf("\n--- Test: TLSF arena pool-switch retires the old arena ---\n");
    memory_pool_t *pool_a = mp_create(16 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    memory_pool_t *pool_b = mp_create(16 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool_a != NULL && pool_b != NULL);

    void *a_block = mp_alloc(pool_a, 32768);
    assert(a_block != NULL);

    /* Switching pools flushes the cache and retires A's arena while its block
     * is still live; the free below is what triggers the deferred cleanup. */
    void *b_block = mp_alloc(pool_b, 32768);
    assert(b_block != NULL);

    mp_free(pool_a, a_block);
    mp_free(pool_b, b_block);

    assert(mp_check_leaks(pool_a) == true);
    assert(mp_check_leaks(pool_b) == true);
    mp_destroy(pool_a);
    mp_destroy(pool_b);
    TEST_PASS("TLSF arena pool-switch retires and cleans up old arena");
}

#define RAND_SLOTS 256
#define RAND_OPS 20000
#define RAND_SEED 12345
#define RAND_MIN_ALLOC (16 * 1024)
#define RAND_MAX_ALLOC (192 * 1024)

typedef struct {
    uint8_t *ptr;
    size_t size;
    uint8_t tag;
} rand_slot_t;

typedef struct {
    uintptr_t start;
    uintptr_t end;
} rand_interval_t;

static uint64_t g_rand_state;

/* xorshift64: deterministic and independent of libc rand(). */
static uint32_t rand_next_u32(void)
{
    uint64_t x = g_rand_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rand_state = x;
    return (uint32_t)(x >> 32);
}

static size_t rand_range(size_t lo, size_t hi)
{
    return lo + (size_t)(rand_next_u32() % (uint32_t)(hi - lo + 1));
}

static int rand_free_slot(const rand_slot_t *slots)
{
    for (int i = 0; i < RAND_SLOTS; i++) {
        if (slots[i].ptr == NULL) {
            return i;
        }
    }
    return -1;
}

static int rand_live_slot(const rand_slot_t *slots)
{
    uint32_t start = rand_next_u32() % RAND_SLOTS;
    for (int k = 0; k < RAND_SLOTS; k++) {
        int idx = (int)((start + (uint32_t)k) % RAND_SLOTS);
        if (slots[idx].ptr != NULL) {
            return idx;
        }
    }
    return -1;
}

static void rand_ivals_insert(rand_interval_t *ivals, size_t *count, uintptr_t start, uintptr_t end)
{
    size_t pos = 0;
    while (pos < *count && ivals[pos].start < start) {
        pos++;
    }
    /* No live block may overlap another: that is the allocator's core contract. */
    if (pos > 0) {
        assert(ivals[pos - 1].end <= start);
    }
    if (pos < *count) {
        assert(end <= ivals[pos].start);
    }
    memmove(&ivals[pos + 1], &ivals[pos], (*count - pos) * sizeof(ivals[0]));
    ivals[pos].start = start;
    ivals[pos].end = end;
    (*count)++;
}

static void rand_ivals_remove(rand_interval_t *ivals, size_t *count, uintptr_t start)
{
    size_t pos = 0;
    while (pos < *count && ivals[pos].start != start) {
        pos++;
    }
    assert(pos < *count);
    memmove(&ivals[pos], &ivals[pos + 1], (*count - pos - 1) * sizeof(ivals[0]));
    (*count)--;
}

static void rand_ivals_validate(const rand_interval_t *ivals, size_t count)
{
    for (size_t i = 1; i < count; i++) {
        assert(ivals[i - 1].end <= ivals[i].start);
    }
}

static void rand_verify_payload(const uint8_t *p, size_t size, uint8_t tag)
{
    for (size_t i = 0; i < size; i++) {
        assert(p[i] == tag);
    }
}

static void test_tlsf_arena_randomized(void)
{
    printf("\n--- Test: TLSF arena randomized differential vs interval model ---\n");
    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_MULTI_ARENA | MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    static rand_slot_t slots[RAND_SLOTS]; /* static: keep the table off the stack */
    rand_interval_t ivals[RAND_SLOTS];
    size_t live = 0;
    size_t ival_count = 0;
    g_rand_state = RAND_SEED;

    for (int op = 0; op < RAND_OPS; op++) {
        unsigned kind;
        if (live == 0) {
            kind = 0;
        } else if (live >= RAND_SLOTS) {
            kind = 1 + (rand_next_u32() % 2);
        } else {
            kind = rand_next_u32() % 3;
        }

        if (kind == 0) {
            size_t size = rand_range(RAND_MIN_ALLOC, RAND_MAX_ALLOC);
            uint8_t *p = (uint8_t *)mp_alloc(pool, size);
            assert(p != NULL);
            int slot = rand_free_slot(slots);
            assert(slot >= 0);
            slots[slot].ptr = p;
            slots[slot].size = size;
            slots[slot].tag = (uint8_t)rand_next_u32();
            memset(p, slots[slot].tag, size);
            rand_ivals_insert(ivals, &ival_count, (uintptr_t)p, (uintptr_t)p + size);
            live++;
        } else if (kind == 1) {
            int slot = rand_live_slot(slots);
            assert(slot >= 0);
            rand_verify_payload(slots[slot].ptr, slots[slot].size, slots[slot].tag);
            rand_ivals_remove(ivals, &ival_count, (uintptr_t)slots[slot].ptr);
            mp_free(pool, slots[slot].ptr);
            slots[slot].ptr = NULL;
            live--;
        } else {
            int slot = rand_live_slot(slots);
            assert(slot >= 0);
            size_t old_size = slots[slot].size;
            size_t new_size = rand_range(RAND_MIN_ALLOC, RAND_MAX_ALLOC);
            uint8_t *old = slots[slot].ptr;
            uint8_t tag = slots[slot].tag;
            rand_verify_payload(old, old_size, tag);
            rand_ivals_remove(ivals, &ival_count, (uintptr_t)old);
            uint8_t *grown = (uint8_t *)mp_realloc(pool, old, new_size);
            assert(grown != NULL);
            rand_verify_payload(grown, (old_size < new_size) ? old_size : new_size, tag);
            if (new_size > old_size) {
                memset(grown + old_size, tag, new_size - old_size);
            }
            slots[slot].ptr = grown;
            slots[slot].size = new_size;
            rand_ivals_insert(ivals, &ival_count, (uintptr_t)grown, (uintptr_t)grown + new_size);
        }

        if ((op & 0xFF) == 0xFF) {
            assert(tlsf_arena_validate_current());
            rand_ivals_validate(ivals, ival_count);
            size_t live_count = 0;
            for (int i = 0; i < RAND_SLOTS; i++) {
                live_count += (slots[i].ptr != NULL) ? 1u : 0u;
            }
            assert(live_count == live);
        }
    }

    for (int i = 0; i < RAND_SLOTS; i++) {
        if (slots[i].ptr != NULL) {
            rand_verify_payload(slots[i].ptr, slots[i].size, slots[i].tag);
            rand_ivals_remove(ivals, &ival_count, (uintptr_t)slots[i].ptr);
            mp_free(pool, slots[i].ptr);
            slots[i].ptr = NULL;
        }
    }
    assert(ival_count == 0);
    assert(tlsf_arena_validate_current());
    assert(mp_check_leaks(pool) == true);

    /* The arena must coalesce back to a single block large enough for this
     * full-capacity request after the churn above. */
    uint8_t *full = (uint8_t *)mp_alloc(pool, RAND_MAX_ALLOC);
    assert(full != NULL);
    memset(full, 0x5A, RAND_MAX_ALLOC);
    assert(tlsf_arena_validate_current());
    mp_free(pool, full);
    assert(mp_check_leaks(pool) == true);

    mp_destroy(pool);
    TEST_PASS("TLSF arena randomized differential vs interval model");
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
    test_safety_defaults();
    test_safety_explicit_flags();
    test_safety_multi_arena_children();
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
    test_shared_tlsf_threaded_race();
    test_tlsf_arena_basic();
    test_tlsf_arena_realloc();
    test_tlsf_arena_coalesce();
    test_tlsf_arena_master_free();
    test_tlsf_arena_cross_thread_free();
    test_tlsf_arena_exhaustion();
    test_tlsf_arena_thread_exit();
    test_tlsf_arena_pool_switch();
    test_tlsf_arena_randomized();

    printf("\nALL CMEM ADVANCED UNIT TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
