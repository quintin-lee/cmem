/**
 * @file test_cpp.cpp
 * @brief C++ STL Container Integration Test for mpool::allocator.
 */

#include "../include/memory_pool.hpp"
#include <iostream>
#include <vector>
#include <unordered_map>
#include <cassert>

int main() {
    std::cout << "\n================ RUNNING C++ STL ALLOCATOR TESTS ================\n";

    mpool::MemoryPool pool(4 * 1024 * 1024, MP_FLAG_THREAD_SAFE);

    {
        mpool::allocator<int> int_alloc(pool);
        std::vector<int, mpool::allocator<int>> vec(int_alloc);
        for (int i = 0; i < 1000; i++) {
            vec.push_back(i * 10);
        }
        assert(vec.size() == 1000);
        assert(vec[500] == 5000);
    }

    {
        using PairAlloc = mpool::allocator<std::pair<const int, int>>;
        PairAlloc pair_alloc(pool);
        using StringMap = std::unordered_map<int, int, std::hash<int>, std::equal_to<int>, PairAlloc>;
        StringMap map(16, std::hash<int>(), std::equal_to<int>(), pair_alloc);

        for (int i = 0; i < 500; i++) {
            map[i] = i * 2;
        }
        assert(map[250] == 500);
    }

    pool.dump_info();
    assert(pool.check_leaks() == true);

    std::cout << "[PASS] C++ STL Container Allocator Test Passed Cleanly!\n";
    return 0;
}
