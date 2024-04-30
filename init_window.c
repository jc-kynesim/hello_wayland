#include "config.h"
#include "init_window.h"

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <wayland-egl.h> // Wayland EGL MUST be included before EGL headers

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include <libdrm/drm_fourcc.h>

#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>

// Local headers
#include "dmabuf_alloc.h"
#include "dmabuf_pool.h"
#include "pollqueue.h"
#include "wayout.h"

#include "freetype/runticker.h"
#include "cube/runcube.h"

//#include "log.h"
#define LOG printf

#define TRACE_ALL 0

typedef struct window_ctx_s {

    struct wl_callback *frame_callback;

    // EGL
    struct wl_egl_window *w_egl_window;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    bool fmt_ok;
    uint32_t last_fmt;
    uint64_t last_mod;

} window_ctx_t;

typedef struct vid_out_env_s {
    atomic_int ref_count;

    window_ctx_t wc;

    wo_env_t * woe;
    wo_window_t * win;
    wo_rect_t win_rect;
    wo_surface_t * vid;
    unsigned int vid_par_num;
    unsigned int vid_par_den;

    int fullscreen;

    bool is_egl;

    struct pollqueue * vid_pq;
    struct dmabufs_ctl * dbsc;
    dmabuf_pool_t * dpool;

#if HAS_RUNCUBE
    runcube_env_t * rce;
#endif
#if HAS_RUNTICKER
    runticker_env_t * rte;
#endif
} vid_out_env_t;

// Structure that holds context whilst waiting for fence release
struct dmabuf_w_env_s {
    int fd;
    AVBufferRef *buf;
    struct polltask *pt;
    window_ctx_t *wc;
};

typedef struct sw_dmabuf_s {
    AVDRMFrameDescriptor desc;
    struct dmabuf_h * dh;
} sw_dmabuf_t;


#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720


// Remove any params from a modifier
static inline uint64_t
canon_mod(const uint64_t m)
{
    return fourcc_mod_is_vendor(m, BROADCOM) ? fourcc_mod_broadcom_mod(m) : m;
}

static const struct {
    enum AVPixelFormat pixfmt;
    uint32_t drm_format;
    uint64_t mod; // 0 = LINEAR
} fmt_table[] = {
    // Monochrome.
#ifdef DRM_FORMAT_R8
    { AV_PIX_FMT_GRAY8,    DRM_FORMAT_R8,      DRM_FORMAT_MOD_LINEAR},
#endif
#ifdef DRM_FORMAT_R16
    { AV_PIX_FMT_GRAY16LE, DRM_FORMAT_R16,     DRM_FORMAT_MOD_LINEAR},
    { AV_PIX_FMT_GRAY16BE, DRM_FORMAT_R16      | DRM_FORMAT_BIG_ENDIAN, DRM_FORMAT_MOD_LINEAR },
#endif
    // <8-bit RGB.
    { AV_PIX_FMT_BGR8,     DRM_FORMAT_BGR233,  DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_RGB555LE, DRM_FORMAT_XRGB1555, DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_RGB555BE, DRM_FORMAT_XRGB1555 | DRM_FORMAT_BIG_ENDIAN, DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_BGR555LE, DRM_FORMAT_XBGR1555, DRM_FORMAT_MOD_LINEAR},
    { AV_PIX_FMT_BGR555BE, DRM_FORMAT_XBGR1555 | DRM_FORMAT_BIG_ENDIAN, DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_RGB565LE, DRM_FORMAT_RGB565,  DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_RGB565BE, DRM_FORMAT_RGB565   | DRM_FORMAT_BIG_ENDIAN, DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_BGR565LE, DRM_FORMAT_BGR565,  DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_BGR565BE, DRM_FORMAT_BGR565   | DRM_FORMAT_BIG_ENDIAN, DRM_FORMAT_MOD_LINEAR },
    // 8-bit RGB.
    { AV_PIX_FMT_RGB24,    DRM_FORMAT_RGB888,   DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_BGR24,    DRM_FORMAT_BGR888,   DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_0RGB,     DRM_FORMAT_BGRX8888, DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_0BGR,     DRM_FORMAT_RGBX8888, DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_RGB0,     DRM_FORMAT_XBGR8888, DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_BGR0,     DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_ARGB,     DRM_FORMAT_BGRA8888, DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_ABGR,     DRM_FORMAT_RGBA8888, DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_RGBA,     DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_BGRA,     DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR },
    // 10-bit RGB.
    { AV_PIX_FMT_X2RGB10LE, DRM_FORMAT_XRGB2101010, DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_X2RGB10BE, DRM_FORMAT_XRGB2101010 | DRM_FORMAT_BIG_ENDIAN, DRM_FORMAT_MOD_LINEAR },
    // 8-bit YUV 4:2:0.
    { AV_PIX_FMT_YUV420P,  DRM_FORMAT_YUV420,  DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_NV12,     DRM_FORMAT_NV12,    DRM_FORMAT_MOD_LINEAR },
    // 8-bit YUV 4:2:2.
    { AV_PIX_FMT_YUYV422,  DRM_FORMAT_YUYV,    DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_YVYU422,  DRM_FORMAT_YVYU,    DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_UYVY422,  DRM_FORMAT_UYVY,    DRM_FORMAT_MOD_LINEAR },
    { AV_PIX_FMT_NONE,     0,                  DRM_FORMAT_MOD_INVALID }
};

uint32_t
fmt_to_drm(enum AVPixelFormat pixfmt, uint64_t * pMod)
{
    unsigned int i;
    for (i = 0; fmt_table[i].pixfmt != AV_PIX_FMT_NONE; ++i) {
        if (fmt_table[i].pixfmt == pixfmt)
            break;
    }
    if (pMod != NULL)
        *pMod = fmt_table[i].mod;
    return fmt_table[i].drm_format;
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
    dbe->pt = NULL;
    dbe->wc = wc;
    return dbe;
}

static void
dmabuf_w_env_free(struct dmabuf_w_env_s *const dbe)
{
    av_buffer_unref(&dbe->buf);
    polltask_delete(&dbe->pt);
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

static wo_rect_t
box_rect(const unsigned int par_num, const unsigned int par_den, const wo_rect_t win_rect)
{
    wo_rect_t r = win_rect;

    if (par_num == 0 || par_den == 0)
        return r;

    if (par_num * win_rect.h < par_den * win_rect.w) {
        // Pillarbox
        r.w = win_rect.h * par_num / par_den;
        r.x = (win_rect.w - r.w) / 2;
    }
    else {
        // Letterbox
        r.h = win_rect.w * par_den / par_num;
        r.y = (win_rect.h - r.h) / 2;
    }
    return r;
}

static void
set_vid_par(vid_out_env_t * const ve, const AVFrame * const frame)
{
    const unsigned int w = av_frame_cropped_width(frame);
    const unsigned int h = av_frame_cropped_height(frame);
    unsigned int par_num = frame->sample_aspect_ratio.num * w;
    unsigned int par_den = frame->sample_aspect_ratio.den * h;

    if (par_den == 0 || par_num == 0) {
        if (((w == 720 || w == 704) && (h == 480 || h == 576)) ||
            ((w == 360 || w == 352) && (h == 240 || h == 288)))
        {
            par_num = 4;
            par_den = 3;
        }
        else
        {
            par_num = w;
            par_den = h;
        }
    }
    ve->vid_par_den = par_den;
    ve->vid_par_num = par_num;
}

static void
w_buffer_release(void *data, wo_fb_t *wofb)
{
    AVBufferRef *buf = data;

    // Sent by the compositor when it's no longer using this buffer
    wo_fb_unref(&wofb);
    av_buffer_unref(&buf);
}

static void
do_display_dmabuf(vid_out_env_t * const ve, AVFrame *const frame)
{
    const AVDRMFrameDescriptor *desc = frame->format == AV_PIX_FMT_DRM_PRIME ?
        (AVDRMFrameDescriptor * ) frame->data[0] :
        &((sw_dmabuf_t *)(frame->buf[0]->data))->desc;
    const uint32_t format = desc->layers[0].format;
    const unsigned int width = av_frame_cropped_width(frame);
    const unsigned int height = av_frame_cropped_height(frame);
    wo_fb_t * wofb = NULL;
    unsigned int n = 0;
    const uint64_t mod = desc->objects[0].format_modifier;
    int i;

#if TRACE_ALL
    LOG("<<< %s\n", __func__);
#endif

    if (!wo_surface_dmabuf_fmt_check(ve->vid, format, mod)) {
        LOG("No support for format %s mod %#"PRIx64"\n", av_fourcc2str(format), mod);
        return;
    }

    {
        struct dmabuf_h * dhs[4];
        size_t offsets[4];
        size_t strides[4];
        unsigned int obj_nos[4];

        for (i = 0; i != desc->nb_objects; ++i) {
            dhs[i] = dmabuf_import(desc->objects[i].fd, desc->objects[i].size);
        }
        for (i = 0, n = 0; i < desc->nb_layers; ++i) {
            int j;
            for (j = 0; j < desc->layers[i].nb_planes; ++j, ++n) {
                const AVDRMPlaneDescriptor *const p = desc->layers[i].planes + j;
                offsets[n] = p->offset;
                strides[n] = p->pitch;
                obj_nos[n] = p->object_index;
            }
        }

        wofb = wo_fb_new_dh(ve->woe, width, height,
                            format, mod,
                            desc->nb_objects, dhs,
                            n, offsets, strides, obj_nos);
    }

    if (wofb == NULL) {
        LOG("Failed to create dmabuf\n");
        return;
    }

    // **** Maybe better to attach buf delete to wofb delete?
    wo_fb_on_release_set(wofb, true, w_buffer_release, av_buffer_ref(frame->buf[0]));

    wo_surface_attach_fb(ve->vid, wofb, box_rect(ve->vid_par_num, ve->vid_par_den, ve->win_rect));
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
do_display_egl(vid_out_env_t * const ve, AVFrame *const frame)
{
    window_ctx_t *const wc = &ve->wc;
    const AVDRMFrameDescriptor *desc = frame->format == AV_PIX_FMT_DRM_PRIME ?
        (AVDRMFrameDescriptor * ) frame->data[0] :
        &((sw_dmabuf_t *)(frame->buf[0]->data))->desc;
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
#if 0
    if (wc->req_w != wc->window_width || wc->req_h != wc->window_height) {
        LOG("%s: Resize %dx%d -> %dx%d\n", __func__, wc->window_width, wc->window_height, wc->req_w, wc->req_h);
        wl_egl_window_resize(wc->w_egl_window, wc->req_w, wc->req_h, 0, 0);
        wc->window_width = wc->req_w;
        wc->window_height = wc->req_h;
        wp_viewport_set_destination(wc->vid.viewport, wc->req_w, wc->req_h);
        wl_surface_commit(wc->vid.surface);
    }
#endif
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
        dbe->pt = polltask_new(ve->vid_pq, desc->objects[0].fd, POLLOUT, dmabuf_fence_release_cb, dbe);
        pollqueue_add_task(dbe->pt, -1);
    }

    // *** EGL plane resize missing - this is a cheat
    wo_surface_dst_pos_set(ve->vid, box_rect(ve->vid_par_num, ve->vid_par_den, ve->win_rect));
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
CreateEGLContext(vid_out_env_t * const ve)
{
    window_ctx_t *const wc = &ve->wc;
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
    EGLDisplay display = eglGetDisplay(wo_env_display(ve->woe));
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

    wc->w_egl_window = wo_surface_egl_window_create(ve->vid, (wo_rect_t){0,0,WINDOW_WIDTH,WINDOW_HEIGHT});

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

static int
do_egl_setup(vid_out_env_t *const vc)
{
    window_ctx_t *const wc = &vc->wc;

    if (!CreateEGLContext(vc))
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
    return 0;

fail:
    return -1;
}

// ---------------------------------------------------------------------------
//
// get_buffer2 for s/w decoders

static void sw_dmabuf_free(void *opaque, uint8_t *data)
{
    sw_dmabuf_t * const swd = opaque;
    (void)data;
    dmabuf_unref(&swd->dh);
    free(swd);
}

static AVBufferRef *
sw_dmabuf_make(struct AVCodecContext * const avctx, vid_out_env_t * const vc, const AVFrame * const frame)
{
    sw_dmabuf_t * const swd = calloc(1, sizeof(*swd));
    AVBufferRef * buf;
    int linesize[4];
    int w = frame->width;
    int h = frame->height;
    int unaligned;
    ptrdiff_t linesize1[4];
    size_t size[4];
    int stride_align[AV_NUM_DATA_POINTERS];
    size_t total_size;
    unsigned int i;
    unsigned int planes;
    uint64_t drm_mod;
    const uint32_t drm_fmt = fmt_to_drm(frame->format, &drm_mod);
    int ret;

    if (swd == NULL)
        return NULL;

    if (drm_fmt == 0 ||
        (buf = av_buffer_create((uint8_t*)swd, sizeof(*swd), sw_dmabuf_free, swd, 0)) == NULL) {
        free(swd);
        return NULL;
    }

    // Size & align code taken from libavcodec/getbuffer.c:update_frame_pool
    avcodec_align_dimensions2(avctx, &w, &h, stride_align);

    do {
        // NOTE: do not align linesizes individually, this breaks e.g. assumptions
        // that linesize[0] == 2*linesize[1] in the MPEG-encoder for 4:2:2
        ret = av_image_fill_linesizes(linesize, avctx->pix_fmt, w);
        if (ret < 0)
            goto fail;
        // increase alignment of w for next try (rhs gives the lowest bit set in w)
        w += w & ~(w - 1);

        unaligned = 0;
        for (i = 0; i < 4; i++)
            unaligned |= linesize[i] % stride_align[i];
    } while (unaligned);

    for (i = 0; i < 4; i++)
        linesize1[i] = linesize[i];
    ret = av_image_fill_plane_sizes(size, avctx->pix_fmt, h, linesize1);
    if (ret < 0)
        goto fail;

    total_size = 0;
    for (planes = 0; planes != 4 && size[planes] != 0; ++planes)
        total_size += size[planes];

    if ((swd->dh = dmabuf_pool_fb_new(vc->dpool, total_size)) == NULL) {
        fprintf(stderr, "dmabuf_alloc failed\n");
        goto fail;
    }

    swd->desc.nb_objects = 1;
    swd->desc.objects[0].fd = dmabuf_fd(swd->dh);
    swd->desc.objects[0].size = dmabuf_size(swd->dh);
    swd->desc.objects[0].format_modifier = drm_mod;
    swd->desc.nb_layers = 1;
    swd->desc.layers[0].format = drm_fmt;
    swd->desc.layers[0].nb_planes = planes;
    total_size = 0;
    for (i = 0; i != planes; ++i) {
        AVDRMPlaneDescriptor *const p = swd->desc.layers[0].planes + i;
        p->object_index = 0;
        p->offset = total_size;
        p->pitch = linesize1[i];
        total_size += size[i];
    }

    return buf;

fail:
    // swd is freed by freeing buf
    av_buffer_unref(&buf);
    fprintf(stderr, "WTF\n");
    return NULL;
}

static void
sw_dmabuf_frame_fill(AVFrame * const frame, const AVBufferRef * const buf)
{
    const sw_dmabuf_t *const swd = (sw_dmabuf_t *)buf->data;
    uint8_t * const data = dmabuf_map(swd->dh);
    int i;

    for (i = 0; i != swd->desc.layers[0].nb_planes; ++i) {
        frame->data[i] = data + swd->desc.layers[0].planes[i].offset;
        frame->linesize[i] = swd->desc.layers[0].planes[i].pitch;
    }
}

// Assumes drmprime_out_env in s->opaque
int vidout_wayland_get_buffer2(struct AVCodecContext *s, AVFrame *frame, int flags)
{
    vid_out_env_t * const vc = s->opaque;
    (void)flags;

    frame->opaque = vc;
    if ((frame->buf[0] = sw_dmabuf_make(s, vc, frame)) == NULL)
        return -1;
    sw_dmabuf_frame_fill(frame, frame->buf[0]);
    return 0;
}

// ---------------------------------------------------------------------------
//
// External entry points

void
vidout_wayland_modeset(vid_out_env_t *vc, int w, int h, AVRational frame_rate)
{
    (void)vc;
    (void)w;
    (void)h;
    (void)frame_rate;
    /* NIF */
}

int
vidout_wayland_display(vid_out_env_t *vc, AVFrame *src_frame)
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
    else if (src_frame->opaque == vc) {
        frame = av_frame_alloc();
        av_frame_ref(frame, src_frame);
    }
    else {
        LOG("Frame (format=%d) not DRM_PRiME\n", src_frame->format);
        return AVERROR(EINVAL);
    }

    set_vid_par(vc, frame);
    if (vc->is_egl)
        do_display_egl(vc, frame);
    else
        do_display_dmabuf(vc, frame);
    av_frame_free(&frame);

    return 0;
}


void
vidout_wayland_delete(vid_out_env_t *vc)
{
    if (vc == NULL)
        return;

    LOG("<<< %s\n", __func__);

#if HAS_RUNCUBE
    runcube_way_stop(&vc->rce);
#endif
#if HAS_RUNTICKER
    runticker_stop(&vc->rte);
#endif

    // **** EGL teardown

    pollqueue_finish(&vc->vid_pq);

    wo_surface_detach_fb(vc->vid);

    wo_surface_unref(&vc->vid);
    wo_window_unref(&vc->win);
    wo_env_unref(&vc->woe);

    dmabuf_pool_kill(&vc->dpool);
    dmabufs_ctl_unref(&vc->dbsc);
    free(vc);
}

static void
vid_resize_dmabuf_cb(void * v, wo_surface_t * wos, const wo_rect_t win_pos)
{
    vid_out_env_t *const ve = v;

    ve->win_rect = win_pos;
    wo_surface_dst_pos_set(wos, box_rect(ve->vid_par_num, ve->vid_par_den, ve->win_rect));
}

static vid_out_env_t*
wayland_out_new(const bool is_egl, const unsigned int flags)
{
    vid_out_env_t *const ve = calloc(1, sizeof(*ve));

    LOG("<<< %s\n", __func__);

    ve->is_egl = is_egl;

    ve->dbsc = dmabufs_ctl_new();
    ve->dpool = dmabuf_pool_new_dmabufs(ve->dbsc, 32);

    ve->vid_pq = pollqueue_new();

    ve->woe = wo_env_new_default();
    ve->win = wo_window_new(ve->woe, (flags & WOUT_FLAG_FULLSCREEN) != 0,
                            (wo_rect_t) {0, 0, WINDOW_WIDTH, WINDOW_HEIGHT},
                            ve->is_egl ? "EGL video" : "Dmabuf video");
    ve->vid = wo_make_surface_z(ve->win, NULL, 10);
    ve->win_rect = wo_window_size(ve->win);
    wo_surface_dst_pos_set(ve->vid, ve->win_rect);

    if (!ve->is_egl)
        wo_surface_on_win_resize_set(ve->vid, vid_resize_dmabuf_cb, ve);

    // Some egl setup must be done on display thread
    if (ve->is_egl) {
        if (do_egl_setup(ve) != 0) {
            LOG("EGL init failed\n");
            goto fail;
        }
    }

    LOG(">>> %s\n", __func__);

    return ve;

fail:
    vidout_wayland_delete(ve);
    return NULL;
}

vid_out_env_t*
vidout_wayland_new(unsigned int flags)
{
    return wayland_out_new(true, flags);
}

vid_out_env_t*
dmabuf_wayland_out_new(unsigned int flags)
{
    return wayland_out_new(false, flags);
}

#if HAS_RUNTICKER
void vidout_wayland_runticker(vid_out_env_t * ve, const char * text)
{
    static const char fontfile[] = "/usr/share/fonts/truetype/freefont/FreeSerif.ttf";
    wo_rect_t r = wo_window_size(ve->win);
    ve->rte = runticker_start(ve->win, r.w / 10, r.h * 8 / 10, r.w * 8 / 10, r.h / 10, text, fontfile);
}
#endif

#if HAS_RUNCUBE
void vidout_wayland_runcube(vid_out_env_t * ve)
{
    wo_rect_t r = wo_window_size(ve->win);
    unsigned int w = r.w > r.h ? r.h : r.w;
    ve->rce = runcube_way_start(ve->win, &(wo_rect_t){r.w / 10, r.h / 10, w / 2, w / 2});
}
#endif
