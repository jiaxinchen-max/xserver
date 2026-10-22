/*
 * Xlorie glamor helpers: import a termux-render LorieBuffer (AHardwareBuffer or
 * dma-buf) into a glamor GL texture / pixmap, back the screen pixmap with the
 * shared root buffer, and drive the per-frame GPU flush. See lorie-glamor.c.
 */

#ifndef LORIE_GLAMOR_H
#define LORIE_GLAMOR_H

#include <X11/Xdefs.h>
#include <stdint.h>

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include "scrnintstr.h"
#include "pixmapstr.h"

#include "lorie.h"        /* LorieBuffer (termux/render) */

/* Import a LorieBuffer into a GL texture (mode-aware: AHB native vs dma-buf).
 * Returns the texture name (0 on failure) and the backing EGLImage in *outImage. */
GLuint lorieGlamorImportTexture(LorieBuffer *buffer, EGLImageKHR *outImage);

/* Create a glamor pixmap backed by an already-imported texture. */
PixmapPtr lorieGlamorCreateTexturedPixmap(ScreenPtr screen, int width,
                                          int height, int depth, GLuint texture);

/* Back the screen (root) pixmap with the shared root LorieBuffer on the GPU.
 * Returns the image/texture in the out params for later teardown. */
Bool lorieGlamorBackScreenPixmap(ScreenPtr screen, LorieBuffer *root,
                                 EGLImageKHR *outImage, GLuint *outTexture);

/* Flush pending glamor GL work into the shared buffer and wait for the GPU, so
 * the termux-render consumer samples a complete frame. */
void lorieGlamorFinishFrame(ScreenPtr screen);

/* Red/blue swizzle override hook (XLORIE_AHB_SWIZZLE=rb|none). */
Bool lorieGlamorNeedsRedBlueSwap(uint8_t ahbFormat);

#endif /* LORIE_GLAMOR_H */
