#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "lorie.h"
#include "log.h"

#ifndef TERMUX_RENDER_USE_SEQPACKET
#define TERMUX_RENDER_USE_SEQPACKET 0
#endif

#if TERMUX_RENDER_USE_SEQPACKET && defined(SOCK_SEQPACKET)
#define RENDER_SOCKET_TYPE SOCK_SEQPACKET
#else
#define RENDER_SOCKET_TYPE SOCK_STREAM
#endif

#define MAX_CONNECT_RETRIES 5

static char renderSocketPath[sizeof(LORIE_RENDER_DEFAULT_SOCKET) + 128] =
    LORIE_RENDER_DEFAULT_SOCKET;
static int renderFd = -1;
static int inputFd = -1;
static LorieBuffer *rootBuffer;
static struct lorie_shared_server_state *serverState;

const char *
lorieEventTypeName(uint8_t type)
{
    switch (type) {
    case EVENT_SHARED_SERVER_STATE: return "EVENT_SHARED_SERVER_STATE";
    case EVENT_ADD_BUFFER: return "EVENT_ADD_BUFFER";
    case EVENT_REMOVE_BUFFER: return "EVENT_REMOVE_BUFFER";
    case EVENT_SCREEN_SIZE: return "EVENT_SCREEN_SIZE";
    case EVENT_TOUCH: return "EVENT_TOUCH";
    case EVENT_MOUSE: return "EVENT_MOUSE";
    case EVENT_KEY: return "EVENT_KEY";
    case EVENT_STYLUS: return "EVENT_STYLUS";
    case EVENT_STYLUS_ENABLE: return "EVENT_STYLUS_ENABLE";
    case EVENT_UNICODE: return "EVENT_UNICODE";
    case EVENT_CLIPBOARD_ENABLE: return "EVENT_CLIPBOARD_ENABLE";
    case EVENT_CLIPBOARD_ANNOUNCE: return "EVENT_CLIPBOARD_ANNOUNCE";
    case EVENT_CLIPBOARD_REQUEST: return "EVENT_CLIPBOARD_REQUEST";
    case EVENT_CLIPBOARD_SEND: return "EVENT_CLIPBOARD_SEND";
    case EVENT_WINDOW_FOCUS_CHANGED: return "EVENT_WINDOW_FOCUS_CHANGED";
    case EVENT_APPLY_SERVER_STATE: return "EVENT_APPLY_SERVER_STATE";
    case EVENT_APPLY_BUFFER: return "EVENT_APPLY_BUFFER";
    case EVENT_APPLY_EVENT_FD: return "EVENT_APPLY_EVENT_FD";
    case EVENT_SHARED_EVENT_FD: return "EVENT_SHARED_EVENT_FD";
    case EVENT_SERVER_VERIFY_SUCCEED: return "EVENT_SERVER_VERIFY_SUCCEED";
    case EVENT_CLIENT_VERIFY_SUCCEED: return "EVENT_CLIENT_VERIFY_SUCCEED";
    case EVENT_STOP_RENDER: return "EVENT_STOP_RENDER";
    default: return "EVENT_UNKNOWN";
    }
}

static int
readFull(int fd, void *buffer, size_t size)
{
    size_t offset = 0;

    while (offset < size) {
        ssize_t count = read(fd, (char *) buffer + offset, size - offset);

        if (count > 0) {
            offset += count;
            continue;
        }
        if (count == 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (errno == EINTR)
            continue;
        return -1;
    }

    return 0;
}

static int
writeFull(int fd, const void *buffer, size_t size)
{
    size_t offset = 0;

    while (offset < size) {
        ssize_t count = write(fd, (const char *) buffer + offset, size - offset);

        if (count > 0) {
            offset += count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return -1;
    }

    return 0;
}

static int
readEvent(int fd, lorieEvent *event)
{
    memset(event, 0, sizeof(*event));
    return readFull(fd, event, sizeof(*event));
}

static int
expectEvent(int fd, eventType expected, lorieEvent *event)
{
    if (readEvent(fd, event) != 0)
        return -1;

    if (event->type != expected) {
        lorieLog("expected %s, got %s\n",
                 lorieEventTypeName(expected),
                 lorieEventTypeName(event->type));
        errno = EPROTO;
        return -1;
    }

    return 0;
}

void
lorieRenderSetSocketPath(const char *path)
{
    if (!path || !path[0])
        return;

    snprintf(renderSocketPath, sizeof(renderSocketPath), "%s", path);
}

static int
connectSocket(void)
{
    struct sockaddr_un addr;
    int fd;

    fd = socket(AF_UNIX, RENDER_SOCKET_TYPE, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", renderSocketPath);

    if (connect(fd, (const struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static bool
performHandshake(int fd, int width, int height, int framerate)
{
    lorieEvent event;
    int stateFd;

    if (writeFull(fd, LORIE_RENDER_MAGIC, sizeof(LORIE_RENDER_MAGIC)) != 0)
        return false;

    if (expectEvent(fd, EVENT_SERVER_VERIFY_SUCCEED, &event) != 0)
        return false;

    event = (lorieEvent) { .type = EVENT_APPLY_BUFFER };
    if (writeFull(fd, &event, sizeof(event)) != 0)
        return false;

    event = (lorieEvent) {
        .screenSize = {
            .t = EVENT_SCREEN_SIZE,
            .width = width,
            .height = height,
            .framerate = framerate,
            .format = AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM,
            .type = LORIEBUFFER_FD,
        },
    };
    if (writeFull(fd, &event, sizeof(event)) != 0)
        return false;

    if (expectEvent(fd, EVENT_ADD_BUFFER, &event) != 0)
        return false;

    LorieBuffer_recvHandleFromUnixSocket(fd, &rootBuffer);
    if (!rootBuffer)
        return false;

    event = (lorieEvent) { .type = EVENT_APPLY_SERVER_STATE };
    if (writeFull(fd, &event, sizeof(event)) != 0)
        return false;

    if (expectEvent(fd, EVENT_SHARED_SERVER_STATE, &event) != 0)
        return false;

    stateFd = ancil_recv_fd(fd);
    if (stateFd < 0)
        return false;

    serverState = mmap(NULL, sizeof(*serverState), PROT_READ | PROT_WRITE,
                       MAP_SHARED, stateFd, 0);
    close(stateFd);
    if (serverState == MAP_FAILED) {
        serverState = NULL;
        return false;
    }

    event = (lorieEvent) { .type = EVENT_APPLY_EVENT_FD };
    if (writeFull(fd, &event, sizeof(event)) != 0)
        return false;

    if (expectEvent(fd, EVENT_SHARED_EVENT_FD, &event) != 0)
        return false;

    inputFd = ancil_recv_fd(fd);
    if (inputFd < 0)
        return false;

    event = (lorieEvent) { .type = EVENT_CLIENT_VERIFY_SUCCEED };
    if (writeFull(fd, &event, sizeof(event)) != 0)
        return false;

    return true;
}

bool
lorieRenderConnect(int width, int height, int framerate)
{
    int retry;

    if (renderFd >= 0)
        return true;

    for (retry = 0; retry < MAX_CONNECT_RETRIES; retry++) {
        renderFd = connectSocket();
        if (renderFd < 0) {
            lorieLog("connect %s failed: %s\n",
                     renderSocketPath, strerror(errno));
            if (retry + 1 < MAX_CONNECT_RETRIES)
                sleep(1);
            continue;
        }

        if (performHandshake(renderFd, width, height, framerate)) {
            const LorieBuffer_Desc *desc = LorieBuffer_description(rootBuffer);
            serverState->rootWindowTextureID = desc->id;
            lorieLog("connected render buffer %dx%d stride=%d id=%llu\n",
                     desc->width, desc->height, desc->stride,
                     (unsigned long long) desc->id);
            return true;
        }

        lorieLog("render handshake failed: %s\n", strerror(errno));
        lorieRenderDisconnect();
        if (retry + 1 < MAX_CONNECT_RETRIES)
            sleep(1);
    }

    return false;
}

void
lorieRenderDisconnect(void)
{
    if (renderFd >= 0) {
        lorieEvent event = { .type = EVENT_STOP_RENDER };
        (void) writeFull(renderFd, &event, sizeof(event));
    }

    if (inputFd >= 0) {
        close(inputFd);
        inputFd = -1;
    }

    if (serverState) {
        munmap(serverState, sizeof(*serverState));
        serverState = NULL;
    }

    if (rootBuffer) {
        LorieBuffer_release(rootBuffer);
        rootBuffer = NULL;
    }

    if (renderFd >= 0) {
        close(renderFd);
        renderFd = -1;
    }
}

bool
lorieRenderSignalFrame(void)
{
    const LorieBuffer_Desc *desc;

    if (!serverState || !rootBuffer)
        return false;

    desc = LorieBuffer_description(rootBuffer);
    serverState->rootWindowTextureID = desc->id;
    serverState->waitForNextFrame = 0;
    serverState->drawRequested = 1;
    pthread_cond_signal(&serverState->cond);
    return true;
}

LorieBuffer *
lorieRenderBuffer(void)
{
    return rootBuffer;
}

struct lorie_shared_server_state *
lorieRenderState(void)
{
    return serverState;
}

int
lorieRenderInputFd(void)
{
    return inputFd;
}
