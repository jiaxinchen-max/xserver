/*
 * Xlorie EGL/GLES bootstrap for the glamor GPU path. See lorie-egl.h.
 *
 * Adapted from:
 *   - packaging/xwayland-ahardwarebuffer-glamor.patch (xwl_glamor_ahb_init_egl)
 *   - src/backends/android/android_egl_backend.cpp (initializeEgl / import)
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>
#include <unistd.h>

#include "os.h"

#include "lorie-egl.h"
#include "log.h"

#include "glamor.h"
#include "glamor_context.h"

/* ---- EGL/GL enum fallbacks (keep self-contained across libepoxy variants) -- */
#ifndef EGL_PLATFORM_ANDROID_KHR
#define EGL_PLATFORM_ANDROID_KHR 0x3141
#endif
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#ifndef EGL_NO_CONFIG_KHR
#define EGL_NO_CONFIG_KHR ((EGLConfig) 0)
#endif
#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif
#ifndef EGL_IMAGE_PRESERVED_KHR
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#endif
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif

typedef EGLClientBuffer (EGLAPIENTRYP PFN_eglGetNativeClientBufferANDROID)(
    const struct AHardwareBuffer *buffer);

static struct {
    EGLDisplay display;
    EGLContext context;
    EGLConfig config;
    EGLSurface pbuffer;         /* EGL_NO_SURFACE in zink/llvmpipe modes */
    enum lorie_gl_mode mode;
    Bool active;
    Bool dmabuf;               /* import client/root buffers as dma-buf */
    Bool has_ahb_import;       /* EGL_ANDROID_image_native_buffer */
    Bool has_dmabuf_import;    /* EGL_EXT_image_dma_buf_import */
    PFN_eglGetNativeClientBufferANDROID getNativeClientBuffer;
} egl;

/* --------------------------------------------------------------------------- */

static enum lorie_gl_mode
mode_from_string(const char *value)
{
    if (!value || !*value)
        return LORIE_GL_FALLBACK;
    if (!strcasecmp(value, "system"))
        return LORIE_GL_SYSTEM;
    if (!strcasecmp(value, "zink") || !strcasecmp(value, "turnip"))
        return LORIE_GL_ZINK;
    if (!strcasecmp(value, "llvmpipe") || !strcasecmp(value, "software"))
        return LORIE_GL_LLVMPIPE;
    return LORIE_GL_FALLBACK;
}

enum lorie_gl_mode
lorieEglResolveMode(void)
{
    const char *value = getenv("XLORIE_GL_MODE");
    enum lorie_gl_mode mode;

    if (!value || !*value)
        value = getenv("KWIN_ANDROID_GL_MODE");

    mode = mode_from_string(value);
    if (mode == LORIE_GL_FALLBACK) {
        if (value && *value)
            lorieLog("Xlorie: unrecognized GL mode \"%s\"; run "
                     "kwin_android_select_rendering first. Using software fb.\n",
                     value);
        else
            lorieLog("Xlorie: no XLORIE_GL_MODE/KWIN_ANDROID_GL_MODE set; "
                     "defaulting to system GLES.\n");
        /* Mirror KWin's default: unset -> system. */
        if (!value || !*value)
            return LORIE_GL_SYSTEM;
    }
    return mode;
}

static Bool
has_egl_extension(EGLDisplay dpy, const char *name)
{
    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    return exts && epoxy_extension_in_string(exts, name);
}

static EGLDisplay
get_platform_display(enum lorie_gl_mode mode)
{
    const char *client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    EGLDisplay dpy = EGL_NO_DISPLAY;

    if (client_exts &&
        epoxy_extension_in_string(client_exts, "EGL_EXT_platform_base")) {
        if (mode == LORIE_GL_SYSTEM &&
            epoxy_extension_in_string(client_exts, "EGL_KHR_platform_android"))
            dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANDROID_KHR,
                                           EGL_DEFAULT_DISPLAY, NULL);
        else if (mode != LORIE_GL_SYSTEM &&
                 epoxy_extension_in_string(client_exts,
                                           "EGL_MESA_platform_surfaceless"))
            dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                           EGL_DEFAULT_DISPLAY, NULL);
    }

    if (dpy == EGL_NO_DISPLAY)
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);

    return dpy;
}

Bool
lorieEglInit(enum lorie_gl_mode mode)
{
    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    static const EGLint pbuffer_attribs[] = {
        EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE,
    };
    const EGLint ctx_versions[] = { 3, 2 };
    EGLint n = 0;
    unsigned i;

    if (egl.active)
        return TRUE;
    if (mode == LORIE_GL_FALLBACK)
        return FALSE;

    memset(&egl, 0, sizeof(egl));
    egl.mode = mode;
    egl.pbuffer = EGL_NO_SURFACE;
    egl.dmabuf = (mode != LORIE_GL_SYSTEM);

    egl.display = get_platform_display(mode);
    if (egl.display == EGL_NO_DISPLAY) {
        lorieLog("Xlorie EGL: eglGetDisplay failed\n");
        return FALSE;
    }
    if (!eglInitialize(egl.display, NULL, NULL)) {
        lorieLog("Xlorie EGL: eglInitialize failed (0x%x)\n", eglGetError());
        return FALSE;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        lorieLog("Xlorie EGL: eglBindAPI(ES) failed\n");
        goto fail;
    }

    if (mode == LORIE_GL_SYSTEM) {
        /* Android system EGL has no surfaceless context: use a 1x1 pbuffer. */
        if (!eglChooseConfig(egl.display, config_attribs, &egl.config, 1, &n) ||
            n < 1) {
            lorieLog("Xlorie EGL: eglChooseConfig failed\n");
            goto fail;
        }
        egl.pbuffer = eglCreatePbufferSurface(egl.display, egl.config,
                                              pbuffer_attribs);
        if (egl.pbuffer == EGL_NO_SURFACE) {
            lorieLog("Xlorie EGL: eglCreatePbufferSurface failed (0x%x)\n",
                     eglGetError());
            goto fail;
        }
    } else {
        /* Mesa (zink/llvmpipe): configless surfaceless context. */
        egl.config = EGL_NO_CONFIG_KHR;
        egl.pbuffer = EGL_NO_SURFACE;
    }

    for (i = 0; i < sizeof(ctx_versions) / sizeof(ctx_versions[0]); i++) {
        const EGLint ctx_attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, ctx_versions[i], EGL_NONE,
        };
        egl.context = eglCreateContext(egl.display, egl.config, EGL_NO_CONTEXT,
                                       ctx_attribs);
        if (egl.context != EGL_NO_CONTEXT)
            break;
    }
    if (egl.context == EGL_NO_CONTEXT) {
        lorieLog("Xlorie EGL: eglCreateContext failed (0x%x)\n", eglGetError());
        goto fail;
    }

    if (!eglMakeCurrent(egl.display, egl.pbuffer, egl.pbuffer, egl.context)) {
        lorieLog("Xlorie EGL: eglMakeCurrent failed (0x%x)\n", eglGetError());
        goto fail;
    }

    /* Import capabilities: the termux-render client may hand us either a native
     * AHB (TERMUX_RENDER_AHB_RECEIVE=native) or a dma-buf fd (default), so probe
     * both and require at least one. */
    egl.has_ahb_import = has_egl_extension(egl.display,
                                           "EGL_ANDROID_image_native_buffer");
    egl.has_dmabuf_import = has_egl_extension(egl.display,
                                              "EGL_EXT_image_dma_buf_import");
    if (!egl.has_ahb_import && !egl.has_dmabuf_import) {
        lorieLog("Xlorie EGL: neither EGL_ANDROID_image_native_buffer nor "
                 "EGL_EXT_image_dma_buf_import available\n");
        goto fail;
    }
    if (egl.has_ahb_import)
        egl.getNativeClientBuffer = (PFN_eglGetNativeClientBufferANDROID)
            eglGetProcAddress("eglGetNativeClientBufferANDROID");
    if (!epoxy_has_gl_extension("GL_OES_EGL_image")) {
        lorieLog("Xlorie EGL: GL_OES_EGL_image missing\n");
        goto fail;
    }

    /* Arm the glamor-core GLES/Turnip relaxations (see glamor patch). */
    setenv("XLORIE_GLAMOR", "1", 1);

    egl.active = TRUE;
    lorieLog("Xlorie EGL up: mode=%s display=%p renderer=\"%s\" version=\"%s\"\n",
             egl.dmabuf ? (mode == LORIE_GL_ZINK ? "zink" : "llvmpipe") : "system",
             (void *) egl.display,
             (const char *) glGetString(GL_RENDERER),
             (const char *) glGetString(GL_VERSION));
    return TRUE;

fail:
    lorieEglFini();
    return FALSE;
}

void
egl_log_backtrace(void)
{
    /* bionic has no backtrace(); resolve return addresses via dladdr. */
    void *frames[] = {
        __builtin_return_address(0),
        __builtin_return_address(1),
        __builtin_return_address(2),
        __builtin_return_address(3),
    };

    for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        Dl_info info;

        if (!frames[i])
            break;
        if (dladdr(frames[i], &info)) {
            lorieLog("  bt[%zu] %p off=%p %s%s%p\n", i, frames[i],
                     (void *) ((char *) frames[i] - (char *) info.dli_fbase),
                     info.dli_sname ? info.dli_sname : "?",
                     info.dli_sname ? "+" : "",
                     (void *) ((char *) frames[i] - (char *) info.dli_saddr));
        } else {
            lorieLog("  bt[%zu] %p (no symbol)\n", i, frames[i]);
        }
    }
}

void
lorieEglFini(void)
{
    if (egl.display != EGL_NO_DISPLAY) {
        lorieLog("Xlorie EGL: lorieEglFini terminating display %p\n",
                 (void *) egl.display);
        egl_log_backtrace();
        eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (egl.context != EGL_NO_CONTEXT)
            eglDestroyContext(egl.display, egl.context);
        if (egl.pbuffer != EGL_NO_SURFACE)
            eglDestroySurface(egl.display, egl.pbuffer);
        eglTerminate(egl.display);
    }
    memset(&egl, 0, sizeof(egl));
}

Bool lorieEglActive(void) { return egl.active; }
enum lorie_gl_mode lorieEglMode(void) { return egl.mode; }
Bool lorieEglUsesDmabuf(void) { return egl.dmabuf; }
Bool lorieEglHasAhbImport(void) { return egl.has_ahb_import; }
Bool lorieEglHasDmabufImport(void) { return egl.has_dmabuf_import; }
EGLDisplay lorieEglDisplay(void) { return egl.display; }
EGLContext lorieEglContext(void) { return egl.context; }

void
lorieEglMakeCurrentOwn(void)
{
    (void) lorieEglMakeCurrentOk();
}

/* glamor make-current callback (matches glamor_context.make_current). */
static void
lorieGlamorMakeCurrent(struct glamor_context *glamor_ctx)
{
    static Bool broken = FALSE;

    if (!eglMakeCurrent(glamor_ctx->display, glamor_ctx->drawable,
                        glamor_ctx->drawable, glamor_ctx->ctx)) {
        /* The vendor (system) EGL display can be invalidated by Android
         * power/surface transitions (observed EGL_BAD_DISPLAY on screen
         * sleep/wake on a Mi 11). Do not FatalError: log once and keep the
         * server alive; the frame flush is suppressed while broken. */
        if (!broken) {
            broken = TRUE;
            lorieLog("Xlorie: glamor eglMakeCurrent failed (0x%x); "
                     "display=%p ctx=%p surf=%p curdisp=%p\n",
                     eglGetError(), (void *) glamor_ctx->display,
                     (void *) glamor_ctx->ctx, glamor_ctx->drawable,
                     (void *) eglGetCurrentDisplay());
            lorieLog("  eglQueryString(display)=%s\n",
                     eglQueryString(glamor_ctx->display, EGL_VERSION));
            egl_log_backtrace();
        }
        return;
    }
    if (broken) {
        broken = FALSE;
        lorieLog("Xlorie: glamor EGL recovered\n");
    }
}

Bool
lorieEglMakeCurrentOk(void)
{
    if (!egl.active)
        return FALSE;

    if (!eglMakeCurrent(egl.display, egl.pbuffer, egl.pbuffer, egl.context)) {
        lorieLog("Xlorie: eglMakeCurrent(own) failed (0x%x)\n", eglGetError());
        return FALSE;
    }
    return TRUE;
}

/*
 * Override of the weak glamor_egl_screen_init (glamor_egl_stubs.c). glamor_init()
 * calls this to learn our EGL context; we must NOT link glamor_egl_stubs.
 */
void
glamor_egl_screen_init(ScreenPtr screen, struct glamor_context *glamor_ctx)
{
    (void) screen;
    glamor_ctx->display = egl.display;
    glamor_ctx->ctx = egl.context;
    glamor_ctx->drawable = egl.pbuffer;   /* EGLSurface field is "drawable" here */
    glamor_ctx->make_current = lorieGlamorMakeCurrent;
    lorieGlamorMakeCurrent(glamor_ctx);
}

/*
 * glamor core (libglamor) references these glamor_egl_* exporters; they live in
 * glamor_egl.c (GBM, Xorg) or glamor_egl_stubs.c. We provide our own
 * glamor_egl_screen_init above and do NOT link glamor_egl_stubs (that would
 * duplicate glamor_egl_screen_init), so we must supply the remaining stubs here.
 * lorie exports DRI3 buffers via its own lorieDri3Fd*FromPixmap (LorieBuffer),
 * so these can safely report "unsupported".
 */
int
glamor_egl_fd_name_from_pixmap(ScreenPtr screen, PixmapPtr pixmap,
                               CARD16 *stride, CARD32 *size)
{
    (void) screen; (void) pixmap; (void) stride; (void) size;
    return -1;
}

int
glamor_egl_fds_from_pixmap(ScreenPtr screen, PixmapPtr pixmap, int *fds,
                           uint32_t *offsets, uint32_t *strides,
                           uint64_t *modifier)
{
    (void) screen; (void) pixmap; (void) fds; (void) offsets;
    (void) strides; (void) modifier;
    return 0;
}

int
glamor_egl_fd_from_pixmap(ScreenPtr screen, PixmapPtr pixmap,
                          CARD16 *stride, CARD32 *size)
{
    (void) screen; (void) pixmap; (void) stride; (void) size;
    return -1;
}

EGLImageKHR
lorieEglImageFromAHB(AHardwareBuffer *ahb)
{
    EGLClientBuffer client;
    const EGLint attribs[] = {
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE,
    };

    if (!egl.active || !egl.has_ahb_import || !ahb || !egl.getNativeClientBuffer)
        return EGL_NO_IMAGE_KHR;

    client = egl.getNativeClientBuffer(ahb);
    if (!client) {
        lorieLog("Xlorie: eglGetNativeClientBufferANDROID returned NULL\n");
        return EGL_NO_IMAGE_KHR;
    }
    return eglCreateImageKHR(egl.display, EGL_NO_CONTEXT,
                             EGL_NATIVE_BUFFER_ANDROID, client, attribs);
}

EGLImageKHR
lorieEglImageFromDmaBuf(int fd, int width, int height, int stride,
                        uint32_t drmFormat)
{
    const EGLint attribs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint) drmFormat,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
        EGL_NONE,
    };

    if (!egl.active || fd < 0)
        return EGL_NO_IMAGE_KHR;

    return eglCreateImageKHR(egl.display, EGL_NO_CONTEXT,
                             EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer) NULL,
                             attribs);
}

void
lorieEglDestroyImage(EGLImageKHR image)
{
    if (egl.active && image != EGL_NO_IMAGE_KHR)
        eglDestroyImageKHR(egl.display, image);
}

GLuint
lorieEglTextureFromImage(EGLImageKHR image)
{
    GLuint texture = 0;

    if (!egl.active || image == EGL_NO_IMAGE_KHR)
        return 0;

    /* gfxstream: generate the texture name BEFORE binding the image, and
     * re-make-current after eglCreateImageKHR perturbed GL dispatch. */
    lorieEglMakeCurrentOwn();
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    while (glGetError() != GL_NO_ERROR) { }
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES) image);
    if (glGetError() != GL_NO_ERROR) {
        lorieLog("Xlorie: glEGLImageTargetTexture2DOES failed\n");
        glDeleteTextures(1, &texture);
        return 0;
    }
    return texture;
}
