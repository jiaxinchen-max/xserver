#ifndef LORIE_H
#define LORIE_H

#include <stdbool.h>

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
LorieBuffer *lorieRenderBuffer(void);
struct lorie_shared_server_state *lorieRenderState(void);
int lorieRenderInputFd(void);

int lorieInputRegisterFd(int fd);
void lorieInputUnregister(void);

#endif
