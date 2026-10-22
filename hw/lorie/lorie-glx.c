/*
 * Xlorie minimal GLX provider.
 *
 * The stock xorg GLX init probes the always-present DRI swrast provider,
 * which loads Mesa into the server process and runs an in-process DRI3
 * loader that blocks on client sync fences (cross-process deadlock with the
 * waiting client). This provider replaces it with a PROTOCOL-ONLY screen:
 * fbconfigs from a small static table, contexts/drawables as shells. Direct
 * rendering is entirely client-side (DRI3 dma-buf imports into the glamor
 * screen), so the server never touches client fences or a GL driver.
 *
 * Register with GlxPushProvider() before xorgGlxCreateVendor(): the provider
 * stack is probed head-first, so this one wins over the swrast fallback.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "glxserver.h"
#include "glxcontext.h"
#include "glxdrawable.h"
#include "glxscreens.h"
#include "glxext.h"

#include "lorie.h"
#include "log.h"

#ifndef GLX_DOUBLE_BUFFER
#define GLX_DOUBLE_BUFFER 5
#define GLX_SINGLE_BUFFER 6
#endif
#ifndef GLX_TRUE_COLOR
#define GLX_TRUE_COLOR 0x8002   /* GLX protocol visual type */
#endif
#ifndef GLX_WINDOW_BIT
#define GLX_WINDOW_BIT 0x00000001
#define GLX_PIXMAP_BIT 0x00000002
#endif
#ifndef GLX_RGBA_BIT
#define GLX_RGBA_BIT 0x00000001
#endif
#ifndef GLX_NONE_EXT
#define GLX_NONE_EXT 0x8000
#endif
#ifndef GLX_SWAP_UNDEFINED_OML
#define GLX_SWAP_UNDEFINED_OML 0x8060
#endif

/* ------------------------------------------------------------------ */

static void
lorieGlxConfigInit(__GLXconfig *c, int doubleBuffer, int alphaBits,
                   int rgbBits)
{
    memset(c, 0, sizeof(*c));
    c->doubleBufferMode = doubleBuffer;
    c->stereoMode = 0;
    c->redBits = c->greenBits = c->blueBits = 8;
    c->alphaBits = alphaBits;
    c->rgbBits = rgbBits;
    /* Masks must match the X TrueColor visual (0xff0000/0xff00/0xff),
     * otherwise pickFBConfig rejects the config for every visual. */
    c->redMask = 0x00ff0000;
    c->greenMask = 0x0000ff00;
    c->blueMask = 0x000000ff;
    c->alphaMask = alphaBits ? 0xff000000 : 0;
    c->indexBits = 0;
    c->depthBits = rgbBits == 32 ? 24 : 24;
    c->stencilBits = 0;
    c->numAuxBuffers = 0;
    c->level = 0;
    c->visualType = GLX_TRUE_COLOR;
    c->visualRating = GLX_NONE_EXT;
    c->transparentPixel = 0;
    c->transparentIndex = 0;
    c->sampleBuffers = 0;
    c->samples = 0;
    c->drawableType = GLX_WINDOW_BIT | GLX_PIXMAP_BIT;
    c->renderType = GLX_RGBA_BIT;
    c->maxPbufferWidth = c->maxPbufferHeight = 0;
    c->maxPbufferPixels = 0;
    c->optimalPbufferWidth = c->optimalPbufferHeight = 0;
    c->visualSelectGroup = 0;
    c->swapMethod = GLX_SWAP_UNDEFINED_OML;
    c->bindToTextureRgb = c->bindToTextureRgba = 0;
    c->bindToMipmapTexture = 0;
    c->bindToTextureTargets = 0;
    c->yInverted = 1;
}

/* Context shells: nothing to create or destroy beyond the struct itself. */
static void
lorieGlxContextDestroy(__GLXcontext *context)
{
    free(context);
}

static int
lorieGlxContextMakeCurrent(__GLXcontext *context)
{
    (void) context;
    return Success;
}

static int
lorieGlxContextLoseCurrent(__GLXcontext *context)
{
    (void) context;
    return Success;
}

static int
lorieGlxContextCopy(__GLXcontext *dst, __GLXcontext *src,
                    unsigned long mask)
{
    (void) dst; (void) src; (void) mask;
    return Success;
}

static Bool
lorieGlxContextWait(__GLXcontext *context, __GLXclientState *cl, int *error)
{
    (void) context; (void) cl; (void) error;
    return TRUE;
}

static int
lorieGlxBindTexImage(__GLXcontext *baseContext, int buffer,
                     __GLXdrawable *pixmap)
{
    (void) baseContext; (void) buffer; (void) pixmap;
    return BadMatch;
}

static int
lorieGlxReleaseTexImage(__GLXcontext *baseContext, int buffer,
                        __GLXdrawable *pixmap)
{
    (void) baseContext; (void) buffer; (void) pixmap;
    return BadMatch;
}

static __GLXcontext *
lorieGlxCreateContext(__GLXscreen *screen, __GLXconfig *config,
                      __GLXcontext *shareContext, unsigned num_attribs,
                      const uint32_t *attribs, int *error)
{
    __GLXcontext *context;

    (void) shareContext;
    (void) num_attribs;
    (void) attribs;
    (void) error;

    context = calloc(1, sizeof *context);
    if (!context)
        return NULL;

    context->config = config;
    context->pGlxScreen = screen;
    context->isDirect = TRUE;   /* rendering is entirely client-side (DRI3) */
    context->destroy = lorieGlxContextDestroy;
    context->makeCurrent = lorieGlxContextMakeCurrent;
    context->loseCurrent = lorieGlxContextLoseCurrent;
    context->copy = lorieGlxContextCopy;
    context->wait = lorieGlxContextWait;
    context->bindTexImage = lorieGlxBindTexImage;
    context->releaseTexImage = lorieGlxReleaseTexImage;
    return context;
}

/* Drawable shells. */
static void
lorieGlxDrawableDestroy(__GLXdrawable *drawable)
{
    free(drawable);
}

static GLboolean
lorieGlxDrawableSwapBuffers(ClientPtr client, __GLXdrawable *drawable)
{
    (void) client; (void) drawable;
    return TRUE;
}

static void
lorieGlxDrawableCopySubBuffer(__GLXdrawable *drawable, int x, int y,
                              int w, int h)
{
    (void) drawable; (void) x; (void) y; (void) w; (void) h;
}

static void
lorieGlxDrawableWaitX(__GLXdrawable *drawable)
{
    (void) drawable;
}

static void
lorieGlxDrawableWaitGL(__GLXdrawable *drawable)
{
    (void) drawable;
}

static __GLXdrawable *
lorieGlxCreateDrawable(ClientPtr client, __GLXscreen *screen,
                       DrawablePtr pDraw, XID drawId, int type,
                       XID glxDrawId, __GLXconfig *config)
{
    __GLXdrawable *drawable;

    (void) client;
    (void) screen;
    (void) glxDrawId;

    drawable = calloc(1, sizeof *drawable);
    if (!drawable)
        return NULL;

    drawable->destroy = lorieGlxDrawableDestroy;
    drawable->swapBuffers = lorieGlxDrawableSwapBuffers;
    drawable->copySubBuffer = lorieGlxDrawableCopySubBuffer;
    drawable->waitX = lorieGlxDrawableWaitX;
    drawable->waitGL = lorieGlxDrawableWaitGL;
    drawable->pDraw = pDraw;
    drawable->drawId = drawId;
    drawable->type = type;
    drawable->config = config;
    return drawable;
}

static int
lorieGlxSwapInterval(__GLXdrawable *drawable, int interval)
{
    (void) drawable; (void) interval;
    return Success;
}

static void
lorieGlxScreenDestroy(__GLXscreen *screen)
{
    __glXScreenDestroy(screen);
}

static __GLXscreen *
lorieGlxScreenProbe(ScreenPtr pScreen)
{
    __GLXscreen *screen;
    __GLXconfig *configs, *configs2;

    /* __glXScreenDestroy frees every config in the list, so allocate them. */
    configs = calloc(2, sizeof *configs);
    if (!configs)
        return NULL;
    configs2 = configs + 1;
    lorieGlxConfigInit(configs, GLX_DOUBLE_BUFFER, 8, 32);
    lorieGlxConfigInit(configs2, GLX_DOUBLE_BUFFER, 0, 24);
    configs->next = configs2;

    screen = calloc(1, sizeof *screen);
    if (!screen) {
        free(configs);
        return NULL;
    }

    screen->destroy = lorieGlxScreenDestroy;
    screen->createContext = lorieGlxCreateContext;
    screen->createDrawable = lorieGlxCreateDrawable;
    screen->swapInterval = lorieGlxSwapInterval;
    screen->fbconfigs = configs;

    __glXInitExtensionEnableBits(screen->glx_enable_bits);
    /* Match the client-side Mesa vendor name. */
    screen->glvnd = strdup("mesa");
    screen->GLXextensions = strdup("GLX_ARB_create_context "
                                   "GLX_ARB_create_context_profile "
                                   "GLX_ARB_multisample "
                                   "GLX_EXT_visual_info "
                                   "GLX_EXT_visual_rating "
                                   "GLX_SGI_make_current_read "
                                   "GLX_ARB_framebuffer_sRGB "
                                   "GLX_EXT_framebuffer_sRGB "
                                   "GLX_ARB_get_proc_address "
                                   "GLX_EXT_texture_from_pixmap "
                                   "GLX_EXT_swap_control ");

    __glXScreenInit(screen, pScreen);

    lorieLog("Xlorie: GLX minimal provider active for screen %d: "
             "numVisuals=%d numFBConfigs=%d screenVisuals=%d\n",
             pScreen->myNum, screen->numVisuals, screen->numFBConfigs,
             pScreen->numVisuals);
    for (int vi = 0; vi < pScreen->numVisuals; vi++) {
        lorieLog("  X visual[%d] class=%d nplanes=%d masks=%lx/%lx/%lx\n",
                 vi, pScreen->visuals[vi].class, pScreen->visuals[vi].nplanes,
                 (unsigned long) pScreen->visuals[vi].redMask,
                 (unsigned long) pScreen->visuals[vi].greenMask,
                 (unsigned long) pScreen->visuals[vi].blueMask);
    }
    return screen;
}

__GLXprovider lorieGlxProvider = {
    lorieGlxScreenProbe,
    "lorie",
    NULL
};
