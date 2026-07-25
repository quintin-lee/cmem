/**
 * @file cmem.hpp
 * @brief C++11 Header-only RAII Wrapper and STL-compliant Allocator for cmem.
 */

#ifndef CMEM_HPP
#define CMEM_HPP

#include "cmem.h"
#include <memory>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>

namespace cmem {

/**
 * @brief C++ RAII Wrapper class for memory_pool_t
 */
class MemoryPool {
public:
    explicit MemoryPool(size_t initial_capacity = 0, mp_flags_t flags = MP_FLAG_DEFAULT)
        : pool_(mp_create(initial_capacity, flags)), shm_name_("") {
        if (!pool_) {
            throw std::runtime_error("Failed to create memory pool instance.");
        }
    }

    MemoryPool(const std::string& shm_name, size_t capacity, mp_flags_t flags)
        : pool_(mp_create_shared(shm_name.c_str(), capacity, flags)), shm_name_(shm_name) {
        if (!pool_) {
            throw std::runtime_error("Failed to create POSIX shared memory pool instance.");
        }
    }

    MemoryPool(MemoryPool& parent, size_t initial_capacity, mp_flags_t flags, const std::string& name)
        : pool_(mp_create_child(parent.get_raw_pool(), initial_capacity, flags, name.c_str())), shm_name_("") {
        if (!pool_) {
            throw std::runtime_error("Failed to create child memory pool instance.");
        }
    }

    ~MemoryPool() {
        if (pool_) {
            if (!shm_name_.empty()) {
                mp_destroy_shared(pool_, shm_name_.c_str());
            } else {
                mp_destroy(pool_);
            }
        }
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    MemoryPool(MemoryPool&& other) noexcept : pool_(other.pool_), shm_name_(std::move(other.shm_name_)) {
        other.pool_ = nullptr;
    }

    MemoryPool& operator=(MemoryPool&& other) noexcept {
        if (this != &other) {
            if (pool_) {
                if (!shm_name_.empty()) mp_destroy_shared(pool_, shm_name_.c_str());
                else mp_destroy(pool_);
            }
            pool_ = other.pool_;
            shm_name_ = std::move(other.shm_name_);
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

    size_t alloc_batch(size_t size, void** out_ptrs, size_t count) {
        return mp_alloc_batch(pool_, size, out_ptrs, count);
    }

    void free_batch(void** ptrs, size_t count) {
        mp_free_batch(pool_, ptrs, count);
    }

    void reset() {
        mp_reset(pool_);
    }

    size_t compact() {
        return mp_compact(pool_);
    }

    void set_memory_limit(size_t max_bytes) {
        mp_set_memory_limit(pool_, max_bytes);
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

    bool export_html_report(const std::string& filepath) const {
        return mp_export_html_report(pool_, filepath.c_str());
    }

    bool export_binary_snapshot(const std::string& filepath) const {
        return mp_export_binary_snapshot(pool_, filepath.c_str());
    }

    static std::string parse_binary_snapshot(const std::string& filepath) {
        char buffer[16384];
        if (mp_parse_binary_snapshot(filepath.c_str(), buffer, sizeof(buffer))) {
            return std::string(buffer);
        }
        return "";
    }

    mp_stats_t get_stats() const {
        mp_stats_t stats;
        mp_get_stats(pool_, &stats);
        return stats;
    }

    void dump_info() const {
        mp_dump_info(pool_);
    }

    void dump_tree_info() const {
        mp_dump_tree_info(pool_);
    }

    void dump_histogram() const {
        mp_dump_histogram(pool_);
    }

    bool check_leaks() const {
        return mp_check_leaks(pool_);
    }

    memory_pool_t* get_raw_pool() const { return pool_; }

private:
    memory_pool_t* pool_;
    std::string shm_name_;
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

} // namespace cmem

namespace mpool = cmem;

#endif // CMEM_HPP
