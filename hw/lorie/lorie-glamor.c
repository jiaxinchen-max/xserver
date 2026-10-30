/*
 * Xlorie glamor import helpers. See lorie-glamor.h.
 *
 * Adapted from packaging/xwayland-ahardwarebuffer-glamor.patch
 * (xwl_glamor_ahb_wrap_pixmap) and src/backends/android/android_egl_backend.cpp.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "lorie-glamor.h"
#include "lorie-egl.h"
#include "log.h"

#include "glamor.h"
#include "drm_fourcc.h"

/* termux AHB format codes (android/hardware_buffer.h + termux buffer.h). */
#ifndef AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM
#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1
#endif
#ifndef AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM
#define AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM 2
#endif
#ifndef AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM
#define AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 5
#endif

static uint32_t
drm_fourcc_for_ahb(uint8_t ahbFormat)
{
    switch (ahbFormat) {
    case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
        return DRM_FORMAT_ABGR8888;
    case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
        return DRM_FORMAT_XBGR8888;
    case AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM:
    default:
        return DRM_FORMAT_ARGB8888;
    }
}

Bool
lorieGlamorNeedsRedBlueSwap(uint8_t ahbFormat)
{
    const char *value = getenv("XLORIE_AHB_SWIZZLE");

    (void) ahbFormat;
    if (value && (!strcasecmp(value, "rb") || !strcasecmp(value, "swap")))
        return TRUE;
    /* Default: termux-render already composites the existing root format with
     * correct colors, so assume identity. A local shader probe can be added
     * here if a device shows swapped R/B. */
    return FALSE;
}

GLuint
lorieGlamorImportTexture(LorieBuffer *buffer, EGLImageKHR *outImage)
{
    const LorieBuffer_Desc *desc;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture;
    const char *receive = getenv("TERMUX_RENDER_AHB_RECEIVE");

    if (outImage)
        *outImage = EGL_NO_IMAGE_KHR;
    if (!buffer || !lorieEglActive())
        return 0;

    desc = LorieBuffer_description(buffer);

    /* The receiver parameter selects one import ABI. An import failure must
     * not silently switch the selected buffer transport. */
    if (receive && !strcmp(receive, "native")) {
        if (desc->type != LORIEBUFFER_AHARDWAREBUFFER || !desc->buffer) {
            lorieLog("Xlorie glamor: native receive mode requires an AHB handle\n");
            return 0;
        }
        image = lorieEglImageFromAHB(desc->buffer);
    } else if (receive && !strcmp(receive, "dmabuf")) {
        int fd = LorieBuffer_dupDmaBufFd(buffer);
        if (fd < 0) {
            lorieLog("Xlorie glamor: dmabuf receive mode requires a dma-buf fd\n");
            return 0;
        }
        image = lorieEglImageFromDmaBuf(fd, desc->width, desc->height,
                                        desc->stride * 4,
                                        drm_fourcc_for_ahb(desc->format));
        close(fd);   /* EGL dups internally */
    } else {
        lorieLog("Xlorie glamor: explicit TERMUX_RENDER_AHB_RECEIVE=native|dmabuf required\n");
        return 0;
    }

    if (image == EGL_NO_IMAGE_KHR) {
        lorieLog("Xlorie glamor: eglCreateImageKHR failed\n");
        return 0;
    }

    texture = lorieEglTextureFromImage(image);
    if (!texture) {
        lorieEglDestroyImage(image);
        return 0;
    }

    if (outImage)
        *outImage = image;
    else
        /* caller does not keep the image; it stays alive via the texture */
        (void) image;
    return texture;
}

PixmapPtr
lorieGlamorCreateTexturedPixmap(ScreenPtr screen, int width, int height,
                                int depth, GLuint texture)
{
    PixmapPtr pixmap;

    pixmap = glamor_create_pixmap(screen, width, height, depth,
                                  GLAMOR_CREATE_PIXMAP_NO_TEXTURE);
    if (!pixmap)
        return NULL;

    /* Type first, then texture (see lorieGlamorBackScreenPixmap). */
    glamor_set_pixmap_type(pixmap, GLAMOR_TEXTURE_DRM);
    if (!glamor_set_pixmap_texture(pixmap, texture)) {
        screen->DestroyPixmap(pixmap);
        return NULL;
    }
    return pixmap;
}

Bool
lorieGlamorBackScreenPixmap(ScreenPtr screen, LorieBuffer *root,
                            EGLImageKHR *outImage, GLuint *outTexture)
{
    PixmapPtr pixmap;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture;
    const char *receive = getenv("TERMUX_RENDER_AHB_RECEIVE");

    if (outImage)
        *outImage = EGL_NO_IMAGE_KHR;
    if (outTexture)
        *outTexture = 0;

    texture = lorieGlamorImportTexture(root, &image);
    if (!texture)
        return FALSE;

    pixmap = screen->GetScreenPixmap(screen);
    if (!pixmap) {
        lorieEglDestroyImage(image);
        return FALSE;
    }

    /* glamor_pixmap_attach_fbo() only marks a pixmap as FBO-backed (gl_fbo = GLAMOR_FBO_NORMAL)
     * when its type is already a texture type at attach time. The screen pixmap starts as
     * GLAMOR_MEMORY (it is created 0x0 by miCreateScreenResources), so the type must be set
     * BEFORE glamor_set_pixmap_texture(), as upstream does in glamor_egl.c. Setting it afterwards
     * leaves gl_fbo UNATTACHED: every glamor operation then takes its CPU fallback and draws
     * into the scratch framebuffer instead of the shared AHB texture. */
    glamor_set_pixmap_type(pixmap, GLAMOR_TEXTURE_DRM);
    if (!glamor_set_pixmap_texture(pixmap, texture)) {
        glamor_set_pixmap_type(pixmap, GLAMOR_MEMORY);
        lorieEglDestroyImage(image);
        return FALSE;
    }

    if (outImage)
        *outImage = image;
    if (outTexture)
        *outTexture = texture;
    return TRUE;
}

void
lorieGlamorFinishFrame(ScreenPtr screen)
{
    if (!lorieEglActive() || !lorieEglMakeCurrentOk())
        return;              /* GPU paused (e.g. EGL display invalidated) */
    glamor_block_handler(screen);
    glamor_finish(screen);   /* make-current + glFinish: frame done in the AHB */
}
