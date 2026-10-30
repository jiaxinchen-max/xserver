#!/usr/bin/env python3
"""Compile the real RandR builder, init, ConfigureNotify and rate parser.

Only RandR/X server boundaries are mocked. No EGL, device or Android build.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
source = (args.source / 'hw/lorie/InitOutput.c').read_text()
helper = source[source.index('static Bool\nlorieRandRModeInfo('):source.index('static Bool\nlorieCreateScreenResources(')]
configure = source[source.index('void\nlorieConfigureNotify('):source.index('static Bool\nlorieScreenInit(')]
rate_parser = source[source.index('    if (strcmp(argv[i], "-framerate") == 0) {'):source.index('    if (strcmp(argv[i], "-render-socket") == 0) {')]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
typedef int Bool;
typedef uint16_t CARD16;
typedef uint32_t CARD32;
typedef int Rotation;
#define TRUE 1
#define FALSE 0
#define RANDR_12_INTERFACE 1
#define RANDR_13_INTERFACE 1
#define PRESENT 1
#define RR_Rotate_0 1
#define RR_Connected 0
#define ErrorF(...) fprintf(stderr, __VA_ARGS__)
#define lorieLog(...) ((void)0)
static void FatalError(const char *format, ...) __attribute__((noreturn));
static void FatalError(const char *format, ...) {
    va_list ap; va_start(ap,format); vfprintf(stderr,format,ap); va_end(ap);
    exit(47);
}
#define CHECK_FOR_REQUIRED_ARGUMENTS(n) do { if (i+(n)>=argc) FatalError("missing rate\n"); } while(0)
typedef struct {
    uint32_t id;
    uint16_t width,height;
    uint32_t dotClock;
    uint16_t hSyncStart,hSyncEnd,hTotal,hSkew,vSyncStart,vSyncEnd,vTotal,nameLength;
    uint32_t modeFlags;
} xRRModeInfo;
typedef struct { xRRModeInfo mode; char name[64]; } RRModeRec, *RRModePtr;
typedef struct { int unused; } RROutputRec, *RROutputPtr;
typedef struct { RRModePtr mode; } RRCrtcRec, *RRCrtcPtr;
typedef struct { int width,height; } ScreenRec, *ScreenPtr;
typedef struct {
    void *rrGetInfo,*rrCrtcSet,*rrScreenSetSize,*rrOutputSetProperty;
    void *rrOutputGetProperty,*rrOutputValidateMode,*rrModeDestroy;
} rrScrPrivRec, *rrScrPrivPtr;
static rrScrPrivRec priv;
static RROutputRec output_rec;
static RRCrtcRec crtc_rec;
static struct { ScreenPtr screen; int framerate; uint64_t vblankInterval; } lorieScreen;
static int monitorResolution=96;
static int tell_events, size_calls, mode_count;
static RRModeRec modes[128];
static RRModePtr advertised;
#define RRScreenInit(screen) TRUE
#define rrGetScrPriv(screen) (&priv)
#define lorieRRGetInfo NULL
#define lorieRRCrtcSet NULL
#define lorieRRScreenSetSize NULL
#define lorieRROutputValidateMode NULL
#define RRScreenSetSizeRange(...) ((void)0)
#define RRCrtcCreate(...) (&crtc_rec)
#define RRCrtcGammaSetSize(...) ((void)0)
#define RROutputCreate(...) (&output_rec)
#define RROutputSetClones(...) TRUE
#define RROutputSetCrtcs(...) TRUE
#define RROutputSetConnection(...) TRUE
#define RRFirstOutput(screen) (&output_rec)
#define RRFirstEnabledCrtc(screen) (&crtc_rec)
static RRModePtr RRModeGet(xRRModeInfo *info,const char *name) {
    for (int i=0;i<mode_count;i++)
        if (!memcmp(&modes[i].mode,info,sizeof(*info)) && !strcmp(modes[i].name,name)) return &modes[i];
    assert(mode_count<128);
    RRModePtr mode=&modes[mode_count++]; mode->mode=*info;
    snprintf(mode->name,sizeof(mode->name),"%s",name);
    return mode;
}
static Bool RROutputSetModes(RROutputPtr output,RRModePtr *mode,int count,int preferred) {
    (void)output;(void)preferred;assert(count==1);advertised=*mode;return TRUE;
}
static Bool RRCrtcNotify(RRCrtcPtr crtc,RRModePtr mode,int x,int y,Rotation rotation,void *transform,int count,RROutputPtr *outputs) {
    (void)x;(void)y;(void)rotation;(void)transform;(void)outputs;assert(count==1);crtc->mode=mode;return TRUE;
}
static Bool RRScreenSizeSet(ScreenPtr screen,int width,int height,CARD32 mmWidth,CARD32 mmHeight) {
    (void)mmWidth;(void)mmHeight;size_calls++;
    /* As in RandR, equal dimensions need not publish a size event. */
    if (screen->width==width && screen->height==height) return TRUE;
    screen->width=width;screen->height=height;return TRUE;
}
static void RRTellChanged(ScreenPtr screen) {(void)screen;tell_events++;}
'''
harness += helper + configure
harness += '\nstatic int parse_rate(int argc,char **argv,int i) {\n' + rate_parser + 'return 0;\n}\n'
harness += r'''
static void verify(RRModePtr mode,int width,int height,int hz) {
    assert(mode && mode==advertised);
    assert(mode->mode.width==width && mode->mode.height==height);
    assert(mode->mode.hTotal>=width && mode->mode.vTotal>=height);
    double reported=(double)mode->mode.dotClock/mode->mode.hTotal/mode->mode.vTotal;
    assert(reported==hz);  /* What XRandR/Mutter compute; zero/rounding fail. */
    char expected[64];snprintf(expected,sizeof(expected),"%dx%d@%d",width,height,hz);
    assert(!strcmp(mode->name,expected));
}
int main(int argc,char **argv) {
    if (argc>1 && !strcmp(argv[1],"parse")) {
        char *command[]={"Xlorie","-framerate",argc>2?argv[2]:NULL};
        parse_rate(argc>2?3:2,command,1);
        printf("%d\n",lorieScreen.framerate);return 0;
    }
    ScreenRec screen={1280,720};lorieScreen.screen=&screen;lorieScreen.framerate=30;
    assert(lorieRandRInit(&screen));verify(crtc_rec.mode,1280,720,30);
    if (argc>1 && !strcmp(argv[1],"bad-configure")) {
        lorieConfigureNotify(1280,720,0,0,NULL);return 99;
    }
    if (argc>1 && !strcmp(argv[1],"overflow-configure")) {
        lorieConfigureNotify(8192,8192,120,0,NULL);return 99;
    }
    RRModePtr rate30=crtc_rec.mode;
    lorieConfigureNotify(1280,720,60,0,NULL);
    verify(crtc_rec.mode,1280,720,60);assert(crtc_rec.mode!=rate30);
    assert(lorieScreen.framerate==60 && lorieScreen.vblankInterval==1000000/60);
    assert(tell_events==1 && size_calls==1); /* refresh-only event must publish */
    lorieConfigureNotify(1280,720,30,0,NULL);
    verify(crtc_rec.mode,1280,720,30);assert(crtc_rec.mode==rate30);
    assert(lorieScreen.framerate==30 && lorieScreen.vblankInterval==1000000/30);
    assert(tell_events==2);
    lorieConfigureNotify(1920,1080,120,0,NULL);
    verify(crtc_rec.mode,1920,1080,120);assert(screen.width==1920 && screen.height==1080);
    assert(lorieScreen.vblankInterval==1000000/120 && tell_events==3);
    lorieConfigureNotify(1920,1080,120,0,NULL);
    verify(crtc_rec.mode,1920,1080,120);assert(tell_events==4);
    for (int hz=1;hz<=120;hz++) {
        xRRModeInfo info;char name[64];
        assert(lorieRandRModeInfo(1280,720,hz,&info,name,sizeof(name)));
        assert((double)info.dotClock/info.hTotal/info.vTotal==hz);
        assert(info.hSyncStart<=info.hSyncEnd && info.hSyncEnd<=info.hTotal);
        assert(info.vSyncStart<=info.vSyncEnd && info.vSyncEnd<=info.vTotal);
    }
    xRRModeInfo info;char name[64];
    assert(lorieRandRModeInfo(65535,1,65537,&info,name,sizeof(name)));
    assert(info.dotClock==UINT32_MAX); /* Inclusive 32-bit protocol boundary. */
    assert(!lorieRandRModeInfo(8192,8192,120,&info,name,sizeof(name)));
    assert(!lorieRandRModeInfo(1280,720,0,&info,name,sizeof(name)));
    assert(!lorieRandRModeInfo(1280,720,-1,&info,name,sizeof(name)));
    assert(!lorieRandRModeInfo(65536,720,30,&info,name,sizeof(name)));
    assert(!lorieRandRModeInfo(1280,720,30,&info,name,2));
    lorieScreen.framerate=0;assert(!lorieRandRInit(&screen));
    lorieScreen.framerate=120;screen.width=8192;screen.height=8192;assert(!lorieRandRInit(&screen));
    puts("PASS: real RandR init/resize/refresh-only transitions, exact 1..120Hz, limits and notifications");
}
'''
with tempfile.TemporaryDirectory(prefix='xserver-randr-timings-') as temporary:
    base = Path(temporary)
    test, binary = base/'test.c', base/'test'
    test.write_text(harness)
    subprocess.run([args.cc,'-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(test),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
    for hz in ('1','30','60','120','1000000'):
        result=subprocess.run([str(binary),'parse',hz],capture_output=True,text=True)
        assert result.returncode==0 and result.stdout.strip()==hz,(hz,result)
    for hz in ('0','-1','30junk','', '99999999999999999999999999','1000001'):
        result=subprocess.run([str(binary),'parse',hz],capture_output=True,text=True)
        assert result.returncode==47,(hz,result)
    for extra in (['parse'],['bad-configure'],['overflow-configure']):
        result=subprocess.run([str(binary),*extra],capture_output=True,text=True)
        assert result.returncode==47,(extra,result)
print('PASS: invalid explicit rates and unrepresentable Configure requests fail without defaulting')
