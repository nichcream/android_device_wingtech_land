/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <pthread.h>

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/mman.h>
#include <unistd.h>

#include "private/bionic_lock.h"

/*
#include "pthread_internal.h"

#include "private/bionic_constants.h"
#include "private/bionic_fortify.h"
#include "private/bionic_futex.h"
#include "private/bionic_sdk_version.h"
#include "private/bionic_systrace.h"
#include "private/bionic_time_conversions.h"
#include "private/bionic_tls.h"
*/

// Priority Inheritance mutex implementation
struct PIMutex {
  // mutex type, can be 0 (normal), 1 (recursive), 2 (errorcheck), constant during lifetime
  uint8_t type;
  // process-shared flag, constant during lifetime
  bool shared;
  // <number of times a thread holding a recursive PI mutex> - 1
  uint16_t counter;
  // owner_tid is read/written by both userspace code and kernel code. It includes three fields:
  // FUTEX_WAITERS, FUTEX_OWNER_DIED and FUTEX_TID_MASK.
  atomic_int owner_tid;
};

static int PIMutexDestroy(PIMutex& mutex) {
    // The mutex should be in unlocked state (owner_tid == 0) when destroyed.
    // Store 0xffffffff to make the mutex unusable.
    int old_owner = 0;
    if (atomic_compare_exchange_strong_explicit(&mutex.owner_tid, &old_owner, 0xffffffff,
                                                memory_order_relaxed, memory_order_relaxed)) {
        return 0;
    }
    return EBUSY;
}

#if !defined(__LP64__)

namespace PIMutexAllocator {
// pthread_mutex_t has only 4 bytes in 32-bit programs, which are not enough to hold PIMutex.
// So we use malloc to allocate PIMutexes and use 16-bit of pthread_mutex_t as indexes to find
// the allocated PIMutexes. This allows at most 65536 PI mutexes.
// When calling operations like pthread_mutex_lock/unlock, the 16-bit index is mapped to the
// corresponding PIMutex. To make the map operation fast, we use a lockless mapping method:
//   Once a PIMutex is allocated, all the data used to map index to the PIMutex isn't changed until
//   it is destroyed.
// Below are the data structures:
//   // struct Node contains a PIMutex.
//   typedef Node NodeArray[256];
//   typedef NodeArray* NodeArrayP;
//   NodeArrayP nodes[256];
//
// A 16-bit index is mapped to Node as below:
//   (*nodes[index >> 8])[index & 0xff]
//
// Also use a free list to allow O(1) finding recycled PIMutexes.

union Node {
    PIMutex mutex;
    int next_free_id;  // If not -1, refer to the next node in the free PIMutex list.
};
typedef Node NodeArray[256];
typedef NodeArray* NodeArrayP;

// lock_ protects below items.
static Lock lock;
static NodeArrayP* nodes;
static int next_to_alloc_id;
static int first_free_id = -1;  // If not -1, refer to the first node in the free PIMutex list.

static inline __always_inline Node& IdToNode(int id) {
    return (*nodes[id >> 8])[id & 0xff];
}

static inline __always_inline PIMutex& IdToPIMutex(int id) {
    return IdToNode(id).mutex;
}

static int AllocIdLocked() {
    if (first_free_id != -1) {
        int result = first_free_id;
        first_free_id = IdToNode(result).next_free_id;
        return result;
    }
    if (next_to_alloc_id >= 0x10000) {
        return -1;
    }
    int array_pos = next_to_alloc_id >> 8;
    int node_pos = next_to_alloc_id & 0xff;
    if (node_pos == 0) {
        if (array_pos == 0) {
            nodes = static_cast<NodeArray**>(calloc(256, sizeof(NodeArray*)));
            if (nodes == nullptr) {
                return -1;
            }
        }
        nodes[array_pos] = static_cast<NodeArray*>(malloc(sizeof(NodeArray)));
        if (nodes[array_pos] == nullptr) {
            return -1;
        }
    }
    return next_to_alloc_id++;
}

// If succeed, return an id referring to a PIMutex, otherwise return -1.
// A valid id is in range [0, 0xffff].
static int AllocId() {
    lock.lock();
    int result = AllocIdLocked();
    lock.unlock();
    if (result != -1) {
        memset(&IdToPIMutex(result), 0, sizeof(PIMutex));
    }
    return result;
}

static void FreeId(int id) {
    lock.lock();
    IdToNode(id).next_free_id = first_free_id;
    first_free_id = id;
    lock.unlock();
}

}  // namespace PIMutexAllocator

#endif  // !defined(__LP64__)


/* Convenience macro, creates a mask of 'bits' bits that starts from
 * the 'shift'-th least significant bit in a 32-bit word.
 *
 * Examples: FIELD_MASK(0,4)  -> 0xf
 *           FIELD_MASK(16,9) -> 0x1ff0000
 */
#define  FIELD_MASK(shift,bits)           (((1 << (bits))-1) << (shift))

/* This one is used to create a bit pattern from a given field value */
#define  FIELD_TO_BITS(val,shift,bits)    (((val) & ((1 << (bits))-1)) << (shift))

/* And this one does the opposite, i.e. extract a field's value from a bit pattern */
#define  FIELD_FROM_BITS(val,shift,bits)  (((val) >> (shift)) & ((1 << (bits))-1))

/* Convenience macros.
 *
 * These are used to form or modify the bit pattern of a given mutex value
 */

/* Mutex state:
 *
 * 0 for unlocked
 * 1 for locked, no waiters
 * 2 for locked, maybe waiters
 */
#define  MUTEX_STATE_SHIFT      0
#define  MUTEX_STATE_LEN        2

#define  MUTEX_STATE_MASK           FIELD_MASK(MUTEX_STATE_SHIFT, MUTEX_STATE_LEN)
#define  MUTEX_STATE_FROM_BITS(v)   FIELD_FROM_BITS(v, MUTEX_STATE_SHIFT, MUTEX_STATE_LEN)
#define  MUTEX_STATE_TO_BITS(v)     FIELD_TO_BITS(v, MUTEX_STATE_SHIFT, MUTEX_STATE_LEN)

#define  MUTEX_STATE_UNLOCKED            0   /* must be 0 to match PTHREAD_MUTEX_INITIALIZER */
#define  MUTEX_STATE_LOCKED_UNCONTENDED  1   /* must be 1 due to atomic dec in unlock operation */
#define  MUTEX_STATE_LOCKED_CONTENDED    2   /* must be 1 + LOCKED_UNCONTENDED due to atomic dec */

#define  MUTEX_STATE_BITS_UNLOCKED            MUTEX_STATE_TO_BITS(MUTEX_STATE_UNLOCKED)
#define  MUTEX_STATE_BITS_LOCKED_UNCONTENDED  MUTEX_STATE_TO_BITS(MUTEX_STATE_LOCKED_UNCONTENDED)
#define  MUTEX_STATE_BITS_LOCKED_CONTENDED    MUTEX_STATE_TO_BITS(MUTEX_STATE_LOCKED_CONTENDED)

// Return true iff the mutex is unlocked.
#define MUTEX_STATE_BITS_IS_UNLOCKED(v) (((v) & MUTEX_STATE_MASK) == MUTEX_STATE_BITS_UNLOCKED)

// Return true iff the mutex is locked with no waiters.
#define MUTEX_STATE_BITS_IS_LOCKED_UNCONTENDED(v)  (((v) & MUTEX_STATE_MASK) == MUTEX_STATE_BITS_LOCKED_UNCONTENDED)

// return true iff the mutex is locked with maybe waiters.
#define MUTEX_STATE_BITS_IS_LOCKED_CONTENDED(v)   (((v) & MUTEX_STATE_MASK) == MUTEX_STATE_BITS_LOCKED_CONTENDED)

/* used to flip from LOCKED_UNCONTENDED to LOCKED_CONTENDED */
#define  MUTEX_STATE_BITS_FLIP_CONTENTION(v)      ((v) ^ (MUTEX_STATE_BITS_LOCKED_CONTENDED ^ MUTEX_STATE_BITS_LOCKED_UNCONTENDED))

/* Mutex counter:
 *
 * We need to check for overflow before incrementing, and we also need to
 * detect when the counter is 0
 */
#define  MUTEX_COUNTER_SHIFT         2
#define  MUTEX_COUNTER_LEN           11
#define  MUTEX_COUNTER_MASK          FIELD_MASK(MUTEX_COUNTER_SHIFT, MUTEX_COUNTER_LEN)

#define  MUTEX_COUNTER_BITS_WILL_OVERFLOW(v)    (((v) & MUTEX_COUNTER_MASK) == MUTEX_COUNTER_MASK)
#define  MUTEX_COUNTER_BITS_IS_ZERO(v)          (((v) & MUTEX_COUNTER_MASK) == 0)

/* Used to increment the counter directly after overflow has been checked */
#define  MUTEX_COUNTER_BITS_ONE      FIELD_TO_BITS(1, MUTEX_COUNTER_SHIFT,MUTEX_COUNTER_LEN)

/* Mutex shared bit flag
 *
 * This flag is set to indicate that the mutex is shared among processes.
 * This changes the futex opcode we use for futex wait/wake operations
 * (non-shared operations are much faster).
 */
#define  MUTEX_SHARED_SHIFT    13
#define  MUTEX_SHARED_MASK     FIELD_MASK(MUTEX_SHARED_SHIFT,1)

/* Mutex type:
 * We support normal, recursive and errorcheck mutexes.
 */
#define  MUTEX_TYPE_SHIFT      14
#define  MUTEX_TYPE_LEN        2
#define  MUTEX_TYPE_MASK       FIELD_MASK(MUTEX_TYPE_SHIFT,MUTEX_TYPE_LEN)

#define  MUTEX_TYPE_TO_BITS(t)       FIELD_TO_BITS(t, MUTEX_TYPE_SHIFT, MUTEX_TYPE_LEN)

#define  MUTEX_TYPE_BITS_NORMAL      MUTEX_TYPE_TO_BITS(PTHREAD_MUTEX_NORMAL)
#define  MUTEX_TYPE_BITS_RECURSIVE   MUTEX_TYPE_TO_BITS(PTHREAD_MUTEX_RECURSIVE)
#define  MUTEX_TYPE_BITS_ERRORCHECK  MUTEX_TYPE_TO_BITS(PTHREAD_MUTEX_ERRORCHECK)
// Use a special mutex type to mark priority inheritance mutexes.
#define  PI_MUTEX_STATE     MUTEX_TYPE_TO_BITS(3)

// For a PI mutex, it includes below fields:
//   Atomic(uint16_t) state;
//   PIMutex pi_mutex;  // uint16_t pi_mutex_id in 32-bit programs
//
//   state holds the following fields:
//
//   bits:   name    description
//   15-14   type    mutex type, should be 3
//   13-0    padding should be 0
//
//   pi_mutex holds the state of a PI mutex.
//   pi_mutex_id holds an integer to find the state of a PI mutex.
//
// For a Non-PI mutex, it includes below fields:
//   Atomic(uint16_t) state;
//   atomic_int owner_tid;  // Atomic(uint16_t) in 32-bit programs
//
//   state holds the following fields:
//
//   bits:     name     description
//   15-14     type     mutex type, can be 0 (normal), 1 (recursive), 2 (errorcheck)
//   13        shared   process-shared flag
//   12-2      counter  <number of times a thread holding a recursive Non-PI mutex> - 1
//   1-0       state    lock state (0, 1 or 2)
//
//   bits 15-13 are constant during the lifetime of the mutex.
//
//   owner_tid is used only in recursive and errorcheck Non-PI mutexes to hold the mutex owner
//   thread id.
//
// PI mutexes and Non-PI mutexes are distinguished by checking type field in state.
#if defined(__LP64__)
struct pthread_mutex_internal_t {
    _Atomic(uint16_t) state;
    uint16_t __pad;
    union {
        atomic_int owner_tid;
        PIMutex pi_mutex;
    };
    char __reserved[28];

    PIMutex& ToPIMutex() {
        return pi_mutex;
    }

    void FreePIMutex() {
    }
} __attribute__((aligned(4)));

#else
struct pthread_mutex_internal_t {
    _Atomic(uint16_t) state;
    union {
        _Atomic(uint16_t) owner_tid;
        uint16_t pi_mutex_id;
    };

    PIMutex& ToPIMutex() {
        return PIMutexAllocator::IdToPIMutex(pi_mutex_id);
    }

    void FreePIMutex() {
        PIMutexAllocator::FreeId(pi_mutex_id);
    }
} __attribute__((aligned(4)));
#endif

static_assert(sizeof(pthread_mutex_t) == sizeof(pthread_mutex_internal_t),
              "pthread_mutex_t should actually be pthread_mutex_internal_t in implementation.");

// For binary compatibility with old version of pthread_mutex_t, we can't use more strict alignment
// than 4-byte alignment.
static_assert(alignof(pthread_mutex_t) == 4,
              "pthread_mutex_t should fulfill the alignment of pthread_mutex_internal_t.");

static inline pthread_mutex_internal_t* __get_internal_mutex(pthread_mutex_t* mutex_interface) {
  return reinterpret_cast<pthread_mutex_internal_t*>(mutex_interface);
}

static inline __always_inline bool IsMutexDestroyed(uint16_t mutex_state) {
    return mutex_state == 0xffff;
}

// Inlining this function in pthread_mutex_lock() adds the cost of stack frame instructions on
// ARM64. So make it noinline.
static int __attribute__((noinline)) HandleUsingDestroyedMutex(pthread_mutex_t* mutex,
                                                               const char* function_name) {
#if 0
    if (bionic_get_application_target_sdk_version() >= __ANDROID_API_P__) {
        __fortify_fatal("%s called on a destroyed mutex (%p)", function_name, mutex);
    }
#else
    /* UGH Qualcomm.. */
    (void)(mutex);
    (void)(function_name);
#endif
    return EBUSY;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex_interface) {
    pthread_mutex_internal_t* mutex = __get_internal_mutex(mutex_interface);
    uint16_t old_state = atomic_load_explicit(&mutex->state, memory_order_relaxed);
    if (__predict_false(IsMutexDestroyed(old_state))) {
        return HandleUsingDestroyedMutex(mutex_interface, __FUNCTION__);
    }
    if (old_state == PI_MUTEX_STATE) {
        int result = PIMutexDestroy(mutex->ToPIMutex());
        if (result == 0) {
            mutex->FreePIMutex();
            atomic_store(&mutex->state, 0xffff);
        }
        return result;
    }
    // Store 0xffff to make the mutex unusable. Although POSIX standard says it is undefined
    // behavior to destroy a locked mutex, we prefer not to change mutex->state in that situation.
    if (MUTEX_STATE_BITS_IS_UNLOCKED(old_state) &&
        atomic_compare_exchange_strong_explicit(&mutex->state, &old_state, 0xffff,
                                                memory_order_relaxed, memory_order_relaxed)) {
      return 0;
    }
    return EBUSY;
}
