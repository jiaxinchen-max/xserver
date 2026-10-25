#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "lorie.h"
#include "log.h"

#define TERMUX_RENDER_SOCKET_PATH "/data/data/com.termux/files/home/tmp/termux-render"

#ifndef AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM
#define AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 5
#endif

#ifndef AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM
#define AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM 2
#endif

/*
 * The app's renderer (termux-x11 lorie renderer.c) samples a root buffer through its ".bgra"
 * shader, i.e. it swaps red and blue, for every AHB whose format is not B8G8R8A8
 * (LorieBuffer_isRgba). Which declaration shows correct colours depends on who writes the buffer:
 *  - GPU (glamor): renders logical RGB straight into the AHB -> declare B8G8R8A8 (no swap).
 *  - software framebuffer (EGL unavailable, -noglamor): X writes its native BGRX bytes, which the
 *    swap in the renderer expects to find in a buffer declared R8G8B8X8, as the legacy in-process
 *    server does.
 * The format is fixed when the buffer is created, so the caller must say whether the GPU path
 * will be used (lorieRenderSetGpuRendering) before lorieRenderConnect().
 */
static int renderBufferFormat = AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;

void
lorieRenderSetGpuRendering(bool gpu)
{
#ifdef XLORIE_RENDER_BUFFER_FORMAT
    (void) gpu;
    renderBufferFormat = XLORIE_RENDER_BUFFER_FORMAT;
#else
    renderBufferFormat = gpu ? AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM
                             : AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;
#endif
}

#ifndef XLORIE_RENDER_BUFFER_TYPE
#if defined(XLORIE_RENDER_USE_FD_ONLY) && XLORIE_RENDER_USE_FD_ONLY
#define XLORIE_RENDER_BUFFER_TYPE LORIEBUFFER_FD
#elif defined(XLORIE_RENDER_USE_AHARDWAREBUFFER) && !XLORIE_RENDER_USE_AHARDWAREBUFFER
#define XLORIE_RENDER_BUFFER_TYPE LORIEBUFFER_FD
#else
#define XLORIE_RENDER_BUFFER_TYPE LORIEBUFFER_AHARDWAREBUFFER
#endif
#endif

static const char *requestedSocketPath;
static bool connected;
static uint64_t activeRootBufferId;

static bool
writeFull(int fd, const void *buffer, size_t size)
{
    size_t offset = 0;

    while (offset < size) {
        ssize_t ret = write(fd, (const char *) buffer + offset, size - offset);

        if (ret > 0) {
            offset += ret;
            continue;
        }
        if (ret < 0 && errno == EINTR)
            continue;
        return false;
    }

    return true;
}

void
lorieRenderSetSocketPath(const char *path)
{
    requestedSocketPath = path;
}

/*
 * libtermux-render hands the root buffer over either as a native AHardwareBuffer handle
 * (TERMUX_RENDER_AHB_RECEIVE=native) or as dma-buf fds (the default). With Android's system
 * EGL/GLES (XLORIE_GL_MODE=system, which is also the default), only the native handle can be
 * imported: eglCreateImageKHR from the dma-buf fails on Adreno, glamor then cannot back the
 * root pixmap on the GPU and nothing is displayed. This must be decided before
 * connectToRender() receives the buffer, i.e. before the EGL mode is resolved, so mirror
 * lorieEglResolveMode() here. An explicit TERMUX_RENDER_AHB_RECEIVE always wins.
 */
static void
lorieRenderChooseReceiveMode(void)
{
    const char *mode = getenv("XLORIE_GL_MODE");

    if (!mode || !*mode)
        mode = getenv("KWIN_ANDROID_GL_MODE");
    if (!mode || !*mode || !strcasecmp(mode, "system"))
        setenv("TERMUX_RENDER_AHB_RECEIVE", "native", 0);
}

bool
lorieRenderConnect(int width, int height, int framerate)
{
    const LorieBuffer_Desc *desc;

    if (connected)
        return true;

    lorieRenderChooseReceiveMode();

    if (requestedSocketPath && strcmp(requestedSocketPath,
                                      TERMUX_RENDER_SOCKET_PATH) != 0) {
        lorieLog("libtermux-render uses %s; ignoring requested socket %s\n",
                 TERMUX_RENDER_SOCKET_PATH, requestedSocketPath);
    }

    setScreenConfig(width, height, framerate, renderBufferFormat,
                    XLORIE_RENDER_BUFFER_TYPE);
    setKeycodeFormat(LORIE_KEYCODE_XKB);
    if (connectToRender() != 0) {
        lorieLog("connectToRender failed: %s\n", strerror(errno));
        return false;
    }

    if (!get_lorieBuffer() || !get_serverState() || get_conn_fd() < 0) {
        lorieLog("termux-render did not initialize required resources\n");
        stopEventLoop();
        return false;
    }

    desc = LorieBuffer_description(get_lorieBuffer());
    get_serverState()->rootWindowTextureID = desc->id;
    activeRootBufferId = desc->id;
    lorieLog("connected termux-render buffer %dx%d stride=%d "
             "format=%d type=%d id=%llu\n",
             desc->width, desc->height, desc->stride, desc->format,
             desc->type,
             (unsigned long long) desc->id);

    connected = true;
    return true;
}

void
lorieRenderDisconnect(void)
{
    if (!connected)
        return;

    connected = false;
    activeRootBufferId = 0;
    stopEventLoop();
}

bool
lorieRenderSignalFrame(void)
{
    LorieBuffer *buffer = get_lorieBuffer();
    struct lorie_shared_server_state *state = get_serverState();

    if (!state || !buffer)
        return false;

    if (!activeRootBufferId)
        activeRootBufferId = LorieBuffer_description(buffer)->id;
    state->rootWindowTextureID = activeRootBufferId;
    state->waitForNextFrame = 0;
    state->drawRequested = 1;
    if (rendererCond)
        pthread_cond_signal(rendererCond);
    return true;
}

bool
lorieRenderUseBuffer(LorieBuffer *buffer)
{
    struct lorie_shared_server_state *state = get_serverState();
    const LorieBuffer_Desc *desc;

    if (!state || !buffer)
        return false;

    desc = LorieBuffer_description(buffer);
    activeRootBufferId = desc->id;
    return lorieRenderSignalFrame();
}

bool
lorieRenderSendEvent(const lorieEvent *event, const void *payload,
                     size_t payloadSize)
{
    int fd = get_conn_fd();

    if (fd < 0 || !event)
        return false;

    if (!writeFull(fd, event, sizeof(*event)))
        return false;

    if (payload && payloadSize && !writeFull(fd, payload, payloadSize))
        return false;

    return true;
}

bool
lorieRenderRegisterBuffer(LorieBuffer *buffer)
{
    lorieLog("Xlorie: render register enter\n");
    if (!buffer)
        return false;

    if (registerBufferToRender(buffer) != 0) {
        lorieLog("registerBufferToRender failed: %s\n", strerror(errno));
        return false;
    }

    return true;
}

bool
lorieRenderUnregisterBuffer(LorieBuffer *buffer)
{
    if (!buffer)
        return false;

    if (unregisterBufferFromRender(buffer) != 0) {
        lorieLog("unregisterBufferFromRender failed: %s\n", strerror(errno));
        return false;
    }

    return true;
}

void
lorieSendClipboardData(const char *data)
{
    size_t len;
    lorieEvent event;

    if (!data)
        return;

    len = strlen(data);
    memset(&event, 0, sizeof(event));
    event.clipboardSend.t = EVENT_CLIPBOARD_SEND;
    event.clipboardSend.count = len;
    (void) lorieRenderSendEvent(&event, data, len);
}

void
lorieRequestClipboard(void)
{
    lorieEvent event;

    memset(&event, 0, sizeof(event));
    event.type = EVENT_CLIPBOARD_REQUEST;
    (void) lorieRenderSendEvent(&event, NULL, 0);
}

LorieBuffer *
lorieRenderBuffer(void)
{
    return get_lorieBuffer();
}

struct lorie_shared_server_state *
lorieRenderState(void)
{
    return get_serverState();
}

int
lorieRenderInputFd(void)
{
    return get_conn_fd();
}
