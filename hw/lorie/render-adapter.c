#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <errno.h>
#include <string.h>

#include "lorie.h"
#include "log.h"

#define TERMUX_RENDER_SOCKET_PATH "/data/data/com.termux/files/home/tmp/termux-render"

static const char *requestedSocketPath;
static bool connected;

void
lorieRenderSetSocketPath(const char *path)
{
    requestedSocketPath = path;
}

bool
lorieRenderConnect(int width, int height, int framerate)
{
    const LorieBuffer_Desc *desc;

    if (connected)
        return true;

    if (requestedSocketPath && strcmp(requestedSocketPath,
                                      TERMUX_RENDER_SOCKET_PATH) != 0) {
        lorieLog("libtermux-render uses %s; ignoring requested socket %s\n",
                 TERMUX_RENDER_SOCKET_PATH, requestedSocketPath);
    }

    if (connectToRenderWithConfig(width, height, framerate,
                                  AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM,
                                  LORIEBUFFER_AHARDWAREBUFFER) != 0) {
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
    stopEventLoop();
}

bool
lorieRenderSignalFrame(void)
{
    const LorieBuffer_Desc *desc;
    LorieBuffer *buffer = get_lorieBuffer();
    struct lorie_shared_server_state *state = get_serverState();

    if (!state || !buffer)
        return false;

    desc = LorieBuffer_description(buffer);
    state->rootWindowTextureID = desc->id;
    state->waitForNextFrame = 0;
    state->drawRequested = 1;
    pthread_cond_signal(&state->cond);
    return true;
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
