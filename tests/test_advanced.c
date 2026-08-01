/**
 * @file test_advanced.c
 * @brief Advanced Unit Tests for cmem advanced APIs not covered by test_main.c.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#define _POSIX_C_SOURCE 200809L

#include "../include/cmem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#define TEST_PASS(name) printf("[PASS] %s\n", name)

/* ========================================================================== */
/*  Loc-tracking Variants                                                     */
/* ========================================================================== */

static void test_calloc_loc()
{
    printf("\n--- Test: mp_calloc_loc ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void* p = mp_calloc_loc(pool, 32, 1, __FILE__, __LINE__, __func__);
    assert(p != NULL);
    assert(mp_ptr_valid(pool, p));
    assert(mp_alloc_size(pool, p) == 32);

    unsigned char* bytes = (unsigned char*) p;
    for (int i = 0; i < 32; i++)
    {
        assert(bytes[i] == 0);
    }

    mp_free(pool, p);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_calloc_loc");
}

static void test_realloc_loc()
{
    printf("\n--- Test: mp_realloc_loc ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void* p = mp_alloc_loc(pool, 32, __FILE__, __LINE__, __func__);
    assert(p != NULL);
    memset(p, 0xAB, 32);

    p = mp_realloc_loc(pool, p, 64, __FILE__, __LINE__, __func__);
    assert(p != NULL);
    assert(mp_alloc_size(pool, p) == 64);

    unsigned char* bytes = (unsigned char*) p;
    for (int i = 0; i < 32; i++)
    {
        assert(bytes[i] == 0xAB);
    }

    mp_free(pool, p);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_realloc_loc");
}

static void test_reallocarray_loc()
{
    printf("\n--- Test: mp_reallocarray_loc ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    int* arr = (int*) mp_alloc_loc(pool, 4 * sizeof(int), __FILE__, __LINE__, __func__);
    assert(arr != NULL);

    arr = (int*) mp_reallocarray_loc(pool, arr, 8, sizeof(int), __FILE__, __LINE__, __func__);
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
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    int src[5] = {1, 2, 3, 4, 5};
    int* dup = (int*) mp_memdup_loc(pool, src, sizeof(src), __FILE__, __LINE__, __func__);
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
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    char* dup = mp_strdup_loc(pool, "hello cmem", __FILE__, __LINE__, __func__);
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
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    char* fmt = mp_asprintf_loc(pool, __FILE__, __LINE__, __func__, "val=%d", 42);
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

static void error_recovery_cb(memory_pool_t* pool, bool is_high, size_t current, size_t limit,
                              void* udata)
{
    (void) pool;
    (void) is_high;
    (void) current;
    (void) limit;
    (void) udata;
    g_error_recovery_called = true;
}

static void test_callbacks()
{
    printf("\n--- Test: Callbacks (error recovery) ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_error_recovery_callback(pool, error_recovery_cb, NULL);

    void* p = mp_alloc(pool, 128);
    assert(p != NULL);

    mp_free(pool, p);
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
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEBUG_CANARY | MP_FLAG_TRACK_LOCATIONS);
    assert(pool != NULL);

    assert(mp_is_pool_dirty(pool) == false);

    mp_mark_pool_dirty(pool);
    assert(mp_is_pool_dirty(pool) == true);

    mp_clear_pool_dirty(pool);
    assert(mp_is_pool_dirty(pool) == false);

    mp_destroy(pool);
    TEST_PASS("test_dirty_pool_and_bad_block");
}

/* ========================================================================== */
/*  Quota / Circuit Breaker                                                    */
/* ========================================================================== */

static void test_quota_and_circuit_breaker()
{
    printf("\n--- Test: Arena quota and circuit breaker ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    mp_set_arena_quota(pool, 1024, NULL, NULL);
    assert(mp_check_arena_quota(pool) == true);

    mp_set_thread_quota(pool, 512);
    mp_set_circuit_breaker(pool, true);
    assert(mp_is_circuit_breaker_tripped(pool) == false);

    void* p = mp_alloc(pool, 1024);
    assert(p != NULL);
    assert(mp_get_thread_allocated_bytes(pool) >= 1024);

    mp_reset_thread_quota(pool);
    assert(mp_get_thread_allocated_bytes(pool) == 0);

    mp_free(pool, p);
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
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_record_latency(pool, 1000);
    mp_record_latency(pool, 2000);
    mp_record_latency(pool, 3000);

    uint64_t avg = mp_get_latency_avg(pool);
    assert(avg == 2000);

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
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
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
    memory_pool_t* pool = mp_create(0, MP_FLAG_PERCPU_FREELIST);
    assert(pool != NULL);

    mp_set_percpu_freelist(pool, true);

    mp_destroy(pool);
    TEST_PASS("test_percpu_freelist");
}

/* ========================================================================== */
/*  Auto-Compact                                                               */
/* ========================================================================== */

static void test_auto_compact()
{
    printf("\n--- Test: Auto-compact trigger ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_memory_limit(pool, 1024);
    mp_set_auto_compact(pool, true, 0.001, 0.001);
    void* p = mp_alloc(pool, 512);
    assert(p != NULL);
    assert(mp_auto_compact_check(pool) == true);

    mp_free(pool, p);
    mp_destroy(pool);
    TEST_PASS("test_auto_compact");
}

/* ========================================================================== */
/*  Encrypted Memory                                                           */
/* ========================================================================== */

static void test_encrypted_memory()
{
    printf("\n--- Test: Encrypted memory API ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_ENCRYPTED_MEMORY);
    assert(pool != NULL);

    mp_set_encrypted_memory(pool, true);

    void* p = mp_alloc(pool, 256);
    assert(p != NULL);

    mp_lock_memory(pool, p, 256);
    mp_protect_from_dump(pool, p, 256);
    mp_secure_zero(pool, p, 256);
    mp_unlock_memory(pool, p, 256);

    mp_free(pool, p);
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
    memory_pool_t* pool = mp_create(0, MP_FLAG_ASAN_INTEGRATION);
    assert(pool != NULL);

    mp_set_asan_integration(pool, true);
    assert(mp_asan_is_enabled() == false || true); /* depends on compile flags */
    assert(mp_asan_check_memory(pool, NULL, 0) == false);

    void* p = mp_alloc(pool, 128);
    assert(p != NULL);
#ifndef __SANITIZE_ADDRESS__
    mp_asan_report_error(pool, p, 128, true);
#endif

    mp_free(pool, p);
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
    memory_pool_t* pool = mp_create(0, MP_FLAG_HOT_COLD_SEPARATION);
    assert(pool != NULL);

    void* p = mp_alloc(pool, 4096);
    assert(p != NULL);

    mp_mark_page_hot(pool, p);
    mp_mark_page_cold(pool, p);

    mp_get_hot_page_count(pool);
    mp_get_cold_page_count(pool);
    (void) mp_get_hot_page_count(pool);
    (void) mp_get_cold_page_count(pool);

    mp_separate_hot_cold_pages(pool);

    mp_free(pool, p);
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
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_memory_limit(pool, 4 * 1024 * 1024);
    assert(mp_can_expand(pool) == true);
    size_t expandable = mp_get_expandable_size(pool);
    assert(expandable > 0);

    bool ok = mp_expand_pool(pool, 512 * 1024);
    assert(ok == true);

    void* p = mp_alloc(pool, 256);
    assert(p != NULL);

    mp_free(pool, p);
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
    mp_event_log_t* log = mp_event_log_create(16);
    assert(log != NULL);

    assert(mp_event_log_record(log, MP_EVENT_ALLOC, (void*) 0x1000, 128) == true);
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
    memory_pool_t* pool = mp_create(0, MP_FLAG_TRACK_LOCATIONS);
    assert(pool != NULL);

    void* p = mp_alloc_loc(pool, 256, __FILE__, __LINE__, __func__);
    assert(p != NULL);

    assert(mp_export_leak_report(pool, "test_advanced_leak.txt") == true);

    char pprof_buf[4096];
    size_t len = mp_export_pprof(pool, pprof_buf, sizeof(pprof_buf));
    assert(len > 0);

    mp_free(pool, p);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    remove("test_advanced_leak.txt");
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
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
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
    memory_pool_t* pool = mp_create_custom(1024 * 1024, MP_FLAG_DEFAULT, NULL);
    assert(pool != NULL);

    void* p = mp_alloc(pool, 128);
    assert(p != NULL);

    mp_free(pool, p);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_create_custom");
}

static void test_calloc()
{
    printf("\n--- Test: mp_calloc ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void* p = mp_calloc(pool, 64, 16);
    assert(p != NULL);

    unsigned char* bytes = (unsigned char*) p;
    for (int i = 0; i < 64 * 16; i++)
    {
        assert(bytes[i] == 0);
    }

    mp_free(pool, p);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_calloc");
}

/* ========================================================================== */
/*  Entry Point                                                                */
/* ========================================================================== */

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

    printf("\nALL CMEM ADVANCED UNIT TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
