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

    void* p1 = mp_alloc(pool, 1024);     // 1 KB
    void* p2 = mp_alloc(pool, 64 * 1024); // 64 KB
    void* p3 = mp_alloc(pool, 512 * 1024);// 512 KB

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

void test_arena_reset_and_json() {
    printf("\n--- Test 5: Fast Arena Reset & JSON Exporter ---\n");
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);

    for (int i = 0; i < 50; i++) {
        mp_alloc(pool, 128 + i * 16);
    }

    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 50);

    // Test JSON export
    char json_buf[512];
    size_t json_len = mp_dump_json_stats(pool, json_buf, sizeof(json_buf));
    assert(json_len > 0);
    assert(strstr(json_buf, "\"active_allocations\": 50") != NULL);
    printf("Generated JSON Telemetry:\n%s\n", json_buf);

    // Perform Fast Reset
    mp_reset(pool);
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 0);
    assert(stats.active_bytes == 0);
    assert(mp_check_leaks(pool) == true);

    mp_destroy(pool);
    TEST_PASS("test_arena_reset_and_json");
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
    test_arena_reset_and_json();
    test_multithread_safety();
    printf("\nALL UNIT TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
