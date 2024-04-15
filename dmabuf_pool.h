#ifndef _DMABUF_POOL_H
#define _DMABUF_POOL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dmabuf_pool_s;
typedef struct dmabuf_pool_s dmabuf_pool_t;

struct dmabuf_h;
struct dmabufs_ctl;

// fb pool

void dmabuf_pool_unref(dmabuf_pool_t ** const pppool);
dmabuf_pool_t * dmabuf_pool_ref(dmabuf_pool_t * const pool);

// cb to allocate a new pool fb
typedef struct dmabuf_h * (* dmabuf_pool_alloc_fn)(void * const v, size_t size);
// cb called when pool deleted or on new_pool failure - takes the same v as alloc
typedef void (* dmabuf_pool_on_delete_fn)(void * const v);
typedef bool (* dmabuf_pool_try_reuse_fn)(struct dmabuf_h * dfb, size_t size);

typedef struct dmabuf_pool_callback_fns_s {
    dmabuf_pool_alloc_fn alloc_fn;
    dmabuf_pool_on_delete_fn on_delete_fn;
    dmabuf_pool_try_reuse_fn try_reuse_fn;
} dmabuf_pool_callback_fns_t;

// Create a new pool with custom alloc & pool delete
// If pool creation fails then on_delete_fn(v) called and NULL returned
// Pool entries are not pre-allocated.
dmabuf_pool_t * dmabuf_pool_new_alloc(const unsigned int total_fbs_max,
                                  const dmabuf_pool_callback_fns_t * const cb_fns,
                                  void * const v);
// Allocate a fb from the pool
// Allocations need not be all of the same size but no guarantees are made about
// efficient memory use if this is the case
struct dmabuf_h * dmabuf_pool_fb_new(dmabuf_pool_t * const pool, size_t size);
// Marks the pool as dead & unrefs this reference
//   No allocs will succeed after this
//   All free fbs are unrefed
void dmabuf_pool_kill(dmabuf_pool_t ** const pppool);

dmabuf_pool_t * dmabuf_pool_new_dmabufs(struct dmabufs_ctl * dbsc, unsigned int total_fbs_max);

#ifdef __cplusplus
}
#endif

#endif
