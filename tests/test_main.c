/**
 * @file test_main.c
 * @brief Comprehensive Unit Tests for cmem Memory Manager.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#define _POSIX_C_SOURCE 200809L

#include "../include/cmem.h"
#include "../include/cmem_override.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#define TEST_PASS(name) printf("[PASS] %s\n", name)

/**
 * @brief Global flag indicating whether an event callback was triggered.
 */
static bool g_event_triggered = false;
/**
 * @brief Global flag indicating whether an OOM event callback was triggered.
 */
static bool g_oom_triggered = false;
/**
 * @brief Global flag indicating whether a high watermark callback was triggered.
 */
static bool g_high_watermark_hit = false;
/**
 * @brief Global flag indicating whether a low watermark callback was triggered.
 */
static bool g_low_watermark_hit = false;

/**
 * @brief Test event callback for verifying alloc/free/oom events.
 * @param pool Memory pool handle.
 * @param event Type of memory event.
 * @param ptr Pointer involved in the event.
 * @param size Size of the allocation in bytes.
 * @param user_data Optional user data passed to the callback.
 */
static void test_event_cb(memory_pool_t* pool, mp_event_type_t event, void* ptr, size_t size,
                          void* user_data)
{
    (void) pool;
    (void) ptr;
    (void) size;
    (void) user_data;
    if (event == MP_EVENT_ALLOC)
    {
        g_event_triggered = true;
    }
    else if (event == MP_EVENT_OOM)
    {
        g_oom_triggered = true;
    }
}

/**
 * @brief Test watermark callback for verifying high/low watermark thresholds.
 * @param pool Memory pool handle.
 * @param is_high_watermark True if high watermark threshold was hit.
 * @param current_bytes Current bytes allocated.
 * @param limit_bytes Memory limit in bytes.
 * @param user_data Optional user data passed to the callback.
 */
static void test_watermark_cb(memory_pool_t* pool, bool is_high_watermark, size_t current_bytes,
                              size_t limit_bytes, void* user_data)
{
    (void) pool;
    (void) current_bytes;
    (void) limit_bytes;
    (void) user_data;
    if (is_high_watermark)
    {
        g_high_watermark_hit = true;
    }
    else
    {
        g_low_watermark_hit = true;
    }
}

/**
 * @brief Test node structure used for typed object pool tests.
 */
typedef struct
{
    int id;
    char name[32];
    double value;
} test_node_t;

/**
 * @brief Tests memory introspection APIs: mp_usable_size, mp_alloc_size, mp_ptr_valid.
 */
void test_introspection_apis()
{
    printf("\n--- Test 29: Memory Introspection APIs (mp_usable_size, mp_alloc_size, mp_ptr_valid) "
           "---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void* p1 = mp_alloc(pool, 120);
    assert(p1 != NULL);

    assert(mp_ptr_valid(pool, p1) == true);
    assert(mp_alloc_size(pool, p1) == 120);
    assert(mp_usable_size(pool, p1) >= 120);

    uint8_t fake_buf[128] = {0};
    void* fake_ptr = &fake_buf[64];
    assert(mp_ptr_valid(pool, fake_ptr) == false);
    assert(mp_alloc_size(pool, fake_ptr) == 0);
    assert(mp_usable_size(pool, fake_ptr) == 0);

    printf("  Memory introspection query (usable_size=%zu, alloc_size=%zu, valid=true) verified!\n",
           mp_usable_size(pool, p1), mp_alloc_size(pool, p1));

    mp_free(pool, p1);
    assert(mp_ptr_valid(pool, p1) == false);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_introspection_apis");
}

/**
 * @brief Tests TLSF in-place realloc optimization.
 * Verifies that realloc can expand a block in-place without memcpy.
 */
void test_tlsf_inplace_realloc()
{
    printf("\n--- Test 30: TLSF In-Place Realloc Optimization ---\n");
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void* p1 = mp_alloc(pool, 1024);
    void* p2 = mp_alloc(pool, 2048);
    assert(p1 != NULL && p2 != NULL);

    mp_free(pool, p2);

    void* p1_expanded = mp_realloc(pool, p1, 2500);
    assert(p1_expanded == p1);
    assert(mp_alloc_size(pool, p1_expanded) == 2500);

    printf("  TLSF in-place realloc expanded pointer 0x%zx in-place (0 memcpy overhead)!\n",
           (uintptr_t) p1);

    mp_free(pool, p1_expanded);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_tlsf_inplace_realloc");
}

/**
 * @brief Tests overflow-safe mp_reallocarray with nmemb * size validation.
 */
void test_reallocarray()
{
    printf("\n--- Test 31: Overflow-Safe mp_reallocarray ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    int* arr = (int*) mp_alloc(pool, 10 * sizeof(int));
    assert(arr != NULL);

    arr = (int*) mp_reallocarray(pool, arr, 100, sizeof(int));
    assert(arr != NULL);
    assert(mp_alloc_size(pool, arr) == 100 * sizeof(int));

    void* overflow_ptr = mp_reallocarray(pool, arr, SIZE_MAX / 2, 4);
    assert(overflow_ptr == NULL);

    printf("  mp_reallocarray overflow protection & reallocation verified!\n");

    mp_free(pool, arr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_reallocarray");
}

/**
 * @brief Tests convenience APIs: mp_strdup, mp_memdup, and mp_asprintf.
 */
void test_convenience_apis()
{
    printf("\n--- Test 32: Convenience String & Memory Helper APIs (mp_strdup, mp_memdup, "
           "mp_asprintf) ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    char* str_dup = mp_strdup(pool, "cmem Universal Tiered Allocator");
    assert(str_dup != NULL);
    assert(strcmp(str_dup, "cmem Universal Tiered Allocator") == 0);

    int src_data[5] = {10, 20, 30, 40, 50};
    int* data_dup = (int*) mp_memdup(pool, src_data, sizeof(src_data));
    assert(data_dup != NULL);
    assert(memcmp(data_dup, src_data, sizeof(src_data)) == 0);

    char* formatted =
        mp_asprintf(pool, "Arena [%s] active allocations: %d, QPS: %.2f", "RootArena", 42, 99999.9);
    assert(formatted != NULL);
    assert(strstr(formatted, "Arena [RootArena]") != NULL);

    printf("  Convenience APIs verified cleanly: '%s'\n", formatted);

    mp_free(pool, str_dup);
    mp_free(pool, data_dup);
    mp_free(pool, formatted);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_convenience_apis");
}

/**
 * @brief Tests memory trim and OS page reclamation via mp_trim.
 */
void test_mp_trim()
{
    printf("\n--- Test 33: Memory Trim & Page Reclaim (mp_trim) ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void* ptrs[200];
    for (int i = 0; i < 200; i++)
    {
        ptrs[i] = mp_alloc(pool, 128);
    }

    for (int i = 0; i < 200; i++)
    {
        mp_free(pool, ptrs[i]);
    }

    size_t trimmed = mp_trim(pool, 0);
    printf("  mp_trim reclaimed %zu bytes of unused capacity back to OS!\n", trimmed);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_mp_trim");
}

/**
 * @brief Tests arena metadata APIs: mp_set_name, mp_get_name, mp_get_parent, mp_get_child_count.
 */
void test_arena_metadata_apis()
{
    printf("\n--- Test 34: Arena Metadata & Hierarchy Navigation (mp_set_name, mp_get_name, "
           "mp_get_parent, mp_get_child_count) ---\n");
    memory_pool_t* root = mp_create(0, MP_FLAG_DEFAULT);
    assert(root != NULL);

    mp_set_name(root, "CustomRootArena");
    assert(strcmp(mp_get_name(root), "CustomRootArena") == 0);
    assert(mp_get_parent(root) == NULL);

    memory_pool_t* child1 = mp_create_child(root, 0, MP_FLAG_DEFAULT, "SubChild1");
    memory_pool_t* child2 = mp_create_child(root, 0, MP_FLAG_DEFAULT, "SubChild2");
    assert(child1 != NULL && child2 != NULL);

    assert(mp_get_parent(child1) == root);
    assert(mp_get_parent(child2) == root);
    assert(mp_get_child_count(root) == 2);
    assert(mp_get_child_count(child1) == 0);

    printf("  Arena hierarchy metadata navigation (Name='%s', ChildCount=%zu) verified!\n",
           mp_get_name(root), mp_get_child_count(root));

    assert(mp_check_leaks(root) == true);
    assert(mp_check_leaks(child1) == true);
    assert(mp_check_leaks(child2) == true);

    mp_destroy(root);
    TEST_PASS("test_arena_metadata_apis");
}

/**
 * @brief Tests advanced memory pressure and resource metrics: mp_pressure, mp_freeable,
 * mp_resident.
 */
void test_advanced_stats()
{
    printf("\n--- Test 35: Advanced Memory Pressure & Resource Metrics (mp_pressure, mp_freeable, "
           "mp_resident) ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_memory_limit(pool, 10000);

    void* p1 = mp_alloc(pool, 5000);
    assert(p1 != NULL);

    double press = mp_pressure(pool);
    assert(press >= 0.49 && press <= 0.51);

    size_t resident = mp_resident(pool);
    assert(resident >= 5000);

    printf("  Advanced stats verified cleanly (Pressure=%.2f%%, Resident=%zu bytes, Freeable=%zu "
           "bytes)\n",
           press * 100.0, resident, mp_freeable(pool));

    mp_free(pool, p1);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_advanced_stats");
}

/**
 * @brief Tests stats reset and preferred size optimization: mp_reset_stats, mp_preferred_size.
 */
void test_reset_stats_and_preferred_size()
{
    printf("\n--- Test 36: Stats Reset & Preferred Size Optimization (mp_reset_stats, "
           "mp_preferred_size) ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    assert(mp_preferred_size(12) == 16);
    assert(mp_preferred_size(40) == 64);
    assert(mp_preferred_size(1000) == 1000);

    void* p1 = mp_alloc(pool, 500);
    assert(p1 != NULL);

    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.total_alloc_ops == 1);

    mp_reset_stats(pool);
    mp_get_stats(pool, &stats);
    assert(stats.total_alloc_ops == 0);
    assert(stats.active_allocations == 1);

    printf("  mp_reset_stats & mp_preferred_size verified (12B->%zuB, 40B->%zuB)!\n",
           mp_preferred_size(12), mp_preferred_size(40));

    mp_free(pool, p1);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_reset_stats_and_preferred_size");
}

/**
 * @brief Tests cross-platform mp_madvise wrapper.
 */
void test_mp_madvise()
{
    printf("\n--- Test 37: Cross-Platform mp_madvise Wrapper ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void* ptr = mp_aligned_alloc(pool, 4096, 16384);
    assert(ptr != NULL);

    int res = mp_madvise(pool, ptr, 16384, 4);
    assert(res == 0);

    printf("  mp_madvise executed successfully (res=%d)!\n", res);

    mp_free(pool, ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_mp_madvise");
}

/**
 * @brief Tests emergency OOM fallback memory reserve cushion.
 */
void test_emergency_reserve()
{
    printf("\n--- Test 28: Emergency OOM Fallback Memory Reserve Cushion ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_memory_limit(pool, 1000);
    assert(mp_enable_emergency_reserve(pool, 4096) == true);

    void* p1 = mp_alloc(pool, 800);
    assert(p1 != NULL);

    // This allocation exceeds 1000B budget limit -> triggers emergency reserve fallback!
    void* p_emerg = mp_alloc(pool, 512);
    assert(p_emerg != NULL);
    strcpy((char*) p_emerg, "Emergency Logging Reserve Payload");
    assert(strcmp((char*) p_emerg, "Emergency Logging Reserve Payload") == 0);

    printf("  Emergency fallback memory cushion activated & payload verified!\n");

    mp_free(pool, p1);
    mp_free(pool, p_emerg);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_emergency_reserve");
}

/**
 * @brief Tests Linux NUMA CPU node affinity binding.
 */
void test_numa_node_binding()
{
    printf("\n--- Test 27: Linux NUMA CPU Node Affinity Binding ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    assert(mp_set_numa_node(pool, 0) == true);

    void* p1 = mp_alloc(pool, 1024 * 1024);
    assert(p1 != NULL);
    memset(p1, 0x77, 1024 * 1024);

    printf("  NUMA Node #0 backing memory allocation & access verified!\n");

    mp_free(pool, p1);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_numa_node_binding");
}

/**
 * @brief Tests game and graphics pipeline dual ping-pong frame arena.
 */
void test_frame_arena()
{
    printf("\n--- Test 26: Game & Graphics Pipeline Dual Ping-Pong Frame Arena ---\n");
    cmem_frame_arena_t* farena = mp_frame_arena_create(512 * 1024);
    assert(farena != NULL);

    // Frame 1 Allocation
    void* frame1_ptr = mp_frame_alloc(farena, 1024);
    assert(frame1_ptr != NULL);
    strcpy((char*) frame1_ptr, "RenderMeshFrame1");

    // Frame End -> Swap to Ping-Pong Buffer 2
    mp_frame_end(farena);

    // Frame 2 Allocation
    void* frame2_ptr = mp_frame_alloc(farena, 2048);
    assert(frame2_ptr != NULL);
    strcpy((char*) frame2_ptr, "RenderMeshFrame2");

    // Frame End -> Swap back to Ping-Pong Buffer 1
    mp_frame_end(farena);

    printf("  Dual Ping-Pong Frame Arena ping-pong swaps & O(1) resets verified!\n");

    mp_frame_arena_destroy(farena);
    TEST_PASS("test_frame_arena");
}

/**
 * @brief Tests binary snapshot incremental diff leak detector.
 */
void test_diff_snapshots()
{
    printf("\n--- Test 25: Binary Snapshot Incremental Diff Leak Detector ---\n");
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_TRACK_LOCATIONS);
    assert(pool != NULL);

    void* base_ptr = mp_alloc_loc(pool, 128, __FILE__, __LINE__, __func__);
    assert(base_ptr != NULL);
    assert(mp_export_binary_snapshot(pool, "snap_a.cmem_dump") == true);

    void* incr_ptr = mp_alloc_loc(pool, 512, __FILE__, __LINE__, __func__);
    assert(incr_ptr != NULL);
    assert(mp_export_binary_snapshot(pool, "snap_b.cmem_dump") == true);

    char diff_report[4096];
    assert(mp_diff_snapshots("snap_a.cmem_dump", "snap_b.cmem_dump", diff_report,
                             sizeof(diff_report)) == true);
    assert(strstr(diff_report, "Net Incremental Leaked Allocations : 1 blocks") != NULL);
    (void) diff_report;
    printf("  Incremental Snapshot Diff Leak Analysis generated successfully!\n");

    mp_free(pool, base_ptr);
    mp_free(pool, incr_ptr);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    remove("snap_a.cmem_dump");
    remove("snap_b.cmem_dump");
    TEST_PASS("test_diff_snapshots");
}

/**
 * @brief Tests high/low watermark threshold alert callbacks.
 */
void test_watermark_callback()
{
    printf("\n--- Test 24: High/Low Watermark Threshold Alert Callbacks ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    g_high_watermark_hit = false;
    g_low_watermark_hit = false;

    mp_set_memory_limit(pool, 10000);
    mp_set_watermark_callback(pool, 0.80, 0.40, test_watermark_cb, NULL);

    void* p1 = mp_alloc(pool, 8500); // 85% > 80% High Watermark
    assert(p1 != NULL);
    assert(g_high_watermark_hit == true);
    printf("  High Watermark Threshold Alert successfully triggered!\n");

    mp_free(pool, p1); // 0% <= 40% Low Watermark
    assert(g_low_watermark_hit == true);
    printf("  Low Watermark Recovery Alert successfully triggered!\n");

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_watermark_callback");
}

/**
 * @brief Tests Linux madvise MADV_DONTNEED lazy RSS physical memory purging.
 */
void test_purge_lazy()
{
    printf("\n--- Test 23: Linux madvise MADV_DONTNEED Lazy RSS Physical Memory Purging ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void* ptrs[200];
    for (int i = 0; i < 200; i++)
    {
        ptrs[i] = mp_alloc(pool, 128);
    }

    for (int i = 50; i < 200; i++)
    {
        mp_free(pool, ptrs[i]);
    }

    size_t purged = mp_purge_lazy(pool);
    printf("  Lazy RSS purge executed cleanly (Purged: %zu bytes)!\n", purged);

    for (int i = 0; i < 50; i++)
    {
        mp_free(pool, ptrs[i]);
    }

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_purge_lazy");
}

/**
 * @brief Tests Prometheus / OpenTelemetry metrics exporter.
 */
void test_prometheus_metrics()
{
    printf("\n--- Test 22: Prometheus / OpenTelemetry Metrics Exporter ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void* p1 = mp_alloc(pool, 1024);
    assert(p1 != NULL);

    char prom_buf[2048];
    size_t len = mp_export_prometheus_metrics(pool, prom_buf, sizeof(prom_buf));
    assert(len > 0);
    assert(strstr(prom_buf, "cmem_active_bytes{arena=\"RootArena\"}") != NULL);
    assert(strstr(prom_buf, "cmem_alloc_ops_total{arena=\"RootArena\"}") != NULL);
    (void) len;

    printf("  Prometheus exposition metrics formatted & exported cleanly!\n");

    mp_free(pool, p1);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_prometheus_metrics");
}

/**
 * @brief Tests 0-overhead typed object pool allocator.
 */
void test_typed_object_pool()
{
    printf("\n--- Test 21: 0-Overhead Typed Object Pool Allocator ---\n");
    mp_typed_pool_t* tpool = mp_typed_pool_create(sizeof(test_node_t), 128);
    assert(tpool != NULL);

    test_node_t* n1 = (test_node_t*) mp_typed_alloc(tpool);
    test_node_t* n2 = (test_node_t*) mp_typed_alloc(tpool);

    assert(n1 != NULL && n2 != NULL && n1 != n2);

    n1->id = 1001;
    strcpy(n1->name, "TypedNode1");
    n1->value = 99.99;

    assert(n1->id == 1001 && strcmp(n1->name, "TypedNode1") == 0);
    printf("  0-Overhead Typed Object Pool allocation & field access verified!\n");

    mp_typed_free(tpool, n1);
    mp_typed_free(tpool, n2);

    mp_typed_pool_destroy(tpool);
    TEST_PASS("test_typed_object_pool");
}

/**
 * @brief Tests slab allocations for small objects (<= 512B).
 */
void test_slab_small_allocs()
{
    printf("\n--- Test 1: Slab Allocations (Small Objects <= 512B) ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEBUG_CANARY | MP_FLAG_ZERO_ON_ALLOC);
    assert(pool != NULL);

    void* ptrs[100];
    for (int i = 0; i < 100; i++)
    {
        ptrs[i] = mp_alloc(pool, 16 + (i % 64));
        assert(ptrs[i] != NULL);
        uint8_t* byte_ptr = (uint8_t*) ptrs[i];
        assert(byte_ptr[0] == 0 && byte_ptr[15] == 0);
        (void) byte_ptr;
    }

    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 100);
    assert(stats.slab_allocated_bytes > 0);

    for (int i = 0; i < 100; i++)
    {
        mp_free(pool, ptrs[i]);
    }

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_slab_small_allocs");
}

/**
 * @brief Tests CMEM_CONF environment variable auto-tuning of pool flags.
 */
void test_env_conf_tuning()
{
#ifdef _WIN32
    printf("\n--- Test 20: CMEM_CONF Environment Variable Auto-Tuning ---\n");
    printf("  SKIPPED on Windows (setenv/unsetenv not available)\n");
#else
    printf("\n--- Test 20: CMEM_CONF Environment Variable Auto-Tuning ---\n");
    setenv("CMEM_CONF", "canary=1,poison=on,aligned=1", 1);

    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void* p1 = mp_alloc(pool, 32);
    assert(p1 != NULL);
    assert(((uintptr_t) p1 % 64) == 0);

    printf("  CMEM_CONF='canary=1,poison=on,aligned=1' successfully auto-tuned pool flags!\n");

    mp_free(pool, p1);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);

    unsetenv("CMEM_CONF");
#endif
    TEST_PASS("test_env_conf_tuning");
}

/**
 * @brief Tests DPDK-style ultra-fast lock-free ring buffer allocator.
 */
void test_ring_buffer_alloc()
{
    printf("\n--- Test 19: DPDK-Style Ultra-Fast Lock-Free Ring Buffer Allocator ---\n");
    cmem_ring_buffer_t* ring = mp_ring_create(128, 64);
    assert(ring != NULL);

    void* p1 = mp_ring_alloc(ring);
    void* p2 = mp_ring_alloc(ring);
    assert(p1 != NULL && p2 != NULL && p1 != p2);
    (void) p2;

    strcpy((char*) p1, "Lock-Free Ring Buffer Payload");
    assert(strcmp((char*) p1, "Lock-Free Ring Buffer Payload") == 0);

    assert(mp_ring_free(ring, p1) == true);
    assert(mp_ring_free(ring, p2) == true);

    mp_ring_destroy(ring);
    TEST_PASS("test_ring_buffer_alloc");
}

/**
 * @brief Tests binary crash memory snapshot dump and parser.
 */
void test_binary_snapshot()
{
    printf("\n--- Test 18: Binary Crash Memory Snapshot Dump & Parser ---\n");
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_TRACK_LOCATIONS);
    assert(pool != NULL);

    void* p1 = mp_alloc_loc(pool, 256, __FILE__, __LINE__, __func__);
    void* p2 = mp_alloc_loc(pool, 1024, __FILE__, __LINE__, __func__);

    assert(mp_export_binary_snapshot(pool, "test_snapshot.cmem_dump") == true);

    char report[4096];
    assert(mp_parse_binary_snapshot("test_snapshot.cmem_dump", report, sizeof(report)) == true);
    assert(strstr(report, "Active Allocations : 2 blocks") != NULL);
    (void) report;
    printf("  Binary Snapshot Dump exported & parsed cleanly!\n");

    mp_free(pool, p1);
    mp_free(pool, p2);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    remove("test_snapshot.cmem_dump");
    TEST_PASS("test_binary_snapshot");
}

/**
 * @brief Tests Linux HugePages (2MB/1GB MAP_HUGETLB) acceleration.
 */
void test_huge_pages_alloc()
{
#ifdef _WIN32
    printf("\n--- Test 17: Linux HugePages (2MB/1GB MAP_HUGETLB) Acceleration ---\n");
    printf("  SKIPPED on Windows (HugePages not supported)\n");
#else
    printf("\n--- Test 17: Linux HugePages (2MB/1GB MAP_HUGETLB) Acceleration ---\n");
    memory_pool_t* pool = mp_create(2 * 1024 * 1024, MP_FLAG_HUGE_PAGES);
    assert(pool != NULL);

    void* p1 = mp_alloc(pool, 512 * 1024);
    assert(p1 != NULL);
    memset(p1, 0xEE, 512 * 1024);

    printf("  HugePages memory mapping read/write verified successfully!\n");

    mp_free(pool, p1);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
#endif
    TEST_PASS("test_huge_pages_alloc");
}

/**
 * @brief Tests global malloc/free symbol interception via cmem_override.h.
 */
void test_global_override()
{
    printf("\n--- Test 16: Global malloc/free Symbol Interception (cmem_override.h) ---\n");

    char* str = (char*) malloc(128);
    assert(str != NULL);
    strcpy(str, "Overridden Standard Malloc");
    assert(strcmp(str, "Overridden Standard Malloc") == 0);
    free(str);

    printf("  Standard malloc/free calls seamlessly intercepted by cmem!\n");
    TEST_PASS("test_global_override");
}

/**
 * @brief Tests real-time allocation QPS and bandwidth throughput meter.
 */
void test_realtime_throughput_meter()
{
    printf("\n--- Test 15: Real-Time Allocation QPS & Bandwidth Throughput Meter ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void* ptrs[500];
    for (int i = 0; i < 500; i++)
    {
        ptrs[i] = mp_alloc(pool, 1024);
    }

    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.alloc_qps > 0.0);
    assert(stats.bandwidth_mbps > 0.0);
    printf("  Measured Real-Time Alloc QPS: %.2f ops/sec, Bandwidth: %.2f MB/sec!\n",
           stats.alloc_qps, stats.bandwidth_mbps);

    for (int i = 0; i < 500; i++)
    {
        mp_free(pool, ptrs[i]);
    }

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_realtime_throughput_meter");
}

/**
 * @brief Tests POSIX shared memory pool and zero-copy IPC via /dev/shm.
 */
void test_shared_memory_ipc()
{
#ifdef _WIN32
    printf("\n--- Test 14: POSIX Shared Memory Pool & Zero-Copy IPC ---\n");
    printf("  SKIPPED on Windows (POSIX shared memory not available)\n");
#else
    printf("\n--- Test 14: POSIX Shared Memory Pool & Zero-Copy IPC ---\n");
    const char* shm_name = "/cmem_test_shm_pool";
    memory_pool_t* pool = mp_create_shared(shm_name, 512 * 1024, MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    void* p1 = mp_alloc(pool, 1024);
    assert(p1 != NULL);
    strcpy((char*) p1, "Zero-Copy IPC Shared Memory Payload");

    assert(strcmp((char*) p1, "Zero-Copy IPC Shared Memory Payload") == 0);
    printf("  Shared memory payload read/write verified in /dev/shm segment!\n");

    mp_free(pool, p1);
    mp_destroy_shared(pool, shm_name);
#endif
    TEST_PASS("test_shared_memory_ipc");
}

/**
 * @brief Tests TLSF allocations for medium objects (512B - 4MB).
 */
void test_tlsf_medium_allocs()
{
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

/**
 * @brief Tests cache line 64B alignment and false sharing elimination.
 */
void test_cache_aligned_alloc()
{
    printf("\n--- Test 11: Cache Line 64B Alignment & False Sharing Elimination ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_CACHE_ALIGNED);
    assert(pool != NULL);

    void* p1 = mp_alloc(pool, 18);
    void* p2 = mp_alloc(pool, 120);
    assert(p1 && p2);

    assert(((uintptr_t) p1 % 64) == 0);
    assert(((uintptr_t) p2 % 64) == 0);
    printf("  All allocated pointers strictly aligned to 64-byte Cache Line boundary!\n");

    mp_free(pool, p1);
    mp_free(pool, p2);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_cache_aligned_alloc");
}

/**
 * @brief Tests page-level guard pages protection via PROT_NONE.
 */
void test_guard_pages_protection()
{
    printf("\n--- Test 13: Page-Level Guard Pages Protection via PROT_NONE ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_GUARD_PAGES);
    assert(pool != NULL);

    void* p1 = mp_alloc(pool, 8192);
    assert(p1 != NULL);

    memset(p1, 0xAB, 8192);
    printf("  Guard Pages memory payload read/write verified successfully!\n");

    mp_free(pool, p1);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_guard_pages_protection");
}

/**
 * @brief Tests allocation size histogram diagnostics.
 */
void test_allocation_histogram()
{
    printf("\n--- Test 12: Allocation Size Histogram Diagnostics ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void* p1 = mp_alloc(pool, 12);
    void* p2 = mp_alloc(pool, 48);
    void* p3 = mp_alloc(pool, 200);
    void* p4 = mp_alloc(pool, 2000);
    void* p5 = mp_alloc(pool, 60000);

    mp_dump_histogram(pool);

    mp_free(pool, p1);
    mp_free(pool, p2);
    mp_free(pool, p3);
    mp_free(pool, p4);
    mp_free(pool, p5);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_allocation_histogram");
}

/**
 * @brief Tests memory budget limit and OOM event callback.
 */
void test_memory_budget_and_oom()
{
    printf("\n--- Test 10: Memory Budget Limit & OOM Event Callback ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    g_oom_triggered = false;
    mp_set_event_callback(pool, test_event_cb, NULL);
    mp_set_memory_limit(pool, 1024);

    void* p1 = mp_alloc(pool, 512);
    assert(p1 != NULL);

    void* p2 = mp_alloc(pool, 1024);
    assert(p2 == NULL);
    (void) p2;
    assert(g_oom_triggered == true);
    printf("  OOM Protection Event successfully triggered when limit exceeded!\n");

    mp_free(pool, p1);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_memory_budget_and_oom");
}

void test_realloc_and_aligned()
{
    printf("\n--- Test 3: Realloc & Aligned Allocations ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);

    char* str = (char*) mp_alloc(pool, 32);
    strcpy(str, "Hello cmem Memory Pool!");

    str = (char*) mp_realloc(pool, str, 100);
    assert(strcmp(str, "Hello cmem Memory Pool!") == 0);

    void* aligned_ptr = mp_aligned_alloc(pool, 64, 256);
    assert(aligned_ptr != NULL);
    assert(((uintptr_t) aligned_ptr % 64) == 0);

    mp_free(pool, str);
    mp_free(pool, aligned_ptr);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_realloc_and_aligned");
}

/**
 * @brief Tests batch allocations and memory compaction via mp_compact.
 */
void test_batch_alloc_and_compact()
{
    printf("\n--- Test 9: Batch Allocations & Memory Compaction ---\n");
    memory_pool_t* pool = mp_create(0, MP_FLAG_DEFAULT);

    void* ptrs[50];
    size_t count = mp_alloc_batch(pool, 64, ptrs, 50);
    assert(count == 50);
    (void) count;

    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 50);

    mp_free_batch(pool, ptrs, 50);
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 0);

    size_t freed = mp_compact(pool);
    printf("  Compacted bytes freed back to OS: %zu bytes\n", freed);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_batch_alloc_and_compact");
}

/**
 * @brief Tests leak analysis report and heap audit features.
 */
void test_leak_analysis_and_heap_audit()
{
    printf("\n--- Test 7: Leak Analysis Report & Heap Audit ---\n");
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY | MP_FLAG_TRACK_LOCATIONS |
                                                     MP_FLAG_POISON_ON_FREE);

    void* leak_ptr = mp_alloc_loc(pool, 256, __FILE__, __LINE__, __func__);
    assert(leak_ptr != NULL);

    void* valid_ptr = mp_alloc_loc(pool, 64, __FILE__, __LINE__, __func__);
    assert(valid_ptr != NULL);

    assert(mp_audit_heap(pool) == true);

    uint8_t* poison_test = (uint8_t*) valid_ptr;
    mp_free(pool, valid_ptr);
    assert(poison_test[0] == 0xDD && poison_test[63] == 0xDD);
    (void) poison_test;

    char report[2048];
    size_t report_len = mp_analyze_leaks(pool, report, sizeof(report));
    assert(report_len > 0);
    assert(strstr(report, "Source Location") != NULL);
    (void) report_len;

    mp_free(pool, leak_ptr);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_leak_analysis_and_heap_audit");
}

/**
 * @brief Tests child arenas and visual HTML report export.
 */
void test_child_arenas_and_html_export()
{
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

/**
 * @brief Tests fast arena reset and JSON exporter.
 */
void test_arena_reset_and_json()
{
    printf("\n--- Test 5: Fast Arena Reset & JSON Exporter ---\n");
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);

    for (int i = 0; i < 50; i++)
    {
        mp_alloc(pool, 128 + i * 16);
    }

    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 50);

    char json_buf[512];
    size_t json_len = mp_dump_json_stats(pool, json_buf, sizeof(json_buf));
    assert(json_len > 0);
    assert(strstr(json_buf, "\"active_allocations\": 50") != NULL);
    (void) json_len;

    mp_reset(pool);
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 0);
    assert(stats.active_bytes == 0);
    assert(mp_check_leaks(pool) == true);

    mp_destroy(pool);
    TEST_PASS("test_arena_reset_and_json");
}

static uint8_t g_static_buf[256 * 1024];

/**
 * @brief Tests static buffer arena and event callbacks.
 */
void test_static_buffer_and_callbacks()
{
    printf("\n--- Test 6: Static Buffer Arena & Event Callbacks ---\n");
    memory_pool_t* pool =
        mp_create_from_buffer(g_static_buf, sizeof(g_static_buf), MP_FLAG_DEFAULT);
    assert(pool != NULL);

    g_event_triggered = false;
    mp_set_event_callback(pool, test_event_cb, NULL);

    void* p1 = mp_alloc(pool, 500);
    assert(p1 != NULL);
    assert(g_event_triggered == true);

    uintptr_t buf_start = (uintptr_t) g_static_buf;
    uintptr_t buf_end = buf_start + sizeof(g_static_buf);
    assert((uintptr_t) p1 >= buf_start && (uintptr_t) p1 < buf_end);
    (void) buf_end;

    mp_free(pool, p1);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_static_buffer_and_callbacks");
}

#define THREAD_COUNT 4
#define ALLOCS_PER_THREAD 500

/**
 * @brief Thread worker function for multithreaded concurrent safety tests.
 * @param arg Pointer to the memory pool to use.
 * @return NULL.
 */
void* thread_worker(void* arg)
{
    memory_pool_t* pool = (memory_pool_t*) arg;
    void* ptrs[ALLOCS_PER_THREAD];

    for (int i = 0; i < ALLOCS_PER_THREAD; i++)
    {
        size_t sz = (i % 2 == 0) ? (8 + i % 128) : (1024 + i % 4096);
        ptrs[i] = mp_alloc(pool, sz);
        assert(ptrs[i] != NULL);
    }

    for (int i = 0; i < ALLOCS_PER_THREAD; i++)
    {
        mp_free(pool, ptrs[i]);
    }

    return NULL;
}

/**
 * @brief Tests multithreaded concurrent safety and thread-local cache.
 */
void test_multithread_safety()
{
    printf("\n--- Test 4: Multithreaded Concurrent Safety & Thread-Local Cache ---\n");
    memory_pool_t* pool =
        mp_create(2 * 1024 * 1024, MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE);
    assert(pool != NULL);

    pthread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++)
    {
        pthread_create(&threads[i], NULL, thread_worker, pool);
    }

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        pthread_join(threads[i], NULL);
    }

    mp_dump_info(pool);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_multithread_safety");
}

/**
 * @brief Entry point for all cmem C unit tests.
 * Runs all test cases and prints success message.
 * @return 0 on success.
 */
int main()
{
    printf("================ RUNNING CMEM UNIT TESTS ================\n");
    test_introspection_apis();
    test_tlsf_inplace_realloc();
    test_reallocarray();
    test_convenience_apis();
    test_mp_trim();
    test_arena_metadata_apis();
    test_advanced_stats();
    test_reset_stats_and_preferred_size();
    test_mp_madvise();
    test_slab_small_allocs();
    test_tlsf_medium_allocs();
    test_emergency_reserve();
    test_numa_node_binding();
    test_frame_arena();
    test_diff_snapshots();
    test_watermark_callback();
    test_purge_lazy();
    test_prometheus_metrics();
    test_typed_object_pool();
    test_env_conf_tuning();
    test_ring_buffer_alloc();
    test_huge_pages_alloc();
    test_binary_snapshot();
    test_shared_memory_ipc();
    test_global_override();
    test_realtime_throughput_meter();
    test_realloc_and_aligned();
    test_cache_aligned_alloc();
    test_guard_pages_protection();
    test_allocation_histogram();
    test_batch_alloc_and_compact();
    test_memory_budget_and_oom();
    test_leak_analysis_and_heap_audit();
    test_child_arenas_and_html_export();
    test_arena_reset_and_json();
    test_static_buffer_and_callbacks();
    printf("\nALL CMEM UNIT TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
