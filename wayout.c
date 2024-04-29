#include "wayout.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <libdrm/drm_fourcc.h>

#include "dmabuf_alloc.h"
#include "pollqueue.h"

#include <wayland-client-protocol.h>
#include <wayland-egl.h> // Wayland EGL MUST be included before EGL headers

// protocol headers that we build as part of the compile
#include "viewporter-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

#define LOG printf

typedef struct fmt_ent_s {
    uint32_t fmt;
    uint64_t mod;
} fmt_ent_t;

typedef struct fmt_list_s {
    fmt_ent_t *fmts;
    unsigned int size;
    unsigned int len;
} fmt_list_t;

typedef struct subplane_s {
    struct wl_surface * surface;
    struct wl_subsurface * subsurface;
    struct wp_viewport * viewport;
} subplane_t;

#define WO_FB_PLANES 4

struct wo_fb_s {
    atomic_int ref_count;

    wo_env_t * woe;
    unsigned int obj_count;
    struct dmabuf_h * dh[WO_FB_PLANES];
    uint32_t fmt;
    uint32_t width, height;
    unsigned int plane_count;
    size_t stride[WO_FB_PLANES];
    size_t offset[WO_FB_PLANES];
    size_t obj_no[WO_FB_PLANES];
    uint64_t mod;
    wo_rect_t crop;

    struct wl_buffer *way_buf;

    wo_fb_on_delete_fn on_delete_fn;
    void * on_delete_v;
    wo_fb_pre_delete_fn pre_delete_fn;
    void * pre_delete_v;
    wo_fb_on_release_fn on_release_fn;
    void * on_release_v;
};


struct wo_surface_s {
    atomic_int ref_count;
    struct wo_surface_s * next;
    struct wo_surface_s * prev;
    bool commit0_done;

    wo_env_t * woe;
    wo_window_t * wowin;
    wo_surface_t * parent;  // No ref - just a pointer (currently)
    wo_fb_t * wofb;         // Currently attached fb
    unsigned int zpos;

    wo_rect_t src_pos;      // Last viewport src set
    wo_rect_t dst_pos;      // Viewport dst size & subsurface pos
    wo_surface_fns_t fns;
    struct wl_egl_window * egl_window;

    wo_surface_win_resize_fn win_resize_fn;
    void * win_resize_v;

    subplane_t s;
};

struct wo_window_s {
    atomic_int ref_count;
    wo_env_t * woe;

    unsigned int req_w;
    unsigned int req_h;

    wo_rect_t pos;
    bool fullscreen;
    const char * title;
    wo_surface_t * wos;
    struct xdg_surface *wm_surface;
    struct xdg_toplevel *wm_toplevel;

    bool sync_wait;
    sem_t sync_sem;
    pthread_mutex_t surface_lock;
    struct wo_surface_s * surface_chain;
};

struct wo_env_s {
    atomic_int ref_count;

    struct wl_display *w_display;

    struct pollqueue *pq;
    struct dmabufs_ctl *dbsc;

    // Bound wayland extensions
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct zwp_linux_dmabuf_v1 *linux_dmabuf_v1;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct wp_viewporter *viewporter;
    struct xdg_wm_base *wm_base;
    struct wp_single_pixel_buffer_manager_v1 * single_pixel_manager;

    // Dmabuf fmts
    fmt_list_t fmt_list;

    struct wl_region * region_all;
};

// ---------------------------------------------------------------------------
//
// Format list creation & lookup
// Currently only used for dmabuf

// Remove any params from a modifier
static inline uint64_t
canon_mod(const uint64_t m)
{
    return fourcc_mod_is_vendor(m, BROADCOM) ? fourcc_mod_broadcom_mod(m) : m;
}

static int
fmt_list_add(fmt_list_t *const fl, uint32_t fmt, uint64_t mod)
{
    if (fl->len >= fl->size) {
        unsigned int n = fl->len == 0 ? 64 : fl->len * 2;
        fmt_ent_t *t = realloc(fl->fmts, n * sizeof(*t));
        if (t == NULL)
            return -1;
        fl->fmts = t;
        fl->size = n;
    }
    fl->fmts[fl->len++] = (fmt_ent_t) {
        .fmt = fmt,
        .mod = mod
    };
    return 0;
}

static int
fmt_sort_cb(const void *va, const void *vb)
{
    const fmt_ent_t *const a = va;
    const fmt_ent_t *const b = vb;
    return a->fmt < b->fmt ? -1 : a->fmt != b->fmt ? 1 :
           a->mod < b->mod ? -1 : a->mod != b->mod ? 1 : 0;
}

static void
fmt_list_sort(fmt_list_t *const fl)
{
    if (fl->len <= 1)
        return;
    qsort(fl->fmts, fl->len, sizeof(*fl->fmts), fmt_sort_cb);
}

static bool
fmt_list_find(const fmt_list_t *const fl, const uint32_t fmt, const uint64_t mod)
{
    if (fl->len == 0) {
        return false;
    }
    else {
        const fmt_ent_t x = {
            .fmt = fmt,
            .mod = mod
        };
        const fmt_ent_t *const fe =
            bsearch(&x, fl->fmts, fl->len, sizeof(x), fmt_sort_cb);
        return fe != NULL;
    }
}

static void
fmt_list_uninit(fmt_list_t *const fl)
{
    free(fl->fmts);
    fl->fmts = NULL;
    fl->size = 0;
    fl->len = 0;
}

static int
fmt_list_init(fmt_list_t *const fl, const size_t initial_size)
{
    fl->size = 0;
    fl->len = 0;
    if ((fl->fmts = malloc(initial_size * sizeof(*fl->fmts))) == NULL)
        return -1;
    fl->size = initial_size;
    return 0;
}

// ---------------------------------------------------------------------------
//
// (sub)plane helpers

static void
buffer_destroy(struct wl_buffer ** ppbuffer)
{
    struct wl_buffer * const buffer = *ppbuffer;
    if (buffer == NULL)
        return;
    *ppbuffer = NULL;
    wl_buffer_destroy(buffer);
}

static void
region_destroy(struct wl_region ** const ppregion)
{
    if (*ppregion == NULL)
        return;
    wl_region_destroy(*ppregion);
    *ppregion = NULL;
}

static void
subsurface_destroy(struct wl_subsurface ** const ppsubsurface)
{
    if (*ppsubsurface == NULL)
        return;
    wl_subsurface_destroy(*ppsubsurface);
    *ppsubsurface = NULL;
}

static void
surface_destroy(struct wl_surface ** const ppsurface)
{
    if (*ppsurface == NULL)
        return;
    wl_surface_destroy(*ppsurface);
    *ppsurface = NULL;
}

static void
viewport_destroy(struct wp_viewport ** const ppviewport)
{
    if (*ppviewport == NULL)
        return;
    wp_viewport_destroy(*ppviewport);
    *ppviewport = NULL;
}

static void
plane_destroy(subplane_t * const spl)
{
    viewport_destroy(&spl->viewport);
    subsurface_destroy(&spl->subsurface);
    surface_destroy(&spl->surface);
}

static int
plane_create(const wo_env_t * const woe, subplane_t * const plane,
             struct wl_surface * const parent,
             struct wl_surface * const above,
             const bool sync)
{
    if ((plane->surface = wl_compositor_create_surface(woe->compositor)) == NULL)
        goto fail;
    if ((plane->viewport = wp_viewporter_get_viewport(woe->viewporter, plane->surface)) == NULL)
        goto fail;
    if (parent == NULL)
        return 0;
    if ((plane->subsurface = wl_subcompositor_get_subsurface(woe->subcompositor, plane->surface, parent)) == NULL)
        goto fail;

    wl_subsurface_place_above(plane->subsurface, above);
    if (sync)
        wl_subsurface_set_sync(plane->subsurface);
    else
        wl_subsurface_set_desync(plane->subsurface);
//    wl_surface_set_input_region(plane->surface, sys->region_none);
    return 0;

fail:
    plane_destroy(plane);
    return -1;
}

// ===========================================================================
wo_fb_t *
wo_make_fb(wo_env_t * woe, uint32_t width, uint32_t height, uint32_t fmt, uint64_t mod)
{
    wo_fb_t * wofb = calloc(1, sizeof(*wofb));
    struct zwp_linux_buffer_params_v1 *params;
    unsigned int i;

    if (wofb == NULL)
        return NULL;
    wofb->woe = woe;
    wofb->fmt = fmt;
    wofb->mod = mod;
    wofb->width = width;
    wofb->height = height;
    wofb->plane_count = 1;
    wofb->stride[0] = width * 4;  // *** Proper fmt calc would be good!
    wofb->crop = (wo_rect_t){0,0,width,height};
    if ((wofb->dh[0] = dmabuf_alloc(woe->dbsc, height * wofb->stride[0])) == NULL)
        goto fail;
    wofb->obj_count = 1;

    // This should be safe to do in this thread
    if ((params = zwp_linux_dmabuf_v1_create_params(woe->linux_dmabuf_v1)) == NULL)
        goto fail;

    for (i = 0; i != wofb->plane_count; ++i) {
        zwp_linux_buffer_params_v1_add(params, dmabuf_fd(wofb->dh[0]), i,
                                       wofb->offset[i], wofb->stride[i],
                                       (unsigned int)(mod >> 32), (unsigned int)(mod & 0xFFFFFFFF));
    }

    wofb->way_buf = zwp_linux_buffer_params_v1_create_immed(params, width, height, fmt, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    if (wofb->way_buf == NULL)
        goto fail;

    return wofb;

fail:
    wo_fb_unref(&wofb);
    return NULL;
}

wo_fb_t *
wo_fb_new_dh(wo_env_t * const woe, const uint32_t w, const uint32_t h, uint32_t fmt, uint64_t mod,
             unsigned int objs, struct dmabuf_h ** dhs,
             unsigned int planes, const size_t * offsets, const size_t * strides, const unsigned int * obj_nos)
{
    struct zwp_linux_buffer_params_v1 *params;
    wo_fb_t * wofb = calloc(1, sizeof(*wofb));
    unsigned int i;

    if (wofb == NULL)
        return NULL;
    wofb->woe = woe;
    wofb->width = w;
    wofb->height = h;
    wofb->fmt = fmt;
    wofb->mod = mod;
    wofb->plane_count = planes;

    for (i = 0; i != objs; ++i)
        wofb->dh[i] = dhs[i];   // ref???

    params = zwp_linux_dmabuf_v1_create_params(woe->linux_dmabuf_v1);

    for (i = 0; i < planes; ++i) {
        wofb->offset[i] = offsets[i];
        wofb->stride[i] = strides[i];
        wofb->obj_no[i] = obj_nos[i];

        zwp_linux_buffer_params_v1_add(params, dmabuf_fd(dhs[obj_nos[i]]),
                                       i, offsets[i], strides[i],
                                       (unsigned int)(mod >> 32),
                                       (unsigned int)(mod & 0xFFFFFFFF));
    }

    wofb->way_buf = zwp_linux_buffer_params_v1_create_immed(params, w, h, fmt, 0);
    zwp_linux_buffer_params_v1_destroy(params);

    if (wofb->way_buf == NULL)
        goto fail;
    return wofb;

fail:
    wo_fb_unref(&wofb);
    return NULL;
}

wo_fb_t *
wo_fb_new_rgba_pixel(wo_env_t * const woe, const uint32_t r, const uint32_t g, const uint32_t b, const uint32_t a)
{
    wo_fb_t * wofb = calloc(1, sizeof(*wofb));

    if (wofb == NULL)
        return NULL;
    wofb->woe = woe;
    wofb->width = 1;
    wofb->height = 1;
    wofb->plane_count = 1;
    wofb->way_buf = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
        woe->single_pixel_manager, r, g, b, a);
    if (wofb->way_buf == NULL)
        goto fail;

    return wofb;

fail:
    wo_fb_unref(&wofb);
    return NULL;
}

wo_fb_t *
wo_fb_ref(wo_fb_t * wfb)
{
    if (wfb != NULL)
        atomic_fetch_add(&wfb->ref_count, 1);
    return wfb;
}

void
wo_fb_unref(wo_fb_t ** ppwfb)
{
    wo_fb_t * wofb = *ppwfb;
    unsigned int i;
    int n;

    if (wofb == NULL)
        return;

    n = atomic_fetch_sub(&wofb->ref_count, 1);
//    LOG("%s: n=%d\n", __func__, n);
    if (n != 0)
        return;

    if (!wofb->pre_delete_fn || !wofb->pre_delete_fn(wofb->pre_delete_v, wofb))
    {
        const wo_fb_on_delete_fn on_delete_fn = wofb->on_delete_fn;
        void * const on_delete_v = wofb->on_delete_v;

        buffer_destroy(&wofb->way_buf);
        for (i = 0; i != WO_FB_PLANES; ++i)
            dmabuf_unref(wofb->dh + i);
        free(wofb);

        if (on_delete_fn)
            on_delete_fn(on_delete_v);
    }
}

void
wo_fb_on_delete_set(wo_fb_t * const wofb, wo_fb_on_delete_fn const fn, void * const v)
{
    wofb->on_delete_fn = fn;
    wofb->on_delete_v = v;
}

void
wo_fb_pre_delete_set(wo_fb_t * const wofb, wo_fb_pre_delete_fn const fn, void * const v)
{
    wofb->pre_delete_fn = fn;
    wofb->pre_delete_v = v;
}

void
wo_fb_pre_delete_unset(wo_fb_t * const wofb)
{
    wofb->pre_delete_fn = (wo_fb_pre_delete_fn)0;
    wofb->pre_delete_v = NULL;
}

struct fb_fence_wait_s {
    wo_fb_t * wofb;
    struct polltask * pt;
};

static void
fb_release_fence2_cb(void * v, short revents)
{
    struct fb_fence_wait_s * ffs = v;
    wo_fb_t *wofb = ffs->wofb;
    (void)revents;

//    LOG("%s\n", __func__);

    polltask_delete(&ffs->pt);
    free(ffs);
    if (wofb->on_release_fn)
        wofb->on_release_fn(wofb->on_release_v, wofb);
    wo_fb_unref(&wofb);
}

static void
fb_release_fence_cb(void *data, struct wl_buffer *wl_buffer)
{
    wo_fb_t * const wofb = data;
    struct fb_fence_wait_s * const ffs = malloc(sizeof(*ffs));
    (void)wl_buffer;

//    LOG("%s\n", __func__);

    ffs->wofb = wofb;
    ffs->pt = polltask_new(wofb->woe->pq, dmabuf_fd(wofb->dh[0]), POLLOUT, fb_release_fence2_cb, ffs);
    pollqueue_add_task(ffs->pt, 1000);
}

static void
fb_release_no_fence_cb(void *data, struct wl_buffer *wl_buffer)
{
    wo_fb_t * wofb = data;
    (void)wl_buffer;
    if (wofb->on_release_fn)
        wofb->on_release_fn(wofb->on_release_v, wofb);
    wo_fb_unref(&wofb);
}

void
wo_fb_on_release_set(wo_fb_t * const wofb, bool wait_for_fence, wo_fb_on_release_fn const fn, void * const v)
{
    static const struct wl_buffer_listener no_fence_listener = {
        .release = fb_release_no_fence_cb
    };
    static const struct wl_buffer_listener fence_listener = {
        .release = fb_release_fence_cb
    };

    wofb->on_release_fn = fn;
    wofb->on_release_v = v;

    wl_buffer_add_listener(wofb->way_buf, wait_for_fence ? &fence_listener : &no_fence_listener, wo_fb_ref(wofb));
}

void
wo_fb_on_release_unset(wo_fb_t * const wofb)
{
    wofb->on_release_fn = (wo_fb_on_release_fn)0;
    wofb->on_release_v = NULL;
}

unsigned int
wo_fb_width(const wo_fb_t * wfb)
{
    return wfb->width;
}

unsigned int
wo_fb_height(const wo_fb_t * wfb)
{
    return wfb->height;
}

unsigned int
wo_fb_pitch(const wo_fb_t * wfb, const unsigned int plane)
{
    return plane >= wfb->plane_count ? 0 : wfb->stride[plane];
}

void *
wo_fb_data(const wo_fb_t * wfb, const unsigned int plane)
{
    return plane >= wfb->plane_count ? NULL : (void *)((uint8_t*)dmabuf_map(wfb->dh[wfb->obj_no[plane]]) + wfb->offset[plane]);
}

void
wo_fb_crop_frac_set(wo_fb_t * wfb, const wo_rect_t crop)
{
    wfb->crop = crop;
}

void
wo_fb_write_start(wo_fb_t * wfb)
{
    unsigned int i;
    for (i = 0; i != wfb->obj_count; ++i)
        dmabuf_write_start(wfb->dh[i]);
}

void
wo_fb_write_end(wo_fb_t * wfb)
{
    unsigned int i;
    for (i = 0; i != wfb->obj_count; ++i)
        dmabuf_write_end(wfb->dh[i]);
}

void
wo_fb_read_start(wo_fb_t * wfb)
{
    unsigned int i;
    for (i = 0; i != wfb->obj_count; ++i)
        dmabuf_read_start(wfb->dh[i]);
}
void
wo_fb_read_end(wo_fb_t * wfb)
{
    unsigned int i;
    for (i = 0; i != wfb->obj_count; ++i)
        dmabuf_read_end(wfb->dh[i]);
}

int
wo_surface_commit(wo_surface_t * wsurf)
{
    (void)wsurf;
    return 0;
}

struct surface_attach_fb_arg_s {
    bool detach;
    wo_surface_t * wos;
    wo_fb_t * wofb;
    wo_rect_t dst_pos;
};

static void
surface_attach_fb_free(struct surface_attach_fb_arg_s * a)
{
    wo_surface_unref(&a->wos);
    wo_fb_unref(&a->wofb);
    free(a);
}

static void
surface_attach_fb_cb(void * v, short revents)
{
    struct surface_attach_fb_arg_s * const a = v;
    wo_surface_t * const wos = a->wos;
    wo_fb_t * const wofb = a->wofb;
    // 1st time through ensure we commit everything
    bool commit_req_this = !wos->commit0_done;
    bool commit_req_parent = !wos->commit0_done && wos->parent != NULL;
    (void)revents;

    // Not the first time anymore
    wos->commit0_done = true;

    if (a->detach) {
        if (wos->wofb != NULL) {
            wl_surface_attach(wos->s.surface, NULL, 0, 0);
            wl_surface_commit(wos->s.surface);
            wo_fb_unref(&wos->wofb);
        }
    }
    else {
        const bool use_dst = (a->dst_pos.w != 0 && a->dst_pos.h != 0);
        if (wofb != NULL && wofb != wos->wofb) {
            wl_surface_attach(wos->s.surface, wofb->way_buf, 0, 0);
            wl_surface_damage_buffer(wos->s.surface, 0, 0, INT_MAX, INT_MAX);
            wo_fb_unref(&wos->wofb);
            wos->wofb = wo_fb_ref(wofb);
            commit_req_this = true;
        }
        if (wofb != NULL &&
            (wofb->crop.x != wos->src_pos.x || wofb->crop.y != wos->src_pos.y ||
             wofb->crop.w != wos->src_pos.w || wofb->crop.h != wos->src_pos.h)) {
            if (wofb->crop.w != 0 && wofb->crop.h != 0) {
                wp_viewport_set_source(wos->s.viewport,
                                       wofb->crop.x >> 8, wofb->crop.y >> 8,
                                       wofb->crop.w >> 8, wofb->crop.h >> 8);
                wos->src_pos = wofb->crop;
                commit_req_this = true;
            }
        }
        if (use_dst) {
            if (wos->dst_pos.w != a->dst_pos.w || wos->dst_pos.h != a->dst_pos.h) {
                commit_req_this = true;
                commit_req_parent = (wos->parent != NULL);
                wp_viewport_set_destination(wos->s.viewport, a->dst_pos.w, a->dst_pos.h);
            }
            if (wos->s.subsurface && (wos->dst_pos.x != a->dst_pos.x || wos->dst_pos.y != a->dst_pos.y)) {
                commit_req_parent = true;
                wl_subsurface_set_position(wos->s.subsurface, a->dst_pos.x, a->dst_pos.y);
            }
            wos->dst_pos = a->dst_pos;
        }
        if (commit_req_this)
            wl_surface_commit(wos->s.surface);
        if (commit_req_parent)
            wl_surface_commit(wos->parent->s.surface); // Need parent commit for position
    }

    surface_attach_fb_free(a);
}

int
wo_surface_attach_fb(wo_surface_t * wos, wo_fb_t * wofb, const wo_rect_t dst_pos)
{
    wo_env_t * const woe = wos->woe;
    struct surface_attach_fb_arg_s * a = calloc(1, sizeof(*a));
    int rv;

    if (a == NULL)
        return -ENOMEM;

    a->detach = (wofb == NULL);
    a->wos = wo_surface_ref(wos);
    a->wofb = wo_fb_ref(wofb);
    a->dst_pos = dst_pos;

    if ((rv = pollqueue_callback_once(woe->pq, surface_attach_fb_cb, a)) != 0) {
        surface_attach_fb_free(a);
        return rv;
    }
    return 0;
}

int
wo_surface_detach_fb(wo_surface_t * wos)
{
    return wo_surface_attach_fb(wos, NULL, (wo_rect_t){0,0,0,0});
}


static void
surface_window_resize_default_cb(void * v, wo_surface_t * wos, const wo_rect_t size)
{
    (void)v;
    (void)wos;
    (void)size;
}

wo_surface_t *
wo_make_surface_z(wo_window_t * wowin, const wo_surface_fns_t * fns, unsigned int zpos)
{
    wo_surface_t * const wos = calloc(1, sizeof(*wos));

    static const wo_surface_fns_t default_surf_fns = {
        surface_window_resize_default_cb,
    };

    if (wos == NULL)
        return NULL;
    wos->wowin = wo_window_ref(wowin);
    wos->woe = wo_window_env(wowin);
    wos->fns = (fns == NULL) ? default_surf_fns : *fns;

    pthread_mutex_lock(&wowin->surface_lock);
    {
        wo_surface_t * const win_surface = wowin->surface_chain;
        wo_surface_t * n = win_surface;
        wo_surface_t * p = NULL;
        while (n != NULL && n->zpos <= zpos) {
            p = n;
            n = n->next;
        }
        wos->prev = p;
        wos->next = n;
        if (p == NULL)
            wowin->surface_chain = wos;
        else
            p->next = wos;
        if (n != NULL)
            n->prev = wos;
        plane_create(wos->woe, &wos->s,
                     win_surface != NULL ? win_surface->s.surface : NULL,  // Parent - all based off window surface
                     p != NULL ? p->s.surface : win_surface != NULL ? win_surface->s.surface : NULL, // Above from Z
                     false);
        wos->parent = win_surface;
    }
    pthread_mutex_unlock(&wowin->surface_lock);
    return wos;
}

bool
wo_surface_dmabuf_fmt_check(wo_surface_t * const wos, const uint32_t fmt, const uint64_t mod)
{
    wo_env_t * const woe = wos->woe;
    const uint64_t cmod = canon_mod(mod);
    return fmt_list_find(&woe->fmt_list, fmt, mod) ||
        (mod != cmod && fmt_list_find(&woe->fmt_list, fmt, cmod));
}

void
wo_surface_on_win_resize_set(wo_surface_t * wos, wo_surface_win_resize_fn fn, void *v)
{
    wos->win_resize_fn = fn;
    wos->win_resize_v = v;
}

int
wo_surface_dst_pos_set(wo_surface_t * const wos, const wo_rect_t pos)
{
    wo_env_t * const woe = wos->woe;
    struct surface_attach_fb_arg_s * a = calloc(1, sizeof(*a));
    int rv;

    if (a == NULL)
        return -ENOMEM;

    a->detach = false;
    a->wos = wo_surface_ref(wos);
    a->wofb = NULL;
    a->dst_pos = pos;

    if ((rv = pollqueue_callback_once(woe->pq, surface_attach_fb_cb, a)) != 0) {
        surface_attach_fb_free(a);
        return rv;
    }
    return 0;
}

unsigned int
wo_surface_dst_width(const wo_surface_t * const wos)
{
    return wos->dst_pos.w;
}

unsigned int
wo_surface_dst_height(const wo_surface_t * const wos)
{
    return wos->dst_pos.h;
}

wo_env_t *
wo_surface_env(const wo_surface_t * const wos)
{
    return wos->woe;
}

struct wl_egl_window *
wo_surface_egl_window_create(wo_surface_t * const wos, const wo_rect_t dst_pos)
{
    if (wos->egl_window == NULL)
        wos->egl_window = wl_egl_window_create(wos->s.surface, dst_pos.w, dst_pos.h);
    wo_surface_dst_pos_set(wos, dst_pos);
    return wos->egl_window;
}

static void
surface_free(wo_surface_t * const wos)
{
    wo_window_t *const wowin = wos->wowin;
    pthread_mutex_lock(&wowin->surface_lock);
    {
        if (wos->prev == NULL)
            wowin->surface_chain = wos->next;
        else
            wos->prev->next = wos->next;
        if (wos->next != NULL)
            wos->next->prev = wos->prev;
    }
    pthread_mutex_unlock(&wowin->surface_lock);
    if (wos->egl_window)
        wl_egl_window_destroy(wos->egl_window);
    plane_destroy(&wos->s);
    wo_window_unref(&wos->wowin);
    free(wos);
}

void
wo_surface_unref(wo_surface_t ** ppWs)
{
    wo_surface_t * const ws = *ppWs;

    if (ws == NULL)
        return;
    if (atomic_fetch_sub(&ws->ref_count, 1) == 0)
        surface_free(ws);
}

wo_surface_t *
wo_surface_ref(wo_surface_t * const wos)
{
    if (wos != NULL)
        atomic_fetch_add(&wos->ref_count, 1);
    return wos;
}

// ---------------------------------------------------------------------------

static void
decoration_configure_cb(void *data,
                        struct zxdg_toplevel_decoration_v1 *zxdg_toplevel_decoration_v1,
                        uint32_t mode)
{
    (void)data;
    (void)mode;
//    LOG("%s: mode %d\n", __func__, mode);
    zxdg_toplevel_decoration_v1_destroy(zxdg_toplevel_decoration_v1);
}

static struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure_cb,
};

// ---------------------------------------------------------------------------
//
// XDG Toplevel callbacks
// Mostly ignored - except resize

static void
xdg_toplevel_configure_cb(void *data,
                          struct xdg_toplevel *xdg_toplevel, int32_t w, int32_t h,
                          struct wl_array *states)
{
    wo_window_t *const wowin = data;

//    enum xdg_toplevel_state *p;
//    LOG("%s: %dx%d\n", __func__, w, h);
//    wl_array_for_each(p, states) {
//        LOG("    State: %d\n", *p);
//    }

    (void)xdg_toplevel;
    (void)states;

    // no window geometry event, ignore
    if (w == 0 && h == 0)
        return;

    wowin->req_h = h;
    wowin->req_w = w;
}

static void
xdg_toplevel_close_cb(void *data, struct xdg_toplevel *xdg_toplevel)
{
    (void)data;
    (void)xdg_toplevel;
}

static void
xdg_toplevel_configure_bounds_cb(void *data,
                                 struct xdg_toplevel *xdg_toplevel,
                                 int32_t width, int32_t height)
{
    (void)data;
    (void)xdg_toplevel;
    (void)width;
    (void)height;
//    LOG("%s: %dx%d\n", __func__, width, height);
}

static void
xdg_toplevel_wm_capabilities_cb(void *data,
                                struct xdg_toplevel *xdg_toplevel,
                                struct wl_array *capabilities)
{
    (void)data;
    (void)xdg_toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure_cb,
    .close = xdg_toplevel_close_cb,
    .configure_bounds = xdg_toplevel_configure_bounds_cb,
    .wm_capabilities = xdg_toplevel_wm_capabilities_cb,
};

// ---------------------------------------------------------------------------
//
// xdg_surface_configure callback
// indictates that a sequence of configuration callbacks has finished
// we could (should?) resize out buffers here but it is easier to leave it to
// the next display. The ack goes with the next commit so that is all OK

static void
xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    wo_window_t * wowin = data;
    (void)data;

//    LOG("%s: Done\n", __func__);

    xdg_surface_ack_configure(xdg_surface, serial);

    if (wowin->sync_wait) {
        wowin->sync_wait = false;
        sem_post(&wowin->sync_sem);
    }

    if (wowin->req_h != 0 && wowin->req_w != 0 &&
        (wowin->pos.w != wowin->req_w || wowin->pos.w != wowin->req_w)) {
        wo_surface_t *p;

        // Size has changed - spin down list
        wowin->pos.w = wowin->req_w;
        wowin->pos.h = wowin->req_h;

        // ** This lock may be be bad in some cases - review when we find them
        pthread_mutex_lock(&wowin->surface_lock);
        for (p = wowin->surface_chain; p != NULL; p = p->next) {
            if (p->win_resize_fn)
                p->win_resize_fn(p->win_resize_v, p, wowin->pos);
        }
        pthread_mutex_unlock(&wowin->surface_lock);
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

// ---------------------------------------------------------------------------

wo_rect_t
wo_window_size(const wo_window_t * const wowin)
{
    return wowin->pos;
}

static void
window_free(wo_window_t * const wowin)
{
    free((char*)wowin->title);
    wo_env_unref(&wowin->woe);
    free(wowin);
}

static void
window_new_pq(void * v, short revents)
{
    wo_window_t * const wowin = v;
    wo_env_t * const woe = wowin->woe;
    (void)revents;

    wowin->wm_surface = xdg_wm_base_get_xdg_surface(woe->wm_base, wowin->wos->s.surface);
    xdg_surface_add_listener(wowin->wm_surface, &xdg_surface_listener, wowin);

    wowin->wm_toplevel = xdg_surface_get_toplevel(wowin->wm_surface);
    xdg_toplevel_add_listener(wowin->wm_toplevel, &xdg_toplevel_listener, wowin);

    xdg_toplevel_set_title(wowin->wm_toplevel, wowin->title);

    if (wowin->fullscreen)
        xdg_toplevel_set_fullscreen(wowin->wm_toplevel, NULL);

    wl_surface_commit(wowin->wos->s.surface);

    if (!woe->decoration_manager) {
        LOG("No decoration manager\n");
    }
    else {
        struct zxdg_toplevel_decoration_v1 *decoration =
            zxdg_decoration_manager_v1_get_toplevel_decoration(woe->decoration_manager, wowin->wm_toplevel);
        zxdg_toplevel_decoration_v1_add_listener(decoration, &decoration_listener, wowin);
        zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        // decoration destroyed in the callback
    }

}

static void
window_win_resize_cb(void * v, wo_surface_t * wos, const wo_rect_t win_pos)
{
//    LOG("%s\n", __func__);
    (void)v;
    wo_surface_dst_pos_set(wos, win_pos);
}

// Creates a window surface and adds a black opaque buffer to it
wo_window_t *
wo_window_new(wo_env_t * const woe, bool fullscreen, const wo_rect_t pos, const char * const title)
{
    wo_window_t * const wowin = calloc(1, sizeof(*wowin));
    wo_fb_t * wofb;

    if (wowin == NULL)
        return NULL;

    wowin->woe = wo_env_ref(woe);
    wowin->fullscreen = fullscreen;
    wowin->pos = pos;
    wowin->title = strdup(title);
    sem_init(&wowin->sync_sem, 0, 0);

    // We have now setup enough that we can make a surface on ourselves
    wowin->wos = wo_make_surface_z(wowin, NULL, 0);
    wo_surface_on_win_resize_set(wowin->wos, window_win_resize_cb, NULL);

    wowin->sync_wait = true;
    pollqueue_callback_once(woe->pq, window_new_pq, wowin);

    while (sem_wait(&wowin->sync_sem) == -1 && errno == EINTR)
        /* loop */;

    wowin->sync_wait = true;
    wofb = wo_fb_new_rgba_pixel(woe, 0, 0, 0, UINT32_MAX);
    wo_surface_attach_fb(wowin->wos, wofb, wowin->pos);

    // This is somewhat kludgy but we always seem to get a 2nd configure after
    // the attach which includes fullscreen if applied
    while (sem_wait(&wowin->sync_sem) == -1 && errno == EINTR)
        /* loop */;
    return wowin;
}

void
wo_window_unref(wo_window_t ** const ppWowin)
{
    wo_window_t * const wowin = *ppWowin;
    if (wowin == NULL)
        return;
    if (atomic_fetch_sub(&wowin->ref_count, 1) == 0)
        window_free(wowin);
}

wo_window_t *
wo_window_ref(wo_window_t * const wowin)
{
    atomic_fetch_add(&wowin->ref_count, 1);
    return wowin;
}

wo_env_t *
wo_window_env(const wo_window_t * wowin)
{
    return wowin->woe;
}

// ---------------------------------------------------------------------------
//
// Registration listener for window mgr - simple ping

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

// ---------------------------------------------------------------------------
//
// Registration listener for dmabuf formats

static void
linux_dmabuf_v1_listener_format(void *data,
                                struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
                                uint32_t format)
{
    // Superceeded by _modifier
    wo_env_t *const woe = data;
    (void)zwp_linux_dmabuf_v1;

    fmt_list_add(&woe->fmt_list, format, DRM_FORMAT_MOD_LINEAR);
}

static void
linux_dmabuf_v1_listener_modifier(void *data,
                                  struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
                                  uint32_t format,
                                  uint32_t modifier_hi,
                                  uint32_t modifier_lo)
{
    wo_env_t *const woe = data;
    (void)zwp_linux_dmabuf_v1;

    fmt_list_add(&woe->fmt_list, format, ((uint64_t)modifier_hi << 32) | modifier_lo);
}

static const struct zwp_linux_dmabuf_v1_listener linux_dmabuf_v1_listener = {
    .format = linux_dmabuf_v1_listener_format,
    .modifier = linux_dmabuf_v1_listener_modifier,
};

// ---------------------------------------------------------------------------

static void
global_registry_handler(void *data, struct wl_registry *registry, uint32_t id,
                                    const char *interface, uint32_t version)
{
    wo_env_t *const woe = data;
    (void)version;

#if TRACE_ALL
    LOG("Got a registry event for %s vers %d id %d\n", interface, version, id);
#endif

    if (strcmp(interface, wl_compositor_interface.name) == 0)
        woe->compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);

    // Want version 3 as that has _create_immed (ver 2) and modifiers (ver 3)
    // v4 reworks format listing again to be more complex - avoid for now
    if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        woe->linux_dmabuf_v1 = wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, 3);
        zwp_linux_dmabuf_v1_add_listener(woe->linux_dmabuf_v1, &linux_dmabuf_v1_listener, woe);
    }

    if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        woe->wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(woe->wm_base, &xdg_wm_base_listener, woe);
    }
    if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0)
        woe->decoration_manager = wl_registry_bind(registry, id, &zxdg_decoration_manager_v1_interface, 1);
    if (strcmp(interface, wp_viewporter_interface.name) == 0)
        woe->viewporter = wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
    if (strcmp(interface, wp_single_pixel_buffer_manager_v1_interface.name) == 0)
        woe->single_pixel_manager = wl_registry_bind(registry, id, &wp_single_pixel_buffer_manager_v1_interface, 1);
    if (strcmp(interface, wl_subcompositor_interface.name) == 0)
        woe->subcompositor = wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
}

static void
global_registry_remover(void *data, struct wl_registry *registry, uint32_t id)
{
    (void)data;
    (void)registry;

    LOG("Got a registry losing event for %d\n", id);
}

static int
get_display_and_registry(wo_env_t *const woe)
{

    struct wl_display *const display = wl_display_connect(NULL);
    struct wl_registry *registry = NULL;

    static const struct wl_registry_listener global_registry_listener = {
        global_registry_handler,
        global_registry_remover
    };

    if (display == NULL) {
        LOG("Can't connect to wayland display !?\n");
        return -1;
    }

    if ((registry = wl_display_get_registry(display)) == NULL) {
        LOG("Failed to get registry\n");
        goto fail;
    }

    wl_registry_add_listener(registry, &global_registry_listener, woe);

    // This calls the attached listener global_registry_handler
    wl_display_roundtrip(display);
    // Roundtrip again to ensure that things that are returned immediately
    // after bind are now done
    wl_display_roundtrip(display);
    // Don't need this anymore
    // In theory supported extensions are dynamic - ignore that
    wl_registry_destroy(registry);

    woe->w_display = display;
    fmt_list_sort(&woe->fmt_list);
    return 0;

fail:
    if (registry)
        wl_registry_destroy(registry);
    if (display)
        wl_display_disconnect(display);
    return -1;
}

// ---------------------------------------------------------------------------
//
// Wayland dispatcher
//
// Contains a flush in the pre-poll function so there is no need for one in
// any callback

// Pre display thread poll function
static void
pollq_pre_cb(void *v, struct pollfd *pfd)
{
    wo_env_t *const woe = v;
    struct wl_display *const display = woe->w_display;

    while (wl_display_prepare_read(display) != 0)
        wl_display_dispatch_pending(display);

    if (wl_display_flush(display) >= 0)
        pfd->events = POLLIN;
    else
        pfd->events = POLLOUT | POLLIN;
    pfd->fd = wl_display_get_fd(display);
}

// Post display thread poll function
// Happens before any other pollqueue callbacks
// Dispatches wayland callbacks
static void
pollq_post_cb(void *v, short revents)
{
    wo_env_t *const woe = v;
    struct wl_display *const display = woe->w_display;

    if ((revents & POLLIN) == 0)
        wl_display_cancel_read(display);
    else
        wl_display_read_events(display);

    wl_display_dispatch_pending(display);
}

// ----------------------------------------------------------------------------
//
// Main env

struct wl_display *
wo_env_display(const wo_env_t * const woe)
{
    return woe->w_display;
}

struct pollqueue *
wo_env_pollqueue(const wo_env_t * const woe)
{
    return woe->pq;
}

wo_env_t *
wo_env_ref(wo_env_t * const woe)
{
    if (woe != NULL)
        atomic_fetch_add(&woe->ref_count, 1);
    return woe;
}


static void
eq_sync_wl_cb(void * data, struct wl_callback * cb, unsigned int cb_data)
{
    sem_t * const sem = data;
    (void)cb_data;
    wl_callback_destroy(cb);
    sem_post(sem);
}

static const struct wl_callback_listener eq_sync_listener = {.done = eq_sync_wl_cb};

struct eq_sync_env_ss {
    wo_env_t * const woe;
    sem_t sem;
};

static void
eq_sync_pq_cb(void * v, short revents)
{
    struct eq_sync_env_ss * const eqs = v;
    struct wl_callback *const cb = wl_display_sync(eqs->woe->w_display);
    (void)revents;
    wl_callback_add_listener(cb, &eq_sync_listener, &eqs->sem);
    // No flush needed as that will occur as part of the pollqueue loop
}

int
wo_env_sync(wo_env_t * const woe)
{
    struct eq_sync_env_ss eqs = {.woe = woe};
    int rv;

    if (!woe)
        return -1;

    sem_init(&eqs.sem, 0, 0);
    // Bounce execution to pollqueue to avoid race setting up listener
    pollqueue_callback_once(woe->pq, eq_sync_pq_cb, &eqs);
    while ((rv = sem_wait(&eqs.sem)) == -1 && errno == EINTR)
        /* Loop */;
    sem_destroy(&eqs.sem);
    return rv;
}

static void
env_free(wo_env_t * const woe)
{
    pollqueue_finish(&woe->pq);

    if (woe->wm_base)
        xdg_wm_base_destroy(woe->wm_base);
    if (woe->decoration_manager)
        zxdg_decoration_manager_v1_destroy(woe->decoration_manager);
    if (woe->viewporter)
        wp_viewporter_destroy(woe->viewporter);
    if (woe->linux_dmabuf_v1)
        zwp_linux_dmabuf_v1_destroy(woe->linux_dmabuf_v1);
    if (woe->single_pixel_manager)
        wp_single_pixel_buffer_manager_v1_destroy(woe->single_pixel_manager);
    if (woe->subcompositor)
        wl_subcompositor_destroy(woe->subcompositor);
    if (woe->compositor)
        wl_compositor_destroy(woe->compositor);
    region_destroy(&woe->region_all);

    wl_display_roundtrip(woe->w_display);

    wl_display_disconnect(woe->w_display);

    dmabufs_ctl_unref(&woe->dbsc);

    fmt_list_uninit(&woe->fmt_list);

    free(woe);
}

void
wo_env_unref(wo_env_t ** const ppWoe)
{
    wo_env_t * const woe = *ppWoe;
    if (woe == NULL)
        return;
    if (atomic_fetch_sub(&woe->ref_count, 1) == 0)
        env_free(woe);
}

wo_env_t *
wo_env_new_default(void)
{
    wo_env_t * woe = calloc(1, sizeof(*woe));

    if (woe == NULL)
        return NULL;

    fmt_list_init(&woe->fmt_list, 16);

    if (get_display_and_registry(woe) != 0)
        goto fail;

    if (!woe->compositor) {
        LOG("Missing wayland compositor\n");
        goto fail;
    }
    if (!woe->viewporter) {
        LOG("Missing wayland viewporter\n");
        goto fail;
    }
    if (!woe->wm_base) {
        LOG("Missing xdg window manager\n");
        goto fail;
    }
    if (!woe->linux_dmabuf_v1) {
        LOG("Missing wayland linux_dmabuf extension\n");
        goto fail;
    }

    if ((woe->pq = pollqueue_new()) == NULL) {
        LOG("Pollqueue setup failed\n");
        goto fail;
    }

    if ((woe->dbsc = dmabufs_ctl_new()) == NULL) {
        LOG("dmabuf setup failed\n");
        goto fail;
    }

    woe->region_all = wl_compositor_create_region(woe->compositor);
    wl_region_add(woe->region_all, 0, 0, INT32_MAX, INT32_MAX);

    pollqueue_set_pre_post(woe->pq, pollq_pre_cb, pollq_post_cb, woe);

    return woe;

fail:
    env_free(woe);
    return NULL;
}

