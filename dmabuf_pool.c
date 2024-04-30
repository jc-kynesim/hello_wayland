#include "dmabuf_pool.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dmabuf_alloc.h"
#include "generic_pool.h"

// Shim the generic functions

static inline generic_pool_t *
gp(dmabuf_pool_t * dp)
{
	return (generic_pool_t *)dp;
}

void
dmabuf_pool_unref(dmabuf_pool_t ** const pppool)
{
    generic_pool_t * pool = gp(*pppool);
    *pppool = NULL;
	generic_pool_unref(&pool);
}

dmabuf_pool_t *
dmabuf_pool_ref(dmabuf_pool_t * const pool)
{
	generic_pool_ref(gp(pool));
    return pool;
}

struct dmabuf_h *
dmabuf_pool_fb_new(dmabuf_pool_t * const pool, size_t size)
{
	return generic_pool_get(gp(pool), size);
}

// Mark pool as dead (i.e. no new allocs) and unref it
// Simple unref will also work but this reclaims storage faster
// Actual pool structure will persist until all referencing fbs are deleted too
void
dmabuf_pool_kill(dmabuf_pool_t ** const pppool)
{
    generic_pool_t * pool = gp(*pppool);
    *pppool = NULL;
	generic_pool_kill(&pool);
}

//----------------------------------------------------------------------------
//
// Dmabuf pool setup

static int
dumb_predel_cb(struct dmabuf_h * dfb, void * v)
{
    generic_pool_t * pool = v;

    // Ensure we cannot end up in a delete loop
    dmabuf_predel_cb_unset(dfb);

    // If dead set then might as well delete now
    // It should all work without this shortcut but this reclaims
    // storage quicker
    if (pool->dead) {
        generic_pool_unref(&pool);
        return 0;
    }

    dmabuf_ref(dfb);  // Restore ref

	generic_pool_put(pool, dfb);

    // May cause suicide & recursion on fb delete, but that should be OK as
    // the 1 we return here should cause simple exit of fb delete
    generic_pool_unref(&pool);
    return 1;  // Stop delete
}

static void *
pool_dumb_alloc_cb(generic_pool_t * pool, void * const v, va_list args)
{
	size_t size = va_arg(args, size_t);
    struct dmabuf_h * dh = dmabuf_alloc(v, size);
	dmabuf_predel_cb_set(dh, dumb_predel_cb, pool);
	return dh;
}

static void
pool_dumb_delete_cb(generic_pool_t * pool, void * v, void * thing)
{
	struct dmabuf_h * dfb = thing;
	(void)pool;
	(void)v;
	dmabuf_unref(&dfb);
}

static void
pool_dumb_on_delete_cb(void * v)
{
    struct dmabufs_ctl * dbsc = v;
    dmabufs_ctl_unref(&dbsc);
}

static int pool_try_reuse_cb(generic_pool_t * pool, void * v, void * dfb, va_list args)
{
	size_t size = va_arg(args, size_t);
	(void)v;
	(void)pool;
    return size <= dmabuf_size(dfb) ? 0 : -1;
}

dmabuf_pool_t *
dmabuf_pool_new_dmabufs(struct dmabufs_ctl * dbsc, unsigned int total_fbs_max)
{
    static const generic_pool_callback_fns_t fns = {
        .alloc_thing_fn = pool_dumb_alloc_cb,
		.delete_thing_fn = pool_dumb_delete_cb, 
        .try_reuse_thing_fn = pool_try_reuse_cb,
        .on_delete_fn = pool_dumb_on_delete_cb,
    };
    return (dmabuf_pool_t *)generic_pool_new(total_fbs_max, &fns, dmabufs_ctl_ref(dbsc));
}


