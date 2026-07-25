/**
 * @file cmem_pmr.hpp
 * @brief C++17 std::pmr::memory_resource Adapter for cmem Memory Pool.
 */

#ifndef CMEM_PMR_HPP
#define CMEM_PMR_HPP

#include "cmem.h"

#if defined(__cplusplus) && __cplusplus >= 201703L
#include <memory_resource>

namespace cmem {

/**
 * @brief C++17 Polymorphic Memory Resource wrapping a cmem memory_pool_t.
 */
class pmr_resource : public std::pmr::memory_resource {
public:
    explicit pmr_resource(memory_pool_t* pool) noexcept : m_pool(pool) {}
    ~pmr_resource() override = default;

    memory_pool_t* pool() const noexcept { return m_pool; }

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (!m_pool) return nullptr;
        if (alignment > sizeof(void*)) {
            return mp_aligned_alloc(m_pool, alignment, bytes);
        }
        return mp_alloc(m_pool, bytes);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) noexcept override {
        (void)bytes;
        (void)alignment;
        if (m_pool && p) {
            mp_free(m_pool, p);
        }
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        const auto* other_cmem = dynamic_cast<const pmr_resource*>(&other);
        return other_cmem && (other_cmem->m_pool == this->m_pool);
    }

private:
    memory_pool_t* m_pool;
};

} // namespace cmem

#endif // __cplusplus >= 201703L

#endif // CMEM_PMR_HPP
