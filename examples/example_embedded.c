/**
 * @file example_embedded.c
 * @brief Static Buffer Arena Example of cmem (Zero OS Malloc Dependency).
 */

#include "cmem.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

/**
 * @brief Pre-allocated static RAM buffer used as the backing store for the arena.
 * Demonstrates embedded/static buffer usage without OS malloc dependency.
 */
static uint8_t g_embedded_ram_buffer[512 * 1024];

/**
 * @brief Demonstrates creating a cmem pool from a static buffer with zero OS malloc dependency.
 * @return 0 on success, 1 on failure.
 */
int main()
{
    printf("=== Example 2: Static Buffer Arena (Zero OS Malloc) ===\n\n");

    memory_pool_t* pool = mp_create_from_buffer(g_embedded_ram_buffer,
                                                sizeof(g_embedded_ram_buffer), MP_FLAG_DEFAULT);
    if (!pool)
    {
        fprintf(stderr, "Failed to initialize cmem static buffer arena!\n");
        return 1;
    }

    printf("Static Arena initialized at %p (Size: 512 KB)\n", (void*) g_embedded_ram_buffer);

    void* block1 = mp_alloc(pool, 1024);
    void* block2 = mp_alloc(pool, 4096);
    void* block3 = mp_alloc(pool, 64.5 * 1024);

    uintptr_t base = (uintptr_t) g_embedded_ram_buffer;
    uintptr_t limit = base + sizeof(g_embedded_ram_buffer);

    assert((uintptr_t) block1 >= base && (uintptr_t) block1 < limit);
    assert((uintptr_t) block2 >= base && (uintptr_t) block2 < limit);
    assert((uintptr_t) block3 >= base && (uintptr_t) block3 < limit);

    printf("All allocations confirmed inside static arena memory boundary!\n");

    mp_dump_info(pool);

    mp_free(pool, block1);
    mp_free(pool, block2);
    mp_free(pool, block3);

    mp_check_leaks(pool);
    mp_destroy(pool);

    printf("\nStatic Buffer Arena Example Completed Successfully!\n");
    return 0;
}
