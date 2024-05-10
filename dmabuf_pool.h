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
