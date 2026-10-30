#!/usr/bin/env python3
"""Exercise the real Xlorie FBConfig list and Cogl's depth/stencil contract."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument('source',type=Path)
parser.add_argument('--cc',default='cc')
args=parser.parse_args()
source=(args.source/'hw/lorie/lorie-glx.c').read_text()
header=(args.source/'glx/glxscreens.h').read_text()
config_type=re.search(r'typedef struct __GLXconfig __GLXconfig;\nstruct __GLXconfig \{.*?\n\};',header,re.S).group(0)
init=source[source.index('static void\nlorieGlxConfigInit('):source.index('/* Context shells:')]
create=source[source.index('static __GLXconfig *\nlorieGlxCreateConfigs('):source.index('static __GLXscreen *\nlorieGlxScreenProbe(')]
constants=source[source.index('#ifndef GLX_TRUE_COLOR'):source.index('/* ------------------------------------------------------------------ */')]
harness=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int GLint;
typedef unsigned int GLuint;
typedef unsigned char GLboolean;
#define GLX_TEXTURE_1D_BIT_EXT 1
#define GLX_TEXTURE_2D_BIT_EXT 2
#define GLX_TEXTURE_RECTANGLE_BIT_EXT 4
static int live_allocations, attempts, fail_after=-1;
static void *tracked_calloc(size_t count,size_t size) {
    if (fail_after>=0 && attempts++==fail_after) return NULL;
    void *result=calloc(count,size);
    if (result) live_allocations++;
    return result;
}
static void tracked_free(void *pointer) {
    if (pointer) live_allocations--;
    free(pointer);
}
#define calloc tracked_calloc
#define free tracked_free
'''
harness+='\n'.join((config_type,constants,init,create))
harness+=r'''
static void release(__GLXconfig *config) {
    while (config) {__GLXconfig *next=config->next;free(config);config=next;}
}
int main(void) {
    int counts[2][3]={{0}}, count=0, cogl_usable=0;
    __GLXconfig *configs=lorieGlxCreateConfigs();assert(configs);
    for (__GLXconfig *c=configs;c;c=c->next) {
        count++;
        assert(c->doubleBufferMode && c->drawableType&GLX_WINDOW_BIT && c->renderType&GLX_RGBA_BIT);
        assert(c->redBits==8 && c->greenBits==8 && c->blueBits==8);
        assert(c->redMask==0xff0000 && c->greenMask==0xff00 && c->blueMask==0xff);
        assert(c->alphaBits==0 || c->alphaBits==8);
        assert(c->rgbBits==(c->alphaBits?32:24));
        assert(c->bindToTextureRgb && c->bindToTextureRgba && c->yInverted);
        assert(c->bindToTextureTargets&GLX_TEXTURE_2D_BIT_EXT);
        int kind=c->depthBits==24?(c->stencilBits==8?0:1):2;
        assert((kind==0 && c->depthBits==24 && c->stencilBits==8) ||
               (kind==1 && c->depthBits==24 && c->stencilBits==0) ||
               (kind==2 && c->depthBits==0 && c->stencilBits==0));
        counts[c->alphaBits?1:0][kind]++;
        /* Mutter 48 Cogl GLX requires depth>=1 and stencil>=2. */
        if (c->depthBits>=1 && c->stencilBits>=2) cogl_usable++;
    }
    assert(count==6 && cogl_usable==2);
    for (int alpha=0;alpha<2;alpha++) for (int kind=0;kind<3;kind++) assert(counts[alpha][kind]==1);
    /* Preserve original configs and their order for existing clients. */
    assert(configs->alphaBits==8 && configs->depthBits==24 && configs->stencilBits==0);
    assert(configs->next->alphaBits==0 && configs->next->depthBits==24 && configs->next->stencilBits==0);
    release(configs);assert(live_allocations==0);
    /* Every intermediate allocation failure must free the whole prefix. */
    for (int failure=0;failure<6;failure++) {
        fail_after=failure;attempts=0;
        assert(!lorieGlxCreateConfigs());assert(live_allocations==0);
    }
    puts("PASS: RGB/RGBA depth24-stencil8 satisfy Cogl; legacy and no-depth variants preserved; OOM cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='xserver-glx-fbconfigs-') as temporary:
    test,binary=Path(temporary)/'test.c',Path(temporary)/'test'
    test.write_text(harness)
    subprocess.run([args.cc,'-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(test),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
