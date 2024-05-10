#include "fb_pool.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "wayout.h"
#include "generic_pool.h"


// Somewhat safe coercion from fb pool to generic pool

static inline generic_pool_t *
gp(fb_pool_t * dp)
{
    return (generic_pool_t *)dp;
}

// ----------------------------------------------------------------------------
//
// fb pre-delete callback to put fb back into pool

static int
fb_predel_cb(wo_fb_t * dfb, void * v)
{
    generic_pool_t * pool = gp(v);
    int rv;

    // Ensure we cannot end up in a delete loop
    wo_fb_pre_delete_unset(dfb);

    rv = generic_pool_put(pool, dfb);

    // May kill the pool which can cause this fb we've just put back into
    // the pool to be deleted - this is in fact safe as we've killed the callback and
    // the 1 we return here should cause simple exit of fb delete
    generic_pool_unref(&pool);

    return rv ==0 ? 1 : 0; // If pool put fails then just delete
}

// ----------------------------------------------------------------------------
//
// Generic pool callbacks

static void *
pool_dumb_alloc_cb(void * const v, va_list args)
{
    const uint32_t width = va_arg(args, uint32_t);
    const uint32_t height = va_arg(args, uint32_t);
    const uint32_t fmt = va_arg(args, uint32_t);
    const uint64_t mod = va_arg(args, uint64_t);

    wo_fb_t * fb = wo_make_fb(v, width, height, fmt, mod);
    return fb;
}

static void
pool_dumb_delete_cb(void * v, void * thing)
{
    wo_fb_t * dfb = thing;
    (void)v;
    wo_fb_unref(&dfb);
}

static int
pool_try_reuse_cb(void * v, void * fb, va_list args)
{
    const uint32_t width = va_arg(args, uint32_t);
    const uint32_t height = va_arg(args, uint32_t);
    const uint32_t fmt = va_arg(args, uint32_t);
    const uint64_t mod = va_arg(args, uint64_t);

    (void)v;
    int rv =  width == wo_fb_width(fb) && height == wo_fb_height(fb) && fmt == wo_fb_fmt(fb) && mod == wo_fb_mod(fb) ? 0 : -1;
    return rv;
}

static void
pool_dumb_on_delete_cb(void * v)
{
    wo_env_t * woe = v;
    wo_env_unref(&woe);
}

// ----------------------------------------------------------------------------
//
// fb pool calls
// Shim the generic functions

// Mark pool as dead (i.e. no new allocs) and unref it
// Simple unref will also work but this reclaims storage faster
// Actual pool structure will persist until all referencing fbs are deleted too
void
fb_pool_kill(fb_pool_t ** const pppool)
{
    generic_pool_t * pool = gp(*pppool);
    *pppool = NULL;
    generic_pool_kill(&pool);
}

wo_fb_t *
fb_pool_fb_new(fb_pool_t * const pool, uint32_t width, uint32_t height, uint32_t fmt, uint64_t mod)
{
    wo_fb_t * const dh = generic_pool_get(gp(pool), width, height, fmt, mod);
    if (dh == NULL)
        return NULL;

    wo_fb_pre_delete_set(dh, fb_predel_cb, fb_pool_ref(pool));
    return dh;
}

void
fb_pool_unref(fb_pool_t ** const pppool)
{
    generic_pool_t * pool = gp(*pppool);
    *pppool = NULL;
    generic_pool_unref(&pool);
}

fb_pool_t *
fb_pool_ref(fb_pool_t * const pool)
{
    generic_pool_ref(gp(pool));
    return pool;
}

fb_pool_t *
fb_pool_new_fbs(wo_env_t * const woe, unsigned int total_fbs_max)
{
    static const generic_pool_callback_fns_t fns = {
        .alloc_thing_fn = pool_dumb_alloc_cb,
        .delete_thing_fn = pool_dumb_delete_cb,
        .try_reuse_thing_fn = pool_try_reuse_cb,
        .on_delete_fn = pool_dumb_on_delete_cb,
    };
    return (fb_pool_t *)generic_pool_new(total_fbs_max, &fns, wo_env_ref(woe));
}


