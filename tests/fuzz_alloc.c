/**
 * @file fuzz_alloc.c
 * @brief libFuzzer harness for cmem allocation/deallocation paths.
 *
 * This harness exercises mp_alloc/mp_free with varying sizes and
 * free/alloc patterns to surface heap corruption, use-after-free,
 * double-free, and out-of-bounds issues under libFuzzer.
 */

#include "cmem.h"
#include <stdint.h>
#include <string.h>

static memory_pool_t* g_pool = NULL;

__attribute__((constructor)) static void init_pool(void)
{
    g_pool = mp_create(4 * 1024 * 1024, MP_FLAG_THREAD_SAFE);
}

__attribute__((destructor)) static void destroy_pool(void)
{
    if (g_pool)
    {
        mp_destroy(g_pool);
        g_pool = NULL;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!g_pool || size == 0)
    {
        return 0;
    }

    if (size < sizeof(uint32_t))
    {
        return 0;
    }

    uint32_t op = ((const uint32_t*) data)[0];
    size_t alloc_size = (size_t) (data[4] % 253) + 1;
    size_t free_idx = (size_t) (data[5] % 253);

    static void* slots[253];
    static int slot_count = 0;

    if ((op & 0x3) == 0)
    {
        void* p = mp_alloc(g_pool, alloc_size);
        if (p && slot_count < 253)
        {
            slots[slot_count++] = p;
        }
    }
    else if ((op & 0x3) == 1 && slot_count > 0)
    {
        void* p = slots[free_idx % slot_count];
        mp_free(g_pool, p);
        slots[free_idx % slot_count] = slots[--slot_count];
    }
    else if ((op & 0x3) == 2 && size > 6)
    {
        uint16_t pattern = (uint16_t) ((data[6] << 8) | data[7]);
        size_t write_len = alloc_size < size - 8 ? alloc_size : size - 8;
        if (write_len > 0)
        {
            void* p = mp_alloc(g_pool, alloc_size);
            if (p)
            {
                memset(p, (int) pattern, write_len);
                mp_free(g_pool, p);
            }
        }
    }
    else if ((op & 0x3) == 3 && size > 8)
    {
        size_t realloc_size = (size_t) (data[8] % 253) + 1;
        if (slot_count > 0)
        {
            void* old = slots[free_idx % slot_count];
            void* p = mp_realloc(g_pool, old, realloc_size);
            if (p)
            {
                slots[free_idx % slot_count] = p;
            }
            else if (old)
            {
                mp_free(g_pool, old);
                slots[free_idx % slot_count] = slots[--slot_count];
            }
        }
    }

    return 0;
}
