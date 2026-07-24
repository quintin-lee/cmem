/**
 * @file example_embedded.c
 * @brief Embedded/Bare-metal Static Buffer Example (Zero OS malloc dependency).
 */

#include "memory_pool.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#define STATIC_BUFFER_SIZE (512 * 1024) // 512 KB static arena buffer
static uint8_t g_static_arena[STATIC_BUFFER_SIZE];

int main() {
    printf("=== Example 2: Static Buffer Arena (Zero OS Malloc) ===\n\n");

    // Initialize memory pool directly inside static byte array
    memory_pool_t* pool = mp_create_from_buffer(g_static_arena, sizeof(g_static_arena), MP_FLAG_ZERO_ON_ALLOC);
    assert(pool != NULL);

    printf("Static Arena initialized at %p (Size: %d KB)\n", (void*)g_static_arena, STATIC_BUFFER_SIZE / 1024);

    // Allocate memory inside static buffer
    void* p1 = mp_alloc(pool, 128);
    void* p2 = mp_alloc(pool, 4096);
    void* p3 = mp_alloc(pool, 64 * 1024);

    assert(p1 && p2 && p3);

    // Verify allocated pointers reside within static arena memory boundary
    uintptr_t arena_start = (uintptr_t)g_static_arena;
    uintptr_t arena_end   = arena_start + STATIC_BUFFER_SIZE;

    assert((uintptr_t)p1 >= arena_start && (uintptr_t)p1 < arena_end);
    assert((uintptr_t)p2 >= arena_start && (uintptr_t)p2 < arena_end);
    assert((uintptr_t)p3 >= arena_start && (uintptr_t)p3 < arena_end);

    printf("All allocations confirmed inside static arena memory boundary!\n");

    mp_dump_info(pool);

    mp_free(pool, p1);
    mp_free(pool, p2);
    mp_free(pool, p3);

    assert(mp_check_leaks(pool) == true);
    mp_destroy(pool);

    printf("\nStatic Buffer Arena Example Completed Successfully!\n");
    return 0;
}
