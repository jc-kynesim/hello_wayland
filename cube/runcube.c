#include "runcube.h"

#include <pthread.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include "common.h"
#include <wayout.h>

struct runcube_env_s {
    atomic_int kill;
    wo_window_t * wowin;
    wo_rect_t pos;
    unsigned int run_no;
    bool thread_ok;
    pthread_t thread_id;
};

void cube_run(runcube_env_t * const rce, const struct egl * const egl)
{
    egl->draw(rce->run_no++);
    eglSwapBuffers(egl->display, egl->surface);

    usleep(20000);
}

static void *
cube_thread(void * v)
{
    runcube_env_t * const rce = v;
    struct egl *egl;
    wo_surface_t * wsurf = wo_make_surface_z(rce->wowin, NULL, 30);

    egl = init_cube_smooth(wo_env_display(wo_window_env(rce->wowin)),
                           wo_surface_egl_window_create(wsurf, rce->pos), rce->pos.w, rce->pos.h, 0);

    while (!atomic_load(&rce->kill)) {
        cube_run(rce, egl);
    }

    destroy_cube_smooth(egl);
    wo_surface_unref(&wsurf);

    return NULL;
}

runcube_env_t *
runcube_way_start(wo_window_t * const wowin, const wo_rect_t * pos)
{
    runcube_env_t * rce = calloc(1, sizeof(*rce));

    if (rce == NULL)
        return NULL;

    rce->wowin = wo_window_ref(wowin);
    rce->pos = *pos;
    if (pthread_create(&rce->thread_id, NULL, cube_thread, rce) != 0)
        goto fail;
    rce->thread_ok = true;

    return rce;

fail:
    runcube_way_stop(&rce);
    return NULL;
}

void
runcube_way_stop(runcube_env_t ** const ppRce)
{
    runcube_env_t * const rce = *ppRce;
    if (rce == NULL)
        return;
    *ppRce = NULL;

    if (rce->thread_ok) {
        atomic_store(&rce->kill, 1);
        pthread_join(rce->thread_id, NULL);
    }
    wo_window_unref(&rce->wowin);

    free(rce);
}

