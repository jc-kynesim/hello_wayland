#include "runticker.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/eventfd.h>

#include <wayout.h>

#include "ticker.h"

struct runticker_env_s {
    atomic_int kill;
    wo_window_t * wowin;
    ticker_env_t *te;
    char *text;
    const char *cchar;
    int prod_fd;
    bool thread_running;
    pthread_t thread_id;
};

static int
next_char_cb(void *v)
{
    runticker_env_t *const dfte = v;

    if (*dfte->cchar == 0)
        dfte->cchar = dfte->text;
    return *dfte->cchar++;
}

static void
do_prod(void *v)
{
    static const uint64_t one = 1;
    runticker_env_t *const dfte = v;
    write(dfte->prod_fd, &one, sizeof(one));
}

static void *
runticker_thread(void * v)
{
    runticker_env_t * const dfte = v;

    while (!atomic_load(&dfte->kill)) {
        ticker_run(dfte->te);
        usleep(20000);
    }

    return NULL;
}

runticker_env_t *
runticker_start(wo_window_t * const wowin,
                unsigned int x, unsigned int y, unsigned int w, unsigned int h,
                const char * const text,
                const char * const fontfile)
{
    runticker_env_t *dfte = calloc(1, sizeof(*dfte));

    if (dfte == NULL)
        return NULL;

    dfte->prod_fd = -1;
    dfte->text  = strdup(text);
    dfte->cchar = dfte->text;
    dfte->wowin = wo_window_ref(wowin);

    if ((dfte->te = ticker_new(wowin, (wo_rect_t){x, y, w, h}, wo_window_size(wowin))) == NULL) {
        fprintf(stderr, "Failed to create ticker\n");
        goto fail;
    }

    if (ticker_set_face(dfte->te, fontfile) != 0) {
        fprintf(stderr, "Failed to set face\n");
        goto fail;
    }

    ticker_next_char_cb_set(dfte->te, next_char_cb, dfte);

    if ((dfte->prod_fd = eventfd(0, 0)) == -1) {
        fprintf(stderr, "Failed to get event fd\n");
        goto fail;
    }
    ticker_commit_cb_set(dfte->te, do_prod, dfte);

    if (ticker_init(dfte->te) != 0) {
        fprintf(stderr, "Failed to init ticker\n");
        goto fail;
    }

    if (pthread_create(&dfte->thread_id, NULL, runticker_thread, dfte) != 0) {
        fprintf(stderr, "Failed to create thread\n");
        goto fail;
    }
    dfte->thread_running = true;

    return dfte;

fail:
    runticker_stop(&dfte);
    return NULL;
}

void
runticker_stop(runticker_env_t ** const ppDfte)
{
    runticker_env_t * const dfte = *ppDfte;
    if (dfte == NULL)
        return;
    *ppDfte = NULL;

    if (dfte->thread_running) {
        atomic_store(&dfte->kill, 1);
        do_prod(dfte);
        pthread_join(dfte->thread_id, NULL);
    }

    ticker_delete(&dfte->te);
    if (dfte->prod_fd != -1)
        close(dfte->prod_fd);
    wo_window_unref(&dfte->wowin);
    free(dfte->text);
    free(dfte);
}

