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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/X.h>
#include <X11/Xos.h>
#include <X11/Xproto.h>

#include "colormapst.h"
#include "dix.h"
#include "fb.h"
#include "gcstruct.h"
#include "glx_extinit.h"
#include "input.h"
#include "micmap.h"
#include "miline.h"
#include "mipointer.h"
#include "os.h"
#include "randrstr.h"
#include "scrnintstr.h"
#include "servermd.h"

#include "lorie.h"
#include "log.h"

#define LORIE_DEFAULT_WIDTH 1280
#define LORIE_DEFAULT_HEIGHT 720
#define LORIE_DEFAULT_DEPTH 24
#define LORIE_DEFAULT_FRAMERATE 60
#define LORIE_DEFAULT_LINEBIAS 0
#define LORIE_DEFAULT_BLACKPIXEL 0
#define LORIE_DEFAULT_WHITEPIXEL 1

typedef struct {
    int width;
    int height;
    int depth;
    int bitsPerPixel;
    int paddedWidth;
    int paddedBytesWidth;
    int framerate;
    char *fb;
    Pixel blackPixel;
    Pixel whitePixel;
    unsigned int lineBias;
    CloseScreenProcPtr closeScreen;
} lorieScreenInfo;

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

    return 0;
}

static void
lorieBlockHandler(void *blockData, void *timeout)
{
    lorieRenderSignalFrame();
}

static void
lorieWakeupHandler(void *blockData, int result)
{
}

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

static Bool
lorieCloseScreen(ScreenPtr pScreen)
{
    pScreen->CloseScreen = lorieScreen.closeScreen;

    if (blockHandlersRegistered) {
        RemoveBlockAndWakeupHandlers(lorieBlockHandler, lorieWakeupHandler,
                                     NULL);
        blockHandlersRegistered = FALSE;
    }

    if (pScreen->devPrivate)
        (*pScreen->DestroyPixmap) (pScreen->devPrivate);
    pScreen->devPrivate = NULL;

    lorieInputUnregister();
    LorieBuffer_unlock(lorieRenderBuffer());
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

static Bool
lorieRRScreenSetSize(ScreenPtr pScreen, CARD16 width, CARD16 height,
                     CARD32 mmWidth, CARD32 mmHeight)
{
    if (width != pScreen->width || height != pScreen->height)
        return FALSE;

    pScreen->mmWidth = mmWidth;
    pScreen->mmHeight = mmHeight;
    RRScreenSizeNotify(pScreen);
    RRTellChanged(pScreen);
    return TRUE;
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

    RRScreenSetSizeRange(pScreen, pScreen->width, pScreen->height,
                         pScreen->width, pScreen->height);

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

    if (!lorieRenderConnect(lorieScreen.width, lorieScreen.height,
                            lorieScreen.framerate))
        return FALSE;

    if (LorieBuffer_lock(lorieRenderBuffer(), &fb) != 0 || !fb) {
        ErrorF("Failed to lock Xlorie framebuffer\n");
        return FALSE;
    }

    desc = LorieBuffer_description(lorieRenderBuffer());
    lorieScreen.fb = fb;
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

    if (Render && !fbPictureInit(pScreen, 0, 0))
        return FALSE;

    if (!lorieRandRInit(pScreen))
        return FALSE;

    miDCInitialize(pScreen, &loriePointerCursorFuncs);

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
