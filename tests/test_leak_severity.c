/**
 * @file test_leak_severity.c
 * @brief Tests for leak severity classification and pattern analysis.
 */

#include "cmem.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_leak_severity_critical(void)
{
    printf("--- Test: Critical Leak Severity ---\n");

    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY);
    assert(pool != NULL);

    // Allocate 2MB (exceeds critical threshold)
    void *ptr = mp_alloc(pool, 2 * 1024 * 1024);
    assert(ptr != NULL);

    mp_allocation_info_t info;
    assert(mp_get_allocation_info(pool, ptr, &info));

    mp_leak_severity_t severity = mp_get_leak_severity(&info);
    assert(severity == MP_LEAK_SEVERITY_CRITICAL);

    mp_leak_pattern_t pattern = mp_analyze_leak_pattern(&info);
    assert(pattern.confidence > 0);
    assert(pattern.pattern_name != NULL);

    mp_free(pool, ptr);
    mp_destroy(pool);
    printf("[PASS] test_leak_severity_critical\n\n");
}

static void test_leak_severity_warning(void)
{
    printf("--- Test: Warning Leak Severity ---\n");

    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY);
    assert(pool != NULL);

    // Allocate 100KB (warning range)
    void *ptr = mp_alloc(pool, 100 * 1024);
    assert(ptr != NULL);

    mp_allocation_info_t info;
    assert(mp_get_allocation_info(pool, ptr, &info));

    mp_leak_severity_t severity = mp_get_leak_severity(&info);
    assert(severity == MP_LEAK_SEVERITY_WARNING);

    mp_free(pool, ptr);
    mp_destroy(pool);
    printf("[PASS] test_leak_severity_warning\n\n");
}

static void test_leak_severity_info(void)
{
    printf("--- Test: Info Leak Severity ---\n");

    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY);
    assert(pool != NULL);

    // Allocate 1KB (info range)
    void *ptr = mp_alloc(pool, 1024);
    assert(ptr != NULL);

    mp_allocation_info_t info;
    assert(mp_get_allocation_info(pool, ptr, &info));

    mp_leak_severity_t severity = mp_get_leak_severity(&info);
    assert(severity == MP_LEAK_SEVERITY_INFO);

    mp_free(pool, ptr);
    mp_destroy(pool);
    printf("[PASS] test_leak_severity_info\n\n");
}

static void test_leak_pattern_large_buffer(void)
{
    printf("--- Test: Large Buffer Leak Pattern ---\n");

    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY);
    assert(pool != NULL);

    // Allocate 128KB (large buffer pattern)
    void *ptr = mp_alloc(pool, 128 * 1024);
    assert(ptr != NULL);

    mp_allocation_info_t info;
    assert(mp_get_allocation_info(pool, ptr, &info));

    mp_leak_pattern_t pattern = mp_analyze_leak_pattern(&info);
    assert(pattern.confidence > 0);
    assert(pattern.suggestion != NULL);
    printf("  Pattern: %s (confidence: %d%%)\n", pattern.pattern_name, pattern.confidence);
    printf("  Suggestion: %s\n", pattern.suggestion);

    mp_free(pool, ptr);
    mp_destroy(pool);
    printf("[PASS] test_leak_pattern_large_buffer\n\n");
}

static void test_leak_pattern_small_repeated(void)
{
    printf("--- Test: Small Repeated Leak Pattern ---\n");

    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_DEBUG_CANARY);
    assert(pool != NULL);

    // Allocate small object (repeated pattern)
    void *ptr = mp_alloc(pool, 64);
    assert(ptr != NULL);

    mp_allocation_info_t info;
    assert(mp_get_allocation_info(pool, ptr, &info));

    mp_leak_pattern_t pattern = mp_analyze_leak_pattern(&info);
    assert(pattern.confidence > 0);
    assert(pattern.suggestion != NULL);
    printf("  Pattern: %s (confidence: %d%%)\n", pattern.pattern_name, pattern.confidence);
    printf("  Suggestion: %s\n", pattern.suggestion);

    mp_free(pool, ptr);
    mp_destroy(pool);
    printf("[PASS] test_leak_pattern_small_repeated\n\n");
}

static void test_leak_pattern_null_info(void)
{
    printf("--- Test: Null Info Pattern ---\n");

    mp_leak_pattern_t pattern = mp_analyze_leak_pattern(NULL);
    assert(pattern.pattern_name != NULL);
    assert(pattern.suggestion != NULL);
    printf("  Pattern: %s\n", pattern.pattern_name);
    printf("  Suggestion: %s\n", pattern.suggestion);
    printf("[PASS] test_leak_pattern_null_info\n\n");
}

int main(void)
{
    test_leak_severity_critical();
    test_leak_severity_warning();
    test_leak_severity_info();
    test_leak_pattern_large_buffer();
    test_leak_pattern_small_repeated();
    test_leak_pattern_null_info();
    return 0;
}
