/**
 * @file example_leak_analysis.c
 * @brief Memory Leak Analysis, Source Location Tracking, and Heap Audit Example.
 */

#include "memory_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void do_leaky_work(memory_pool_t* pool) {
    // Allocate blocks with location tracking
    void* leaked_buf = mp_alloc_loc(pool, 128, __FILE__, __LINE__, __func__);
    void* normal_buf = mp_alloc_loc(pool, 64,  __FILE__, __LINE__, __func__);

    strcpy((char*)leaked_buf, "This buffer will be intentionally leaked for report demonstration!");
    strcpy((char*)normal_buf, "This buffer will be properly freed.");

    mp_free(pool, normal_buf);
    // Intentionally NOT freeing leaked_buf to demonstrate leak analysis!
}

int main() {
    printf("=== Example 3: Memory Leak Analysis & Heap Audit Demo ===\n\n");

    // Create pool with Debug Canary, Location Tracking, and UAF Poisoning
    memory_pool_t* pool = mp_create(1024 * 1024,
        MP_FLAG_THREAD_SAFE | MP_FLAG_DEBUG_CANARY | MP_FLAG_TRACK_LOCATIONS | MP_FLAG_POISON_ON_FREE);

    if (!pool) return 1;

    printf("1. Performing work with intentional leak...\n");
    do_leaky_work(pool);

    printf("\n2. Running Heap Integrity Audit (mp_audit_heap):\n");
    bool healthy = mp_audit_heap(pool);
    printf("   Heap Audit Result: %s\n\n", healthy ? "HEALTHY" : "CORRUPTED");

    printf("3. Generating Detailed Leak Analysis Report:\n");
    char report[4096];
    mp_analyze_leaks(pool, report, sizeof(report));
    printf("%s\n", report);

    printf("4. Exporting Leak Report to File ('leak_report.txt')...\n");
    mp_export_leak_report(pool, "leak_report.txt");

    // Clean up
    mp_reset(pool); // Clear leaks
    mp_destroy(pool);

    printf("\nLeak Analysis Example Completed Successfully!\n");
    return 0;
}
