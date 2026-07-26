/**
 * @file cmem_pmr.hpp
 * @brief C++17 std::pmr::memory_resource Adapter for cmem Memory Pool.
 *
 * This header provides a C++17 Polymorphic Memory Resource (PMR) adapter
 * that wraps a cmem memory_pool_t, allowing standard library containers
 * like std::pmr::vector, std::pmr::string, and std::pmr::map to use
 * cmem as their backing allocator.
 *
 * Requires C++17 or later.
 */

#ifndef CMEM_PMR_HPP
#define CMEM_PMR_HPP

#include "cmem.h"

#if defined(__cplusplus) && __cplusplus >= 201703L
#include <memory_resource>

namespace cmem {

/**
 * @brief C++17 Polymorphic Memory Resource wrapping a cmem memory_pool_t.
 *
 * This class adapts a cmem memory pool to the std::pmr::memory_resource interface,
 * enabling all C++17 polymorphic containers to use cmem for their allocations.
 *
 * Example usage:
 * @code
 *   cmem::MemoryPool pool(1024 * 1024, MP_FLAG_THREAD_SAFE);
 *   cmem::pmr_resource res(pool.get());
 *   std::pmr::vector<int> vec(&res);
 *   vec.push_back(42);
 * @endcode
 */
class pmr_resource : public std::pmr::memory_resource {
public:
    /**
     * @brief Constructs a PMR resource wrapping the given cmem pool.
     * @param pool Pointer to an existing cmem memory pool (must outlive this resource)
     */
    explicit pmr_resource(memory_pool_t* pool) noexcept : m_pool(pool) {}

    /**
     * @brief Destroys the PMR resource.
     *
     * Note: This does NOT destroy the underlying cmem pool.
     * The pool must be managed separately.
     */
    ~pmr_resource() override = default;

    /**
     * @brief Returns the underlying cmem memory pool pointer.
     * @return Pointer to the memory pool
     */
    memory_pool_t* pool() const noexcept { return m_pool; }

protected:
    /**
     * @brief Allocates memory from the cmem pool.
     *
     * Uses mp_aligned_alloc when alignment exceeds sizeof(void*),
     * otherwise uses standard mp_alloc.
     *
     * @param bytes Number of bytes to allocate
     * @param alignment Alignment requirement in bytes
     * @return Pointer to the allocated memory, or nullptr on failure
     */
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (!m_pool) return nullptr;
        if (alignment > sizeof(void*)) {
            return mp_aligned_alloc(m_pool, alignment, bytes);
        }
        return mp_alloc(m_pool, bytes);
    }

    /**
     * @brief Deallocates memory back to the cmem pool.
     *
     * @param p Pointer to the memory to deallocate
     * @param bytes Size of the allocation (unused, kept for interface compatibility)
     * @param alignment Alignment of the allocation (unused, kept for interface compatibility)
     */
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) noexcept override {
        (void)bytes;
        (void)alignment;
        if (m_pool && p) {
            mp_free(m_pool, p);
        }
    }

    /**
     * @brief Compares this resource with another for equality.
     *
     * Two pmr_resource objects are considered equal if they wrap the same cmem pool.
     *
     * @param other Another memory_resource to compare with
     * @return true if both resources wrap the same cmem pool
     */
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        const auto* other_cmem = dynamic_cast<const pmr_resource*>(&other);
        return other_cmem && (other_cmem->m_pool == this->m_pool);
    }

private:
    memory_pool_t* m_pool; /**< Underlying cmem memory pool pointer */
};

} // namespace cmem

#endif // __cplusplus >= 201703L

#endif // CMEM_PMR_HPP
