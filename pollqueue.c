#include "pollqueue.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#define request_log(...) fprintf(stderr, __VA_ARGS__)

struct pollqueue;

enum polltask_state {
    POLLTASK_UNQUEUED = 0,
    POLLTASK_QUEUED,
    POLLTASK_RUNNING,
    POLLTASK_Q_KILL,
    POLLTASK_Q_DEAD,
    POLLTASK_RUN_KILL,
};

#define POLLTASK_FLAG_ONCE 1

struct polltask {
    struct polltask *next;
    struct polltask *prev;
    struct pollqueue *q;
    enum polltask_state state;

    int fd;
    short events;
    unsigned short flags;

    void (*fn)(void *v, short revents);
    void * v;

    uint64_t timeout; /* CLOCK_MONOTONIC time, 0 => never */
};

struct pollqueue {
    atomic_int ref_count;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    struct polltask *head;
    struct polltask *tail;

    struct prepost_ss {
        void (*pre)(void *v, struct pollfd *pfd);
        void (*post)(void *v, short revents);
        void *v;
    } prepost;

    bool kill;
    bool join_req;  // On thread exit do not detach
    bool no_prod;

    bool sig_seq; // Signal cond when seq incremented
    uint32_t seq;

    int prod_fd;
    struct polltask *prod_pt;
    pthread_t worker;
};

static struct polltask *
polltask_new2(struct pollqueue *const pq,
              const int fd, const short events,
              void (*const fn)(void *v, short revents),
              void *const v,
              const unsigned short flags)
{
    struct polltask *pt;

    if (!events && fd != -1)
        return NULL;

    pt = malloc(sizeof(*pt));
    if (!pt)
        return NULL;

    *pt = (struct polltask){
        .next = NULL,
        .prev = NULL,
        .q = pollqueue_ref(pq),
        .fd = fd,
        .events = events,
        .flags = flags,
        .fn = fn,
        .v = v
    };

    return pt;
}

struct polltask *
polltask_new(struct pollqueue *const pq,
             const int fd, const short events,
             void (*const fn)(void *v, short revents),
             void *const v)
{
    return polltask_new2(pq, fd, events, fn, v, 0);
}

struct polltask *polltask_new_timer(struct pollqueue *const pq,
                  void (*const fn)(void *v, short revents),
                  void *const v)
{
    return polltask_new(pq, -1, 0, fn, v);
}

int
pollqueue_callback_once(struct pollqueue *const pq,
                        void (*const fn)(void *v, short revents),
                        void *const v)
{
    struct polltask * const pt = polltask_new2(pq, -1, 0, fn, v, POLLTASK_FLAG_ONCE);
    if (pt == NULL)
        return -EINVAL;
    pollqueue_add_task(pt, 0);
    return 0;
}

static void pollqueue_rem_task(struct pollqueue *const pq, struct polltask *const pt)
{
    if (pt->prev)
        pt->prev->next = pt->next;
    else
        pq->head = pt->next;
    if (pt->next)
        pt->next->prev = pt->prev;
    else
        pq->tail = pt->prev;
    pt->next = NULL;
    pt->prev = NULL;
}

static void polltask_free(struct polltask * const pt)
{
    free(pt);
}

static void polltask_kill(struct polltask * const pt)
{
    struct pollqueue * pq = pt->q;
    polltask_free(pt);
    pollqueue_unref(&pq);
}

static void polltask_dead(struct polltask * const pt)
{
    pt->state = POLLTASK_Q_DEAD;
    pthread_cond_broadcast(&pt->q->cond);
}

static int pollqueue_prod(const struct pollqueue *const pq)
{
    static const uint64_t one = 1;
    return write(pq->prod_fd, &one, sizeof(one));
}

void polltask_delete(struct polltask **const ppt)
{
    struct polltask *const pt = *ppt;
    struct pollqueue * pq;
    enum polltask_state state;
    bool prodme;
    bool inthread;

    if (!pt)
        return;

    pq = pt->q;
    inthread = pthread_equal(pthread_self(), pq->worker);

    pthread_mutex_lock(&pq->lock);
    state = pt->state;
    pt->state = inthread ? POLLTASK_RUN_KILL : POLLTASK_Q_KILL;
    prodme = !pq->no_prod;
    pthread_mutex_unlock(&pq->lock);

    switch (state) {
        case POLLTASK_UNQUEUED:
            *ppt = NULL;
            polltask_kill(pt);
            break;

        case POLLTASK_QUEUED:
        case POLLTASK_RUNNING:
        {
            int rv = 0;

            if (inthread) {
                // We are in worker thread - kill in main loop to avoid confusion or deadlock
                *ppt = NULL;
                break;
            }

            if (prodme)
                pollqueue_prod(pq);

            pthread_mutex_lock(&pq->lock);
            while (rv == 0 && pt->state != POLLTASK_Q_DEAD)
                rv = pthread_cond_wait(&pq->cond, &pq->lock);
            pthread_mutex_unlock(&pq->lock);

            // Leave zapping the ref until we have DQed the PT as might well be
            // legitimately used in it
            *ppt = NULL;
            polltask_kill(pt);
            break;
        }
        default:
            request_log("%s: Unexpected task state: %d\n", __func__, state);
            *ppt = NULL;
            break;
    }
}

static uint64_t pollqueue_now(int timeout)
{
    struct timespec now;
    uint64_t now_ms;

    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return 0;
    now_ms = (now.tv_nsec / 1000000) + (uint64_t)now.tv_sec * 1000 + timeout;
    return now_ms ? now_ms : (uint64_t)1;
}

void pollqueue_add_task(struct polltask *const pt, const int timeout)
{
    bool prodme = false;
    struct pollqueue * const pq = pt->q;
    const uint64_t timeout_time = timeout < 0 ? 0 : pollqueue_now(timeout);

    pthread_mutex_lock(&pq->lock);
    if (pt->state == POLLTASK_UNQUEUED || pt->state == POLLTASK_RUNNING) {
        if (pq->tail)
            pq->tail->next = pt;
        else
            pq->head = pt;
        pt->prev = pq->tail;
        pt->next = NULL;
        pt->state = POLLTASK_QUEUED;
        pt->timeout = timeout_time;
        pq->tail = pt;
        prodme = !pq->no_prod;
    }
    pthread_mutex_unlock(&pq->lock);
    if (prodme)
        pollqueue_prod(pq);
}

static void *poll_thread(void *v)
{
    struct pollqueue *const pq = v;

    pthread_mutex_lock(&pq->lock);
    do {
        struct pollfd a[POLLQUEUE_MAX_QUEUE];
        unsigned int i, j;
        unsigned int nall = 0;
        unsigned int npoll = 0;
        struct polltask *pt;
        struct polltask *pt_next;
        struct prepost_ss prepost;
        uint64_t now = pollqueue_now(0);
        int timeout = -1;
        int rv;

        for (pt = pq->head; pt; pt = pt_next) {
            int64_t t;

            pt_next = pt->next;

            if (pt->state == POLLTASK_Q_KILL) {
                pollqueue_rem_task(pq, pt);
                polltask_dead(pt);
                continue;
            }
            if (pt->state == POLLTASK_RUN_KILL) {
                pollqueue_rem_task(pq, pt);
                polltask_kill(pt);
                continue;
            }

            if (pt->fd != -1) {
                assert(npoll < POLLQUEUE_MAX_QUEUE - 1); // Allow for pre/post
                a[npoll++] = (struct pollfd){
                    .fd = pt->fd,
                    .events = pt->events
                };
            }

            t = (int64_t)(pt->timeout - now);
            if (pt->timeout && t < INT_MAX &&
                (timeout < 0 || (int)t < timeout))
                timeout = (t < 0) ? 0 : (int)t;
            ++nall;
        }
        prepost = pq->prepost;
        pthread_mutex_unlock(&pq->lock);

        a[npoll] = (struct pollfd){.fd=-1, .events=0, .revents=0};
        if (prepost.pre)
            prepost.pre(prepost.v, a + npoll);

        while ((rv = poll(a, npoll + (a[npoll].fd != -1), timeout)) == -1)
        {
            if (errno != EINTR)
                break;
        }

        if (prepost.post)
            prepost.post(prepost.v, a[npoll].revents);

        if (rv == -1) {
            request_log("Poll error: %s\n", strerror(errno));
            goto fail_unlocked;
        }

        now = pollqueue_now(0);

        pthread_mutex_lock(&pq->lock);
        /* Prodding in this loop is pointless and might lead to
         * infinite looping
        */
        pq->no_prod = true;

        // Sync for prepost changes
        ++pq->seq;
        if (pq->sig_seq) {
            pq->sig_seq = false;
            pthread_cond_broadcast(&pq->cond);
        }

        for (i = 0, j = 0, pt = pq->head; i < nall; ++i, pt = pt_next) {
            const short r = pt->fd == -1 ? 0 : a[j++].revents;
            pt_next = pt->next;

            if (pt->state != POLLTASK_QUEUED)
                continue;

            /* Pending? */
            if (r || (pt->timeout && (int64_t)(now - pt->timeout) >= 0)) {
                pollqueue_rem_task(pq, pt);
                pt->state = POLLTASK_RUNNING;
                pthread_mutex_unlock(&pq->lock);

                /* This can add new entries to the Q but as
                 * those are added to the tail our existing
                 * chain remains intact
                */
                pt->fn(pt->v, r);

                pthread_mutex_lock(&pq->lock);
                if (pt->state == POLLTASK_Q_KILL)
                    polltask_dead(pt);
                else if (pt->state == POLLTASK_RUN_KILL ||
                    (pt->flags & POLLTASK_FLAG_ONCE) != 0)
                    polltask_kill(pt);
                else if (pt->state == POLLTASK_RUNNING)
                    pt->state = POLLTASK_UNQUEUED;
            }
        }
        pq->no_prod = false;

    } while (!pq->kill);

    pthread_mutex_unlock(&pq->lock);
fail_unlocked:

    polltask_free(pq->prod_pt);
    pthread_cond_destroy(&pq->cond);
    pthread_mutex_destroy(&pq->lock);
    close(pq->prod_fd);
    if (!pq->join_req)
        pthread_detach(pthread_self());
    free(pq);

    return NULL;
}

static void prod_fn(void *v, short revents)
{
    struct pollqueue *const pq = v;
    char buf[8];
    if (revents)
        read(pq->prod_fd, buf, 8);
    if (!pq->kill)
        pollqueue_add_task(pq->prod_pt, -1);
}

struct pollqueue * pollqueue_new(void)
{
    struct pollqueue *pq = malloc(sizeof(*pq));
    if (!pq)
        return NULL;
    *pq = (struct pollqueue){
        .ref_count = ATOMIC_VAR_INIT(0),
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .head = NULL,
        .tail = NULL,
        .kill = false,
        .prod_fd = -1
    };

    pq->prod_fd = eventfd(0, EFD_NONBLOCK);
    if (pq->prod_fd == -1)
        goto fail1;
    pq->prod_pt = polltask_new(pq, pq->prod_fd, POLLIN, prod_fn, pq);
    if (!pq->prod_pt)
        goto fail2;
    pollqueue_add_task(pq->prod_pt, -1);
    if (pthread_create(&pq->worker, NULL, poll_thread, pq))
        goto fail3;
    // Reset ref count which will have been inced by the add_task
    atomic_store(&pq->ref_count, 0);
    return pq;

fail3:
    polltask_free(pq->prod_pt);
fail2:
    close(pq->prod_fd);
fail1:
    free(pq);
    return NULL;
}

static void pollqueue_free(struct pollqueue *const pq)
{
    const pthread_t worker = pq->worker;

    if (pthread_equal(worker, pthread_self())) {
        pq->kill = true;
        if (!pq->no_prod)
            pollqueue_prod(pq);
    }
    else
    {
        pthread_mutex_lock(&pq->lock);
        pq->kill = true;
        // Must prod inside lock here as otherwise there is a potential race
        // where the worker terminates and pq is freed before the prod
        if (!pq->no_prod)
            pollqueue_prod(pq);
        pthread_mutex_unlock(&pq->lock);
    }
}

struct pollqueue * pollqueue_ref(struct pollqueue *const pq)
{
    atomic_fetch_add(&pq->ref_count, 1);
    return pq;
}

void pollqueue_unref(struct pollqueue **const ppq)
{
    struct pollqueue * const pq = *ppq;

    if (!pq)
        return;
    *ppq = NULL;

    if (atomic_fetch_sub(&pq->ref_count, 1) != 0)
        return;

    pollqueue_free(pq);
}

void pollqueue_finish(struct pollqueue **const ppq)
{
    struct pollqueue * pq = *ppq;
    pthread_t worker;

    if (!pq)
        return;

    pq->join_req = true;
    worker = pq->worker;

    pollqueue_unref(&pq);

    pthread_join(worker, NULL);

    // Delay zapping the ref until after the join as it is legit for the
    // remaining active polltasks to use it.
    *ppq = NULL;
}

void pollqueue_set_pre_post(struct pollqueue *const pq,
                            void (*fn_pre)(void *v, struct pollfd *pfd),
                            void (*fn_post)(void *v, short revents),
                            void *v)
{
    bool no_prod;

    pthread_mutex_lock(&pq->lock);
    pq->prepost.pre = fn_pre;
    pq->prepost.post = fn_post;
    pq->prepost.v = v;
    no_prod = pq->no_prod;

    if (!no_prod) {
        const uint32_t seq = pq->seq;
        int rv = 0;

        pollqueue_prod(pq);

        pq->sig_seq = true;
        while (rv == 0 && pq->seq == seq)
            rv = pthread_cond_wait(&pq->cond, &pq->lock);
    }
    pthread_mutex_unlock(&pq->lock);
}

