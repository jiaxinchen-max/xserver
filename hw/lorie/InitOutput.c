/*
 * Minimal Xlorie DDX backend.
 *
 * The framebuffer is allocated by the Termux render server and imported over
 * the termux-render unix socket.  fb writes directly into that shared buffer.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <X11/X.h>
#include <X11/Xos.h>
#include <X11/Xproto.h>

#include "colormapst.h"
#include "cursor.h"
#include "cursorstr.h"
#include "damage.h"
#include "dix.h"
#ifdef DRI3
#include "dri3.h"
#include "drm_fourcc.h"
#endif
#include "fb.h"
#include "gcstruct.h"
#include "glx_extinit.h"
#include "input.h"
#include "list.h"
#include "micmap.h"
#include "miline.h"
#include "mipointer.h"
#include "os.h"
#ifdef PRESENT
#include "present.h"
#endif
#include "pixmapstr.h"
#include "privates.h"
#include "randrstr.h"
#include "scrnintstr.h"
#include "servermd.h"
#include "window.h"

#include "lorie.h"
#include "log.h"

#define LORIE_DEFAULT_WIDTH 1280
#define LORIE_DEFAULT_HEIGHT 720
#define LORIE_DEFAULT_DEPTH 24
#define LORIE_DEFAULT_FRAMERATE 60
#define LORIE_DEFAULT_LINEBIAS 0
#define LORIE_DEFAULT_BLACKPIXEL 0
#define LORIE_DEFAULT_WHITEPIXEL 1

#if defined(DRI3) && !defined(DRM_FORMAT_MOD_LINEAR)
#define DRM_FORMAT_MOD_LINEAR 0
#endif

typedef struct {
    ScreenPtr screen;
    int width;
    int height;
    int depth;
    int bitsPerPixel;
    int paddedWidth;
    int paddedBytesWidth;
    int framerate;
    char *fb;
    LorieBuffer *rootBuffer;
    Bool ownsRootBuffer;
    DamagePtr damage;
    Pixel blackPixel;
    Pixel whitePixel;
    unsigned int lineBias;
    CloseScreenProcPtr closeScreen;
#ifdef DRI3
    DestroyPixmapProcPtr destroyPixmap;
#endif
#ifdef PRESENT
    uint64_t vblankInterval;
    uint64_t currentMsc;
    struct xorg_list vblankQueue;
#endif
} lorieScreenInfo;

#ifdef PRESENT
typedef struct {
    struct xorg_list link;
    uint64_t id;
    uint64_t msc;
} lorieVblankRec;
#endif

#ifdef DRI3
typedef struct {
    LorieBuffer *buffer;
    void *locked;
    Bool registered;
} loriePixmapPriv;

static DevPrivateKeyRec loriePixmapPrivateKey;
#endif

static lorieScreenInfo lorieScreen = {
    .width = LORIE_DEFAULT_WIDTH,
    .height = LORIE_DEFAULT_HEIGHT,
    .depth = LORIE_DEFAULT_DEPTH,
    .framerate = LORIE_DEFAULT_FRAMERATE,
    .blackPixel = LORIE_DEFAULT_BLACKPIXEL,
    .whitePixel = LORIE_DEFAULT_WHITEPIXEL,
    .lineBias = LORIE_DEFAULT_LINEBIAS,
};

static Bool loriePixmapDepths[33];
static Bool Render = TRUE;
static Bool blockHandlersRegistered = FALSE;
#ifdef DRI3
static Bool Dri3 = TRUE;
#endif

#ifdef PRESENT
static void loriePerformVblanks(void);
#endif
static void lorieReleaseOwnedRootBuffer(void);
static Bool lorieCreateRootDamage(ScreenPtr pScreen);
static void lorieDestroyRootDamage(void);

static Bool
lorieFlushRootBuffer(void)
{
    LorieBuffer *buffer = lorieScreen.rootBuffer ?
        lorieScreen.rootBuffer : lorieRenderBuffer();
    const LorieBuffer_Desc *desc = LorieBuffer_description(buffer);
    PixmapPtr pixmap;
    void *fb = NULL;
    int ret;

    if (!buffer || desc->type != LORIEBUFFER_AHARDWAREBUFFER)
        return TRUE;

    if (!lorieScreen.fb)
        return TRUE;

    ret = LorieBuffer_unlock(buffer);
    if (ret != 0)
        return FALSE;

    ret = LorieBuffer_lock(buffer, &fb);
    if (ret != 0 || !fb)
        return FALSE;

    if (fb == lorieScreen.fb)
        return TRUE;

    lorieScreen.fb = fb;
    pixmap = lorieScreen.screen ? lorieScreen.screen->GetScreenPixmap(lorieScreen.screen) :
        NULL;
    if (!pixmap)
        return TRUE;

    return lorieScreen.screen->ModifyPixmapHeader(pixmap,
                                                  lorieScreen.width,
                                                  lorieScreen.height,
                                                  lorieScreen.depth,
                                                  lorieScreen.bitsPerPixel,
                                                  lorieScreen.paddedBytesWidth,
                                                  lorieScreen.fb);
}

static Bool
lorieRootDamaged(void)
{
    if (!lorieScreen.damage)
        return TRUE;

    return RegionNotEmpty(DamageRegion(lorieScreen.damage));
}

static void
lorieMarkRootClean(void)
{
    if (lorieScreen.damage)
        DamageEmpty(lorieScreen.damage);
}

static void
lorieInitializePixmapDepths(void)
{
    int i;

    loriePixmapDepths[1] = TRUE;
    for (i = 2; i <= 32; i++)
        loriePixmapDepths[i] = FALSE;
}

static int
lorieBitsPerPixel(int depth)
{
    if (depth == 1)
        return 1;
    if (depth <= 8)
        return 8;
    if (depth <= 16)
        return 16;
    return 32;
}

void
ddxGiveUp(enum ExitCode error)
{
    lorieInputUnregister();
    lorieRenderDisconnect();
}

#ifdef __APPLE__
void
DarwinHandleGUI(int argc, char *argv[])
{
}
#endif

void
OsVendorInit(void)
{
}

void
OsVendorFatalError(const char *f, va_list args)
{
}

#if defined(DDXBEFORERESET)
void
ddxBeforeReset(void)
{
}
#endif

#if INPUTTHREAD
void
ddxInputThreadInit(void)
{
}
#endif

void
ddxUseMsg(void)
{
    ErrorF("-screen 0 WxHx24       set Xlorie screen size\n");
    ErrorF("-framerate n          set requested renderer framerate\n");
    ErrorF("-render-socket path   accepted for compatibility; "
           "libtermux-render uses its default socket\n");
    ErrorF("-pixdepths list       support additional pixmap depths\n");
    ErrorF("+/-render             turn on/off RENDER extension support"
           " (default on)\n");
#ifdef DRI3
    ErrorF("-disable-dri3         disable Xlorie DRI3 import support\n");
#endif
}

int
ddxProcessArgument(int argc, char *argv[], int i)
{
    static Bool firstTime = TRUE;

    if (firstTime) {
        lorieInitializePixmapDepths();
        firstTime = FALSE;
    }

    if (strcmp(argv[i], "-screen") == 0) {
        int screenNum;

        CHECK_FOR_REQUIRED_ARGUMENTS(2);
        screenNum = atoi(argv[i + 1]);
        if (screenNum != 0)
            FatalError("Xlorie supports only screen 0\n");

        if (sscanf(argv[i + 2], "%dx%dx%d",
                   &lorieScreen.width,
                   &lorieScreen.height,
                   &lorieScreen.depth) != 3) {
            ErrorF("Invalid screen configuration %s\n", argv[i + 2]);
            UseMsg();
            FatalError("Invalid Xlorie screen configuration\n");
        }
        return 3;
    }

    if (strcmp(argv[i], "-framerate") == 0) {
        CHECK_FOR_REQUIRED_ARGUMENTS(1);
        lorieScreen.framerate = atoi(argv[i + 1]);
        if (lorieScreen.framerate <= 0)
            lorieScreen.framerate = LORIE_DEFAULT_FRAMERATE;
        return 2;
    }

    if (strcmp(argv[i], "-render-socket") == 0) {
        CHECK_FOR_REQUIRED_ARGUMENTS(1);
        lorieRenderSetSocketPath(argv[i + 1]);
        return 2;
    }

    if (strcmp(argv[i], "-pixdepths") == 0) {
        int depth, ret = 1;

        CHECK_FOR_REQUIRED_ARGUMENTS(1);
        while ((++i < argc) && (depth = atoi(argv[i])) != 0) {
            if (depth < 0 || depth > 32)
                FatalError("Invalid pixmap depth %d\n", depth);
            loriePixmapDepths[depth] = TRUE;
            ret++;
        }
        return ret;
    }

    if (strcmp(argv[i], "+render") == 0) {
        Render = TRUE;
        return 1;
    }

    if (strcmp(argv[i], "-render") == 0) {
        Render = FALSE;
#ifdef COMPOSITE
        noCompositeExtension = TRUE;
#endif
        return 1;
    }

    if (strcmp(argv[i], "-blackpixel") == 0) {
        CHECK_FOR_REQUIRED_ARGUMENTS(1);
        lorieScreen.blackPixel = atoi(argv[i + 1]);
        return 2;
    }

    if (strcmp(argv[i], "-whitepixel") == 0) {
        CHECK_FOR_REQUIRED_ARGUMENTS(1);
        lorieScreen.whitePixel = atoi(argv[i + 1]);
        return 2;
    }

    if (strcmp(argv[i], "-linebias") == 0) {
        CHECK_FOR_REQUIRED_ARGUMENTS(1);
        lorieScreen.lineBias = atoi(argv[i + 1]);
        return 2;
    }

#ifdef DRI3
    if (strcmp(argv[i], "-disable-dri3") == 0) {
        Dri3 = FALSE;
        return 1;
    }
#endif

    return 0;
}

static void
lorieBlockHandler(void *blockData, void *timeout)
{
    struct lorie_shared_server_state *state = lorieRenderState();

#ifdef PRESENT
    loriePerformVblanks();
#endif
    if (lorieRootDamaged()) {
        if (!lorieFlushRootBuffer())
            lorieLog("Failed to flush Xlorie root AHardwareBuffer\n");
        lorieMarkRootClean();
        lorieRenderSignalFrame();
    } else if (state && (state->drawRequested || state->cursor.moved ||
                         state->cursor.updated)) {
        state->waitForNextFrame = 0;
        pthread_cond_signal(&state->cond);
    }
}

static void
lorieWakeupHandler(void *blockData, int result)
{
}

static Bool
lorieRealizeCursor(DeviceIntPtr pDev, ScreenPtr pScreen, CursorPtr pCurs)
{
    (void) pDev;
    (void) pScreen;
    (void) pCurs;
    return TRUE;
}

static Bool
lorieDeviceCursorInitialize(DeviceIntPtr pDev, ScreenPtr pScreen)
{
    (void) pDev;
    (void) pScreen;
    return TRUE;
}

static void
lorieDeviceCursorCleanup(DeviceIntPtr pDev, ScreenPtr pScreen)
{
    (void) pDev;
    (void) pScreen;
}

static void
lorieMoveCursor(DeviceIntPtr pDev, ScreenPtr pScreen, int x, int y)
{
    struct lorie_shared_server_state *state = lorieRenderState();

    (void) pDev;
    (void) pScreen;

    if (!state)
        return;

    state->cursor.x = x;
    state->cursor.y = y;
    state->cursor.moved = TRUE;
    pthread_cond_signal(&state->cond);
}

static void
lorieConvertCursor(CursorPtr pCurs, uint32_t *data)
{
    CursorBitsPtr bits = pCurs->bits;
    int x, y;

    if (bits->argb) {
        int count = bits->width * bits->height;

        for (int i = 0; i < count; i++) {
            CARD32 p = bits->argb[i];

            data[i] = (p & 0xff000000) |
                ((p & 0x00ff0000) >> 16) |
                (p & 0x0000ff00) |
                ((p & 0x000000ff) << 16);
        }
        return;
    }

    uint32_t *p = data;
    uint32_t fg = ((pCurs->foreBlue & 0xff00) << 8) |
        (pCurs->foreGreen & 0xff00) | (pCurs->foreRed >> 8);
    uint32_t bg = ((pCurs->backBlue & 0xff00) << 8) |
        (pCurs->backGreen & 0xff00) | (pCurs->backRed >> 8);
    int stride = BitmapBytePad(bits->width);

    for (y = 0; y < bits->height; y++) {
        for (x = 0; x < bits->width; x++) {
            int i = y * stride + x / 8;
            int bit = 1 << (x & 7);
            uint32_t pixel = (bits->source[i] & bit) ? fg : bg;

            *p++ = (bits->mask[i] & bit) ? pixel | 0xff000000 : 0;
        }
    }
}

static void
lorieSetCursor(DeviceIntPtr pDev, ScreenPtr pScreen, CursorPtr pCurs,
               int x, int y)
{
    struct lorie_shared_server_state *state = lorieRenderState();
    CursorBitsPtr bits;

    (void) pDev;
    (void) pScreen;

    if (!state)
        return;

    if (pCurs && (pCurs->bits->width >= 512 || pCurs->bits->height >= 512))
        pCurs = rootCursor;

    bits = pCurs ? pCurs->bits : NULL;

    lorie_mutex_lock(&state->cursor.lock, &state->cursor.lockingPid);
    if (bits) {
        state->cursor.xhot = bits->xhot;
        state->cursor.yhot = bits->yhot;
        state->cursor.width = bits->width;
        state->cursor.height = bits->height;
        lorieConvertCursor(pCurs, state->cursor.bits);
    } else {
        state->cursor.xhot = 0;
        state->cursor.yhot = 0;
        state->cursor.width = 0;
        state->cursor.height = 0;
    }
    state->cursor.updated = TRUE;
    lorie_mutex_unlock(&state->cursor.lock, &state->cursor.lockingPid);

    lorieMoveCursor(NULL, NULL, x, y);
}

static miPointerSpriteFuncRec loriePointerSpriteFuncs = {
    lorieRealizeCursor,
    lorieRealizeCursor,
    lorieSetCursor,
    lorieMoveCursor,
    lorieDeviceCursorInitialize,
    lorieDeviceCursorCleanup
};

static Bool
lorieCursorOffScreen(ScreenPtr *ppScreen, int *x, int *y)
{
    return FALSE;
}

static void
lorieCrossScreen(ScreenPtr pScreen, Bool entering)
{
}

static miPointerScreenFuncRec loriePointerCursorFuncs = {
    lorieCursorOffScreen,
    lorieCrossScreen,
    miPointerWarpCursor
};

#ifdef PRESENT
static void
loriePresentUpdateMsc(void)
{
    uint64_t interval = lorieScreen.vblankInterval;

    if (!interval)
        interval = 1000000 / LORIE_DEFAULT_FRAMERATE;

    lorieScreen.currentMsc = GetTimeInMicros() / interval;
}

static RRCrtcPtr
loriePresentGetCrtc(WindowPtr window)
{
    return RRFirstEnabledCrtc(window->drawable.pScreen);
}

static int
loriePresentGetUstMsc(RRCrtcPtr crtc, uint64_t *ust, uint64_t *msc)
{
    (void) crtc;

    *ust = GetTimeInMicros();
    loriePresentUpdateMsc();
    *msc = lorieScreen.currentMsc;
    return Success;
}

static Bool
lorieQueuePresentEvent(uint64_t eventId, uint64_t msc)
{
    lorieVblankRec *vblank = calloc(1, sizeof(*vblank));

    if (!vblank)
        return FALSE;

    vblank->id = eventId;
    vblank->msc = msc;
    xorg_list_add(&vblank->link, &lorieScreen.vblankQueue);
    return TRUE;
}

static Bool
loriePresentQueueVblank(RRCrtcPtr crtc, uint64_t eventId, uint64_t msc)
{
    (void) crtc;

    loriePresentUpdateMsc();
    if (msc <= lorieScreen.currentMsc) {
        present_event_notify(eventId, GetTimeInMicros(), lorieScreen.currentMsc);
        return Success;
    }

    if (!lorieQueuePresentEvent(eventId, msc))
        return BadAlloc;

    return Success;
}

static void
loriePresentAbortVblank(RRCrtcPtr crtc, uint64_t eventId, uint64_t msc)
{
    lorieVblankRec *vblank, *tmp;

    (void) crtc;
    (void) msc;

    xorg_list_for_each_entry_safe(vblank, tmp, &lorieScreen.vblankQueue, link) {
        if (vblank->id == eventId) {
            xorg_list_del(&vblank->link);
            free(vblank);
            return;
        }
    }
}

static void
loriePerformVblanks(void)
{
    lorieVblankRec *vblank, *tmp;

    loriePresentUpdateMsc();
    xorg_list_for_each_entry_safe(vblank, tmp, &lorieScreen.vblankQueue, link) {
        if (vblank->msc <= lorieScreen.currentMsc) {
            present_event_notify(vblank->id, GetTimeInMicros(),
                                 lorieScreen.currentMsc);
            xorg_list_del(&vblank->link);
            free(vblank);
        }
    }
}

static Bool
loriePresentCheckFlip(RRCrtcPtr crtc, WindowPtr window, PixmapPtr pixmap,
                      Bool syncFlip)
{
#ifdef DRI3
    loriePixmapPriv *priv;

    (void) crtc;
    (void) syncFlip;

    if (!Dri3 || !window || !pixmap)
        return FALSE;

    priv = dixLookupPrivate(&pixmap->devPrivates, &loriePixmapPrivateKey);
    if (!priv || !priv->buffer)
        return FALSE;

    return pixmap->drawable.width == window->drawable.pScreen->width &&
        pixmap->drawable.height == window->drawable.pScreen->height;
#else
    (void) crtc;
    (void) window;
    (void) pixmap;
    (void) syncFlip;
    return FALSE;
#endif
}

static Bool
loriePresentFlip(RRCrtcPtr crtc, uint64_t eventId, uint64_t targetMsc,
                 PixmapPtr pixmap, Bool syncFlip)
{
#ifdef DRI3
    loriePixmapPriv *priv;
    lorieVblankRec *event;
    Bool registeredNow = FALSE;

    (void) crtc;
    (void) syncFlip;

    if (!Dri3 || !pixmap)
        return FALSE;

    priv = dixLookupPrivate(&pixmap->devPrivates, &loriePixmapPrivateKey);
    if (!priv || !priv->buffer)
        return FALSE;

    loriePresentUpdateMsc();
    if (targetMsc <= lorieScreen.currentMsc)
        targetMsc = lorieScreen.currentMsc + 1;

    event = calloc(1, sizeof(*event));
    if (!event)
        return FALSE;

    if (!priv->registered) {
        if (!lorieRenderRegisterBuffer(priv->buffer)) {
            free(event);
            return FALSE;
        }
        priv->registered = TRUE;
        registeredNow = TRUE;
    }

    if (!lorieRenderUseBuffer(priv->buffer)) {
        if (registeredNow) {
            (void) lorieRenderUnregisterBuffer(priv->buffer);
            priv->registered = FALSE;
        }
        free(event);
        return FALSE;
    }

    event->id = eventId;
    event->msc = targetMsc;
    xorg_list_add(&event->link, &lorieScreen.vblankQueue);

    return TRUE;
#else
    (void) crtc;
    (void) eventId;
    (void) targetMsc;
    (void) pixmap;
    (void) syncFlip;
    return FALSE;
#endif
}

static void
loriePresentUnflip(ScreenPtr screen, uint64_t eventId)
{
    LorieBuffer *buffer = lorieScreen.rootBuffer ?
        lorieScreen.rootBuffer : lorieRenderBuffer();

    (void) screen;

    (void) lorieRenderUseBuffer(buffer);
    present_event_notify(eventId, 0, 0);
}

static present_screen_info_rec loriePresentInfo = {
    .version = PRESENT_SCREEN_INFO_VERSION,
    .get_crtc = loriePresentGetCrtc,
    .get_ust_msc = loriePresentGetUstMsc,
    .queue_vblank = loriePresentQueueVblank,
    .abort_vblank = loriePresentAbortVblank,
    .capabilities = PresentCapabilityNone,
    .check_flip = loriePresentCheckFlip,
    .flip = loriePresentFlip,
    .unflip = loriePresentUnflip,
};
#endif

#ifdef DRI3
static loriePixmapPriv *
loriePixmapPrivate(PixmapPtr pixmap)
{
    return dixLookupPrivate(&pixmap->devPrivates, &loriePixmapPrivateKey);
}

static Bool
lorieDestroyPixmap(PixmapPtr pixmap)
{
    loriePixmapPriv *priv = loriePixmapPrivate(pixmap);

    if (pixmap->refcnt == 1 && priv && priv->buffer) {
        if (priv->registered)
            (void) lorieRenderUnregisterBuffer(priv->buffer);
        if (priv->locked)
            (void) LorieBuffer_unlock(priv->buffer);
        LorieBuffer_release(priv->buffer);
        priv->buffer = NULL;
        priv->locked = NULL;
        priv->registered = FALSE;
    }

    return lorieScreen.destroyPixmap(pixmap);
}

static PixmapPtr
lorieDri3PixmapFromFds(ScreenPtr screen, CARD8 numFds, const int *fds,
                       CARD16 width, CARD16 height, const CARD32 *strides,
                       const CARD32 *offsets, CARD8 depth, CARD8 bpp,
                       CARD64 modifier)
{
    PixmapPtr pixmap = NullPixmap;
    loriePixmapPriv *priv;
    LorieBuffer *buffer;
    void *data = NULL;

    if (numFds != 1 || !fds || !strides || !offsets)
        return NullPixmap;

    if (modifier != DRM_FORMAT_MOD_INVALID && modifier != DRM_FORMAT_MOD_LINEAR)
        return NullPixmap;

    if (width == 0 || height == 0 || strides[0] == 0 || bpp != 32)
        return NullPixmap;

    if (depth != 24 && depth != 32)
        return NullPixmap;

    if (offsets[0] != 0 || strides[0] % sizeof(uint32_t) != 0)
        return NullPixmap;

    buffer = LorieBuffer_wrapFileDescriptor(width,
                                            strides[0] / sizeof(uint32_t),
                                            height,
                                            AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM,
                                            fds[0], offsets[0]);
    if (!buffer)
        return NullPixmap;

    if (LorieBuffer_lock(buffer, &data) != 0 || !data) {
        LorieBuffer_release(buffer);
        return NullPixmap;
    }

    pixmap = screen->CreatePixmap(screen, 0, 0, depth, 0);
    if (!pixmap)
        goto fail;

    if (!screen->ModifyPixmapHeader(pixmap, width, height, depth, bpp,
                                    strides[0], data))
        goto fail;

    priv = loriePixmapPrivate(pixmap);
    if (!priv)
        goto fail;
    priv->buffer = buffer;
    priv->locked = data;
    priv->registered = FALSE;
    return pixmap;

fail:
    if (pixmap)
        screen->DestroyPixmap(pixmap);
    LorieBuffer_unlock(buffer);
    LorieBuffer_release(buffer);
    return NullPixmap;
}

static int
lorieDri3FdFromPixmap(ScreenPtr screen, PixmapPtr pixmap, CARD16 *stride,
                      CARD32 *size)
{
    (void) screen;
    (void) pixmap;

    if (stride)
        *stride = 0;
    if (size)
        *size = 0;
    return -1;
}

static int
lorieDri3FdsFromPixmap(ScreenPtr screen, PixmapPtr pixmap, int *fds,
                       uint32_t *strides, uint32_t *offsets,
                       uint64_t *modifier)
{
    (void) screen;
    (void) pixmap;
    (void) fds;
    (void) strides;
    (void) offsets;
    (void) modifier;
    return 0;
}

static int
lorieDri3GetFormats(ScreenPtr screen, CARD32 *numFormats, CARD32 **formats)
{
    static CARD32 supportedFormats[] = {
        DRM_FORMAT_XRGB8888,
        DRM_FORMAT_ARGB8888,
    };

    (void) screen;

    *numFormats = sizeof(supportedFormats) / sizeof(supportedFormats[0]);
    *formats = supportedFormats;
    return TRUE;
}

static int
lorieDri3GetModifiers(ScreenPtr screen, uint32_t format,
                      uint32_t *numModifiers, uint64_t **modifiers)
{
    static uint64_t supportedModifiers[] = {
        DRM_FORMAT_MOD_LINEAR,
    };

    (void) screen;

    if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888) {
        *numModifiers = 0;
        *modifiers = NULL;
        return TRUE;
    }

    *numModifiers = sizeof(supportedModifiers) / sizeof(supportedModifiers[0]);
    *modifiers = supportedModifiers;
    return TRUE;
}

static int
lorieDri3GetDrawableModifiers(DrawablePtr drawable, uint32_t format,
                              uint32_t *numModifiers, uint64_t **modifiers)
{
    uint64_t *out;

    (void) drawable;

    if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888) {
        *numModifiers = 0;
        *modifiers = NULL;
        return TRUE;
    }

    out = calloc(1, sizeof(*out));
    if (!out)
        return FALSE;

    out[0] = DRM_FORMAT_MOD_LINEAR;
    *numModifiers = 1;
    *modifiers = out;
    return TRUE;
}

static dri3_screen_info_rec lorieDri3Info = {
    .version = 2,
    .pixmap_from_fds = lorieDri3PixmapFromFds,
    .fd_from_pixmap = lorieDri3FdFromPixmap,
    .fds_from_pixmap = lorieDri3FdsFromPixmap,
    .get_formats = lorieDri3GetFormats,
    .get_modifiers = lorieDri3GetModifiers,
    .get_drawable_modifiers = lorieDri3GetDrawableModifiers,
};
#endif

static Bool
lorieCloseScreen(ScreenPtr pScreen)
{
#ifdef PRESENT
    lorieVblankRec *vblank, *tmp;
#endif

    pScreen->CloseScreen = lorieScreen.closeScreen;

    if (blockHandlersRegistered) {
        RemoveBlockAndWakeupHandlers(lorieBlockHandler, lorieWakeupHandler,
                                     NULL);
        blockHandlersRegistered = FALSE;
    }

#ifdef PRESENT
    xorg_list_for_each_entry_safe(vblank, tmp, &lorieScreen.vblankQueue, link) {
        xorg_list_del(&vblank->link);
        free(vblank);
    }
#endif

    lorieDestroyRootDamage();

    if (pScreen->devPrivate)
        (*pScreen->DestroyPixmap) (pScreen->devPrivate);
    pScreen->devPrivate = NULL;

    lorieInputUnregister();
    lorieReleaseOwnedRootBuffer();
    lorieRenderDisconnect();

    return pScreen->CloseScreen(pScreen);
}

static Bool
lorieRROutputValidateMode(ScreenPtr pScreen, RROutputPtr output,
                          RRModePtr mode)
{
    rrScrPriv(pScreen);

    return pScrPriv->minWidth <= mode->mode.width &&
        pScrPriv->maxWidth >= mode->mode.width &&
        pScrPriv->minHeight <= mode->mode.height &&
        pScrPriv->maxHeight >= mode->mode.height;
}

static void
lorieReleaseOwnedRootBuffer(void)
{
    if (!lorieScreen.rootBuffer)
        return;

    (void) LorieBuffer_unlock(lorieScreen.rootBuffer);
    if (lorieScreen.ownsRootBuffer) {
        (void) lorieRenderUnregisterBuffer(lorieScreen.rootBuffer);
        LorieBuffer_release(lorieScreen.rootBuffer);
    }

    lorieScreen.rootBuffer = NULL;
    lorieScreen.ownsRootBuffer = FALSE;
    lorieScreen.fb = NULL;
}

static void
lorieDestroyRootDamage(void)
{
    if (!lorieScreen.damage)
        return;

    DamageUnregister(lorieScreen.damage);
    DamageDestroy(lorieScreen.damage);
    lorieScreen.damage = NULL;
}

static Bool
lorieCreateRootDamage(ScreenPtr pScreen)
{
    PixmapPtr pixmap;

    lorieDestroyRootDamage();
    if (!pScreen)
        return FALSE;

    pixmap = pScreen->GetScreenPixmap(pScreen);
    if (!pixmap)
        return FALSE;

    lorieScreen.damage = DamageCreate(NULL, NULL, DamageReportNone,
                                      TRUE, pScreen, NULL);
    if (!lorieScreen.damage)
        return FALSE;

    DamageRegister(&pixmap->drawable, lorieScreen.damage);
    return TRUE;
}

static Bool
lorieReplaceRootBuffer(ScreenPtr pScreen, CARD16 width, CARD16 height,
                       CARD32 mmWidth, CARD32 mmHeight)
{
    LorieBuffer *oldBuffer = lorieScreen.rootBuffer ?
        lorieScreen.rootBuffer : lorieRenderBuffer();
    const LorieBuffer_Desc *oldDesc = LorieBuffer_description(oldBuffer);
    LorieBuffer *newBuffer;
    const LorieBuffer_Desc *newDesc;
    PixmapPtr pixmap;
    void *fb = NULL;
    Bool oldOwned = lorieScreen.ownsRootBuffer;

    if (!pScreen || width == 0 || height == 0 || !oldBuffer)
        return FALSE;

    if (width == pScreen->width && height == pScreen->height) {
        pScreen->mmWidth = mmWidth;
        pScreen->mmHeight = mmHeight;
        RRScreenSizeNotify(pScreen);
        RRTellChanged(pScreen);
        return TRUE;
    }

    newBuffer = LorieBuffer_allocate(width, height, oldDesc->format,
                                     oldDesc->type);
    if (!newBuffer)
        return FALSE;

    if (LorieBuffer_lock(newBuffer, &fb) != 0 || !fb) {
        LorieBuffer_release(newBuffer);
        return FALSE;
    }

    if (!lorieRenderRegisterBuffer(newBuffer)) {
        (void) LorieBuffer_unlock(newBuffer);
        LorieBuffer_release(newBuffer);
        return FALSE;
    }

    newDesc = LorieBuffer_description(newBuffer);
    memset(fb, 0, newDesc->stride * newDesc->height * sizeof(uint32_t));

    pixmap = pScreen->GetScreenPixmap(pScreen);
    SetRootClip(pScreen, ROOT_CLIP_NONE);

    if (pixmap && !pScreen->ModifyPixmapHeader(pixmap,
                                               newDesc->width,
                                               newDesc->height,
                                               lorieScreen.depth,
                                               lorieScreen.bitsPerPixel,
                                               newDesc->stride *
                                               sizeof(uint32_t),
                                               fb)) {
        SetRootClip(pScreen, ROOT_CLIP_FULL);
        (void) lorieRenderUnregisterBuffer(newBuffer);
        (void) LorieBuffer_unlock(newBuffer);
        LorieBuffer_release(newBuffer);
        return FALSE;
    }

    lorieScreen.rootBuffer = newBuffer;
    lorieScreen.ownsRootBuffer = TRUE;
    lorieScreen.fb = fb;
    lorieScreen.width = newDesc->width;
    lorieScreen.height = newDesc->height;
    lorieScreen.paddedWidth = newDesc->stride;
    lorieScreen.paddedBytesWidth = newDesc->stride * sizeof(uint32_t);

    pScreen->width = newDesc->width;
    pScreen->height = newDesc->height;
    pScreen->mmWidth = mmWidth;
    pScreen->mmHeight = mmHeight;

    if (!lorieCreateRootDamage(pScreen))
        lorieLog("Failed to recreate root damage tracking\n");

    if (pScreen->root)
        pScreen->ResizeWindow(pScreen->root, 0, 0, width, height, NULL);

    (void) lorieRenderUseBuffer(newBuffer);

    if (oldBuffer) {
        (void) LorieBuffer_unlock(oldBuffer);
        if (oldOwned) {
            (void) lorieRenderUnregisterBuffer(oldBuffer);
            LorieBuffer_release(oldBuffer);
        }
    }

    SetRootClip(pScreen, ROOT_CLIP_FULL);
    RRScreenSizeNotify(pScreen);
    RRTellChanged(pScreen);
    update_desktop_dimensions();
    return TRUE;
}

static Bool
lorieRRScreenSetSize(ScreenPtr pScreen, CARD16 width, CARD16 height,
                     CARD32 mmWidth, CARD32 mmHeight)
{
    return lorieReplaceRootBuffer(pScreen, width, height, mmWidth, mmHeight);
}

static Bool
lorieRRCrtcSet(ScreenPtr pScreen, RRCrtcPtr crtc, RRModePtr mode,
               int x, int y, Rotation rotation, int numOutput,
               RROutputPtr *outputs)
{
    return RRCrtcNotify(crtc, mode, x, y, rotation, NULL, numOutput, outputs);
}

static Bool
lorieRRGetInfo(ScreenPtr pScreen, Rotation *rotations)
{
    *rotations = RR_Rotate_0;
    return TRUE;
}

static Bool
lorieRandRInit(ScreenPtr pScreen)
{
    rrScrPrivPtr pScrPriv;
#if RANDR_12_INTERFACE
    RRModePtr mode;
    RRCrtcPtr crtc;
    RROutputPtr output;
    xRRModeInfo modeInfo;
    char name[64];
#endif

    if (!RRScreenInit(pScreen))
        return FALSE;

    pScrPriv = rrGetScrPriv(pScreen);
    pScrPriv->rrGetInfo = lorieRRGetInfo;

#if RANDR_12_INTERFACE
    pScrPriv->rrCrtcSet = lorieRRCrtcSet;
    pScrPriv->rrScreenSetSize = lorieRRScreenSetSize;
    pScrPriv->rrOutputSetProperty = NULL;
#if RANDR_13_INTERFACE
    pScrPriv->rrOutputGetProperty = NULL;
#endif
    pScrPriv->rrOutputValidateMode = lorieRROutputValidateMode;
    pScrPriv->rrModeDestroy = NULL;

    RRScreenSetSizeRange(pScreen, 1, 1, 32767, 32767);

    snprintf(name, sizeof(name), "%dx%d", pScreen->width, pScreen->height);
    memset(&modeInfo, 0, sizeof(modeInfo));
    modeInfo.width = pScreen->width;
    modeInfo.height = pScreen->height;
    modeInfo.nameLength = strlen(name);

    mode = RRModeGet(&modeInfo, name);
    if (!mode)
        return FALSE;

    crtc = RRCrtcCreate(pScreen, NULL);
    if (!crtc)
        return FALSE;

    RRCrtcGammaSetSize(crtc, 256);

    output = RROutputCreate(pScreen, "lorie", 5, NULL);
    if (!output)
        return FALSE;
    if (!RROutputSetClones(output, NULL, 0))
        return FALSE;
    if (!RROutputSetModes(output, &mode, 1, 0))
        return FALSE;
    if (!RROutputSetCrtcs(output, &crtc, 1))
        return FALSE;
    if (!RROutputSetConnection(output, RR_Connected))
        return FALSE;
    RRCrtcNotify(crtc, mode, 0, 0, RR_Rotate_0, NULL, 1, &output);
#endif

    return TRUE;
}

void
lorieConfigureNotify(int width, int height, int framerate,
                     size_t nameSize, const char *name)
{
#if RANDR_12_INTERFACE
    ScreenPtr pScreen = lorieScreen.screen;
    RROutputPtr output;
    RRCrtcPtr crtc;
    RRModePtr mode;
    xRRModeInfo modeInfo;
    CARD32 mmWidth, mmHeight;
    char modeName[64];

    (void) nameSize;
    (void) name;

    if (!pScreen || width <= 0 || height <= 0)
        return;

    if (framerate <= 0)
        framerate = lorieScreen.framerate > 0 ?
            lorieScreen.framerate : LORIE_DEFAULT_FRAMERATE;

    lorieLog("configure screen %dx%d@%d\n", width, height, framerate);

    snprintf(modeName, sizeof(modeName), "%dx%d", width, height);
    memset(&modeInfo, 0, sizeof(modeInfo));
    modeInfo.width = width;
    modeInfo.height = height;
    modeInfo.nameLength = strlen(modeName);

    mode = RRModeGet(&modeInfo, modeName);
    output = RRFirstOutput(pScreen);
    crtc = RRFirstEnabledCrtc(pScreen);
    if (!mode || !output || !crtc)
        return;

    mmWidth = ((double) width) * 25.4 / monitorResolution;
    mmHeight = ((double) height) * 25.4 / monitorResolution;

    if (!RROutputSetModes(output, &mode, 1, 0))
        return;
    if (!RRCrtcNotify(crtc, mode, 0, 0, RR_Rotate_0, NULL, 1, &output))
        return;
    if (!RRScreenSizeSet(pScreen, width, height, mmWidth, mmHeight))
        return;

    lorieScreen.framerate = framerate;
#ifdef PRESENT
    lorieScreen.vblankInterval = 1000000 / framerate;
#endif
#else
    (void) width;
    (void) height;
    (void) framerate;
    (void) nameSize;
    (void) name;
#endif
}

static Bool
lorieScreenInit(ScreenPtr pScreen, int argc, char **argv)
{
    const LorieBuffer_Desc *desc;
    int dpix = monitorResolution, dpiy = monitorResolution;
    int ret;
    void *fb = NULL;

    if (lorieScreen.depth != 24) {
        ErrorF("Xlorie currently supports depth 24 only\n");
        return FALSE;
    }

#ifdef DRI3
    if (Dri3 &&
        !dixRegisterPrivateKey(&loriePixmapPrivateKey, PRIVATE_PIXMAP,
                               sizeof(loriePixmapPriv)))
        return FALSE;
#endif

#ifdef PRESENT
    xorg_list_init(&lorieScreen.vblankQueue);
    lorieScreen.vblankInterval =
        1000000 / (lorieScreen.framerate > 0 ?
                   lorieScreen.framerate : LORIE_DEFAULT_FRAMERATE);
    loriePresentUpdateMsc();
#endif

    if (!lorieRenderConnect(lorieScreen.width, lorieScreen.height,
                            lorieScreen.framerate))
        return FALSE;

    lorieScreen.rootBuffer = lorieRenderBuffer();
    lorieScreen.ownsRootBuffer = FALSE;

    if (LorieBuffer_lock(lorieScreen.rootBuffer, &fb) != 0 || !fb) {
        ErrorF("Failed to lock Xlorie framebuffer\n");
        return FALSE;
    }

    desc = LorieBuffer_description(lorieScreen.rootBuffer);
    lorieScreen.fb = fb;
    lorieScreen.screen = pScreen;
    lorieScreen.width = desc->width;
    lorieScreen.height = desc->height;
    lorieScreen.bitsPerPixel = 32;
    lorieScreen.paddedWidth = desc->stride;
    lorieScreen.paddedBytesWidth = desc->stride * 4;

    if (dpix == 0)
        dpix = 100;
    if (dpiy == 0)
        dpiy = 100;

    miSetVisualTypesAndMasks(24,
                             ((1 << TrueColor) | (1 << DirectColor)),
                             8, TrueColor,
                             0xff0000, 0x00ff00, 0x0000ff);
    miSetPixmapDepths();

    ret = fbScreenInit(pScreen, lorieScreen.fb, lorieScreen.width,
                       lorieScreen.height, dpix, dpiy,
                       lorieScreen.paddedWidth, lorieScreen.bitsPerPixel);
    if (!ret)
        return FALSE;

    if (!lorieCreateRootDamage(pScreen))
        lorieLog("Failed to initialize root damage tracking\n");

#ifdef DRI3
    if (Dri3) {
        lorieScreen.destroyPixmap = pScreen->DestroyPixmap;
        pScreen->DestroyPixmap = lorieDestroyPixmap;

        if (!dri3_screen_init(pScreen, &lorieDri3Info))
            return FALSE;
    }
#endif

    if (Render && !fbPictureInit(pScreen, 0, 0))
        return FALSE;

    if (!lorieRandRInit(pScreen))
        return FALSE;

#ifdef PRESENT
    if (!present_screen_init(pScreen, &loriePresentInfo))
        return FALSE;
#endif

    if (!miPointerInitialize(pScreen, &loriePointerSpriteFuncs,
                             &loriePointerCursorFuncs, TRUE))
        return FALSE;

    pScreen->blackPixel = lorieScreen.blackPixel;
    pScreen->whitePixel = lorieScreen.whitePixel;

    if (!fbCreateDefColormap(pScreen))
        return FALSE;

    miSetZeroLineBias(pScreen, lorieScreen.lineBias);

    if (!blockHandlersRegistered) {
        if (!RegisterBlockAndWakeupHandlers(lorieBlockHandler,
                                            lorieWakeupHandler, NULL))
            return FALSE;
        blockHandlersRegistered = TRUE;
    }

    lorieScreen.closeScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = lorieCloseScreen;

    lorieRenderSignalFrame();
    return TRUE;
}

void
InitOutput(ScreenInfo *screenInfo, int argc, char **argv)
{
    int i;
    int numFormats = 0;

    loriePixmapDepths[lorieScreen.depth] = TRUE;

    if (Render) {
        loriePixmapDepths[1] = TRUE;
        loriePixmapDepths[4] = TRUE;
        loriePixmapDepths[8] = TRUE;
        loriePixmapDepths[16] = TRUE;
        loriePixmapDepths[24] = TRUE;
        loriePixmapDepths[32] = TRUE;
    }

    xorgGlxCreateVendor();
    lorieInitClipboard();

    for (i = 1; i <= 32; i++) {
        if (loriePixmapDepths[i]) {
            if (numFormats >= MAXFORMATS)
                FatalError("MAXFORMATS is too small for this server\n");
            screenInfo->formats[numFormats].depth = i;
            screenInfo->formats[numFormats].bitsPerPixel = lorieBitsPerPixel(i);
            screenInfo->formats[numFormats].scanlinePad = BITMAP_SCANLINE_PAD;
            numFormats++;
        }
    }

    screenInfo->imageByteOrder = IMAGE_BYTE_ORDER;
    screenInfo->bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    screenInfo->bitmapScanlinePad = BITMAP_SCANLINE_PAD;
    screenInfo->bitmapBitOrder = BITMAP_BIT_ORDER;
    screenInfo->numPixmapFormats = numFormats;

    if (AddScreen(lorieScreenInit, argc, argv) == -1)
        FatalError("Couldn't add Xlorie screen\n");
}
