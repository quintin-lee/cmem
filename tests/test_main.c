/**
 * @file test_main.c
 * @brief Comprehensive Unit Tests for the C Memory Pool Manager.
 */

#include "../include/memory_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#define TEST_PASS(name) printf("[PASS] %s\n", name)

static bool g_event_triggered = false;
static void test_event_cb(memory_pool_t* pool, mp_event_type_t event, void* ptr, size_t size, void* user_data) {
    (void)pool; (void)ptr; (void)size; (void)user_data;
    if (event == MP_EVENT_ALLOC) {
        g_event_triggered = true;
    }
}

void test_slab_small_allocs() {
    printf("\n--- Test 1: Slab Allocations (Small Objects <= 512B) ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEBUG_CANARY | MP_FLAG_ZERO_ON_ALLOC);
    assert(pool != NULL);

    void* ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = mp_alloc(pool, 16 + (i % 64));
        assert(ptrs[i] != NULL);
        uint8_t* byte_ptr = (uint8_t*)ptrs[i];
        assert(byte_ptr[0] == 0 && byte_ptr[15] == 0);
    }

    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 100);
    assert(stats.slab_allocated_bytes > 0);

    for (int i = 0; i < 100; i++) {
        mp_free(pool, ptrs[i]);
    }

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_slab_small_allocs");
}

void test_tlsf_medium_allocs() {
    printf("\n--- Test 2: TLSF Allocations (Medium Objects 512B - 4MB) ---\n");
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void* p1 = mp_alloc(pool, 1024);
    void* p2 = mp_alloc(pool, 64 * 1024);
    void* p3 = mp_alloc(pool, 512 * 1024);

    assert(p1 && p2 && p3);

    memset(p1, 0xAA, 1024);
    memset(p2, 0xBB, 64 * 1024);
    memset(p3, 0xCC, 512 * 1024);

    mp_free(pool, p2);
    void* p4 = mp_alloc(pool, 32 * 1024);
    assert(p4 != NULL);

    mp_free(pool, p1);
    mp_free(pool, p3);
    mp_free(pool, p4);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_tlsf_medium_allocs");
}

void test_realloc_and_aligned() {
    printf("\n--- Test 3: Realloc & Aligned Allocations ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);

    char* str = (char*)mp_alloc(pool, 32);
    strcpy(str, "Hello Antigravity Memory Pool!");

    str = (char*)mp_realloc(pool, str, 100);
    assert(strcmp(str, "Hello Antigravity Memory Pool!") == 0);

    void* aligned_ptr = mp_aligned_alloc(pool, 64, 256);
    assert(aligned_ptr != NULL);
    assert(((uintptr_t)aligned_ptr % 64) == 0);

    mp_free(pool, str);
    mp_free(pool, aligned_ptr);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_realloc_and_aligned");
}

void test_leak_analysis_and_heap_audit() {
    printf("\n--- Test 7: Leak Analysis Report & Heap Audit ---\n");
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY | MP_FLAG_TRACK_LOCATIONS | MP_FLAG_POISON_ON_FREE);

    void* leak_ptr = mp_alloc_loc(pool, 256, __FILE__, __LINE__, __func__);
    assert(leak_ptr != NULL);

    void* valid_ptr = mp_alloc_loc(pool, 64, __FILE__, __LINE__, __func__);
    assert(valid_ptr != NULL);

    assert(mp_audit_heap(pool) == true);

    uint8_t* poison_test = (uint8_t*)valid_ptr;
    mp_free(pool, valid_ptr);
    assert(poison_test[0] == 0xDD && poison_test[63] == 0xDD);

    char report[2048];
    size_t report_len = mp_analyze_leaks(pool, report, sizeof(report));
    assert(report_len > 0);
    assert(strstr(report, "Source Location") != NULL);

    mp_free(pool, leak_ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_leak_analysis_and_heap_audit");
}

void test_child_arenas_and_html_export() {
    printf("\n--- Test 8: Child Arenas & Visual HTML Report Export ---\n");
    memory_pool_t* root = mp_create(1024 * 1024, MP_FLAG_TRACK_LOCATIONS);
    memory_pool_t* child1 = mp_create_child(root, 512 * 1024, MP_FLAG_DEFAULT, "TestChildArena1");

    void* p1 = mp_alloc_loc(root, 128, __FILE__, __LINE__, __func__);
    void* p2 = mp_alloc_loc(child1, 256, __FILE__, __LINE__, __func__);

    assert(p1 != NULL && p2 != NULL);

    mp_dump_tree_info(root);
    assert(mp_export_html_report(root, "test_report.html") == true);

    mp_free(root, p1);
    mp_free(child1, p2);

    assert(mp_check_leaks(root) == true);
    assert(mp_check_leaks(child1) == true);

    mp_destroy(root);
    TEST_PASS("test_child_arenas_and_html_export");
}

void test_arena_reset_and_json() {
    printf("\n--- Test 5: Fast Arena Reset & JSON Exporter ---\n");
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);

    for (int i = 0; i < 50; i++) {
        mp_alloc(pool, 128 + i * 16);
    }

    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 50);

    char json_buf[512];
    size_t json_len = mp_dump_json_stats(pool, json_buf, sizeof(json_buf));
    assert(json_len > 0);
    assert(strstr(json_buf, "\"active_allocations\": 50") != NULL);

    mp_reset(pool);
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 0);
    assert(stats.active_bytes == 0);
    assert(mp_check_leaks(pool) == true);

    mp_destroy(pool);
    TEST_PASS("test_arena_reset_and_json");
}

static uint8_t g_static_buf[256 * 1024];

void test_static_buffer_and_callbacks() {
    printf("\n--- Test 6: Static Buffer Arena & Event Callbacks ---\n");
    memory_pool_t* pool = mp_create_from_buffer(g_static_buf, sizeof(g_static_buf), MP_FLAG_DEFAULT);
    assert(pool != NULL);

    g_event_triggered = false;
    mp_set_event_callback(pool, test_event_cb, NULL);

    void* p1 = mp_alloc(pool, 500);
    assert(p1 != NULL);
    assert(g_event_triggered == true);

    uintptr_t buf_start = (uintptr_t)g_static_buf;
    uintptr_t buf_end   = buf_start + sizeof(g_static_buf);
    assert((uintptr_t)p1 >= buf_start && (uintptr_t)p1 < buf_end);

    mp_free(pool, p1);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_static_buffer_and_callbacks");
}

#define THREAD_COUNT 4
#define ALLOCS_PER_THREAD 500

void* thread_worker(void* arg) {
    memory_pool_t* pool = (memory_pool_t*)arg;
    void* ptrs[ALLOCS_PER_THREAD];

    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        size_t sz = (i % 2 == 0) ? (8 + i % 128) : (1024 + i % 4096);
        ptrs[i] = mp_alloc(pool, sz);
        assert(ptrs[i] != NULL);
    }

    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        mp_free(pool, ptrs[i]);
    }

    return NULL;
}

void test_multithread_safety() {
    printf("\n--- Test 4: Multithreaded Concurrent Safety & Thread-Local Cache ---\n");
    memory_pool_t* pool = mp_create(2 * 1024 * 1024, MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE);
    assert(pool != NULL);

    pthread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_create(&threads[i], NULL, thread_worker, pool);
    }

    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    mp_dump_info(pool);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_multithread_safety");
}

int main() {
    printf("================ RUNNING MEMORY POOL UNIT TESTS ================\n");
    test_slab_small_allocs();
    test_tlsf_medium_allocs();
    test_realloc_and_aligned();
    test_leak_analysis_and_heap_audit();
    test_child_arenas_and_html_export();
    test_arena_reset_and_json();
    test_static_buffer_and_callbacks();
    test_multithread_safety();
    printf("\nALL UNIT TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
