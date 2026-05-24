#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/hardware_buffer.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "list.h"
#include "lorie.h"
#include "log.h"

struct LorieBuffer {
    int16_t refcount;
    LorieBuffer_Desc desc;
    int8_t locked;
    void *lockedData;
    int fd;
    size_t size;
    off_t offset;
    GLuint id;
    EGLImage image;
    struct xorg_list link;
};

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

const LorieBuffer_Desc *
LorieBuffer_description(LorieBuffer *buffer)
{
    static const LorieBuffer_Desc empty = { 0 };

    return buffer ? &buffer->desc : &empty;
}

int
LorieBuffer_lock(LorieBuffer *buffer, void **out)
{
    int ret = 0;

    if (!buffer)
        return ENODEV;

    if (buffer->locked) {
        if (out)
            *out = buffer->lockedData;
        return 0;
    }

    if (buffer->desc.type == LORIEBUFFER_REGULAR ||
        buffer->desc.type == LORIEBUFFER_FD) {
        buffer->lockedData = buffer->desc.data;
    }
    else if (buffer->desc.type == LORIEBUFFER_AHARDWAREBUFFER) {
        ret = AHardwareBuffer_lock(buffer->desc.buffer,
                                   AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                                   AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                                   -1, NULL, &buffer->lockedData);
    }
    else {
        return EINVAL;
    }

    if (out)
        *out = buffer->lockedData;

    if (ret == 0)
        buffer->locked = 1;

    return ret;
}

int
LorieBuffer_unlock(LorieBuffer *buffer)
{
    int ret = 0;

    if (!buffer)
        return ENODEV;

    if (!buffer->locked)
        return 0;

    if (buffer->desc.type == LORIEBUFFER_AHARDWAREBUFFER)
        ret = AHardwareBuffer_unlock(buffer->desc.buffer, NULL);

    buffer->lockedData = NULL;
    buffer->locked = 0;
    return ret;
}

void
LorieBuffer_release(LorieBuffer *buffer)
{
    if (!buffer)
        return;

    LorieBuffer_unlock(buffer);

    if (buffer->desc.type == LORIEBUFFER_FD && buffer->desc.data)
        munmap(buffer->desc.data, buffer->size);

    if (buffer->fd >= 0)
        close(buffer->fd);

    if (buffer->desc.type == LORIEBUFFER_AHARDWAREBUFFER && buffer->desc.buffer)
        AHardwareBuffer_release(buffer->desc.buffer);

    free(buffer);
}

int
ancil_recv_fd(int sock)
{
    char byte = 0;
    struct iovec iov = { .iov_base = &byte, .iov_len = sizeof(byte) };
    char control[CMSG_SPACE(sizeof(int))] = { 0 };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    struct cmsghdr *cmsg;
    int fd = -1;

    if (recvmsg(sock, &msg, 0) < 0)
        return -1;

    cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg ||
        cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len < CMSG_LEN(sizeof(int))) {
        errno = EPROTO;
        return -1;
    }

    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return fd;
}

void
LorieBuffer_recvHandleFromUnixSocket(int socketFd, LorieBuffer **outBuffer)
{
    LorieBuffer wire = { 0 };
    LorieBuffer *buffer;

    if (outBuffer)
        *outBuffer = NULL;

    if (socketFd < 0 || readFull(socketFd, &wire, sizeof(wire)) != 0)
        return;

    wire.refcount = 1;
    wire.locked = 0;
    wire.lockedData = NULL;
    wire.image = NULL;
    wire.fd = -1;

    if (wire.desc.type == LORIEBUFFER_FD) {
        size_t size = wire.desc.stride * wire.desc.height * sizeof(uint32_t);

        wire.fd = ancil_recv_fd(socketFd);
        if (wire.fd < 0) {
            lorieLog("failed to receive LorieBuffer fd: %s\n", strerror(errno));
            return;
        }

        wire.size = size;
        wire.offset = 0;
        wire.desc.data = mmap(NULL, size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, wire.fd, 0);
        if (wire.desc.data == MAP_FAILED) {
            lorieLog("failed to map LorieBuffer fd: %s\n", strerror(errno));
            close(wire.fd);
            return;
        }
    }
    else if (wire.desc.type == LORIEBUFFER_AHARDWAREBUFFER) {
        AHardwareBuffer_recvHandleFromUnixSocket(socketFd, &wire.desc.buffer);
        if (!wire.desc.buffer)
            return;
    }
    else {
        errno = EPROTO;
        return;
    }

    buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        if (wire.desc.type == LORIEBUFFER_FD && wire.desc.data)
            munmap(wire.desc.data, wire.size);
        if (wire.fd >= 0)
            close(wire.fd);
        if (wire.desc.type == LORIEBUFFER_AHARDWAREBUFFER && wire.desc.buffer)
            AHardwareBuffer_release(wire.desc.buffer);
        return;
    }

    *buffer = wire;
    xorg_list_init(&buffer->link);
    if (outBuffer)
        *outBuffer = buffer;
}
