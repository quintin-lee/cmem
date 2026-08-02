/**
 * @file cmem.hpp
 * @brief C++11 Header-only RAII Wrapper and STL-compliant Allocator for cmem.
 *
 * Provides:
 *  - cmem::MemoryPool: RAII wrapper around memory_pool_t with exception safety
 *  - cmem::allocator<T>: STL-compliant allocator template for use with std::vector, std::map, etc.
 *
 * Requires C++11 or later.
 */

#ifndef CMEM_HPP
#define CMEM_HPP

#include "cmem.h"
#include <memory>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>

namespace cmem
{

/**
 * @brief C++ RAII Wrapper class for memory_pool_t.
 *
 * Provides automatic resource management for cmem memory pools.
 * The pool is automatically destroyed when the MemoryPool object goes out of scope.
 *
 * This class is non-copyable but movable, allowing transfer of ownership.
 *
 * Example usage:
 * @code
 *   cmem::MemoryPool pool(1024 * 1024, MP_FLAG_THREAD_SAFE);
 *   void* p = pool.alloc(128);
 *   pool.free(p);
 * @endcode
 */
class MemoryPool
{
  public:
    /**
     * @brief Constructs a new memory pool with default OS backing.
     *
     * @param initial_capacity Initial memory capacity in bytes (0 for default)
     * @param flags Configuration flags (thread safety, canary, etc.)
     * @throw std::runtime_error if pool creation fails
     */
    explicit MemoryPool(size_t initial_capacity = 0, mp_flags_t flags = MP_FLAG_DEFAULT)
        : pool_(mp_create(initial_capacity, flags)), shm_name_("")
    {
        if (!pool_)
        {
            throw std::runtime_error("Failed to create memory pool instance.");
        }
    }

    /**
     * @brief Constructs a POSIX shared memory pool.
     *
     * @param shm_name Name of the shared memory object (e.g. "/my_pool")
     * @param capacity Capacity in bytes
     * @param flags Configuration flags
     * @throw std::runtime_error if shared pool creation fails
     */
    MemoryPool(const std::string& shm_name, size_t capacity, mp_flags_t flags)
        : pool_(mp_create_shared(shm_name.c_str(), capacity, flags)), shm_name_(shm_name)
    {
        if (!pool_)
        {
            throw std::runtime_error("Failed to create POSIX shared memory pool instance.");
        }
    }

    /**
     * @brief Constructs a child memory pool linked to a parent.
     *
     * Child pools form a tree structure with their parent. Destroying or resetting
     * the parent recursively affects all linked children.
     *
     * @param parent Reference to the parent MemoryPool
     * @param initial_capacity Initial capacity for the child pool
     * @param flags Configuration flags
     * @param name Human-readable name for the child arena
     * @throw std::runtime_error if child pool creation fails
     */
    MemoryPool(MemoryPool& parent, size_t initial_capacity, mp_flags_t flags,
               const std::string& name)
        : pool_(mp_create_child(parent.get_raw_pool(), initial_capacity, flags, name.c_str())),
          shm_name_("")
    {
        if (!pool_)
        {
            throw std::runtime_error("Failed to create child memory pool instance.");
        }
    }

    /**
     * @brief Destroys the memory pool and releases all resources.
     *
     * If this is a shared memory pool, it also unlinks the POSIX shared memory segment.
     * If this is a child pool, it recursively destroys all linked children.
     */
    ~MemoryPool()
    {
        if (pool_)
        {
            if (!shm_name_.empty())
            {
                mp_destroy_shared(pool_, shm_name_.c_str());
            }
            else
            {
                mp_destroy(pool_);
            }
        }
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    /**
     * @brief Move constructor.
     *
     * Transfers ownership of the pool from another MemoryPool.
     * The source MemoryPool is left in a valid but empty state.
     *
     * @param other Source MemoryPool to move from
     */
    MemoryPool(MemoryPool&& other) noexcept
        : pool_(other.pool_), shm_name_(std::move(other.shm_name_))
    {
        other.pool_ = nullptr;
    }

    /**
     * @brief Move assignment operator.
     *
     * Transfers ownership of the pool from another MemoryPool.
     * Any existing pool is destroyed before the transfer.
     *
     * @param other Source MemoryPool to move from
     * @return Reference to this MemoryPool
     */
    MemoryPool& operator=(MemoryPool&& other) noexcept
    {
        if (this != &other)
        {
            if (pool_)
            {
                if (!shm_name_.empty())
                    mp_destroy_shared(pool_, shm_name_.c_str());
                else
                    mp_destroy(pool_);
            }
            pool_ = other.pool_;
            shm_name_ = std::move(other.shm_name_);
            other.pool_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief Allocates a memory block of the given size.
     *
     * If MP_FLAG_CACHE_ALIGNED was set, this allocates with 64-byte alignment.
     *
     * @param size Requested size in bytes
     * @return Pointer to the allocated memory, or nullptr on failure
     */
    void* alloc(size_t size)
    {
        return mp_alloc(pool_, size);
    }

    /**
     * @brief Allocates a memory block with source location tracking.
     *
     * @param size Requested size in bytes
     * @param file Source file name (usually __FILE__)
     * @param line Source line number (usually __LINE__)
     * @param func Source function name (usually __func__)
     * @return Pointer to the allocated memory, or nullptr on failure
     */
    void* alloc_loc(size_t size, const char* file, int line, const char* func)
    {
        return mp_alloc_loc(pool_, size, file, line, func);
    }

    /**
     * @brief Allocates and zero-initializes memory for an array.
     *
     * @param num Number of elements
     * @param size Size of each element in bytes
     * @return Pointer to the allocated memory, or nullptr on failure
     */
    void* calloc(size_t num, size_t size)
    {
        return mp_calloc(pool_, num, size);
    }

    /**
     * @brief Reallocates a memory block to a new size.
     *
     * Attempts in-place expansion for TLSF blocks to avoid memcpy.
     *
     * @param ptr Existing allocation pointer (or nullptr for new allocation)
     * @param new_size New requested size in bytes
     * @return Pointer to the reallocated memory, or nullptr on failure
     */
    void* realloc(void* ptr, size_t new_size)
    {
        return mp_realloc(pool_, ptr, new_size);
    }

    /**
     * @brief Allocates memory with a specific byte alignment.
     *
     * @param alignment Byte alignment (must be power of two, minimum sizeof(void*))
     * @param size Requested payload size in bytes
     * @return Pointer to the aligned memory, or nullptr on failure
     */
    void* aligned_alloc(size_t alignment, size_t size)
    {
        return mp_aligned_alloc(pool_, alignment, size);
    }

    /**
     * @brief Frees a memory block back to the pool.
     *
     * Performs canary checks and poison fill if those flags are enabled.
     *
     * @param ptr Pointer to the memory to free
     */
    void free(void* ptr)
    {
        mp_free(pool_, ptr);
    }

    /**
     * @brief Allocates multiple memory blocks of the same size.
     *
     * @param size Size of each block in bytes
     * @param out_ptrs Output array to store allocated pointers
     * @param count Maximum number of blocks to allocate
     * @return Number of blocks successfully allocated
     */
    size_t alloc_batch(size_t size, void** out_ptrs, size_t count)
    {
        return mp_alloc_batch(pool_, size, out_ptrs, count);
    }

    /**
     * @brief Frees multiple memory blocks at once.
     *
     * @param ptrs Array of pointers to free
     * @param count Number of pointers in the array
     */
    void free_batch(void** ptrs, size_t count)
    {
        mp_free_batch(pool_, ptrs, count);
    }

    /**
     * @brief Resets the pool to an empty state in O(1) time.
     *
     * All allocations are logically freed; underlying memory is retained for reuse.
     * This also resets all child arenas.
     */
    void reset()
    {
        mp_reset(pool_);
    }

    /**
     * @brief Compacts the pool by freeing completely empty Slab pages.
     *
     * @return Number of bytes freed back to the OS
     */
    size_t compact()
    {
        return mp_compact(pool_);
    }

    /**
     * @brief Sets a hard maximum memory budget limit.
     *
     * When active_bytes exceeds this limit, allocations may fail or
     * fall back to the emergency reserve if enabled.
     *
     * @param max_bytes Maximum allowed active bytes (0 for unlimited)
     */
    void set_memory_limit(size_t max_bytes)
    {
        mp_set_memory_limit(pool_, max_bytes);
    }

    /**
     * @brief Audits heap integrity by checking canary redzones and header magic.
     *
     * @return true if heap is healthy, false if corruption detected
     */
    bool audit_heap() const
    {
        return mp_audit_heap(pool_);
    }

    /**
     * @brief Generates a detailed memory leak analysis report.
     *
     * @return String containing the leak report
     */
    std::string analyze_leaks() const
    {
        char buffer[16384];
        size_t len = mp_analyze_leaks(pool_, buffer, sizeof(buffer));
        return std::string(buffer, len);
    }

    /**
     * @brief Exports the memory leak analysis report to a text file.
     *
     * @param filepath Path to the output text file
     * @return true on success
     */
    bool export_leak_report(const std::string& filepath) const
    {
        return mp_export_leak_report(pool_, filepath.c_str());
    }

    /**
     * @brief Exports an interactive HTML profiler dashboard to a file.
     *
     * @param filepath Path to the output HTML file
     * @return true on success
     */
    bool export_html_report(const std::string& filepath) const
    {
        return mp_export_html_report(pool_, filepath.c_str());
    }

    /**
     * @brief Exports a binary crash snapshot dump to a file.
     *
     * @param filepath Path to the output binary snapshot file
     * @return true on success
     */
    bool export_binary_snapshot(const std::string& filepath) const
    {
        return mp_export_binary_snapshot(pool_, filepath.c_str());
    }

    /**
     * @brief Parses a binary crash snapshot file into a readable string.
     *
     * @param filepath Path to the binary snapshot file
     * @return String containing the parsed snapshot report, or empty string on failure
     */
    static std::string parse_binary_snapshot(const std::string& filepath)
    {
        char buffer[16384];
        if (mp_parse_binary_snapshot(filepath.c_str(), buffer, sizeof(buffer)))
        {
            return std::string(buffer);
        }
        return "";
    }

    /**
     * @brief Retrieves current statistical metrics of the pool.
     *
     * @return mp_stats_t structure with current statistics
     */
    mp_stats_t get_stats() const
    {
        mp_stats_t stats;
        mp_get_stats(pool_, &stats);
        return stats;
    }

    /**
     * @brief Prints detailed diagnostics summary to stdout.
     */
    void dump_info() const
    {
        mp_dump_info(pool_);
    }

    /**
     * @brief Prints arena tree hierarchy to stdout.
     */
    void dump_tree_info() const
    {
        mp_dump_tree_info(pool_);
    }

    /**
     * @brief Prints allocation size histogram to stdout.
     */
    void dump_histogram() const
    {
        mp_dump_histogram(pool_);
    }

    /**
     * @brief Checks for memory leaks and reports them to stderr if found.
     *
     * @return true if no leaks detected, false otherwise
     */
    bool check_leaks() const
    {
        return mp_check_leaks(pool_);
    }

    /**
     * @brief Retrieves detailed metadata for a single allocation.
     *
     * @param ptr Payload pointer returned by alloc()/calloc()/realloc()
     * @param info Output structure filled with allocation metadata
     * @return true if ptr is valid and info was filled, false otherwise
     */
    bool get_allocation_info(void* ptr, mp_allocation_info_t* info) const
    {
        return mp_get_allocation_info(pool_, ptr, info);
    }

    /**
     * @brief Enumerates all memory regions backing this pool.
     *
     * @param regions Output array of mp_region_info_t
     * @param max_regions Maximum number of entries the array can hold
     * @return Number of regions written to the array
     */
    size_t enumerate_regions(mp_region_info_t* regions, size_t max_regions) const
    {
        return mp_enumerate_regions(pool_, regions, max_regions);
    }

    /**
     * @brief Returns the underlying C memory pool pointer.
     *
     * Useful for calling C API functions not wrapped by this class.
     *
     * @return Pointer to the underlying memory_pool_t
     */
    memory_pool_t* get_raw_pool() const
    {
        return pool_;
    }

    /**
     * @brief Returns the underlying C memory pool pointer (STL-compatible alias).
     *
     * @return Pointer to the underlying memory_pool_t
     */
    memory_pool_t* get() const
    {
        return pool_;
    }

  private:
    memory_pool_t* pool_;  /**< Underlying C memory pool */
    std::string shm_name_; /**< Shared memory name (empty if not shared) */
};

/**
 * @brief STL-compliant allocator template wrapping cmem::MemoryPool.
 *
 * This allocator conforms to the C++11/14/17 Allocator requirements and can be
 * used with all standard library containers.
 *
 * @tparam T Type of elements to allocate
 *
 * Example usage:
 * @code
 *   cmem::MemoryPool pool(1024 * 1024);
 *   cmem::allocator<int> alloc(pool);
 *   std::vector<int, cmem::allocator<int>> vec(alloc);
 *   vec.push_back(42);
 * @endcode
 */
template <typename T> class allocator
{
  public:
    using value_type = T;                   /**< Element type */
    using pointer = T*;                     /**< Pointer to element */
    using const_pointer = const T*;         /**< Pointer to const element */
    using reference = T&;                   /**< Reference to element */
    using const_reference = const T&;       /**< Reference to const element */
    using size_type = std::size_t;          /**< Unsigned size type */
    using difference_type = std::ptrdiff_t; /**< Signed difference type */

    /**
     * @brief Rebind template for converting allocator to another type.
     *
     * Required by the C++ Allocator concept.
     */
    template <typename U> struct rebind
    {
        using other = allocator<U>;
    };

    /**
     * @brief Constructs an allocator from a cmem::MemoryPool reference.
     *
     * @param pool Reference to the MemoryPool to use for allocations
     */
    explicit allocator(MemoryPool& pool) noexcept : pool_(pool.get_raw_pool())
    {
    }

    /**
     * @brief Constructs an allocator from a raw memory_pool_t pointer.
     *
     * @param raw_pool Pointer to the underlying C memory pool
     */
    explicit allocator(memory_pool_t* raw_pool) noexcept : pool_(raw_pool)
    {
    }

    /**
     * @brief Copy constructor from an allocator of a different type.
     *
     * Allows converting allocator<T> to allocator<U> when both use the same pool.
     *
     * @tparam U Other element type
     * @param other Source allocator
     */
    template <typename U> allocator(const allocator<U>& other) noexcept : pool_(other.pool_)
    {
    }

    /**
     * @brief Allocates memory for n objects of type T.
     *
     * @param n Number of objects to allocate
     * @return Pointer to the allocated memory
     * @throw std::bad_alloc if allocation fails
     */
    T* allocate(size_t n)
    {
        void* ptr = mp_alloc(pool_, n * sizeof(T));
        if (!ptr)
            throw std::bad_alloc();
        return static_cast<T*>(ptr);
    }

    /**
     * @brief Deallocates memory previously allocated by this allocator.
     *
     * @param p Pointer to the memory to deallocate
     * @param n Number of objects (unused, kept for interface compatibility)
     */
    void deallocate(T* p, size_t) noexcept
    {
        mp_free(pool_, p);
    }

    /**
     * @brief Constructs an object in the allocated memory.
     *
     * @tparam U Type of object to construct
     * @param p Pointer to the memory where the object will be constructed
     * @param args Constructor arguments
     */
    template <typename U, typename... Args> void construct(U* p, Args&&... args)
    {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    /**
     * @brief Destroys an object in the allocated memory.
     *
     * @tparam U Type of object to destroy
     * @param p Pointer to the object to destroy
     */
    template <typename U> void destroy(U* p)
    {
        p->~U();
    }

    memory_pool_t* pool_; /**< Underlying C memory pool */
};

/**
 * @brief Equality comparison for cmem::allocator instances.
 *
 * Two allocators are equal if they use the same underlying memory pool.
 *
 * @tparam T First allocator element type
 * @tparam U Second allocator element type
 * @param a First allocator
 * @param b Second allocator
 * @return true if both allocators wrap the same pool
 */
template <typename T, typename U>
bool operator==(const allocator<T>& a, const allocator<U>& b) noexcept
{
    return a.pool_ == b.pool_;
}

/**
 * @brief Inequality comparison for cmem::allocator instances.
 *
 * @tparam T First allocator element type
 * @tparam U Second allocator element type
 * @param a First allocator
 * @param b Second allocator
 * @return true if allocators wrap different pools
 */
template <typename T, typename U>
bool operator!=(const allocator<T>& a, const allocator<U>& b) noexcept
{
    return !(a == b);
}

} // namespace cmem

/**
 * @brief Convenience namespace alias for cmem.
 *
 * Allows using mpool::MemoryPool instead of cmem::MemoryPool.
 */
namespace mpool = cmem;

#endif // CMEM_HPP
