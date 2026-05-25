#ifndef LORIE_H
#define LORIE_H

#include <stdbool.h>
#include <stddef.h>
#include <X11/Xdefs.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#endif
#include <termux/render/render.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

void lorieRenderSetSocketPath(const char *path);
bool lorieRenderConnect(int width, int height, int framerate);
void lorieRenderDisconnect(void);
bool lorieRenderSignalFrame(void);
bool lorieRenderSendEvent(const lorieEvent *event, const void *payload,
                          size_t payloadSize);
bool lorieRenderRegisterBuffer(LorieBuffer *buffer);
bool lorieRenderUnregisterBuffer(LorieBuffer *buffer);
bool lorieRenderUseBuffer(LorieBuffer *buffer);
LorieBuffer *lorieRenderBuffer(void);
struct lorie_shared_server_state *lorieRenderState(void);
int lorieRenderInputFd(void);

int lorieInputRegisterFd(int fd);
void lorieInputUnregister(void);
void lorieConfigureNotify(int width, int height, int framerate,
                          size_t nameSize, const char *name);

void lorieEnableClipboardSync(Bool enable);
void lorieInitClipboard(void);
void lorieHandleClipboardAnnounce(void);
void lorieHandleClipboardData(const char *data);
void lorieRequestClipboard(void);
void lorieSendClipboardData(const char *data);

#endif
