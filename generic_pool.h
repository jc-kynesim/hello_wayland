#ifndef _GENERIC_POOL_H
#define _GENERIC_POOL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct generic_pool_s;
typedef struct generic_pool_s generic_pool_t;

// fb pool

void generic_pool_unref(generic_pool_t ** const pppool);
generic_pool_t * generic_pool_ref(generic_pool_t * const pool);

// cb to allocate a new pool thing
typedef void * (* generic_pool_alloc_thing_fn)(void * v, va_list args);
// cb to delete (or unref) a thing
typedef void (* generic_pool_delete_thing_fn)(void * v, void * const thing);
// can we reuse this thing for this get request?
// -1  => No
//  0  => Yes
// +ve => Choose thing with lowest +ve score from those in pool (0 is simple case)
typedef int (* generic_pool_try_reuse_thing_fn)(void * v, void * dfb, va_list args);

// cb called when pool deleted or on new_pool failure - takes the same v as alloc
typedef void (* generic_pool_on_delete_fn)(void * const v);

typedef struct generic_pool_callback_fns_s {
    generic_pool_alloc_thing_fn     alloc_thing_fn;
    generic_pool_delete_thing_fn    delete_thing_fn;
    generic_pool_try_reuse_thing_fn try_reuse_thing_fn;
    generic_pool_on_delete_fn       on_delete_fn;
} generic_pool_callback_fns_t;

// Create a new pool with custom alloc & pool delete
// If pool creation fails then on_delete_fn(v) called and NULL returned
// Pool entries are not pre-allocated.
generic_pool_t * generic_pool_new(const unsigned int total_fbs_max,
                                  const generic_pool_callback_fns_t * const cb_fns,
                                  void * const v);
// Get a thing from the pool
// Allocations need not be all of the same size but no guarantees are made about
// efficient memory use if this is the case
void * generic_pool_get(generic_pool_t * const pool, ...);

// Put thing back in the pool
// Return:
//  0  OK
// -1 Fail (pool kiled)
int generic_pool_put(generic_pool_t * pool, void * thing);

// Marks the pool as dead & unrefs this reference
//   No allocs will succeed after this
//   All free fbs are unrefed
void generic_pool_kill(generic_pool_t ** const pppool);

#ifdef __cplusplus
}
#endif

#endif
