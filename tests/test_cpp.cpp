/**
 * @file test_cpp.cpp
 * @brief Unit tests verifying cmem C++ RAII wrapper and STL-compatible allocator.
 */

#include "../include/cmem.hpp"
#include <iostream>
#include <vector>
#include <unordered_map>
#include <cassert>

void test_cpp_stl_allocator() {
    std::cout << "\n================ RUNNING C++ STL ALLOCATOR TESTS ================\n" << std::endl;

    cmem::MemoryPool pool(1024 * 1024, MP_FLAG_THREAD_SAFE);

    {
        // Scope for STL Containers to ensure destructors release all nodes/buckets
        cmem::allocator<int> vec_alloc(pool);
        std::vector<int, cmem::allocator<int>> vec(vec_alloc);

        for (int i = 0; i < 100; i++) {
            vec.push_back(i * 10);
        }
        assert(vec.size() == 100);
        assert(vec[50] == 500);

        using MapAlloc = cmem::allocator<std::pair<const int, std::string>>;
        MapAlloc map_alloc(pool);
        std::unordered_map<int, std::string, std::hash<int>, std::equal_to<int>, MapAlloc> map(10, std::hash<int>(), std::equal_to<int>(), map_alloc);

        map[1] = "cmem";
        map[2] = "High-Performance";
        map[3] = "C++ Allocator";

        assert(map[1] == "cmem");
    }

    pool.dump_info();

    // Check memory clean after container destruction
    assert(pool.check_leaks() == true);
    std::cout << "[PASS] C++ STL Container Allocator Test Passed Cleanly!" << std::endl;
}

int main() {
    test_cpp_stl_allocator();
    return 0;
}
