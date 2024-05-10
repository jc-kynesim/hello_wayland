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


// Somewhat safe coercion from dmabuf pool to generic pool

static inline generic_pool_t *
gp(dmabuf_pool_t * dp)
{
    return (generic_pool_t *)dp;
}

// ----------------------------------------------------------------------------
//
// Dmabuf pre-delete callback to put dmabuf back into pool

static int
dumb_predel_cb(struct dmabuf_h * dfb, void * v)
{
    generic_pool_t * pool = gp(v);
    int rv;

    // Ensure we cannot end up in a delete loop
    dmabuf_predel_cb_unset(dfb);

    rv = generic_pool_put(pool, dfb);

    // May kill the pool which can cause this dmabuf we've just put back into
    // the pool to be deleted - this is in fact safe as we've killed the callback and
    // the 1 we return here should cause simple exit of fb delete
    generic_pool_unref(&pool);

    return rv == 0 ? 1 : 0; // If pool put fails then just delete
}

// ----------------------------------------------------------------------------
//
// Generic pool callbacks

static void *
pool_dumb_alloc_cb(void * const v, va_list args)
{
    size_t size = va_arg(args, size_t);
    struct dmabuf_h * dh = dmabuf_alloc(v, size);
    return dh;
}

static void
pool_dumb_delete_cb(void * v, void * thing)
{
    struct dmabuf_h * dfb = thing;
    (void)v;
    dmabuf_unref(&dfb);
}

static int
pool_try_reuse_cb(void * v, void * dfb, va_list args)
{
    size_t size = va_arg(args, size_t);
    (void)v;
    return size <= dmabuf_size(dfb) ? 0 : -1;
}

static void
pool_dumb_on_delete_cb(void * v)
{
    struct dmabufs_ctl * dbsc = v;
    dmabufs_ctl_unref(&dbsc);
}

// ----------------------------------------------------------------------------
//
// Dmabuf pool calls
// Shim the generic functions

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

struct dmabuf_h *
dmabuf_pool_fb_new(dmabuf_pool_t * const pool, size_t size)
{
    struct dmabuf_h * const dh = generic_pool_get(gp(pool), size);
    if (dh == NULL)
        return NULL;

    dmabuf_predel_cb_set(dh, dumb_predel_cb, dmabuf_pool_ref(pool));
    return dh;
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


