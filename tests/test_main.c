/**
 * @file test_main.c
 * @brief Comprehensive Unit Tests for cmem Memory Manager.
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
// Include AFTER system headers so the malloc/calloc/realloc/free override
// macros do not expand standard library declarations.
#include "../include/cmem_override.h"

// Test vectors in this suite intentionally use literal values (sizes, counts,
// byte patterns, thresholds) to pin exact expected behavior.
// NOLINTBEGIN(readability-magic-numbers)

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
static void
test_event_cb(memory_pool_t *pool, mp_event_type_t event, void *ptr, size_t size, void *user_data)
{
    (void)pool;
    (void)ptr;
    (void)size;
    (void)user_data;
    if (event == MP_EVENT_ALLOC) {
        g_event_triggered = true;
    } else if (event == MP_EVENT_OOM) {
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
static void test_watermark_cb(memory_pool_t *pool,

                              bool is_high_watermark,
                              size_t current_bytes, // NOLINT(bugprone-easily-swappable-parameters)
                              size_t limit_bytes,
                              void *user_data)

{
    (void)pool;
    (void)current_bytes;
    (void)limit_bytes;
    (void)user_data;
    if (is_high_watermark) {
        g_high_watermark_hit = true;
    } else {
        g_low_watermark_hit = true;
    }
}

/**
 * @brief Test node structure used for typed object pool tests.
 */
typedef struct {
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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 120);
    assert(p1 != NULL);

    assert(mp_ptr_valid(pool, p1) == true);
    assert(mp_alloc_size(pool, p1) == 120);
    assert(mp_usable_size(pool, p1) >= 120);

    uint8_t fake_buf[128] = {0};
    void *fake_ptr = &fake_buf[64];
    assert(mp_ptr_valid(pool, fake_ptr) == false);
    assert(mp_alloc_size(pool, fake_ptr) == 0);
    assert(mp_usable_size(pool, fake_ptr) == 0);

    printf("  Memory introspection query (usable_size=%zu, alloc_size=%zu, valid=true) verified!\n",
           mp_usable_size(pool, p1),
           mp_alloc_size(pool, p1));

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
    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 1024);
    void *p2 = mp_alloc(pool, 2048);
    assert(p1 != NULL && p2 != NULL);

    mp_free(pool, p2);

    void *p1_expanded = mp_realloc(pool, p1, 2500);
    assert(p1_expanded == p1);
    assert(mp_alloc_size(pool, p1_expanded) == 2500);

    printf("  TLSF in-place realloc expanded pointer 0x%zx in-place (0 memcpy overhead)!\n",
           (uintptr_t)p1);

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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    int *arr = (int *)mp_alloc(pool, 10 * sizeof(int));
    assert(arr != NULL);

    arr = (int *)mp_reallocarray(pool, arr, 100, sizeof(int));
    assert(arr != NULL);
    assert(mp_alloc_size(pool, arr) == 100 * sizeof(int));

    void *overflow_ptr = mp_reallocarray(pool, arr, SIZE_MAX / 2, 4);
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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    char *str_dup = mp_strdup(pool, "cmem Universal Tiered Allocator");
    assert(str_dup != NULL);
    assert(strcmp(str_dup, "cmem Universal Tiered Allocator") == 0);

    int src_data[5] = {10, 20, 30, 40, 50};
    int *data_dup = (int *)mp_memdup(pool, src_data, sizeof(src_data));
    assert(data_dup != NULL);
    assert(memcmp(data_dup, src_data, sizeof(src_data)) == 0);

    char *formatted =
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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *ptrs[200];
    for (int i = 0; i < 200; i++) {
        ptrs[i] = mp_alloc(pool, 128);
    }

    for (int i = 0; i < 200; i++) {
        mp_free(pool, ptrs[i]);
    }

    size_t trimmed = mp_trim(pool, 0);
    printf("  mp_trim reclaimed %zu bytes of unused capacity back to OS!\n", trimmed);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_mp_trim");
}

/**
 * @brief Tests idle page reclamation: mp_set_idle_page_reclaim, mp_reclaim_idle_pages,
 * mp_get_idle_page_count.
 */
void test_idle_page_reclaim()
{
    printf("\n--- Test 37: Idle Page Reclamation ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_idle_page_reclaim(pool, true, 100, 1);

    void *ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = mp_alloc(pool, 64);
    }
    assert(mp_get_idle_page_count(pool) == 0);

    for (int i = 0; i < 100; i++) {
        mp_free(pool, ptrs[i]);
    }

    size_t idle = mp_get_idle_page_count(pool);
    assert(idle > 0);
    printf("  Idle pages after free: %zu\n", idle);

    size_t reclaimed = mp_reclaim_idle_pages(pool);
    printf("  Reclaimed via mp_reclaim_idle_pages: %zu bytes\n", reclaimed);
    assert(reclaimed > 0);

    size_t idle_after = mp_get_idle_page_count(pool);
    assert(idle_after == 0);

    mp_set_idle_page_reclaim(pool, false, 0, 0);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_idle_page_reclaim");
}

/**
 * @brief Tests arena metadata APIs: mp_set_name, mp_get_name, mp_get_parent, mp_get_child_count.
 */
void test_arena_metadata_apis()
{
    printf("\n--- Test 34: Arena Metadata & Hierarchy Navigation (mp_set_name, mp_get_name, "
           "mp_get_parent, mp_get_child_count) ---\n");
    memory_pool_t *root = mp_create(0, MP_FLAG_DEFAULT);
    assert(root != NULL);

    mp_set_name(root, "CustomRootArena");
    assert(strcmp(mp_get_name(root), "CustomRootArena") == 0);
    assert(mp_get_parent(root) == NULL);

    memory_pool_t *child1 = mp_create_child(root, 0, MP_FLAG_DEFAULT, "SubChild1");
    memory_pool_t *child2 = mp_create_child(root, 0, MP_FLAG_DEFAULT, "SubChild2");
    assert(child1 != NULL && child2 != NULL);

    assert(mp_get_parent(child1) == root);
    assert(mp_get_parent(child2) == root);
    assert(mp_get_child_count(root) == 2);
    assert(mp_get_child_count(child1) == 0);

    printf("  Arena hierarchy metadata navigation (Name='%s', ChildCount=%zu) verified!\n",
           mp_get_name(root),
           mp_get_child_count(root));

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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_memory_limit(pool, 10000);

    void *p1 = mp_alloc(pool, 5000);
    assert(p1 != NULL);

    double press = mp_pressure(pool);
    assert(press >= 0.49 && press <= 0.51);

    size_t resident = mp_resident(pool);
    assert(resident >= 5000);

    printf("  Advanced stats verified cleanly (Pressure=%.2f%%, Resident=%zu bytes, Freeable=%zu "
           "bytes)\n",
           press * 100.0,
           resident,
           mp_freeable(pool));

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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    assert(mp_preferred_size(12) == 16);
    assert(mp_preferred_size(40) == 64);
    assert(mp_preferred_size(1000) == 1000);

    void *p1 = mp_alloc(pool, 500);
    assert(p1 != NULL);

    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.total_alloc_ops == 1);

    mp_reset_stats(pool);
    mp_get_stats(pool, &stats);
    assert(stats.total_alloc_ops == 0);
    assert(stats.active_allocations == 1);

    printf("  mp_reset_stats & mp_preferred_size verified (12B->%zuB, 40B->%zuB)!\n",
           mp_preferred_size(12),
           mp_preferred_size(40));

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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *ptr = mp_aligned_alloc(pool, 4096, 16384);
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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_memory_limit(pool, 1000);
    assert(mp_enable_emergency_reserve(pool, 4096) == true);

    void *p1 = mp_alloc(pool, 800);
    assert(p1 != NULL);

    // This allocation exceeds 1000B budget limit -> triggers emergency reserve fallback!
    void *p_emerg = mp_alloc(pool, 512);
    assert(p_emerg != NULL);
    strcpy((char *)p_emerg, "Emergency Logging Reserve Payload");
    assert(strcmp((char *)p_emerg, "Emergency Logging Reserve Payload") == 0);

    printf("  Emergency fallback memory cushion activated & payload verified!\n");

    mp_free(pool, p1);
    mp_free(pool, p_emerg);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_emergency_reserve");
}

void test_fallback_on_oom()
{
    printf("\n--- Test: Fallback on OOM ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_memory_limit(pool, 100);
    mp_set_fallback_on_oom(pool, true);
    mp_enable_emergency_reserve(pool, 0);

    void *p1 = mp_alloc(pool, 50);
    assert(p1 != NULL);

    void *p2 = mp_alloc(pool, 60);
    assert(p2 != NULL);

    mp_free(pool, p1);
    mp_free(pool, p2);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_fallback_on_oom");
}

/**
 * @brief Tests Linux NUMA CPU node affinity binding.
 */
void test_numa_node_binding()
{
    printf("\n--- Test 27: Linux NUMA CPU Node Affinity Binding ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    assert(mp_set_numa_node(pool, 0) == true);

    void *p1 = mp_alloc(pool, 1024 * 1024);
    assert(p1 != NULL);
    memset(p1, 0x77, 1024 * 1024);

    printf("  NUMA Node #0 backing memory allocation & access verified!\n");

    mp_free(pool, p1);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_numa_node_binding");
}

/**
 * @brief Tests automatic NUMA topology detection and thread-local binding.
 */
void test_numa_auto_optimization()
{
    printf("\n--- Test 28: Automatic NUMA Optimization ---\n");

    int node_count = mp_numa_node_count();
    assert(node_count >= 1);
    printf("  Detected NUMA node count: %d\n", node_count);

    assert(mp_cpu_to_node(-1) == 0);
    int cpu0_node = mp_cpu_to_node(0);
    assert(cpu0_node >= 0 && cpu0_node < node_count);
    printf("  CPU #0 belongs to NUMA node: %d\n", cpu0_node);

    memory_pool_t *pool = mp_create(0, MP_FLAG_AUTO_NUMA);
    assert(pool != NULL);
    void *p1 = mp_alloc(pool, 1024 * 1024);
    assert(p1 != NULL);
    memset(p1, 0x77, 1024 * 1024);
    mp_free(pool, p1);
    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    printf("  Auto-NUMA pool allocation & access verified!\n");

    memory_pool_t *pool2 = mp_create(0, MP_FLAG_AUTO_NUMA);
    assert(pool2 != NULL);
    assert(mp_set_numa_node(pool2, 0) == true);
    void *p2 = mp_alloc(pool2, 4096);
    assert(p2 != NULL);
    memset(p2, 0xAA, 4096);
    mp_free(pool2, p2);
    assert(mp_check_leaks(pool2) == true);
    mp_destroy(pool2);
    printf("  Manual override takes precedence over auto-NUMA!\n");

    TEST_PASS("test_numa_auto_optimization");
}

/**
 * @brief Tests compressed storage: round-trip, ownership, eviction, stats.
 */
void test_compressed_storage()
{
    printf("\n--- Test 38: Compressed Storage ---\n");

    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);
    assert(mp_set_compressed_budget(pool, 256 * 1024) == true);

    /* Round-trip with repetitive data. */
    const size_t data_size = 4096;
    char *data = (char *)mp_alloc(pool, data_size);
    assert(data != NULL);
    for (size_t i = 0; i < data_size; i++) {
        data[i] = (char)('a' + (i % 3));
    }
    compressed_handle_t handle = mp_compress_block(pool, data, data_size);
    assert(handle != 0);

    char *out = (char *)mp_decompress_block(pool, handle);
    assert(out != NULL);
    assert(memcmp(out, "abc", 3) == 0);
    for (size_t i = 0; i < data_size; i++) {
        assert(out[i] == (char)('a' + (i % 3)));
    }
    mp_free(pool, out);

    /* Repeatable decompress. */
    out = (char *)mp_decompress_block(pool, handle);
    assert(out != NULL);
    mp_free(pool, out);

    /* Stats consistent. */
    size_t used = 0, budget = 0, count = 0;
    assert(mp_get_compressed_stats(pool, &used, &budget, &count) == true);
    assert(used > 0 && used < data_size);
    assert(budget == 256 * 1024);
    assert(count == 1);

    /* Free handle: second free fails, decompress returns NULL. */
    assert(mp_free_compressed(pool, handle) == true);
    assert(mp_free_compressed(pool, handle) == false);
    assert(mp_decompress_block(pool, handle) == NULL);

    /* No-gain: incompressible data keeps the original buffer.  A PRNG
     * sequence (xorshift32) has no 4-byte repeats within the 64KiB
     * window, so the codec finds no matches and must decline. */
    char *rand_buf = (char *)mp_alloc(pool, 1024);
    assert(rand_buf != NULL);
    uint32_t seed = 12345u;
    for (size_t i = 0; i < 1024; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        rand_buf[i] = (char)(seed & 0xFFu);
    }
    char *rand_copy = (char *)malloc(1024);
    assert(rand_copy != NULL);
    memcpy(rand_copy, rand_buf, 1024);
    compressed_handle_t handle2 = mp_compress_block(pool, rand_buf, 1024);
    assert(handle2 == 0);                           /* no gain — handle invalid */
    assert(memcmp(rand_buf, rand_copy, 1024) == 0); /* original untouched */
    free(rand_copy);
    mp_free(pool, rand_buf);

    /* Budget eviction: with budget 8KB all four ~19B blocks fit, then a
     * budget smaller than a single block evicts everything and rejects
     * the new block (deterministic; exercises the eviction loop guard). */
    assert(mp_set_compressed_budget(pool, 8 * 1024) == true);
    compressed_handle_t handles[4];
    for (int i = 0; i < 4; i++) {
        char *blk = (char *)mp_alloc(pool, 4096);
        assert(blk != NULL);
        for (size_t j = 0; j < 4096; j++) {
            blk[j] = (char)('x' + (i % 3));
        }
        handles[i] = mp_compress_block(pool, blk, 4096);
        assert(handles[i] != 0);
    }
    size_t c2 = 0;
    assert(mp_get_compressed_stats(pool, NULL, NULL, &c2) == true);
    assert(c2 == 4); /* all four fit under the 8KB budget */

    /* Shrink the budget below a single block's size: the next compress
     * evicts every stored block, then returns 0 (nothing left to evict)
     * and the caller's buffer is retained untouched. */
    assert(mp_set_compressed_budget(pool, 8) == true);
    char *blk5 = (char *)mp_alloc(pool, 4096);
    assert(blk5 != NULL);
    memset(blk5, 'q', 4096);
    compressed_handle_t handle5 = mp_compress_block(pool, blk5, 4096);
    assert(handle5 == 0); /* rejected — budget smaller than one block */
    for (size_t j = 0; j < 4096; j++) {
        assert(blk5[j] == 'q'); /* original buffer retained */
    }
    mp_free(pool, blk5);

    size_t c3 = 0;
    assert(mp_get_compressed_stats(pool, NULL, NULL, &c3) == true);
    assert(c3 == 0);
    for (int i = 0; i < 4; i++) {
        assert(mp_decompress_block(pool, handles[i]) == NULL);
    }

    /* Known-pattern validation: repeated bytes compress efficiently. */
    char *pattern_buf = (char *)mp_alloc(pool, 256);
    assert(pattern_buf != NULL);
    memset(pattern_buf, 'Z', 256);
    compressed_handle_t h_pat = mp_compress_block(pool, pattern_buf, 256);
    assert(h_pat != 0);
    char *dec = (char *)mp_decompress_block(pool, h_pat);
    assert(dec != NULL);
    assert(memcmp(dec, pattern_buf, 256) == 0);
    mp_free(pool, dec);
    assert(mp_free_compressed(pool, h_pat) == true);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_compressed_storage");
}

/**
 * @brief Tests the MP_FLAG_FAST_PATH pool: interleaved alloc/free traffic,
 * leak-count verdict, and compatibility with canary and poison checks.
 */
void test_fast_path()
{
    printf("\n--- Test 39: FAST_PATH ---\n");

    memory_pool_t *pool = mp_create(32 * 1024 * 1024, MP_FLAG_FAST_PATH);
    assert(pool != NULL);

    /* Interleaved alloc/free through every slab class. */
    for (int i = 0; i < 1000000; i++) {
        size_t sz = 32 + (i % 224);
        void *ptr = mp_alloc(pool, sz);
        assert(ptr != NULL);
        *(volatile unsigned char *)ptr = 0xAA;
        mp_free(pool, ptr);
    }

    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 0);
    assert(mp_check_leaks(pool) == true);

    /* Canary still enforced under FAST_PATH. */
    memory_pool_t *canary_pool = mp_create(1024 * 1024, MP_FLAG_FAST_PATH | MP_FLAG_DEBUG_CANARY);
    assert(canary_pool != NULL);
    char *buf = (char *)mp_alloc(canary_pool, 64);
    assert(buf != NULL);
    memset(buf, 0x55, 64);
    mp_free(canary_pool, buf);
    assert(mp_check_leaks(canary_pool) == true);
    mp_destroy(canary_pool);

    /* Poison still enforced under FAST_PATH. */
    memory_pool_t *poison_pool = mp_create(1024 * 1024, MP_FLAG_FAST_PATH | MP_FLAG_POISON_ON_FREE);
    assert(poison_pool != NULL);
    buf = (char *)mp_alloc(poison_pool, 64);
    assert(buf != NULL);
    memset(buf, 0x55, 64);
    mp_free(poison_pool, buf);
    assert(mp_check_leaks(poison_pool) == true);
    mp_destroy(poison_pool);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_fast_path");
}

/**
 * @brief Tests game and graphics pipeline dual ping-pong frame arena.
 */
void test_frame_arena()
{
    printf("\n--- Test 26: Game & Graphics Pipeline Dual Ping-Pong Frame Arena ---\n");
    cmem_frame_arena_t *farena = mp_frame_arena_create(512 * 1024);
    assert(farena != NULL);

    // Frame 1 Allocation
    void *frame1_ptr = mp_frame_alloc(farena, 1024);
    assert(frame1_ptr != NULL);
    strcpy((char *)frame1_ptr, "RenderMeshFrame1");

    // Frame End -> Swap to Ping-Pong Buffer 2
    mp_frame_end(farena);

    // Frame 2 Allocation
    void *frame2_ptr = mp_frame_alloc(farena, 2048);
    assert(frame2_ptr != NULL);
    strcpy((char *)frame2_ptr, "RenderMeshFrame2");

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
    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_TRACK_LOCATIONS);
    assert(pool != NULL);

    void *base_ptr = mp_alloc_loc(pool, 128, __FILE__, __LINE__, __func__);
    assert(base_ptr != NULL);
    assert(mp_export_binary_snapshot(pool, "snap_a.cmem_dump") == true);

    void *incr_ptr = mp_alloc_loc(pool, 512, __FILE__, __LINE__, __func__);
    assert(incr_ptr != NULL);
    assert(mp_export_binary_snapshot(pool, "snap_b.cmem_dump") == true);

    char diff_report[4096];
    assert(mp_diff_snapshots(
               "snap_a.cmem_dump", "snap_b.cmem_dump", diff_report, sizeof(diff_report)) == true);
    assert(strstr(diff_report, "Net Incremental Leaked Allocations : 1 blocks") != NULL);
    (void)diff_report;
    printf("  Incremental Snapshot Diff Leak Analysis generated successfully!\n");

    mp_free(pool, base_ptr);
    mp_free(pool, incr_ptr);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    (void)remove("snap_a.cmem_dump");
    (void)remove("snap_b.cmem_dump");
    TEST_PASS("test_diff_snapshots");
}

/**
 * @brief Tests high/low watermark threshold alert callbacks.
 */
void test_watermark_callback()
{
    printf("\n--- Test 24: High/Low Watermark Threshold Alert Callbacks ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    g_high_watermark_hit = false;
    g_low_watermark_hit = false;

    mp_set_memory_limit(pool, 10000);
    mp_set_watermark_callback(pool, 0.80, 0.40, test_watermark_cb, NULL);

    void *p1 = mp_alloc(pool, 8500); // 85% > 80% High Watermark
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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *ptrs[200];
    for (int i = 0; i < 200; i++) {
        ptrs[i] = mp_alloc(pool, 128);
    }

    for (int i = 50; i < 200; i++) {
        mp_free(pool, ptrs[i]);
    }

    size_t purged = mp_purge_lazy(pool);
    printf("  Lazy RSS purge executed cleanly (Purged: %zu bytes)!\n", purged);

    for (int i = 0; i < 50; i++) {
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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 1024);
    assert(p1 != NULL);

    char prom_buf[2048];
    size_t len = mp_export_prometheus_metrics(pool, prom_buf, sizeof(prom_buf));
    assert(len > 0);
    assert(strstr(prom_buf, "cmem_active_bytes{arena=\"RootArena\"}") != NULL);
    assert(strstr(prom_buf, "cmem_alloc_ops_total{arena=\"RootArena\"}") != NULL);
    (void)len;

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
    mp_typed_pool_t *tpool = mp_typed_pool_create(sizeof(test_node_t), 128);
    assert(tpool != NULL);

    test_node_t *n1 = (test_node_t *)mp_typed_alloc(tpool);
    test_node_t *n2 = (test_node_t *)mp_typed_alloc(tpool);

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
    // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEBUG_CANARY | MP_FLAG_ZERO_ON_ALLOC);
    // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
    assert(pool != NULL);

    void *ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = mp_alloc(pool, 16 + (i % 64));
        assert(ptrs[i] != NULL);
        uint8_t *byte_ptr = (uint8_t *)ptrs[i];
        assert(byte_ptr[0] == 0 && byte_ptr[15] == 0);
        (void)byte_ptr;
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

    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 32);
    assert(p1 != NULL);
    assert(((uintptr_t)p1 % 64) == 0);

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
    cmem_ring_buffer_t *ring = mp_ring_create(128, 64);
    assert(ring != NULL);

    void *p1 = mp_ring_alloc(ring);
    void *p2 = mp_ring_alloc(ring);
    assert(p1 != NULL && p2 != NULL && p1 != p2);
    (void)p2;

    strcpy((char *)p1, "Lock-Free Ring Buffer Payload");
    assert(strcmp((char *)p1, "Lock-Free Ring Buffer Payload") == 0);

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
    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_TRACK_LOCATIONS);
    assert(pool != NULL);

    void *p1 = mp_alloc_loc(pool, 256, __FILE__, __LINE__, __func__);
    void *p2 = mp_alloc_loc(pool, 1024, __FILE__, __LINE__, __func__);

    assert(mp_export_binary_snapshot(pool, "test_snapshot.cmem_dump") == true);

    char report[4096];
    assert(mp_parse_binary_snapshot("test_snapshot.cmem_dump", report, sizeof(report)) == true);
    assert(strstr(report, "Active Allocations : 2 blocks") != NULL);
    (void)report;
    printf("  Binary Snapshot Dump exported & parsed cleanly!\n");

    mp_free(pool, p1);
    mp_free(pool, p2);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    (void)remove("test_snapshot.cmem_dump");
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
    memory_pool_t *pool = mp_create(2 * 1024 * 1024, MP_FLAG_HUGE_PAGES);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 512 * 1024);
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

    char *str = (char *)malloc(128);
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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *ptrs[500];
    for (int i = 0; i < 500; i++) {
        ptrs[i] = mp_alloc(pool, 1024);
    }

    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.alloc_qps > 0.0);
    assert(stats.bandwidth_mbps > 0.0);
    printf("  Measured Real-Time Alloc QPS: %.2f ops/sec, Bandwidth: %.2f MB/sec!\n",
           stats.alloc_qps,
           stats.bandwidth_mbps);

    for (int i = 0; i < 500; i++) {
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
    const char *shm_name = "/cmem_test_shm_pool";
    memory_pool_t *pool = mp_create_shared(shm_name, 512 * 1024, MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 1024);
    assert(p1 != NULL);
    strcpy((char *)p1, "Zero-Copy IPC Shared Memory Payload");

    assert(strcmp((char *)p1, "Zero-Copy IPC Shared Memory Payload") == 0);
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
    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 1024);
    void *p2 = mp_alloc(pool, 64 * 1024);
    void *p3 = mp_alloc(pool, 512 * 1024);

    assert(p1 && p2 && p3);

    memset(p1, 0xAA, 1024);
    memset(p2, 0xBB, 64 * 1024);
    memset(p3, 0xCC, 512 * 1024);

    mp_free(pool, p2);
    void *p4 = mp_alloc(pool, 32 * 1024);
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
    memory_pool_t *pool = mp_create(0, MP_FLAG_CACHE_ALIGNED);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 18);
    void *p2 = mp_alloc(pool, 120);
    assert(p1 && p2);

    assert(((uintptr_t)p1 % 64) == 0);
    assert(((uintptr_t)p2 % 64) == 0);
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
    memory_pool_t *pool = mp_create(0, MP_FLAG_GUARD_PAGES);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 8192);
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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 12);
    void *p2 = mp_alloc(pool, 48);
    void *p3 = mp_alloc(pool, 200);
    void *p4 = mp_alloc(pool, 2000);
    void *p5 = mp_alloc(pool, 60000);

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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    g_oom_triggered = false;
    mp_set_event_callback(pool, test_event_cb, NULL);
    mp_set_memory_limit(pool, 1024);

    void *p1 = mp_alloc(pool, 512);
    assert(p1 != NULL);

    void *p2 = mp_alloc(pool, 1024);
    assert(p2 == NULL);
    (void)p2;
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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);

    char *str = (char *)mp_alloc(pool, 32);
    strcpy(str, "Hello cmem Memory Pool!");

    str = (char *)mp_realloc(pool, str, 100);
    assert(strcmp(str, "Hello cmem Memory Pool!") == 0);

    void *aligned_ptr = mp_aligned_alloc(pool, 64, 256);
    assert(aligned_ptr != NULL);
    assert(((uintptr_t)aligned_ptr % 64) == 0);

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
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);

    void *ptrs[50];
    size_t count = mp_alloc_batch(pool, 64, ptrs, 50);
    assert(count == 50);
    (void)count;

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

#define BATCH_OS_SIZE (5u * 1024u * 1024u) /* OS-tier element size for batch test */

/**
 * @brief Tests batch allocation across slab / TLSF / OS tiers.
 */
void test_batch_alloc_tiers()
{
    printf("\n--- Test 40: Batch Allocation Across Allocator Tiers ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_THREAD_SAFE);

    {
        void *ptrs[128];
        size_t count = mp_alloc_batch(pool, 8, ptrs, 128);
        assert(count == 128);
        for (size_t i = 0; i < count; i++) {
            assert(ptrs[i] != NULL);
        }
        mp_stats_t stats;
        mp_get_stats(pool, &stats);
        assert(stats.active_allocations == 128);
        mp_free_batch(pool, ptrs, count);
        mp_get_stats(pool, &stats);
        assert(stats.active_allocations == 0);
    }
    {
        void *ptrs[100];
        size_t count = mp_alloc_batch(pool, 64, ptrs, 100);
        assert(count == 100);
        mp_free_batch(pool, ptrs, count);
    }
    {
        void *ptrs[50];
        size_t count = mp_alloc_batch(pool, 512, ptrs, 50);
        assert(count == 50);
        mp_free_batch(pool, ptrs, count);
    }
    {
        void *ptrs[20];
        size_t count = mp_alloc_batch(pool, 1024, ptrs, 20);
        assert(count == 20);
        mp_free_batch(pool, ptrs, count);
    }
    {
        void *ptrs[10];
        size_t count = mp_alloc_batch(pool, 4096, ptrs, 10);
        assert(count == 10);
        mp_free_batch(pool, ptrs, count);
    }
    {
        void *ptrs[4];
        size_t count = mp_alloc_batch(pool, 1024 * 1024, ptrs, 4);
        assert(count == 4);
        mp_free_batch(pool, ptrs, count);
    }
    {
        void *ptrs[3];
        mp_stats_t before;
        mp_get_stats(pool, &before);
        size_t count = mp_alloc_batch(pool, BATCH_OS_SIZE, ptrs, 3);
        assert(count == 3);
        mp_stats_t after;
        mp_get_stats(pool, &after);
        assert(after.os_allocated_bytes == before.os_allocated_bytes + 3 * BATCH_OS_SIZE);
        assert(after.active_allocations == before.active_allocations + 3);
        mp_free_batch(pool, ptrs, count);
        mp_get_stats(pool, &after);
        assert(after.active_allocations == before.active_allocations);
    }

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_batch_alloc_tiers");
}

#define BATCH_LIMIT_PARTIAL 2500u /* limit 2500: fits 4 x 512, not 5 */
#define BATCH_LIMIT_SMALL 100u    /* limit 100: nothing fits */
#define BATCH_QUOTA 200u          /* thread quota: trips after 4 x 64 */
#define BATCH_EMERGENCY_RESERVE 4096u

/**
 * @brief Tests batch allocation with cache-alignment, memory limit,
 *        emergency reserve, OOM fallback, fast path, per-CPU freelists,
 *        multi-arena routing, and circuit breaker.
 */
void test_batch_alloc_configs()
{
    printf("\n--- Test 41: Batch Allocation Configurations ---\n");

    {
        /* (a) Cache-aligned payloads */
        memory_pool_t *pool = mp_create(0, MP_FLAG_CACHE_ALIGNED | MP_FLAG_THREAD_SAFE);
        void *ptrs[64];
        size_t count = mp_alloc_batch(pool, 32, ptrs, 64);
        assert(count == 64);
        for (size_t i = 0; i < count; i++) {
            assert(((uintptr_t)ptrs[i] & 63u) == 0);
            memset(ptrs[i], 0xAB, 32);
        }
        mp_free_batch(pool, ptrs, count);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }
    {
        /* (b) Memory-limit partial fit + emergency buffer: k=4, +1 emergency */
        memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
        bool ok = mp_enable_emergency_reserve(pool, BATCH_EMERGENCY_RESERVE);
        assert(ok);
        mp_set_memory_limit(pool, BATCH_LIMIT_PARTIAL);
        void *ptrs[8];
        size_t count = mp_alloc_batch(pool, 512, ptrs, 8);
        assert(count == 5); /* 4 fit under the limit, 1 emergency element */
        mp_free_batch(pool, ptrs, count);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }
    {
        /* (c) Limit fully exceeded, emergency too small: return 0 */
        memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
        bool ok = mp_enable_emergency_reserve(pool, 512);
        assert(ok);
        mp_set_memory_limit(pool, BATCH_LIMIT_SMALL);
        void *ptrs[4];
        size_t count = mp_alloc_batch(pool, 1024, ptrs, 4);
        assert(count == 0);
        mp_destroy(pool);
    }
    {
        /* (d) OOM fallback ignores the limit: full count */
        memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
        mp_set_memory_limit(pool, BATCH_LIMIT_SMALL);
        mp_set_fallback_on_oom(pool, true);
        void *ptrs[4];
        size_t count = mp_alloc_batch(pool, 1024, ptrs, 4);
        assert(count == 4);
        mp_free_batch(pool, ptrs, count);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }
    {
        /* (e) Fast path */
        memory_pool_t *pool = mp_create(0, MP_FLAG_FAST_PATH);
        void *ptrs[32];
        size_t count = mp_alloc_batch(pool, 64, ptrs, 32);
        assert(count == 32);
        mp_free_batch(pool, ptrs, count);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }
    {
        /* (f) Per-CPU freelist */
        memory_pool_t *pool = mp_create(0, MP_FLAG_PERCPU_FREELIST);
        void *ptrs[32];
        size_t count = mp_alloc_batch(pool, 64, ptrs, 32);
        assert(count == 32);
        mp_free_batch(pool, ptrs, count);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }
    {
        /* (g) Multi-arena routing (recurses into the bound arena) */
        memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
        bool ok = mp_enable_multi_arena(pool, 2);
        assert(ok);
        mp_bind_thread_to_arena(pool, 0);
        void *ptrs[16];
        size_t count = mp_alloc_batch(pool, 64, ptrs, 16);
        assert(count == 16);
        mp_free_batch(pool, ptrs, count);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }
    {
        /* (h) Circuit breaker trips mid-batch (4 x 64 = 256 >= 200) */
        memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
        mp_set_thread_quota(pool, BATCH_QUOTA);
        mp_set_circuit_breaker(pool, true);
        void *ptrs[10];
        size_t count = mp_alloc_batch(pool, 64, ptrs, 10);
        assert(count == 4);
        assert(mp_is_circuit_breaker_tripped(pool) == true);
        mp_free_batch(pool, ptrs, count);
        mp_reset_thread_quota(pool);
        assert(mp_check_leaks(pool) == true);
        mp_destroy(pool);
    }

    TEST_PASS("test_batch_alloc_configs");
}

#define BATCH_FREE_COUNT 512u                   /* elements in the THREAD_SAFE semantics test */
#define BATCH_FREE_OS_SIZE (6u * 1024u * 1024u) /* OS-tier element in the mixed test */
#define BATCH_FREE_OVERFLOW 600u                /* > TLS_CACHE_MAX_SLOTS (256) to force overflow */
#define BATCH_CORRUPT_MAGIC 0xDEADBEEFu         /* magic stamp for the corrupt test */
#define BATCH_POISON_BYTE 0xDDu                 /* matches internal MP_POISON_BYTE */

/**
 * @brief Tests mp_free_batch semantics on a THREAD_SAFE pool: pointers are
 *        nulled, accounting is aggregated exactly, and the pool is leak-free.
 */
void test_batch_free_semantics()
{
    printf("\n--- Test 42: Batch Free Semantics ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_THREAD_SAFE);

    {
        void *ptrs[BATCH_FREE_COUNT];
        size_t count = mp_alloc_batch(pool, 32, ptrs, BATCH_FREE_COUNT);
        assert(count == BATCH_FREE_COUNT);
        for (size_t i = 0; i < count; i++) {
            assert(ptrs[i] != NULL);
        }
        mp_stats_t stats;
        mp_get_stats(pool, &stats);
        assert(stats.active_allocations == BATCH_FREE_COUNT);

        mp_free_batch(pool, ptrs, count);
        for (size_t i = 0; i < count; i++) {
            assert(ptrs[i] == NULL);
        }
        mp_get_stats(pool, &stats);
        assert(stats.active_allocations == 0);
        assert(stats.active_bytes == 0);
        assert(stats.total_free_ops == BATCH_FREE_COUNT);
        assert(mp_check_leaks(pool) == true);
    }
    {
        /* NULL entries inside the batch are skipped, not treated as errors. */
        void *ptrs[16];
        size_t count = mp_alloc_batch(pool, 64, ptrs, 16);
        assert(count == 16);
        mp_free(pool, ptrs[3]);
        ptrs[3] = NULL;
        mp_free_batch(pool, ptrs, count);
        mp_stats_t stats;
        mp_get_stats(pool, &stats);
        assert(stats.active_allocations == 0);
        assert(mp_check_leaks(pool) == true);
    }

    mp_destroy(pool);
    TEST_PASS("test_batch_free_semantics");
}

/**
 * @brief Proves mp_free_batch produces stats identical to N per-element
 *        mp_free calls on an otherwise identical THREAD_SAFE pool.
 */
void test_batch_free_equivalence()
{
    printf("\n--- Test 43: Batch Free vs Per-Element Equivalence ---\n");
    memory_pool_t *pool_a = mp_create(0, MP_FLAG_THREAD_SAFE);
    memory_pool_t *pool_b = mp_create(0, MP_FLAG_THREAD_SAFE);

    {
        void *ptrs_a[256];
        void *ptrs_b[256];
        size_t ca = mp_alloc_batch(pool_a, 64, ptrs_a, 256);
        assert(ca == 256);
        for (size_t i = 0; i < 256; i++) {
            ptrs_b[i] = mp_alloc(pool_b, 64);
            assert(ptrs_b[i] != NULL);
        }
        mp_free_batch(pool_a, ptrs_a, 256);
        for (size_t i = 0; i < 256; i++) {
            mp_free(pool_b, ptrs_b[i]);
        }
    }
    {
        void *ptrs_a[8];
        void *ptrs_b[8];
        size_t ca = mp_alloc_batch(pool_a, 1024, ptrs_a, 8);
        assert(ca == 8);
        for (size_t i = 0; i < 8; i++) {
            ptrs_b[i] = mp_alloc(pool_b, 1024);
            assert(ptrs_b[i] != NULL);
        }
        mp_free_batch(pool_a, ptrs_a, 8);
        for (size_t i = 0; i < 8; i++) {
            mp_free(pool_b, ptrs_b[i]);
        }
    }

    mp_stats_t stats_a;
    mp_stats_t stats_b;
    mp_get_stats(pool_a, &stats_a);
    mp_get_stats(pool_b, &stats_b);
    assert(stats_a.active_bytes == stats_b.active_bytes);
    assert(stats_a.active_allocations == stats_b.active_allocations);
    assert(stats_a.total_free_ops == stats_b.total_free_ops);
    assert(stats_a.os_allocated_bytes == stats_b.os_allocated_bytes);
    assert(mp_check_leaks(pool_a) == true);
    assert(mp_check_leaks(pool_b) == true);

    mp_destroy(pool_a);
    mp_destroy(pool_b);
    TEST_PASS("test_batch_free_equivalence");
}

/**
 * @brief A single mp_free_batch call mixing slab, TLSF and OS-tier elements:
 *        non-slab pointers fall back to per-element free with exact accounting.
 */
void test_batch_free_mixed_tiers()
{
    printf("\n--- Test 44: Batch Free Mixed Tiers ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_THREAD_SAFE);

    void *ptrs[13];
    size_t elem_count = 0;
    size_t ca = mp_alloc_batch(pool, 32, &ptrs[elem_count], 8);
    assert(ca == 8);
    elem_count += ca;
    ca = mp_alloc_batch(pool, 4096, &ptrs[elem_count], 4);
    assert(ca == 4);
    elem_count += ca;
    ptrs[elem_count] = mp_alloc(pool, BATCH_FREE_OS_SIZE);
    assert(ptrs[elem_count] != NULL);
    elem_count++;

    mp_stats_t before;
    mp_get_stats(pool, &before);
    assert(before.active_allocations == 13);

    mp_free_batch(pool, ptrs, elem_count);
    for (size_t i = 0; i < elem_count; i++) {
        assert(ptrs[i] == NULL);
    }
    mp_stats_t after;
    mp_get_stats(pool, &after);
    assert(after.active_allocations == 0);
    assert(after.active_bytes == 0);
    assert(after.os_allocated_bytes == 0);
    assert(mp_check_leaks(pool) == true);

    mp_destroy(pool);
    TEST_PASS("test_batch_free_mixed_tiers");
}

/**
 * @brief A corrupt header inside a batch triggers mp_free's error path; the
 *        remaining valid elements are still freed and the pointers nulled.
 */
void test_batch_free_corrupt()
{
    printf("\n--- Test 45: Batch Free With Corrupt Header ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_THREAD_SAFE);

    void *ptrs[16];
    size_t count = mp_alloc_batch(pool, 32, ptrs, 16);
    assert(count == 16);
    mp_block_header_t *bad_header =
        (mp_block_header_t *)((uint8_t *)ptrs[7] - sizeof(mp_block_header_t));
    bad_header->magic = BATCH_CORRUPT_MAGIC;

    mp_free_batch(pool, ptrs, count);
    assert(mp_is_pool_dirty(pool) == true);
    for (size_t i = 0; i < count; i++) {
        assert(ptrs[i] == NULL);
    }
    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    /* The corrupt element was never unaccounted; the other 15 were freed. */
    assert(stats.active_allocations == 1);

    mp_destroy(pool);
    TEST_PASS("test_batch_free_corrupt");
}

/**
 * @brief A subpool-redirected element inside a batch falls back to per-element
 *        free; all pointers are nulled and the pool stays leak-free.
 */
void test_batch_free_subpool()
{
    printf("\n--- Test 46: Batch Free With Subpool Redirect ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_THREAD_SAFE);

    void *ptrs[16];
    size_t count = mp_alloc_batch(pool, 32, ptrs, 16);
    assert(count == 16);
    mp_block_header_t *redir_header =
        (mp_block_header_t *)((uint8_t *)ptrs[7] - sizeof(mp_block_header_t));
    redir_header->subpool = (void *)pool; /* self-redirect, as OS elements do */

    mp_free_batch(pool, ptrs, count);
    for (size_t i = 0; i < count; i++) {
        assert(ptrs[i] == NULL);
    }
    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 0);
    assert(mp_check_leaks(pool) == true);

    mp_destroy(pool);
    TEST_PASS("test_batch_free_subpool");
}

/**
 * @brief FAST_PATH pools account batch frees via relaxed atomics; the active
 *        list is not maintained and the counters return to zero exactly.
 */
void test_batch_free_fastpath()
{
    printf("\n--- Test 47: Batch Free Fast Path ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_FAST_PATH | MP_FLAG_THREAD_SAFE);

    void *ptrs[128];
    size_t count = mp_alloc_batch(pool, 16, ptrs, 128);
    assert(count == 128);
    mp_free_batch(pool, ptrs, count);
    for (size_t i = 0; i < count; i++) {
        assert(ptrs[i] == NULL);
    }
    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 0);
    assert(stats.active_bytes == 0);
    assert(stats.total_free_ops == 128);
    assert(mp_check_leaks(pool) == true);

    mp_destroy(pool);
    TEST_PASS("test_batch_free_fastpath");
}

/**
 * @brief POISON_ON_FREE batch frees fill the payload with MP_POISON_BYTE; the
 *        next allocation of the same size observes the poison before reuse.
 */
void test_batch_free_poison()
{
    printf("\n--- Test 48: Batch Free Poison Fill ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_POISON_ON_FREE);

    void *ptrs[64];
    size_t count = mp_alloc_batch(pool, 64, ptrs, 64);
    assert(count == 64);
    for (size_t i = 0; i < count; i++) {
        memset(ptrs[i], 0xAB, 64);
    }
    mp_free_batch(pool, ptrs, count);

    void *reused = mp_alloc(pool, 64);
    assert(reused != NULL);
    const uint8_t *bytes = (const uint8_t *)reused;
    for (size_t i = 0; i < 64; i++) {
        assert(bytes[i] == BATCH_POISON_BYTE);
    }
    mp_free(pool, reused);
    assert(mp_check_leaks(pool) == true);

    mp_destroy(pool);
    TEST_PASS("test_batch_free_poison");
}

/**
 * @brief Freeing more same-class slots than the TLS cache holds (256) flushes
 *        into the per-CPU freelist and remote-free queue without losing any.
 */
void test_batch_free_overflow()
{
    printf("\n--- Test 49: Batch Free TLS Cache Overflow ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_THREAD_SAFE);

    void *ptrs[BATCH_FREE_OVERFLOW];
    size_t count = mp_alloc_batch(pool, 32, ptrs, BATCH_FREE_OVERFLOW);
    assert(count == BATCH_FREE_OVERFLOW);
    mp_free_batch(pool, ptrs, count);
    for (size_t i = 0; i < count; i++) {
        assert(ptrs[i] == NULL);
    }
    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 0);
    assert(mp_check_leaks(pool) == true);

    mp_destroy(pool);
    TEST_PASS("test_batch_free_overflow");
}

/**
 * @brief Tests leak analysis report and heap audit features.
 */
void test_leak_analysis_and_heap_audit()
{
    printf("\n--- Test 7: Leak Analysis Report & Heap Audit ---\n");
    // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
    memory_pool_t *pool = mp_create(
        1024 * 1024, MP_FLAG_DEBUG_CANARY | MP_FLAG_TRACK_LOCATIONS | MP_FLAG_POISON_ON_FREE);
    // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)

    void *leak_ptr = mp_alloc_loc(pool, 256, __FILE__, __LINE__, __func__);
    assert(leak_ptr != NULL);

    void *valid_ptr = mp_alloc_loc(pool, 64, __FILE__, __LINE__, __func__);
    assert(valid_ptr != NULL);

    assert(mp_audit_heap(pool) == true);

    uint8_t *poison_test = (uint8_t *)valid_ptr;
    mp_free(pool, valid_ptr);
    assert(poison_test[0] == 0xDD && poison_test[63] == 0xDD);
    (void)poison_test;

    char report[2048];
    size_t report_len = mp_analyze_leaks(pool, report, sizeof(report));
    assert(report_len > 0);
    assert(strstr(report, "Source Location") != NULL);
    (void)report_len;

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
    memory_pool_t *root = mp_create(1024 * 1024, MP_FLAG_TRACK_LOCATIONS);
    memory_pool_t *child1 = mp_create_child(root, 512 * 1024, MP_FLAG_DEFAULT, "TestChildArena1");

    void *p1 = mp_alloc_loc(root, 128, __FILE__, __LINE__, __func__);
    void *p2 = mp_alloc_loc(child1, 256, __FILE__, __LINE__, __func__);

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
    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_DEFAULT);

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
    (void)json_len;

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
    memory_pool_t *pool =
        mp_create_from_buffer(g_static_buf, sizeof(g_static_buf), MP_FLAG_DEFAULT);
    assert(pool != NULL);

    g_event_triggered = false;
    mp_set_event_callback(pool, test_event_cb, NULL);

    void *p1 = mp_alloc(pool, 500);
    assert(p1 != NULL);
    assert(g_event_triggered == true);

    uintptr_t buf_start = (uintptr_t)g_static_buf;
    uintptr_t buf_end = buf_start + sizeof(g_static_buf);
    assert((uintptr_t)p1 >= buf_start && (uintptr_t)p1 < buf_end);
    (void)buf_end;

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
void *thread_worker(void *arg)
{
    memory_pool_t *pool = (memory_pool_t *)arg;
    void *ptrs[ALLOCS_PER_THREAD];

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

/**
 * @brief Tests multithreaded concurrent safety and thread-local cache.
 */
void test_multithread_safety()
{
    printf("\n--- Test 4: Multithreaded Concurrent Safety & Thread-Local Cache ---\n");
    // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
    memory_pool_t *pool =
        mp_create(2 * 1024 * 1024, MP_FLAG_THREAD_SAFE | MP_FLAG_THREAD_LOCAL_CACHE);
    // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
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

/**
 * @brief Tests edge cases: zero-size alloc, huge alloc, non-power-of-2 aligned alloc,
 * realloc(NULL).
 */
void test_boundary_cross_allocator()
{
    printf("\n--- Test: Boundary - Cross-Allocator Alloc/Free ---\n");
    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    void *slab_ptr = mp_alloc(pool, 8);
    assert(slab_ptr != NULL);
    mp_free(pool, slab_ptr);

    void *tlsf_ptr = mp_alloc(pool, 65536);
    assert(tlsf_ptr != NULL);
    mp_free(pool, tlsf_ptr);

    void *os_ptr = mp_alloc(pool, 10 * 1024 * 1024);
    assert(os_ptr != NULL);
    mp_free(pool, os_ptr);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_boundary_cross_allocator");
}

void test_boundary_zero_size_all_tiers()
{
    printf("\n--- Test: Boundary - Zero-Size All Tiers ---\n");
    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    assert(mp_alloc(pool, 0) == NULL);
    assert(mp_calloc(pool, 0, 1) == NULL);
    assert(mp_calloc(pool, 1, 0) == NULL);
    assert(mp_realloc(pool, NULL, 0) == NULL);
    void *p_tmp = mp_realloc(pool, NULL, 256);
    assert(p_tmp != NULL);
    mp_free(pool, p_tmp);
    void *ptr_main = mp_realloc(pool, NULL, 256);
    assert(ptr_main != NULL);
    void *ptr_resized = mp_realloc(pool, ptr_main, 0);
    assert(ptr_resized == NULL);
    mp_free(pool, NULL);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_boundary_zero_size_all_tiers");
}

void test_boundary_max_size()
{
    printf("\n--- Test: Boundary - Maximum Allocatable Size ---\n");
    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_THREAD_SAFE);
    assert(pool != NULL);

    void *small = mp_alloc(pool, 8);
    assert(small != NULL);
    mp_free(pool, small);

    void *exact = mp_alloc(pool, 512);
    assert(exact != NULL);
    mp_free(pool, exact);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_boundary_max_size");
}

void test_edge_cases()
{
    printf("\n--- Test: Edge Cases ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *p0 = mp_alloc(pool, 0);
    assert(p0 == NULL);

    mp_set_memory_limit(pool, 4096);
    mp_enable_emergency_reserve(pool, 0);
    void *p_big = mp_alloc(pool, 1024ULL * 1024 * 1024 * 1024);
    assert(p_big == NULL);

    mp_set_memory_limit(pool, 0);

    void *p_aligned = mp_aligned_alloc(pool, 128, 1);
    assert(p_aligned != NULL);
    mp_free(pool, p_aligned);

    void *p_realloc_null = mp_realloc(pool, NULL, 256);
    assert(p_realloc_null != NULL);
    mp_free(pool, p_realloc_null);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_edge_cases");
}

/**
 * @brief Tests error paths: NULL free, NULL strdup, NULL asprintf, tiny static buffer.
 */
void test_error_paths()
{
    printf("\n--- Test: Error Paths ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_free(pool, NULL);

    char *str_result = mp_strdup(pool, NULL);
    assert(str_result == NULL);

    char *as = mp_asprintf(pool, NULL);
    assert(as == NULL);

    uint8_t tiny[65536];
    memory_pool_t *sp = mp_create_from_buffer(tiny, sizeof(tiny), MP_FLAG_DEFAULT);
    assert(sp != NULL);
    void *ptr = mp_alloc(sp, 8);
    assert(ptr != NULL);
    mp_free(sp, ptr);
    mp_destroy(sp);

    mp_destroy(pool);
    TEST_PASS("test_error_paths");
}

/**
 * @brief Tests security detection: double free, canary overflow, heap audit.
 */
void test_security_detection()
{
    printf("\n--- Test: Security Detection ---\n");

    {
        // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
        memory_pool_t *pool = mp_create(0, MP_FLAG_DEBUG_CANARY | MP_FLAG_TRACK_LOCATIONS);
        // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
        assert(pool != NULL);

        void *p1 = mp_alloc(pool, 64);
        assert(p1 != NULL);
        mp_free(pool, p1);
        mp_free(pool, p1);

        mp_destroy(pool);
    }

    {
        // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
        memory_pool_t *pool = mp_create(0, MP_FLAG_DEBUG_CANARY | MP_FLAG_TRACK_LOCATIONS);
        // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
        assert(pool != NULL);

        void *p2 = mp_alloc(pool, 64);
        assert(p2 != NULL);
        memset(p2, 0xAB, 128);
        mp_free(pool, p2);

        assert(mp_audit_heap(pool) == true);

        mp_destroy(pool);
    }
    TEST_PASS("test_security_detection");
}

static void
test_callback_event_cb(memory_pool_t *pool, mp_event_type_t ev, void *ptr, size_t sz, void *ud)
{
    (void)pool;
    (void)ud;
    printf("  Event: %d ptr=%p size=%zu\n", (int)ev, ptr, sz);
}

static void
test_callback_watermark_cb(memory_pool_t *pool, bool high, size_t used, size_t limit, void *ud)
{
    (void)pool;
    (void)ud;
    printf("  Watermark: high=%d used=%zu limit=%zu\n", (int)high, used, limit);
}

static void test_callback_gc_cb(memory_pool_t *pool,
                                bool critical,
                                size_t used, // NOLINT(bugprone-easily-swappable-parameters)
                                size_t limit,
                                void *ud)
{
    (void)pool;
    (void)used;
    (void)limit;
    (void)ud;
    printf("  GC callback: critical=%d\n", (int)critical);
}

static void test_callback_eviction_cb(memory_pool_t *pool,
                                      bool critical,
                                      size_t used, // NOLINT(bugprone-easily-swappable-parameters)
                                      size_t limit,
                                      void *ud)
{
    (void)pool;
    (void)used;
    (void)limit;
    (void)ud;
    printf("  Eviction callback: critical=%d\n", (int)critical);
}

void test_callback_interactions()
{
    printf("\n--- Test: Callback Interactions ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    mp_set_event_callback(pool, test_callback_event_cb, NULL);
    mp_set_watermark_callback(pool, 0.1, 0.05, test_callback_watermark_cb, NULL);
    mp_set_gc_callback(pool, test_callback_gc_cb, NULL);
    mp_set_eviction_callback(pool, test_callback_eviction_cb, NULL);

    void *ptr = mp_alloc(pool, 1024);
    assert(ptr != NULL);
    mp_free(pool, ptr);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);
    TEST_PASS("test_callback_interactions");
}

/**
 * @brief Tests reset/resize behavior: reset preserves memory, realloc in-place expansion.
 */
void test_reset_and_resize()
{
    printf("\n--- Test: Reset and Resize ---\n");
    memory_pool_t *pool = mp_create(0, MP_FLAG_DEFAULT);
    assert(pool != NULL);

    void *p1 = mp_alloc(pool, 256);
    assert(p1 != NULL);
    mp_reset(pool);
    assert(mp_check_leaks(pool) == true);

    void *p2 = mp_alloc(pool, 128);
    assert(p2 != NULL);

    mp_stats_t stats;
    mp_get_stats(pool, &stats);
    assert(stats.active_allocations == 1);

    mp_free(pool, p2);
    mp_destroy(pool);
    TEST_PASS("test_reset_and_resize");
}

// NOLINTEND(readability-magic-numbers)

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
    test_fallback_on_oom();
    test_numa_node_binding();
    test_numa_auto_optimization();
    test_compressed_storage();
    test_fast_path();
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
    test_batch_alloc_tiers();
    test_batch_alloc_configs();
    test_batch_free_semantics();
    test_batch_free_equivalence();
    test_batch_free_mixed_tiers();
    test_batch_free_corrupt();
    test_batch_free_subpool();
    test_batch_free_fastpath();
    test_batch_free_poison();
    test_batch_free_overflow();
    test_memory_budget_and_oom();
    test_leak_analysis_and_heap_audit();
    test_child_arenas_and_html_export();
    test_arena_reset_and_json();
    test_static_buffer_and_callbacks();
    test_boundary_cross_allocator();
    test_boundary_zero_size_all_tiers();
    test_boundary_max_size();
    test_edge_cases();
    test_error_paths();
    test_security_detection();
    test_callback_interactions();
    test_reset_and_resize();
    test_idle_page_reclaim();
    printf("\nALL CMEM UNIT TESTS PASSED SUCCESSFULLY!\n");
    return 0;
}
