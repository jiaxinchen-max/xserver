/*
 * Xlorie minimal GLX provider.
 *
 * Provide GLX protocol objects without loading a server-side GLX driver:
 * fbconfigs from a small static table, contexts/drawables as shells. Direct
 * rendering is entirely client-side (Zink/Kopper presents DRI3 dma-bufs to
 * the glamor screen). Shared X Sync fences still use normal xshmfence
 * semantics and require the same futex ABI in native and container clients.
 *
 * Register with GlxPushProvider() before xorgGlxCreateVendor(): the provider
 * stack is probed head-first, so this one wins over the swrast fallback.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <GL/glxtokens.h>

#include "glxserver.h"
#include "glxcontext.h"
#include "glxdrawable.h"
#include "glxscreens.h"
#include "glxext.h"
#include "glxutil.h"
#include "extension_string.h"

#include "lorie.h"
#include "log.h"

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
                   int rgbBits, int depthBits, int stencilBits)
{
    memset(c, 0, sizeof(*c));
    c->doubleBufferMode = !!doubleBuffer;
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
    c->depthBits = depthBits;
    c->stencilBits = stencilBits;
    c->numAuxBuffers = 0;
    c->level = 0;
    c->visualType = GLX_TRUE_COLOR;
    c->visualRating = GLX_NONE_EXT;
    c->transparentPixel = GLX_NONE_EXT;
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
    /* Mesa's GLX loader matches these attributes even for window configs.
     * Direct texture-from-pixmap binding is handled by the client driver. */
    c->bindToTextureRgb = c->bindToTextureRgba = 1;
    c->bindToMipmapTexture = 0;
    c->bindToTextureTargets = GLX_TEXTURE_1D_BIT_EXT |
                             GLX_TEXTURE_2D_BIT_EXT |
                             GLX_TEXTURE_RECTANGLE_BIT_EXT;
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
    __glXDrawableRelease(drawable);
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
    (void) drawId;

    drawable = calloc(1, sizeof *drawable);
    if (!drawable)
        return NULL;

    /* drawId names the underlying X drawable; GLX validation and paired
     * resource destruction require the distinct GLXWindow/GLXPixmap ID. */
    if (!__glXDrawableInit(drawable, screen, pDraw, type, glxDrawId, config)) {
        free(drawable);
        return NULL;
    }

    drawable->destroy = lorieGlxDrawableDestroy;
    drawable->swapBuffers = lorieGlxDrawableSwapBuffers;
    drawable->copySubBuffer = lorieGlxDrawableCopySubBuffer;
    drawable->waitX = lorieGlxDrawableWaitX;
    drawable->waitGL = lorieGlxDrawableWaitGL;
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

/* Configs describe client-side Mesa renderbuffers, not the root AHB's
 * attachments. Keep the existing depth24/no-stencil variants and offer
 * packed depth24/stencil8 for Cogl's mandatory stencil clipping, plus
 * attachment-free variants for clients that do not need depth/stencil. */
static __GLXconfig *
lorieGlxCreateConfigs(void)
{
    static const struct {
        int alphaBits, rgbBits, depthBits, stencilBits;
    } formats[] = {
        { 8, 32, 24, 0 },
        { 0, 24, 24, 0 },
        { 8, 32, 24, 8 },
        { 0, 24, 24, 8 },
        { 8, 32,  0, 0 },
        { 0, 24,  0, 0 },
    };
    __GLXconfig *configs = NULL, **tail = &configs;
    size_t i;

    for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        __GLXconfig *config = calloc(1, sizeof(*config));
        if (!config) {
            while (configs) {
                __GLXconfig *next = configs->next;
                free(configs);
                configs = next;
            }
            return NULL;
        }
        lorieGlxConfigInit(config, 1, formats[i].alphaBits,
                           formats[i].rgbBits, formats[i].depthBits,
                           formats[i].stencilBits);
        *tail = config;
        tail = &config->next;
    }
    return configs;
}

static __GLXscreen *
lorieGlxScreenProbe(ScreenPtr pScreen)
{
    __GLXscreen *screen;
    __GLXconfig *configs;

    /* __glXScreenDestroy frees each node separately. */
    configs = lorieGlxCreateConfigs();
    if (!configs)
        FatalError("Xlorie: selected GLX provider could not allocate FBConfigs; startup aborted\n");

    screen = calloc(1, sizeof *screen);
    if (!screen) {
        while (configs) {
            __GLXconfig *next = configs->next;
            free(configs);
            configs = next;
        }
        FatalError("Xlorie: selected GLX provider could not allocate its screen; startup aborted\n");
    }

    screen->destroy = lorieGlxScreenDestroy;
    screen->createContext = lorieGlxCreateContext;
    screen->createDrawable = lorieGlxCreateDrawable;
    screen->swapInterval = lorieGlxSwapInterval;
    /* XLORIE_GLX=0 is handled before extension registration in InitOutput.
     * This provider is only created when GLX is enabled and has real configs. */
    screen->fbconfigs = configs;

    __glXInitExtensionEnableBits(screen->glx_enable_bits);
    /* The client-side Mesa GLX vendor requires these extensions; the DRI
     * driver path normally reports them, but this protocol-only provider has
     * no driver, so enable them explicitly. */
    {
        const char *needed[] = {
            "GLX_ARB_create_context",
            "GLX_ARB_create_context_profile",
            "GLX_ARB_create_context_robustness",
            "GLX_ARB_context_flush_control",
            "GLX_ARB_create_context_no_error",
            "GLX_EXT_create_context_es2_profile",
            "GLX_EXT_no_config_context",
            "GLX_EXT_texture_from_pixmap",
            "GLX_ARB_framebuffer_sRGB",
            "GLX_EXT_framebuffer_sRGB",
            "GLX_SGI_swap_control",
            "GLX_MESA_copy_sub_buffer",
            "GLX_ARB_fbconfig_float",
            "GLX_ARB_multisample",
            "GLX_EXT_import_context",
            "GLX_EXT_swap_control",
        };
        size_t k;
        for (k = 0; k < sizeof(needed) / sizeof(needed[0]); k++)
            __glXEnableExtension(screen->glx_enable_bits, needed[k]);
    }
    /* Match the client-side Mesa vendor name. */
    screen->glvnd = strdup("mesa");
    if (!screen->glvnd)
        FatalError("Xlorie: selected GLX provider could not allocate its vendor name; startup aborted\n");
    /* __glXScreenInit owns and builds GLXextensions from glx_enable_bits.
     * A preallocated string would be discarded by that initializer. */

    __glXScreenInit(screen, pScreen);
    /* The core initializer returns void, including when private-key setup
     * fails. Never return an incomplete provider or NULL to the provider
     * stack: that would allow another renderer to be selected. */
    if (screen->pScreen != pScreen || !screen->fbconfigs ||
        screen->numFBConfigs <= 0 || !screen->visuals ||
        screen->numVisuals <= 0 || screen->numVisuals > screen->numFBConfigs ||
        !screen->GLextensions || !screen->GLextensions[0] ||
        !screen->GLXextensions || !screen->GLXextensions[0] ||
        !pScreen->visuals || pScreen->numVisuals <= 0)
        FatalError("Xlorie: selected GLX provider initialization is incomplete; startup aborted\n");
    for (int vi = 0; vi < screen->numVisuals; vi++) {
        if (!screen->visuals[vi] || !screen->visuals[vi]->visualID)
            FatalError("Xlorie: selected GLX provider has an invalid visual; startup aborted\n");
    }

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
