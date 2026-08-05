/**
 * @file fuzz_alloc.c
 * @brief Enhanced libFuzzer harness for cmem allocation/deallocation paths.
 *
 * This harness exercises a wider range of cmem APIs with varying sizes and
 * patterns to surface heap corruption, use-after-free, double-free,
 * out-of-bounds, and other memory safety issues under libFuzzer.
 */

#include "cmem.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>

static memory_pool_t *g_pool = NULL;

__attribute__((constructor)) static void init_pool(void)
{
    g_pool = mp_create(8 * 1024 * 1024, MP_FLAG_THREAD_SAFE | MP_FLAG_DEBUG_CANARY);
}

__attribute__((destructor)) static void destroy_pool(void)
{
    if (g_pool) {
        mp_destroy(g_pool);
        g_pool = NULL;
    }
}

static void consume_fuzz_input(const uint8_t *data, size_t size)
{
    if (size < 2) {
        return;
    }

    uint8_t op = data[0] & 0x0F;
    uint8_t arg = data[1];
    const uint8_t *payload = size > 2 ? data + 2 : data;
    size_t payload_len = size > 2 ? size - 2 : 0;

    static void *slots[256];
    static size_t slot_count = 0;
#ifndef CMEM_DISABLE_DIAGNOSTICS
    static char event_log_buf[4096];
#endif

    switch (op) {
    case 0x00: {
        size_t alloc_size = (arg % 200) + 1;
        void *p = mp_alloc(g_pool, alloc_size);
        if (p && slot_count < 256) {
            slots[slot_count++] = p;
            if (payload_len > 0 && alloc_size > 0) {
                size_t write_len = alloc_size < payload_len ? alloc_size : payload_len;
                memcpy(p, payload, write_len);
            }
        }
        break;
    }

    case 0x01: {
        size_t alloc_size = (arg % 200) + 1;
        void *p = mp_calloc(g_pool, alloc_size, 1);
        if (p && slot_count < 256) {
            slots[slot_count++] = p;
        }
        break;
    }

    case 0x02: {
        size_t alloc_size = (arg % 200) + 1;
        size_t alignment = 1u << (arg % 5);
        if (alignment < sizeof(void *)) {
            alignment = sizeof(void *);
        }
        void *p = mp_aligned_alloc(g_pool, alloc_size, alignment);
        if (p && slot_count < 256) {
            slots[slot_count++] = p;
            if (payload_len > 0 && alloc_size > 0) {
                size_t write_len = alloc_size < payload_len ? alloc_size : payload_len;
                memcpy(p, payload, write_len);
            }
        }
        break;
    }

    case 0x03: {
        if (slot_count == 0) {
            break;
        }
        if (payload_len > 0) {
            size_t dup_len = payload_len < 256 ? payload_len : 255;
            char tmp[256];
            memcpy(tmp, payload, dup_len);
            tmp[dup_len] = '\0';
            char *s = mp_strdup(g_pool, tmp);
            if (s && slot_count < 256) {
                slots[slot_count++] = s;
            }
        }
        break;
    }

    case 0x07: {
        if (slot_count == 0) {
            break;
        }
        size_t idx = arg % slot_count;
        void *p = slots[idx];
        if (payload_len > 0) {
            size_t dup_len = payload_len < 256 ? payload_len : 255;
            void *m = mp_memdup(g_pool, p, dup_len);
            if (m && slot_count < 256) {
                slots[slot_count++] = m;
            }
        }
        break;
    }

    case 0x08: {
        if (slot_count == 0) {
            break;
        }
        char *fmt = (char *)payload;
        if (payload_len == 0) {
            fmt = "%zu";
        }
        char *as = mp_asprintf(g_pool, fmt, arg);
        if (as && slot_count < 256) {
            slots[slot_count++] = as;
        }
        break;
    }

    case 0x09: {
        mp_set_memory_limit(g_pool, (arg + 1) * 1024);
        mp_enable_emergency_reserve(g_pool, (arg % 4 + 1) * 4096);
        mp_set_event_callback(g_pool, NULL, NULL);
        mp_set_watermark_callback(g_pool, 0.1, 0.05, NULL, NULL);
        mp_set_gc_callback(g_pool, NULL, NULL);
        mp_set_eviction_callback(g_pool, NULL, NULL);
        break;
    }

    case 0x0A: {
#ifndef CMEM_DISABLE_DIAGNOSTICS
        mp_stats_t stats;
        mp_get_stats(g_pool, &stats);
        (void)stats;
        mp_check_leaks(g_pool);
        mp_audit_heap(g_pool);
        mp_dump_info(g_pool);
        mp_dump_histogram(g_pool);
        mp_dump_tree_info(g_pool);
        mp_dump_json_stats(g_pool, event_log_buf, sizeof(event_log_buf));
        mp_export_prometheus_metrics(g_pool, event_log_buf, sizeof(event_log_buf));
        mp_analyze_leaks(g_pool, event_log_buf, sizeof(event_log_buf));
#endif
        break;
    }

    case 0x0B: {
        mp_compact(g_pool);
        mp_purge_lazy(g_pool);
        mp_reset(g_pool);
        if (payload_len > 0) {
            size_t zero_len = payload_len < 64 ? payload_len : 64;
            char stack_buf[64];
            memcpy(stack_buf, payload, zero_len);
            mp_secure_zero(g_pool, stack_buf, zero_len);
        }
        mp_madvise(g_pool, g_pool, 4096, 0);
        break;
    }

    case 0x0C: {
        if (slot_count < 2) {
            break;
        }
        size_t idx1 = arg % slot_count;
        size_t idx2 = (arg + 1) % slot_count;
        void *a = slots[idx1];
        void *b = slots[idx2];
        size_t usable_a = mp_usable_size(g_pool, a);
        size_t alloc_a = mp_alloc_size(g_pool, a);
        (void)usable_a;
        (void)alloc_a;
        mp_ptr_valid(g_pool, a);
        mp_ptr_valid(g_pool, b);
        break;
    }

    case 0x0D: {
        if (slot_count >= 256) {
            break;
        }
        size_t alloc_size = (arg % 200) + 1;
        void *p = mp_alloc(g_pool, alloc_size);
        if (p) {
            memset(p, 0xCD, alloc_size);
            mp_free(g_pool, p);
        }
        break;
    }

    case 0x0E: {
        size_t batch = (arg % 10) + 1;
        void *ptrs[10];
        size_t got = mp_alloc_batch(g_pool, 16, ptrs, batch);
        if (got > 0 && slot_count + got <= 256) {
            memcpy(slots + slot_count, ptrs, got * sizeof(void *));
            slot_count += got;
        }
        mp_free_batch(g_pool, ptrs, got);
        break;
    }

    case 0x0F: {
        if (slot_count > 0) {
            mp_free_batch(g_pool, slots, slot_count);
            slot_count = 0;
        }
        break;
    }

    default:
        break;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!g_pool || size == 0) {
        return 0;
    }

    consume_fuzz_input(data, size);
    return 0;
}

#ifdef STANDALONE_FUZZ
#include <errno.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <corpus_file>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "Cannot stat %s: %s\n", path, strerror(errno));
        fclose(f);
        return 1;
    }

    size_t size = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(size > 0 ? size : 1);
    if (!buf) {
        fclose(f);
        return 1;
    }

    if (size > 0) {
        fread(buf, 1, size, f);
    }
    fclose(f);

    LLVMFuzzerTestOneInput(buf, size);
    free(buf);
    return 0;
}
#endif
