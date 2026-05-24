#ifndef LORIE_H
#define LORIE_H

#include <android/hardware_buffer.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <pthread.h>

#ifndef AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM
#define AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 5
#endif

#define LORIE_RENDER_MAGIC "0xDEADBEEF"
#define LORIE_RENDER_DEFAULT_SOCKET \
    "/data/data/com.termux/files/home/tmp/termux-render"

typedef enum {
    LORIEBUFFER_UNKNOWN = 0,
    LORIEBUFFER_REGULAR = 1,
    LORIEBUFFER_FD = 2,
    LORIEBUFFER_AHARDWAREBUFFER = 3,
} LorieBufferType;

typedef struct {
    int32_t width, height, stride;
    uint8_t format, type;
    uint64_t id;
    AHardwareBuffer *buffer;
    void *data;
} LorieBuffer_Desc;

typedef struct LorieBuffer LorieBuffer;

typedef enum {
    EVENT_UNKNOWN = 0,
    EVENT_SHARED_SERVER_STATE,
    EVENT_ADD_BUFFER,
    EVENT_REMOVE_BUFFER,
    EVENT_SCREEN_SIZE,
    EVENT_TOUCH,
    EVENT_MOUSE,
    EVENT_KEY,
    EVENT_STYLUS,
    EVENT_STYLUS_ENABLE,
    EVENT_UNICODE,
    EVENT_CLIPBOARD_ENABLE,
    EVENT_CLIPBOARD_ANNOUNCE,
    EVENT_CLIPBOARD_REQUEST,
    EVENT_CLIPBOARD_SEND,
    EVENT_WINDOW_FOCUS_CHANGED,
    EVENT_APPLY_SERVER_STATE,
    EVENT_APPLY_BUFFER,
    EVENT_APPLY_EVENT_FD,
    EVENT_SHARED_EVENT_FD,
    EVENT_SERVER_VERIFY_SUCCEED,
    EVENT_CLIENT_VERIFY_SUCCEED,
    EVENT_STOP_RENDER,
} eventType;

typedef union {
    uint8_t type;
    struct {
        uint8_t t;
        uint16_t width, height, framerate;
        size_t name_size;
        char *name;
        uint8_t format;
        uint8_t type;
    } screenSize;
    struct {
        uint8_t t;
        unsigned long id;
    } removeBuffer;
    struct {
        uint8_t t;
        uint16_t type, id, x, y;
    } touch;
    struct {
        uint8_t t;
        float x, y;
        uint8_t detail, down, relative;
    } mouse;
    struct {
        uint8_t t;
        uint16_t key;
        uint8_t state;
    } key;
    struct {
        uint8_t t;
        float x, y;
        uint16_t pressure;
        int8_t tilt_x, tilt_y;
        int16_t orientation;
        uint8_t buttons, eraser, mouse;
    } stylus;
    struct {
        uint8_t t, enable;
    } stylusEnable;
    struct {
        uint8_t t;
        uint32_t code;
    } unicode;
    struct {
        uint8_t t;
        uint8_t enable;
    } clipboardEnable;
    struct {
        uint8_t t;
        uint32_t count;
    } clipboardSend;
} lorieEvent;

struct lorie_shared_server_state {
    pthread_mutex_t lock;
    pid_t lockingPid;
    pthread_cond_t cond;
    uint64_t rootWindowTextureID;
    volatile uint8_t drawRequested;
    volatile uint8_t surfaceAvailable;
    volatile uint8_t waitForNextFrame;
    volatile int renderedFrames;
    struct {
        pthread_mutex_t lock;
        pid_t lockingPid;
        uint32_t x, y, xhot, yhot, width, height;
        uint32_t bits[512 * 512];
        volatile uint8_t updated, moved;
    } cursor;
};

const LorieBuffer_Desc *LorieBuffer_description(LorieBuffer *buffer);
int LorieBuffer_lock(LorieBuffer *buffer, void **out);
int LorieBuffer_unlock(LorieBuffer *buffer);
void LorieBuffer_release(LorieBuffer *buffer);
void LorieBuffer_recvHandleFromUnixSocket(int socketFd, LorieBuffer **outBuffer);
int ancil_recv_fd(int sock);

const char *lorieEventTypeName(uint8_t type);
void lorieRenderSetSocketPath(const char *path);
bool lorieRenderConnect(int width, int height, int framerate);
void lorieRenderDisconnect(void);
bool lorieRenderSignalFrame(void);
LorieBuffer *lorieRenderBuffer(void);
struct lorie_shared_server_state *lorieRenderState(void);
int lorieRenderInputFd(void);

int lorieInputRegisterFd(int fd);
void lorieInputUnregister(void);

#endif
