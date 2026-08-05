/**
 * @file example_basic.c
 * @brief Basic Usage Example of cmem Memory Manager with Event Profiler.
 */

#include "cmem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Event logger callback for profiling allocation and free events.
 * @param pool Memory pool handle.
 * @param event Type of memory event (alloc, free, or other).
 * @param ptr Pointer involved in the event.
 * @param size Size of the allocation in bytes.
 * @param user_data Optional user data passed to the callback.
 */
void my_event_logger(
    memory_pool_t *pool, mp_event_type_t event, void *ptr, size_t size, void *user_data)
{
    (void)pool;
    (void)user_data;
    const char *ev_name =
        (event == MP_EVENT_ALLOC) ? "ALLOC" : ((event == MP_EVENT_FREE) ? "FREE" : "OTHER");
    printf(" [PROFILER LOG] Event: %-5s | Address: %p | Size: %zu bytes\n", ev_name, ptr, size);
}

/**
 * @brief Demonstrates basic cmem usage with event profiling enabled.
 * @return 0 on success, 1 on failure.
 */
int main()
{
    printf("=== Example 1: Basic cmem Usage with Event Profiler ===\n\n");

    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    memory_pool_t *pool = mp_create(1024 * 1024, MP_FLAG_THREAD_SAFE | MP_FLAG_DEBUG_CANARY);
    if (!pool) {
        (void)fprintf(stderr, "Failed to create cmem pool!\n");
        return 1;
    }

    mp_set_event_callback(pool, my_event_logger, NULL);

    printf("1. Allocating memory blocks...\n");

    char *greeting = (char *)mp_alloc(pool, 40); // NOLINT(readability-magic-numbers)
    int *numbers = (int *)mp_alloc(pool, 64);

    strcpy(greeting, "Hello cmem Memory Manager!");
    printf("   greeting: %s\n\n", greeting);

#ifndef CMEM_DISABLE_DIAGNOSTICS
    printf("2. Dumping Diagnostics Snapshot:\n");
    mp_dump_info(pool);
#endif

    printf("3. Freeing allocated blocks...\n");
    mp_free(pool, greeting);
    mp_free(pool, numbers);

#ifndef CMEM_DISABLE_DIAGNOSTICS
    printf("\n4. Leak Checking:\n");
    mp_check_leaks(pool);
#endif

    mp_destroy(pool);
    printf("\nBasic Example Completed Successfully!\n");
    return 0;
}
