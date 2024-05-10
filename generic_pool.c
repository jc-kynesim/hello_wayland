#include "generic_pool.h"

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

//----------------------------------------------------------------------------
//
// Pool fns

typedef struct generic_fb_slot_s {
    void * fb;
    struct generic_fb_slot_s * next;
    struct generic_fb_slot_s * prev;
} generic_fb_slot_t;

typedef struct generic_fb_list_s {
    generic_fb_slot_t * head;      // Double linked list of free FBs; LRU @ head
    generic_fb_slot_t * tail;
    generic_fb_slot_t * unused;    // Single linked list of unused slots
} generic_fb_list_t;

struct generic_pool_s {
    atomic_int ref_count;       // 0 == 1 ref for ease of init
    bool dead;                  // Pool killed - never alloc again

    unsigned int fb_count;      // FBs allocated (not free count)
    unsigned int fb_max;        // Max FBs to allocate

    generic_pool_callback_fns_t callback_fns;
    void * callback_v;

    pthread_mutex_t lock;

    generic_fb_list_t free_fbs;    // Free FB list header
    generic_fb_slot_t * slots;     // [fb_max]
};

static void
fb_list_add_tail(generic_fb_list_t * const fbl, void * const dfb)
{
    generic_fb_slot_t * const slot = fbl->unused;

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

static void *
fb_list_extract(generic_fb_list_t * const fbl, generic_fb_slot_t * const slot)
{
    void * dfb;

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

static void *
fb_list_extract_head(generic_fb_list_t * const fbl)
{
    return fb_list_extract(fbl, fbl->head);
}

static void
pool_free_pool(generic_pool_t * const pool)
{
    struct generic_h * dfb;
    pthread_mutex_lock(&pool->lock);
    while ((dfb = fb_list_extract_head(&pool->free_fbs)) != NULL) {
        --pool->fb_count;
        pthread_mutex_unlock(&pool->lock);
        pool->callback_fns.delete_thing_fn(pool->callback_v, dfb);
        pthread_mutex_lock(&pool->lock);
    }
    pthread_mutex_unlock(&pool->lock);
}

static void
pool_free(generic_pool_t * const pool)
{
    void *const v = pool->callback_v;
    const generic_pool_on_delete_fn on_delete_fn = pool->callback_fns.on_delete_fn;
    pool_free_pool(pool);
    free(pool->slots);
    pthread_mutex_destroy(&pool->lock);
    free(pool);

    on_delete_fn(v);
}

void
generic_pool_unref(generic_pool_t ** const pppool)
{
    generic_pool_t * const pool = *pppool;
    int n;

    if (pool == NULL)
        return;
    *pppool = NULL;

    n = atomic_fetch_sub(&pool->ref_count, 1);
    assert(n >= 0);
    if (n == 0)
        pool_free(pool);
}

generic_pool_t *
generic_pool_ref(generic_pool_t * const pool)
{
    atomic_fetch_add(&pool->ref_count, 1);
    return pool;
}

generic_pool_t *
generic_pool_new(const unsigned int total_fbs_max,
                 const generic_pool_callback_fns_t * const cb_fns,
                 void * const v)
{
    generic_pool_t * const pool = calloc(1, sizeof(*pool));
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

int
generic_pool_put(generic_pool_t * const pool, void * dfb)
{
    int rv = -1;
    pthread_mutex_lock(&pool->lock);
    if (!pool->dead) {
        fb_list_add_tail(&pool->free_fbs, dfb);
        rv = 0;
    }
    pthread_mutex_unlock(&pool->lock);
    return rv;
}

void *
generic_pool_get(generic_pool_t * const pool, ...)
{
    struct generic_h * dfb;
    generic_fb_slot_t * slot;
    generic_fb_slot_t * best_slot = NULL;
    int best_score = INT_MAX;
    va_list args;

    va_start(args, pool);

    pthread_mutex_lock(&pool->lock);

    // If pool killed then _fb_new must fail
    if (pool->dead)
        goto fail_unlock;

    slot = pool->free_fbs.head;
    while (slot != NULL) {
        va_list a2;
        int score;

        va_copy(a2, args);
        score = pool->callback_fns.try_reuse_thing_fn(pool->callback_v, slot->fb, a2);
        va_end(a2);

        if (score < best_score) {
            best_score = score;
            best_slot = slot;
            if (score == 0)
                break;
        }

        slot = slot->next;
    }

    if (best_slot != NULL) {
        dfb = best_slot->fb;
        fb_list_extract(&pool->free_fbs, best_slot);
        pthread_mutex_unlock(&pool->lock);
        goto found;
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

    pool->callback_fns.delete_thing_fn(pool->callback_v, dfb);

    if ((dfb = pool->callback_fns.alloc_thing_fn(pool->callback_v, args)) == NULL) {
        pthread_mutex_lock(&pool->lock);
        --pool->fb_count;
        goto fail_unlock;
    }

found:
    va_end(args);
    return dfb;

fail_unlock:
    va_end(args);
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

// Mark pool as dead (i.e. no new allocs) and unref it
// Simple unref will also work but this reclaims storage faster
// Actual pool structure will persist until all referencing fbs are deleted too
void
generic_pool_kill(generic_pool_t ** const pppool)
{
    generic_pool_t * pool = *pppool;

    if (pool == NULL)
        return;
    *pppool = NULL;

    pool->dead = true;
    pool_free_pool(pool);

    generic_pool_unref(&pool);
}

