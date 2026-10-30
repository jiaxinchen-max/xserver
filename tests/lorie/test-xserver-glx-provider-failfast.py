#!/usr/bin/env python3
"""Inject provider allocation and incomplete core-init failures; forbid fallback."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument('source',type=Path)
parser.add_argument('--cc',default='cc')
parser.add_argument('--provider',type=Path)
args=parser.parse_args()
source=(args.provider or args.source/'hw/lorie/lorie-glx.c').read_text()
header=(args.source/'glx/glxscreens.h').read_text()
config=re.search(r'typedef struct __GLXconfig __GLXconfig;\nstruct __GLXconfig \{.*?\n\};',header,re.S).group(0)
screen=re.search(r'typedef struct __GLXscreen __GLXscreen;\nstruct __GLXscreen \{.*?\n\};',header,re.S).group(0)
constants=source[source.index('#ifndef GLX_TRUE_COLOR'):source.index('/* ------------------------------------------------------------------ */')]
init=source[source.index('static void\nlorieGlxConfigInit('):source.index('/* Context shells:')]
create=source[source.index('static __GLXconfig *\nlorieGlxCreateConfigs('):source.index('static __GLXscreen *\nlorieGlxScreenProbe(')]
probe=source[source.index('static __GLXscreen *\nlorieGlxScreenProbe('):source.index('__GLXprovider lorieGlxProvider')]
harness=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef int GLint,Bool;
typedef unsigned int GLuint,XID;
typedef unsigned char GLboolean;
typedef void __GLXcontext;
typedef void __GLXdrawable;
typedef void *ClientPtr,*DrawablePtr;
#define __GLX_EXT_BYTES 64
#define GLX_TEXTURE_1D_BIT_EXT 1
#define GLX_TEXTURE_2D_BIT_EXT 2
#define GLX_TEXTURE_RECTANGLE_BIT_EXT 4
typedef struct { int class,nplanes; unsigned long redMask,greenMask,blueMask; } Visual;
typedef struct Screen {int myNum,numVisuals;Visual *visuals;} ScreenRec,*ScreenPtr;
'''
harness+='\n'.join((config,screen,constants))
harness+=r'''
static int failure=-1,allocation,core_case;
static void FatalError(const char *format,...) __attribute__((noreturn,unused));
static void FatalError(const char *format,...) {
    va_list ap;va_start(ap,format);vfprintf(stderr,format,ap);va_end(ap);exit(47);
}
static void *failing_calloc(size_t count,size_t size) {
    if (allocation++==failure) return NULL;
    return calloc(count,size);
}
static char *failing_strdup(const char *text) {
    if (allocation++==failure) return NULL;
    return strdup(text);
}
/* Only the void core-init boundary is mocked. Its states model the early
 * return and missing fields the provider must refuse before returning. */
static void __glXScreenInit(__GLXscreen *screen,ScreenPtr output) {
    int i=0;
    if (core_case==1) return; /* private-key registration failed */
    screen->pScreen=output;
    screen->GLextensions=strdup("GL_EXT_texture");
    screen->GLXextensions=strdup("GLX_EXT_libglvnd");
    for (__GLXconfig *c=screen->fbconfigs;c;c=c->next) {c->visualID=++i;c->fbconfigID=i;}
    screen->numFBConfigs=i;
    screen->visuals=calloc(i,sizeof(*screen->visuals));screen->numVisuals=i;
    i=0;for (__GLXconfig *c=screen->fbconfigs;c;c=c->next) screen->visuals[i++]=c;
    switch(core_case) {
      case 2:free(screen->GLextensions);screen->GLextensions=NULL;break;
      case 3:screen->GLextensions[0]=0;break;
      case 4:free(screen->GLXextensions);screen->GLXextensions=NULL;break;
      case 5:screen->GLXextensions[0]=0;break;
      case 6:free(screen->visuals);screen->visuals=NULL;break;
      case 7:screen->numVisuals=0;break;
      case 8:screen->numFBConfigs=0;break;
      case 9:screen->numVisuals=screen->numFBConfigs+1;break;
      case 10:screen->visuals[0]=NULL;break;
      case 11:screen->visuals[0]->visualID=0;break;
      case 12:output->visuals=NULL;break;
      case 13:output->numVisuals=0;break;
      case 14:screen->fbconfigs=NULL;break;
    }
}
#define calloc failing_calloc
#define strdup failing_strdup
#define lorieGlxScreenDestroy NULL
#define lorieGlxCreateContext NULL
#define lorieGlxCreateDrawable NULL
#define lorieGlxSwapInterval NULL
#define __glXInitExtensionEnableBits(bits) ((void)(bits))
#define __glXEnableExtension(bits,name) ((void)(bits),(void)(name))
#define lorieLog(...) ((void)0)
'''
harness+='\n'.join((init,create,probe))
harness+=r'''
int main(int argc,char **argv) {
    Visual visual={0};ScreenRec output={.myNum=0,.numVisuals=1,.visuals=&visual};
    if (argc>1) failure=atoi(argv[1]);
    if (argc>2) core_case=atoi(argv[2]);
    __GLXscreen *screen=lorieGlxScreenProbe(&output);
    /* The provider manager tries swrast when probe returns NULL. */
    if (!screen) {puts("FALLBACK");return 91;}
    assert(failure<0 && core_case==0 && screen->numFBConfigs==6);
    free(screen->glvnd);free(screen->GLextensions);free(screen->GLXextensions);free(screen->visuals);
    __GLXconfig *c=screen->fbconfigs;
    while (c) {__GLXconfig *next=c->next;free(c);c=next;}
    free(screen);puts("READY");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='xserver-glx-failfast-') as temporary:
    test,binary=Path(temporary)/'test.c',Path(temporary)/'test'
    test.write_text(harness)
    subprocess.run([args.cc,'-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(test),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],capture_output=True,text=True)
    assert result.returncode==0 and result.stdout.strip()=='READY',result
    # Six config nodes, provider screen, then glvnd strdup.
    for failure in range(8):
        result=subprocess.run([str(binary),str(failure)],capture_output=True,text=True)
        assert result.returncode==47 and 'startup aborted' in result.stderr and 'FALLBACK' not in result.stdout,(failure,result)
    for core_case in range(1,15):
        result=subprocess.run([str(binary),'-1',str(core_case)],capture_output=True,text=True)
        assert result.returncode==47 and 'startup aborted' in result.stderr and 'FALLBACK' not in result.stdout,(core_case,result)
print('PASS: 8 injected provider allocation failures + 14 incomplete core states abort; no NULL/swrast fallback')
