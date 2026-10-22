/*
 * Xlorie EGL/GLES bootstrap for the glamor GPU-accelerated path.
 *
 * Creates an EGL display + GLES context owned by the X server process so that
 * glamor can render the X scene directly into the shared AHardwareBuffer the
 * termux-render process samples.  Two rendering backends are supported and
 * selected by environment (see lorieEglResolveMode):
 *
 *   - system : Android system EGL/GLES (vendor driver); client/root buffers are
 *              imported as EGLImages via EGL_NATIVE_BUFFER_ANDROID.
 *   - zink   : Mesa zink on Turnip; buffers are imported via dma-buf
 *              (EGL_EXT_image_dma_buf_import).
 *
 * This mirrors the KWin Android backend (src/backends/android/android_egl_backend.cpp).
 */

#ifndef LORIE_EGL_H
#define LORIE_EGL_H

#include <X11/Xdefs.h>
#include <stdint.h>

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <android/hardware_buffer.h>

#ifndef EGL_NO_IMAGE_KHR
#define EGL_NO_IMAGE_KHR ((EGLImageKHR) 0)
#endif

#include "scrnintstr.h"

struct glamor_context;

/* Diagnostic: log a dladdr-resolved backtrace of the caller. */
void egl_log_backtrace(void);

enum lorie_gl_mode {
    LORIE_GL_FALLBACK = 0,  /* no GPU: keep the software fb path */
    LORIE_GL_SYSTEM,        /* Android system EGL/GLES, AHB native import */
    LORIE_GL_ZINK,          /* Mesa zink/Turnip, dma-buf import */
    LORIE_GL_LLVMPIPE,      /* Mesa software GL through glamor */
};

/* Resolve XLORIE_GL_MODE (falling back to KWIN_ANDROID_GL_MODE). */
enum lorie_gl_mode lorieEglResolveMode(void);

/* Bring up the EGL display/context for the given mode. Returns FALSE on any
 * failure (the caller must then fall back to the software fb path). */
Bool lorieEglInit(enum lorie_gl_mode mode);
void lorieEglFini(void);

Bool lorieEglActive(void);
enum lorie_gl_mode lorieEglMode(void);
/* TRUE when client/root buffers must be imported as dma-buf (zink), FALSE when
 * they are imported as Android native buffers (system). */
Bool lorieEglUsesDmabuf(void);
/* Import-capability probe results (decides native-AHB vs dma-buf import). */
Bool lorieEglHasAhbImport(void);
Bool lorieEglHasDmabufImport(void);

EGLDisplay lorieEglDisplay(void);
EGLContext lorieEglContext(void);

/* Bind the Xlorie EGL context (pbuffer for system, EGL_NO_SURFACE for zink).
 * Returns FALSE (and logs) when the context cannot be bound -- the GPU path is
 * then paused until it recovers. */
Bool lorieEglMakeCurrentOk(void);
void lorieEglMakeCurrentOwn(void);

/* EGLImage import helpers (context must be current). */
EGLImageKHR lorieEglImageFromAHB(AHardwareBuffer *ahb);
EGLImageKHR lorieEglImageFromDmaBuf(int fd, int width, int height,
                                    int stride, uint32_t drmFormat);
void lorieEglDestroyImage(EGLImageKHR image);

/* Bind an imported EGLImage to a freshly generated GL texture. Returns the
 * texture name (0 on failure). Performs the gfxstream-safe import dance. */
GLuint lorieEglTextureFromImage(EGLImageKHR image);

#endif /* LORIE_EGL_H */
