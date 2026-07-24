/**
 * @file memory_pool.hpp
 * @brief C++11 Header-only RAII Wrapper and STL-compliant Allocator for C Memory Pool.
 */

#ifndef MEMORY_POOL_HPP
#define MEMORY_POOL_HPP

#include "memory_pool.h"
#include <memory>
#include <utility>
#include <stdexcept>
#include <string>

namespace mpool {

/**
 * @brief C++ RAII Wrapper class for memory_pool_t
 */
class MemoryPool {
public:
    explicit MemoryPool(size_t initial_capacity = 0, mp_flags_t flags = MP_FLAG_DEFAULT)
        : pool_(mp_create(initial_capacity, flags)) {
        if (!pool_) {
            throw std::runtime_error("Failed to create memory pool instance.");
        }
    }

    ~MemoryPool() {
        if (pool_) {
            mp_destroy(pool_);
        }
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    MemoryPool(MemoryPool&& other) noexcept : pool_(other.pool_) {
        other.pool_ = nullptr;
    }

    MemoryPool& operator=(MemoryPool&& other) noexcept {
        if (this != &other) {
            if (pool_) mp_destroy(pool_);
            pool_ = other.pool_;
            other.pool_ = nullptr;
        }
        return *this;
    }

    void* alloc(size_t size) {
        return mp_alloc(pool_, size);
    }

    void* alloc_loc(size_t size, const char* file, int line, const char* func) {
        return mp_alloc_loc(pool_, size, file, line, func);
    }

    void* calloc(size_t num, size_t size) {
        return mp_calloc(pool_, num, size);
    }

    void* realloc(void* ptr, size_t new_size) {
        return mp_realloc(pool_, ptr, new_size);
    }

    void* aligned_alloc(size_t alignment, size_t size) {
        return mp_aligned_alloc(pool_, alignment, size);
    }

    void free(void* ptr) {
        mp_free(pool_, ptr);
    }

    void reset() {
        mp_reset(pool_);
    }

    bool audit_heap() const {
        return mp_audit_heap(pool_);
    }

    std::string analyze_leaks() const {
        char buffer[16384];
        size_t len = mp_analyze_leaks(pool_, buffer, sizeof(buffer));
        return std::string(buffer, len);
    }

    bool export_leak_report(const std::string& filepath) const {
        return mp_export_leak_report(pool_, filepath.c_str());
    }

    mp_stats_t get_stats() const {
        mp_stats_t stats;
        mp_get_stats(pool_, &stats);
        return stats;
    }

    void dump_info() const {
        mp_dump_info(pool_);
    }

    bool check_leaks() const {
        return mp_check_leaks(pool_);
    }

    memory_pool_t* get_raw_pool() const { return pool_; }

private:
    memory_pool_t* pool_;
};

/**
 * @brief STL-compatible allocator implementation wrapping MemoryPool
 */
template <typename T>
class allocator {
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template <typename U>
    struct rebind {
        using other = allocator<U>;
    };

    explicit allocator(MemoryPool& pool) noexcept : pool_(pool.get_raw_pool()) {}
    explicit allocator(memory_pool_t* raw_pool) noexcept : pool_(raw_pool) {}

    template <typename U>
    allocator(const allocator<U>& other) noexcept : pool_(other.pool_) {}

    T* allocate(size_t n) {
        void* ptr = mp_alloc(pool_, n * sizeof(T));
        if (!ptr) throw std::bad_alloc();
        return static_cast<T*>(ptr);
    }

    void deallocate(T* p, size_t) noexcept {
        mp_free(pool_, p);
    }

    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template <typename U>
    void destroy(U* p) {
        p->~U();
    }

    memory_pool_t* pool_;
};

template <typename T, typename U>
bool operator==(const allocator<T>& a, const allocator<U>& b) noexcept {
    return a.pool_ == b.pool_;
}

template <typename T, typename U>
bool operator!=(const allocator<T>& a, const allocator<U>& b) noexcept {
    return !(a == b);
}

} // namespace mpool

#endif // MEMORY_POOL_HPP
