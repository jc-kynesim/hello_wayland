#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <wayland-client-protocol.h>
#include <wayland-egl.h> // Wayland EGL MUST be included before EGL headers

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include <libdrm/drm_fourcc.h>

#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>

// protocol headers that we build as part of the compile
#include "viewporter-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

// Local headers
#include "init_window.h"
#include "pollqueue.h"

//#include "log.h"
#define LOG printf

#define TRACE_ALL 0

typedef struct fmt_ent_s {
    uint32_t fmt;
    uint64_t mod;
} fmt_ent_t;

typedef struct fmt_list_s {
    fmt_ent_t *fmts;
    unsigned int size;
    unsigned int len;
} fmt_list_t;

typedef struct window_ctx_s {
    struct wl_display *w_display;
    int window_width;
    int window_height;
    int req_w;
    int req_h;
    struct pollqueue *pq;

    // Bound wayland extensions
    struct wl_compositor *compositor;
    struct zwp_linux_dmabuf_v1 *linux_dmabuf_v1;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct wp_viewporter *viewporter;
    struct xdg_wm_base *wm_base;

    // Wayland objects
    struct wl_surface *surface;
    struct wp_viewport *viewport;
    struct xdg_surface *wm_surface;
    struct xdg_toplevel *wm_toplevel;

    struct wl_callback *frame_callback;

    // EGL
    struct wl_egl_window *w_egl_window;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    bool fmt_ok;
    uint32_t last_fmt;
    uint64_t last_mod;

    // Dmabuf
    fmt_list_t fmt_list;
} window_ctx_t;

typedef struct wayland_out_env_s {
    window_ctx_t wc;

    int show_all;
    int fullscreen;

    pthread_mutex_t q_lock;
    sem_t egl_setup_sem;
    bool egl_setup_fail;
    bool is_egl;

    sem_t q_sem;
    AVFrame *q_next;

    bool frame_wait;
} wayland_out_env_t;

// Structure that holds context whilst waiting for fence release
struct dmabuf_w_env_s {
    int fd;
    AVBufferRef *buf;
    struct pollqueue *pq;
    struct polltask *pt;
    window_ctx_t *wc;
};


#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720


// Remove any params from a modifier
static inline uint64_t
canon_mod(const uint64_t m)
{
    return fourcc_mod_is_vendor(m, BROADCOM) ? fourcc_mod_broadcom_mod(m) : m;
}

// ---------------------------------------------------------------------------
//
// Format list creation & lookup
// Currently only used for dmabuf

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
// Dmabuf environment passed between callbacks

static struct dmabuf_w_env_s*
dmabuf_w_env_new(window_ctx_t *const wc, AVBufferRef *const buf, const int fd)
{
    struct dmabuf_w_env_s *const dbe = malloc(sizeof(*dbe));
    if (!dbe)
        return NULL;

    dbe->fd = fd;
    dbe->buf = av_buffer_ref(buf);
    dbe->pq = pollqueue_ref(wc->pq);
    dbe->pt = NULL;
    dbe->wc = wc;
    return dbe;
}

static void
dmabuf_w_env_free(struct dmabuf_w_env_s *const dbe)
{
    av_buffer_unref(&dbe->buf);
    polltask_delete(&dbe->pt);
    pollqueue_unref(&dbe->pq);
    free(dbe);
}

static void
dmabuf_fence_release_cb(void *v, short revents)
{
    (void)revents;
    dmabuf_w_env_free(v);
}

// ---------------------------------------------------------------------------
//
// Wayland dmabuf display function

static void
w_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    struct dmabuf_w_env_s *const dbe = data;

    // Sent by the compositor when it's no longer using this buffer
    wl_buffer_destroy(wl_buffer);
    // Whilst the wl_buffer isn't in use the underlying dmabuf may (and often
    // is) still be in use with fences set on it. We have to wait for those
    // as V4L2 doesn't respect them.
    // * Arguably if we have >1 object we should wait for all but just waiting
    //   for the 1st works fine.
    dbe->pt = polltask_new(dbe->pq, dbe->fd, POLLOUT, dmabuf_fence_release_cb, dbe);
    pollqueue_add_task(dbe->pt, -1);
}

static void
do_display_dmabuf(window_ctx_t *const wc, AVFrame *const frame)
{
    struct zwp_linux_buffer_params_v1 *params;
    const AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
    const uint32_t format = desc->layers[0].format;
    const uint64_t cmod = canon_mod(desc->objects[0].format_modifier);
    const unsigned int width = av_frame_cropped_width(frame);
    const unsigned int height = av_frame_cropped_height(frame);
    struct wl_buffer *w_buffer;
    unsigned int n = 0;
    unsigned int flags = 0;
    int i;

    static const struct wl_buffer_listener w_buffer_listener = {
        .release = w_buffer_release,
    };

#if TRACE_ALL
    LOG("<<< %s\n", __func__);
#endif

    if (!fmt_list_find(&wc->fmt_list, format, cmod)) {
        LOG("No support for format %s mod %#"PRIx64"\n", av_fourcc2str(format), cmod);
        return;
    }

    /* Creation and configuration of planes  */
    params = zwp_linux_dmabuf_v1_create_params(wc->linux_dmabuf_v1);
    if (!params) {
        LOG("zwp_linux_dmabuf_v1_create_params FAILED\n");
        return;
    }

    for (i = 0; i < desc->nb_layers; ++i) {
        int j;
        for (j = 0; j < desc->layers[i].nb_planes; ++j) {
            const AVDRMPlaneDescriptor *const p = desc->layers[i].planes + j;
            const AVDRMObjectDescriptor *const obj = desc->objects + p->object_index;

            zwp_linux_buffer_params_v1_add(params, obj->fd, n++, p->offset, p->pitch,
                                           (unsigned int)(obj->format_modifier >> 32),
                                           (unsigned int)(obj->format_modifier & 0xFFFFFFFF));
        }
    }

    if (frame->interlaced_frame) {
        flags |= ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_INTERLACED;
        if (!frame->top_field_first)
            flags |= ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_BOTTOM_FIRST;
    }

    w_buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, format, flags);
    zwp_linux_buffer_params_v1_destroy(params);

    if (w_buffer == NULL) {
        LOG("Failed to create dmabuf\n");
        return;
    }

    wl_buffer_add_listener(w_buffer, &w_buffer_listener,
                           dmabuf_w_env_new(wc, frame->buf[0], desc->objects[0].fd));

    wl_surface_attach(wc->surface, w_buffer, 0, 0);
    wp_viewport_set_destination(wc->viewport, wc->req_w, wc->req_h);
    wl_surface_damage(wc->surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(wc->surface);
}

// ---------------------------------------------------------------------------
//
// EGL display function

static bool
check_support_egl(window_ctx_t *const wc, const uint32_t fmt, const uint64_t mod)
{
    EGLuint64KHR mods[16];
    GLint mod_count = 0;
    GLint i;
    const uint64_t cmod = canon_mod(mod);

    if (fmt == wc->last_fmt && cmod == wc->last_mod)
        return wc->fmt_ok;

    wc->last_fmt = fmt;
    wc->last_mod = cmod;
    wc->fmt_ok = false;

    if (!eglQueryDmaBufModifiersEXT(wc->egl_display, fmt, 16, mods, NULL, &mod_count)) {
        LOG("queryDmaBufModifiersEXT Failed for %s\n", av_fourcc2str(fmt));
        return false;
    }

    for (i = 0; i < mod_count; ++i) {
        if (mods[i] == cmod) {
            wc->fmt_ok = true;
            return true;
        }
    }

    LOG("Failed to find modifier %"PRIx64"\n", cmod);
    return false;
}

static void
do_display_egl(window_ctx_t *const wc, AVFrame *const frame)
{
    const AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
    EGLint attribs[50];
    EGLint *a = attribs;
    int i, j;
    GLuint texture;
    EGLImage image;

    static const EGLint anames[] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
    };
    const EGLint *b = anames;

#if TRACE_ALL
    LOG("<<< %s\n", __func__);
#endif

    if (!check_support_egl(wc, desc->layers[0].format, desc->objects[0].format_modifier)) {
        LOG("No support for format %s mod %#"PRIx64"\n", av_fourcc2str(desc->layers[0].format), desc->objects[0].format_modifier);
        return;
    }

    if (wc->req_w != wc->window_width || wc->req_h != wc->window_height) {
        LOG("%s: Resize %dx%d -> %dx%d\n", __func__, wc->window_width, wc->window_height, wc->req_w, wc->req_h);
        wl_egl_window_resize(wc->w_egl_window, wc->req_w, wc->req_h, 0, 0);
        wc->window_width = wc->req_w;
        wc->window_height = wc->req_h;
    }

    *a++ = EGL_WIDTH;
    *a++ = av_frame_cropped_width(frame);
    *a++ = EGL_HEIGHT;
    *a++ = av_frame_cropped_height(frame);
    *a++ = EGL_LINUX_DRM_FOURCC_EXT;
    *a++ = desc->layers[0].format;

    for (i = 0; i < desc->nb_layers; ++i) {
        for (j = 0; j < desc->layers[i].nb_planes; ++j) {
            const AVDRMPlaneDescriptor *const p = desc->layers[i].planes + j;
            const AVDRMObjectDescriptor *const obj = desc->objects + p->object_index;
            *a++ = *b++;
            *a++ = obj->fd;
            *a++ = *b++;
            *a++ = p->offset;
            *a++ = *b++;
            *a++ = p->pitch;
            if (obj->format_modifier == 0) {
                b += 2;
            }
            else {
                *a++ = *b++;
                *a++ = (EGLint)(obj->format_modifier & 0xFFFFFFFF);
                *a++ = *b++;
                *a++ = (EGLint)(obj->format_modifier >> 32);
            }
        }
    }
    *a = EGL_NONE;

    if (!(image = eglCreateImageKHR(wc->egl_display, EGL_NO_CONTEXT,
                                    EGL_LINUX_DMA_BUF_EXT, NULL, attribs))) {
        LOG("Failed to import fd %d\n", desc->objects[0].fd);
        return;
    }

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

    eglDestroyImageKHR(wc->egl_display, image);

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    eglSwapBuffers(wc->egl_display, wc->egl_surface);

    glDeleteTextures(1, &texture);

    // A fence is set on the fd by the egl render - we can reuse the buffer once it goes away
    // (same as the direct wayland output after buffer release)
    {
        struct dmabuf_w_env_s *const dbe = dmabuf_w_env_new(wc, frame->buf[0], desc->objects[0].fd);
        dbe->pt = polltask_new(dbe->pq, desc->objects[0].fd, POLLOUT, dmabuf_fence_release_cb, dbe);
        pollqueue_add_task(dbe->pt, -1);
    }
}

// ---------------------------------------------------------------------------
//
// GL setup code
// Builds shaders, finds context

static GLint
compile_shader(GLenum target, const char *source)
{
    GLuint s = glCreateShader(target);

    if (s == 0) {
        LOG("Failed to create shader\n");
        return 0;
    }

    glShaderSource(s, 1, (const GLchar **)&source, NULL);
    glCompileShader(s);

    {
        GLint ok;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);

        if (!ok) {
            GLchar *info;
            GLint size;

            glGetShaderiv(s, GL_INFO_LOG_LENGTH, &size);
            info = malloc(size);

            glGetShaderInfoLog(s, size, NULL, info);
            LOG("Failed to compile shader: %ssource:\n%s\n", info, source);
            free(info);

            return 0;
        }
    }

    return s;
}

static GLuint
link_program(GLint vs, GLint fs)
{
    GLuint prog = glCreateProgram();

    if (prog == 0) {
        LOG("Failed to create program\n");
        return 0;
    }

    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    {
        GLint ok;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            /* Some drivers return a size of 1 for an empty log.  This is the size
             * of a log that contains only a terminating NUL character.
             */
            GLint size;
            GLchar *info = NULL;
            glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &size);
            if (size > 1) {
                info = malloc(size);
                glGetProgramInfoLog(prog, size, NULL, info);
            }

            LOG("Failed to link: %s\n",
                (info != NULL) ? info : "<empty log>");
            return 0;
        }
    }

    return prog;
}

static int
gl_setup()
{
    const char *vs =
        "attribute vec4 pos;\n"
        "varying vec2 texcoord;\n"
        "\n"
        "void main() {\n"
        "  gl_Position = pos;\n"
        "  texcoord.x = (pos.x + 1.0) / 2.0;\n"
        "  texcoord.y = (-pos.y + 1.0) / 2.0;\n"
        "}\n";
    const char *fs =
        "#extension GL_OES_EGL_image_external : enable\n"
        "precision mediump float;\n"
        "uniform samplerExternalOES s;\n"
        "varying vec2 texcoord;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(s, texcoord);\n"
        "}\n";

    GLuint vs_s;
    GLuint fs_s;
    GLuint prog;

    if (!(vs_s = compile_shader(GL_VERTEX_SHADER, vs)) ||
        !(fs_s = compile_shader(GL_FRAGMENT_SHADER, fs)) ||
        !(prog = link_program(vs_s, fs_s)))
        return -1;

    glUseProgram(prog);

    {
        static const float verts[] = {
            -1, -1,
            1, -1,
            1, 1,
            -1, 1,
        };
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
    }

    glEnableVertexAttribArray(0);
    return 0;
}

static EGLBoolean
CreateEGLContext(window_ctx_t *const wc)
{
    EGLint numConfigs;
    EGLint majorVersion;
    EGLint minorVersion;
    EGLContext context;
    EGLSurface surface;
    EGLConfig config;
    EGLint fbAttribs[] =
    {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_NONE
    };
    EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE, EGL_NONE };
    EGLDisplay display = eglGetDisplay(wc->w_display);
    if (display == EGL_NO_DISPLAY) {
        LOG("No EGL Display...\n");
        return EGL_FALSE;
    }

    // Initialize EGL
    if (!eglInitialize(display, &majorVersion, &minorVersion)) {
        LOG("No Initialisation...\n");
        return EGL_FALSE;
    }

    LOG("EGL init: version %d.%d\n", majorVersion, minorVersion);

    eglBindAPI(EGL_OPENGL_ES_API);

    // Get configs
    if ((eglGetConfigs(display, NULL, 0, &numConfigs) != EGL_TRUE) || (numConfigs == 0)) {
        LOG("No configuration...\n");
        return EGL_FALSE;
    }
    LOG("GL Configs: %d\n", numConfigs);

    // Choose config
    if ((eglChooseConfig(display, fbAttribs, &config, 1, &numConfigs) != EGL_TRUE) || (numConfigs != 1)) {
        LOG("No configuration...\n");
        return EGL_FALSE;
    }

    wc->w_egl_window =
        wl_egl_window_create(wc->surface, wc->window_width, wc->window_height);

    if (wc->w_egl_window == EGL_NO_SURFACE) {
        LOG("No window !?\n");
        return EGL_FALSE;
    }

    // Create a surface
    surface = eglCreateWindowSurface(display, config, wc->w_egl_window, NULL);
    if (surface == EGL_NO_SURFACE) {
        LOG("No surface...\n");
        return EGL_FALSE;
    }

    // Create a GL context
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT) {
        LOG("No context...\n");
        return EGL_FALSE;
    }

    wc->egl_display = display;
    wc->egl_surface = surface;
    wc->egl_context = context;
    return EGL_TRUE;
}

static void
do_egl_setup(void *v, short revents)
{
    wayland_out_env_t *const woe = v;
    window_ctx_t *const wc = &woe->wc;
    (void)revents;

    if (!CreateEGLContext(wc))
        goto fail;

    // Make the context current
    if (!eglMakeCurrent(wc->egl_display, wc->egl_surface, wc->egl_surface, wc->egl_context)) {
        LOG("Could not make the current window current !\n");
        goto fail;
    }

    LOG("GL Vendor: %s\n", glGetString(GL_VENDOR));
    LOG("GL Version: %s\n", glGetString(GL_VERSION));
    LOG("GL Renderer: %s\n", glGetString(GL_RENDERER));
    LOG("GL Extensions: %s\n", glGetString(GL_EXTENSIONS));
    LOG("EGL Extensions: %s\n", eglQueryString(wc->egl_display, EGL_EXTENSIONS));

    if (!epoxy_has_egl_extension(wc->egl_display, "EGL_EXT_image_dma_buf_import")) {
        LOG("Missing EGL EXT image dma_buf extension\n");
        goto fail;
    }

    if (gl_setup()) {
        LOG("%s: gl_setup failed\n", __func__);
        goto fail;
    }
    sem_post(&woe->egl_setup_sem);
    return;

fail:
    woe->egl_setup_fail = true;
    sem_post(&woe->egl_setup_sem);
}

// ---------------------------------------------------------------------------
//
// Display a new frame when (a) we have one and (b) we have had a frame
// callback from wayland

static void try_display(wayland_out_env_t *const de);

static void
surface_frame_done_cb(void *data, struct wl_callback *cb, uint32_t time)
{
    wayland_out_env_t *const woe = data;
    (void)time;

    woe->wc.frame_callback = NULL;
    wl_callback_destroy(cb);

    woe->frame_wait = false;
    try_display(woe);
}

static void
do_prod_display(void *v, short revents)
{
    (void)revents;
    try_display(v);
}

static void
try_display(wayland_out_env_t *const woe)
{
    window_ctx_t *const wc = &woe->wc;
    AVFrame *frame;

    if (woe->frame_wait)
        return;

    pthread_mutex_lock(&woe->q_lock);
    frame = woe->q_next;
    woe->q_next = NULL;
    pthread_mutex_unlock(&woe->q_lock);

    if (frame) {
        static const struct wl_callback_listener frame_listener = { .done = surface_frame_done_cb };
        wc->frame_callback = wl_surface_frame(wc->surface);
        wl_callback_add_listener(wc->frame_callback, &frame_listener, woe);
        woe->frame_wait = true;
        if (woe->show_all)
            sem_post(&woe->q_sem);

        if (woe->is_egl)
            do_display_egl(wc, frame);
        else
            do_display_dmabuf(wc, frame);
        av_frame_free(&frame);
    }
}

// ---------------------------------------------------------------------------
//
// XDG Toplevel callbacks
// Mostly ignored - except resize

static void
xdg_toplevel_configure_cb(void *data,
                                      struct xdg_toplevel *xdg_toplevel, int32_t w, int32_t h,
                                      struct wl_array *states)
{
    window_ctx_t *const wc = data;
    (void)xdg_toplevel;
    (void)states;

    LOG("%s: %dx%d\n", __func__, w, h);

    // no window geometry event, ignore
    if (w == 0 && h == 0)
        return;

    wc->req_h = h;
    wc->req_w = w;
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
    LOG("%s: %dx%d\n", __func__, width, height);
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
    (void)data;

    xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

// ---------------------------------------------------------------------------

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

static void
linux_dmabuf_v1_listener_format(void *data,
                                            struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
                                            uint32_t format)
{
    // Superceeded by _modifier
    window_ctx_t *const wc = data;
    (void)zwp_linux_dmabuf_v1;

    fmt_list_add(&wc->fmt_list, format, DRM_FORMAT_MOD_LINEAR);
}

static void
linux_dmabuf_v1_listener_modifier(void *data,
                                  struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
                                  uint32_t format,
                                  uint32_t modifier_hi,
                                  uint32_t modifier_lo)
{
    window_ctx_t *const wc = data;
    (void)zwp_linux_dmabuf_v1;

    fmt_list_add(&wc->fmt_list, format, ((uint64_t)modifier_hi << 32) | modifier_lo);
}

static const struct zwp_linux_dmabuf_v1_listener linux_dmabuf_v1_listener = {
    .format = linux_dmabuf_v1_listener_format,
    .modifier = linux_dmabuf_v1_listener_modifier,
};

// ---------------------------------------------------------------------------

static void
decoration_configure_cb(void *data,
                        struct zxdg_toplevel_decoration_v1 *zxdg_toplevel_decoration_v1,
                        uint32_t mode)
{
    (void)data;
    LOG("%s: mode %d\n", __func__, mode);
    zxdg_toplevel_decoration_v1_destroy(zxdg_toplevel_decoration_v1);
}

static struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure_cb,
};

// ---------------------------------------------------------------------------

static void
global_registry_handler(void *data, struct wl_registry *registry, uint32_t id,
                                    const char *interface, uint32_t version)
{
    wayland_out_env_t *const woe = data;
    window_ctx_t *const wc = &woe->wc;
    (void)version;

#if TRACE_ALL
    LOG("Got a registry event for %s vers %d id %d\n", interface, version, id);
#endif

    if (strcmp(interface, wl_compositor_interface.name) == 0)
        wc->compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);

    // Want version 3 as that has _create_immed (ver 2) and modifiers (ver 3)
    // v4 reworks format listing again to be more complex - avoid for now
    if (!woe->is_egl &&
        strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        wc->linux_dmabuf_v1 = wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, 3);
        zwp_linux_dmabuf_v1_add_listener(wc->linux_dmabuf_v1, &linux_dmabuf_v1_listener, wc);
    }

    if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wc->wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wc->wm_base, &xdg_wm_base_listener, NULL);
    }
    if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0)
        wc->decoration_manager = wl_registry_bind(registry, id, &zxdg_decoration_manager_v1_interface, 1);
    if (strcmp(interface, wp_viewporter_interface.name) == 0)
        wc->viewporter = wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
}

static void
global_registry_remover(void *data, struct wl_registry *registry, uint32_t id)
{
    (void)data;
    (void)registry;

    LOG("Got a registry losing event for %d\n", id);
}

static int
get_display_and_registry(wayland_out_env_t *const woe, window_ctx_t *const wc)
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

    wc->w_display = display;
    fmt_list_sort(&wc->fmt_list);
    return 0;

fail:
    if (registry)
        wl_registry_destroy(registry);
    if (display)
        wl_display_disconnect(display);
    return -1;
}

static void
destroy_window(window_ctx_t *const wc)
{
    if (!wc->w_display)
        return;

    if (wc->viewport)
        wp_viewport_destroy(wc->viewport);

    if (wc->egl_surface)
        eglDestroySurface(wc->egl_display, wc->egl_surface);
    if (wc->egl_context)
        eglDestroyContext(wc->egl_display, wc->egl_context);
    if (wc->w_egl_window)
        wl_egl_window_destroy(wc->w_egl_window);

    if (wc->wm_toplevel)
        xdg_toplevel_destroy(wc->wm_toplevel);
    if (wc->wm_surface)
        xdg_surface_destroy(wc->wm_surface);
    if (wc->surface)
        wl_surface_destroy(wc->surface);

    // The frame callback would destroy this but there is no guarantee it
    // would ever be called so there is no point waiting
    if (wc->frame_callback)
        wl_callback_destroy(wc->frame_callback);

    if (wc->wm_base)
        xdg_wm_base_destroy(wc->wm_base);
    if (wc->decoration_manager)
        zxdg_decoration_manager_v1_destroy(wc->decoration_manager);
    if (wc->viewporter)
        wp_viewporter_destroy(wc->viewporter);
    if (wc->linux_dmabuf_v1)
        zwp_linux_dmabuf_v1_destroy(wc->linux_dmabuf_v1);
    if (wc->compositor)
        wl_compositor_destroy(wc->compositor);

    wl_display_roundtrip(wc->w_display);
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
    window_ctx_t *const wc = v;
    struct wl_display *const display = wc->w_display;

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
    window_ctx_t *const wc = v;
    struct wl_display *const display = wc->w_display;

    if ((revents & POLLIN) == 0)
        wl_display_cancel_read(display);
    else
        wl_display_read_events(display);

    wl_display_dispatch_pending(display);
}

// ---------------------------------------------------------------------------
//
// External entry points

void
egl_wayland_out_modeset(wayland_out_env_t *woe, int w, int h, AVRational frame_rate)
{
    (void)woe;
    (void)w;
    (void)h;
    (void)frame_rate;
    /* NIF */
}

int
egl_wayland_out_display(wayland_out_env_t *woe, AVFrame *src_frame)
{
    AVFrame *frame = NULL;

#if TRACE_ALL
    LOG("<<< %s\n", __func__);
#endif

    if (src_frame->format == AV_PIX_FMT_DRM_PRIME) {
        frame = av_frame_alloc();
        av_frame_ref(frame, src_frame);
    }
    else if (src_frame->format == AV_PIX_FMT_VAAPI) {
        frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_DRM_PRIME;
        if (av_hwframe_map(frame, src_frame, 0) != 0) {
            LOG("Failed to map frame (format=%d) to DRM_PRiME\n", src_frame->format);
            av_frame_free(&frame);
            return AVERROR(EINVAL);
        }
    }
    else {
        LOG("Frame (format=%d) not DRM_PRiME\n", src_frame->format);
        return AVERROR(EINVAL);
    }

    // If show_all then wait for q_next to be empty otherwise
    // (run decode @ max speed) just plow on
    if (woe->show_all) {
        while (sem_wait(&woe->q_sem) == 0 && errno == EINTR)
        /* Loop */;
    }

    pthread_mutex_lock(&woe->q_lock);
    {
        AVFrame *const t = woe->q_next;
        woe->q_next = frame;
        frame = t;
    }
    pthread_mutex_unlock(&woe->q_lock);

    if (frame == NULL)
        pollqueue_callback_once(woe->wc.pq, do_prod_display, woe);
    else
        av_frame_free(&frame);

    return 0;
}


void
egl_wayland_out_delete(wayland_out_env_t *woe)
{
    window_ctx_t *const wc = &woe->wc;

    if (woe == NULL)
        return;

    LOG("<<< %s\n", __func__);

    if (wc->surface) {
        wl_surface_attach(wc->surface, NULL, 0, 0);
        wl_surface_commit(wc->surface);
        wl_display_flush(wc->w_display);
    }

    pollqueue_finish(&wc->pq);

    destroy_window(wc);
    wl_display_disconnect(wc->w_display);
    fmt_list_uninit(&wc->fmt_list);

    av_frame_free(&woe->q_next);
    sem_destroy(&woe->q_sem);
    sem_destroy(&woe->egl_setup_sem);
    pthread_mutex_destroy(&woe->q_lock);

    free(woe);
}


static wayland_out_env_t*
wayland_out_new(const bool is_egl, const unsigned int flags)
{
    wayland_out_env_t *const woe = calloc(1, sizeof(*woe));
    window_ctx_t *const wc = &woe->wc;

    LOG("<<< %s\n", __func__);

    woe->is_egl = is_egl;
    woe->show_all = !(flags & WOUT_FLAG_NO_WAIT);

    wc->req_w = WINDOW_WIDTH;
    wc->req_h = WINDOW_HEIGHT;

    pthread_mutex_init(&woe->q_lock, NULL);
    sem_init(&woe->egl_setup_sem, 0, 0);
    sem_init(&woe->q_sem, 0, 1);

    fmt_list_init(&wc->fmt_list, 16);

    if (get_display_and_registry(woe, wc) != 0)
        goto fail;

    if (!wc->compositor) {
        LOG("Missing wayland compositor\n");
        goto fail;
    }
    if (!wc->viewporter) {
        LOG("Missing wayland viewporter\n");
        goto fail;
    }
    if (!wc->wm_base) {
        LOG("Missing xdg window manager\n");
        goto fail;
    }
    if (!woe->is_egl && !wc->linux_dmabuf_v1) {
        LOG("Missing wayland linux_dmabuf extension\n");
        goto fail;
    }

    if ((wc->surface = wl_compositor_create_surface(wc->compositor)) == NULL) {
        LOG("No Compositor surface\n");
        goto fail;
    }

    wc->viewport = wp_viewporter_get_viewport(wc->viewporter, wc->surface);
    wc->wm_surface = xdg_wm_base_get_xdg_surface(wc->wm_base, wc->surface);

    xdg_surface_add_listener(wc->wm_surface, &xdg_surface_listener, woe);

    wc->wm_toplevel = xdg_surface_get_toplevel(wc->wm_surface);
    xdg_toplevel_add_listener(wc->wm_toplevel, &xdg_toplevel_listener, wc);

    xdg_toplevel_set_title(wc->wm_toplevel,
                           woe->is_egl ? "EGL video" : "Dmabuf video");

    if ((flags & WOUT_FLAG_FULLSCREEN) != 0)
        xdg_toplevel_set_fullscreen(wc->wm_toplevel, NULL);

    if (!wc->decoration_manager) {
        LOG("No decoration manager\n");
    }
    else {
        struct zxdg_toplevel_decoration_v1 *decoration =
            zxdg_decoration_manager_v1_get_toplevel_decoration(wc->decoration_manager, wc->wm_toplevel);
        zxdg_toplevel_decoration_v1_add_listener(decoration, &decoration_listener, wc);
        zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        // decoration destroyed in the callback
    }

    {
        struct wl_region *const region = wl_compositor_create_region(wc->compositor);

        wl_region_add(region, 0, 0, wc->req_w, wc->req_h);
        wl_surface_set_opaque_region(wc->surface, region);
        wl_region_destroy(region);

        LOG("%s: %dx%d\n", __func__, wc->req_w, wc->req_h);
        wc->window_width = wc->req_w;
        wc->window_height = wc->req_h;
    }

    wl_surface_commit(wc->surface);

    wc->pq = pollqueue_new();
    pollqueue_set_pre_post(wc->pq, pollq_pre_cb, pollq_post_cb, wc);

    LOG("<<< %s\n", __func__);

    // Some egl setup must be done on display thread
    if (woe->is_egl) {
        pollqueue_callback_once(wc->pq, do_egl_setup, woe);
        sem_wait(&woe->egl_setup_sem);
        if (woe->egl_setup_fail) {
            LOG("EGL init failed\n");
            goto fail;
        }
    }

    return woe;

fail:
    egl_wayland_out_delete(woe);
    return NULL;
}

wayland_out_env_t*
egl_wayland_out_new(unsigned int flags)
{
    return wayland_out_new(true, flags);
}

wayland_out_env_t*
dmabuf_wayland_out_new(unsigned int flags)
{
    return wayland_out_new(false, flags);
}

