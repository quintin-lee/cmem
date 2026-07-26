/**
 * @file example_arena_tree.c
 * @brief Hierarchical Parent-Child Arena Trees and HTML Dashboard Exporter Example for cmem.
 */

#include "cmem.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/**
 * @brief Demonstrates hierarchical parent-child arena trees and HTML profiler dashboard export.
 * @return 0 on success.
 */
int main() {
    printf("=== Example 4: Hierarchical Child Arenas & HTML Profiler Dashboard ===\n\n");

    memory_pool_t* root_pool = mp_create(2 * 1024 * 1024, MP_FLAG_THREAD_SAFE | MP_FLAG_TRACK_LOCATIONS);
    assert(root_pool != NULL);

    memory_pool_t* scene_arena = mp_create_child(root_pool, 1024 * 1024, MP_FLAG_DEFAULT, "SceneNodeArena");
    memory_pool_t* ast_arena   = mp_create_child(root_pool, 512 * 1024,  MP_FLAG_DEFAULT, "ASTCompilerArena");
    memory_pool_t* net_arena   = mp_create_child(root_pool, 512 * 1024,  MP_FLAG_DEFAULT, "NetworkPacketArena");

    printf("1. Allocating in Root and Child Arenas...\n");
    void* r1 = mp_alloc_loc(root_pool, 4096, __FILE__, __LINE__, __func__);
    void* s1 = mp_alloc_loc(scene_arena, 256, __FILE__, __LINE__, __func__);
    void* s2 = mp_alloc_loc(scene_arena, 512, __FILE__, __LINE__, __func__);
    void* a1 = mp_alloc_loc(ast_arena, 64,    __FILE__, __LINE__, __func__);
    void* n1 = mp_alloc_loc(net_arena, 1024,  __FILE__, __LINE__, __func__);

    assert(r1 && s1 && s2 && a1 && n1);

    printf("\n2. Dumping Hierarchical Arena Tree Structure (mp_dump_tree_info):\n");
    mp_dump_tree_info(root_pool);

    printf("3. Exporting Interactive Visual HTML Dashboard Report ('memory_profile.html')...\n");
    mp_export_html_report(root_pool, "memory_profile.html");

    mp_destroy(root_pool);

    printf("\nHierarchical Arena Example Completed Successfully!\n");
    return 0;
}
