/**
 * @file cmem_ring.h
 * @brief Lock-free ring buffer allocator and event logging ring buffer for cmem.
 */

#ifndef CMEM_RING_H
#define CMEM_RING_H

#include "cmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to a lock-free ring buffer allocator.
 */
typedef struct cmem_ring_buffer cmem_ring_buffer_t;

/**
 * @brief Opaque handle to a structured event log ring buffer.
 */
typedef struct mp_event_log mp_event_log_t;

/**
 * @brief Single entry in the event log ring buffer.
 */
typedef struct {
    uint64_t timestamp_ns;      /**< Monotonic timestamp in nanoseconds */
    mp_event_type_t event_type; /**< Event typeRecorded */
    size_t size;                /**< Allocation size in bytes */
    uintptr_t ptr;              /**< Pointer involved in the event */
} mp_event_log_entry_t;

/**
 * @brief Creates a DPDK-style SPSC lock-free ring buffer allocator.
 *
 * @param slot_size Fixed byte size of each payload slot
 * @param capacity Number of slots held (must be a power of two)
 * @return Handle to the ring buffer, or NULL on allocation failure
 */
cmem_ring_buffer_t *mp_ring_create(size_t slot_size, size_t capacity);

/**
 * @brief Allocates one slot from the lock-free ring buffer.
 *
 * @param ring Pointer to the ring buffer
 * @return Payload pointer to the allocated slot, or NULL if ring is full
 */
void *mp_ring_alloc(cmem_ring_buffer_t *ring);

/**
 * @brief Frees one slot back to the lock-free ring buffer.
 *
 * @param ring Pointer to the ring buffer
 * @param ptr Pointer to the slot payload
 * @return true on success, false if ptr does not belong to the ring
 */
bool mp_ring_free(cmem_ring_buffer_t *ring, void *ptr);

/**
 * @brief Destroys the lock-free ring buffer and releases its backing memory.
 *
 * @param ring Pointer to the ring buffer
 */
void mp_ring_destroy(cmem_ring_buffer_t *ring);

/**
 * @brief Creates a structured event log ring buffer for pool auditing.
 *
 * @param capacity Maximum number of log entries
 * @return Pointer to event log instance, or NULL on failure
 */
mp_event_log_t *mp_event_log_create(size_t capacity);

/**
 * @brief Destroys the event log and frees all associated memory.
 *
 * @param log Pointer to the event log
 */
void mp_event_log_destroy(mp_event_log_t *log);

/**
 * @brief Records a new event into the event log ring buffer.
 *
 * @param log Pointer to the event log
 * @param event_type Event type
 * @param ptr Pointer involved in the event
 * @param size Size of the allocation
 * @return true on success, false if ring buffer is full
 */
bool mp_event_log_record(mp_event_log_t *log, mp_event_type_t event_type, void *ptr, size_t size);

/**
 * @brief Consumes and returns the next event from the ring buffer.
 *
 * @param log Pointer to the event log
 * @param entry Output event entry
 * @return true if an event was consumed, false if buffer is empty
 */
bool mp_event_log_consume(mp_event_log_t *log, mp_event_log_entry_t *entry);

/**
 * @brief Returns the number of unread events in the ring buffer.
 *
 * @param log Pointer to the event log
 * @return Number of pending events
 */
size_t mp_event_log_pending(mp_event_log_t *log);

/**
 * @brief Clears all pending events from the ring buffer.
 *
 * @param log Pointer to the event log
 */
void mp_event_log_clear(mp_event_log_t *log);

#ifdef __cplusplus
}
#endif

#endif // CMEM_RING_H
