#include "dmabuf_pool.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dmabuf_alloc.h"

//----------------------------------------------------------------------------
//
// Pool fns

typedef struct dmabuf_fb_slot_s {
    struct dmabuf_h * fb;
    struct dmabuf_fb_slot_s * next;
    struct dmabuf_fb_slot_s * prev;
} dmabuf_fb_slot_t;

typedef struct dmabuf_fb_list_s {
    dmabuf_fb_slot_t * head;      // Double linked list of free FBs; LRU @ head
    dmabuf_fb_slot_t * tail;
    dmabuf_fb_slot_t * unused;    // Single linked list of unused slots
} dmabuf_fb_list_t;

struct dmabuf_pool_s {
    atomic_int ref_count;       // 0 == 1 ref for ease of init
    bool dead;                  // Pool killed - never alloc again

    unsigned int fb_count;      // FBs allocated (not free count)
    unsigned int fb_max;        // Max FBs to allocate

    dmabuf_pool_callback_fns_t callback_fns;
    void * callback_v;

    pthread_mutex_t lock;

    dmabuf_fb_list_t free_fbs;    // Free FB list header
    dmabuf_fb_slot_t * slots;     // [fb_max]
};

static void
fb_list_add_tail(dmabuf_fb_list_t * const fbl, struct dmabuf_h * const dfb)
{
    dmabuf_fb_slot_t * const slot = fbl->unused;

    assert(slot != NULL);
    fbl->unused = slot->next;
    slot->fb = dfb;
    slot->next = NULL;

    if (fbl->tail == NULL)
        fbl->head = slot;
    else
        fbl->tail->next = slot;
    slot->prev = fbl->tail;
    fbl->tail = slot;
}

static struct dmabuf_h *
fb_list_extract(dmabuf_fb_list_t * const fbl, dmabuf_fb_slot_t * const slot)
{
    struct dmabuf_h * dfb;

    if (slot == NULL)
        return NULL;

    if (slot->prev == NULL)
        fbl->head = slot->next;
    else
        slot->prev->next = slot->next;

    if (slot->next == NULL)
        fbl->tail = slot->prev;
    else
        slot->next->prev = slot->prev;

    dfb = slot->fb;
    slot->fb = NULL;
    slot->next = fbl->unused;
    slot->prev = NULL;
    fbl->unused = slot;
    return dfb;
}

static struct dmabuf_h *
fb_list_extract_head(dmabuf_fb_list_t * const fbl)
{
    return fb_list_extract(fbl, fbl->head);
}

static void
pool_free_pool(dmabuf_pool_t * const pool)
{
    struct dmabuf_h * dfb;
    pthread_mutex_lock(&pool->lock);
    while ((dfb = fb_list_extract_head(&pool->free_fbs)) != NULL) {
        --pool->fb_count;
        pthread_mutex_unlock(&pool->lock);
        dmabuf_unref(&dfb);
        pthread_mutex_lock(&pool->lock);
    }
    pthread_mutex_unlock(&pool->lock);
}

static void
pool_free(dmabuf_pool_t * const pool)
{
    void *const v = pool->callback_v;
    const dmabuf_pool_on_delete_fn on_delete_fn = pool->callback_fns.on_delete_fn;
    pool_free_pool(pool);
    free(pool->slots);
    pthread_mutex_destroy(&pool->lock);
    free(pool);

    on_delete_fn(v);
}

void
dmabuf_pool_unref(dmabuf_pool_t ** const pppool)
{
    dmabuf_pool_t * const pool = *pppool;
    int n;

    if (pool == NULL)
        return;
    *pppool = NULL;

    n = atomic_fetch_sub(&pool->ref_count, 1);
    assert(n >= 0);
    if (n == 0)
        pool_free(pool);
}

dmabuf_pool_t *
dmabuf_pool_ref(dmabuf_pool_t * const pool)
{
    atomic_fetch_add(&pool->ref_count, 1);
    return pool;
}

dmabuf_pool_t *
dmabuf_pool_new_alloc(const unsigned int total_fbs_max,
                    const dmabuf_pool_callback_fns_t * const cb_fns,
                    void * const v)
{
    dmabuf_pool_t * const pool = calloc(1, sizeof(*pool));
    unsigned int i;

    if (pool == NULL)
        goto fail0;
    if ((pool->slots = calloc(total_fbs_max, sizeof(*pool->slots))) == NULL)
        goto fail1;

    pool->fb_max = total_fbs_max;
    pool->callback_fns = *cb_fns;
    pool->callback_v = v;

    for (i = 1; i != total_fbs_max; ++i)
        pool->slots[i - 1].next = pool->slots + i;
    pool->free_fbs.unused = pool->slots + 0;

    pthread_mutex_init(&pool->lock, NULL);

    return pool;

fail1:
    free(pool);
fail0:
    cb_fns->on_delete_fn(v);
    fprintf(stderr, "Failed pool env alloc\n");
    return NULL;
}

static int
pool_fb_pre_delete_cb(struct dmabuf_h * dfb, void * v)
{
    dmabuf_pool_t * pool = v;

    // Ensure we cannot end up in a delete loop
    dmabuf_predel_cb_unset(dfb);

    // If dead set then might as well delete now
    // It should all work without this shortcut but this reclaims
    // storage quicker
    if (pool->dead) {
        dmabuf_pool_unref(&pool);
        return 0;
    }

    dmabuf_ref(dfb);  // Restore ref

    pthread_mutex_lock(&pool->lock);
    fb_list_add_tail(&pool->free_fbs, dfb);
    pthread_mutex_unlock(&pool->lock);

    // May cause suicide & recursion on fb delete, but that should be OK as
    // the 1 we return here should cause simple exit of fb delete
    dmabuf_pool_unref(&pool);
    return 1;  // Stop delete
}

struct dmabuf_h *
dmabuf_pool_fb_new(dmabuf_pool_t * const pool, size_t size)
{
    struct dmabuf_h * dfb;
    dmabuf_fb_slot_t * slot;

    pthread_mutex_lock(&pool->lock);

    // If pool killed then _fb_new must fail
    if (pool->dead)
        goto fail_unlock;

    slot = pool->free_fbs.head;
    while (slot != NULL) {
        dfb = slot->fb;
        if (pool->callback_fns.try_reuse_fn(dfb, size)) {
            fb_list_extract(&pool->free_fbs, slot);
            pthread_mutex_unlock(&pool->lock);
            goto found;
        }
        slot = slot->next;
    }
    // Nothing reusable
    dfb = NULL;

    // Simply allocate new buffers until we hit fb_max then free LRU
    // first. If nothing to free then fail.
    if (pool->fb_count++ >= pool->fb_max) {
        --pool->fb_count;
        if ((dfb = fb_list_extract_head(&pool->free_fbs)) == NULL)
            goto fail_unlock;
    }
    pthread_mutex_unlock(&pool->lock);

    dmabuf_unref(&dfb);  // Will free the dfb as pre-delete CB will be unset

    if ((dfb = pool->callback_fns.alloc_fn(pool->callback_v, size)) == NULL) {
        pthread_mutex_lock(&pool->lock);
        --pool->fb_count;
        goto fail_unlock;
    }

found:
    dmabuf_predel_cb_set(dfb, pool_fb_pre_delete_cb, dmabuf_pool_ref(pool));
    return dfb;

fail_unlock:
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

// Mark pool as dead (i.e. no new allocs) and unref it
// Simple unref will also work but this reclaims storage faster
// Actual pool structure will persist until all referencing fbs are deleted too
void
dmabuf_pool_kill(dmabuf_pool_t ** const pppool)
{
    dmabuf_pool_t * pool = *pppool;

    if (pool == NULL)
        return;
    *pppool = NULL;

    pool->dead = true;
    pool_free_pool(pool);

    dmabuf_pool_unref(&pool);
}

//----------------------------------------------------------------------------
//
// Dmabuf pool setup

static struct dmabuf_h *
pool_dumb_alloc_cb(void * const v, size_t size)
{
    return dmabuf_alloc(v, size);
}

static void
pool_dumb_on_delete_cb(void * const v)
{
    struct dmabufs_ctl * dbsc = v;
    dmabufs_ctl_unref(&dbsc);
}

static bool pool_try_reuse_cb(struct dmabuf_h * dfb, size_t size)
{
    return size <= dmabuf_size(dfb);
}

dmabuf_pool_t *
dmabuf_pool_new_dmabufs(struct dmabufs_ctl * dbsc, unsigned int total_fbs_max)
{
    static const dmabuf_pool_callback_fns_t fns = {
        .alloc_fn = pool_dumb_alloc_cb,
        .on_delete_fn = pool_dumb_on_delete_cb,
        .try_reuse_fn = pool_try_reuse_cb,
    };
    return dmabuf_pool_new_alloc(total_fbs_max, &fns, dmabufs_ctl_ref(dbsc));
}


