/**
 * @file example_basic.c
 * @brief Basic C Example demonstrating memory pool usage and event profiling.
 */

#include "memory_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void my_event_profiler(memory_pool_t* pool, mp_event_type_t event, void* ptr, size_t size, void* user_data) {
    (void)pool; (void)user_data;
    const char* ev_name = (event == MP_EVENT_ALLOC) ? "ALLOC" :
                         ((event == MP_EVENT_FREE) ? "FREE" : "OTHER");
    printf(" [PROFILER LOG] Event: %-5s | Address: %p | Size: %zu bytes\n", ev_name, ptr, size);
}

int main() {
    printf("=== Example 1: Basic Memory Pool Usage with Event Profiler ===\n\n");

    // Create pool with Debug Canary and Thread Safety
    memory_pool_t* pool = mp_create(1024 * 1024, MP_FLAG_THREAD_SAFE | MP_FLAG_DEBUG_CANARY);
    if (!pool) {
        fprintf(stderr, "Failed to create memory pool!\n");
        return 1;
    }

    // Register profiler callback
    mp_set_event_callback(pool, my_event_profiler, NULL);

    printf("1. Allocating memory blocks...\n");
    int* numbers = (int*)mp_alloc(pool, sizeof(int) * 10);
    char* greeting = (char*)mp_alloc(pool, 64);

    for (int i = 0; i < 10; i++) numbers[i] = (i + 1) * 100;
    strcpy(greeting, "Hello Antigravity Memory Pool!");

    printf("   greeting: %s\n", greeting);

    printf("\n2. Dumping Diagnostics Snapshot:\n");
    mp_dump_info(pool);

    printf("3. Freeing allocated blocks...\n");
    mp_free(pool, numbers);
    mp_free(pool, greeting);

    printf("\n4. Leak Checking:\n");
    mp_check_leaks(pool);

    mp_destroy(pool);
    printf("\nBasic Example Completed Successfully!\n");
    return 0;
}
