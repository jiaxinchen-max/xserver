#!/usr/bin/env python3
"""Test real provider/core GLX drawable identity, queries and paired cleanup."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument('source',type=Path)
parser.add_argument('--cc',default='cc')
parser.add_argument('--provider',type=Path,help='Use an earlier provider for a negative regression run')
args=parser.parse_args()
provider=(args.provider or args.source/'hw/lorie/lorie-glx.c').read_text()
core=(args.source/'glx/glxcmds.c').read_text()
ext=(args.source/'glx/glxext.c').read_text()
header=(args.source/'glx/glxdrawable.h').read_text()
struct=re.search(r'struct __GLXdrawable \{.*?\n\};',header,re.S).group(0)
validate=core[core.index('int\nvalidGlxDrawable('):core.index('void\n__glXContextDestroy(')]
create=core[core.index('GLboolean\n__glXDrawableInit('):core.index('static int\nDoCreateGLXPixmap(')]
query=core[core.index('static int\nDoGetDrawableAttributes('):core.index('int\n__glXDisp_GetDrawableAttributes(')]
change=core[core.index('static int\nDoChangeDrawableAttributes('):core.index('int\n__glXDisp_ChangeDrawableAttributes(')]
destroy=ext[ext.index('static Bool\nDrawableGone('):ext.index('Bool\n__glXAddContext(')]
provider_drawable=provider[provider.index('/* Drawable shells. */'):provider.index('static int\nlorieGlxSwapInterval(')]
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char GLboolean;
typedef unsigned int GLenum;
typedef unsigned int XID;
typedef uint32_t CARD32;
typedef int Bool;
#define TRUE 1
#define FALSE 0
#define GL_TRUE 1
#define GL_FALSE 0
#define Success 0
#define BadValue 2
#define BadWindow 3
#define BadMatch 8
#define BadAlloc 11
#define GLXBadWindow 1
#define GLXBadPixmap 2
#define GLXBadPbuffer 3
#define GLXBadDrawable 4
#define __glXError(value) (100+(value))
#define DixGetAttrAccess 1
#define DixSetAttrAccess 2
#define GLX_DRAWABLE_WINDOW 0
#define GLX_DRAWABLE_PIXMAP 1
#define GLX_DRAWABLE_PBUFFER 2
#define GLX_DRAWABLE_ANY 3
#define GLX_Y_INVERTED_EXT 1
#define GLX_WIDTH 2
#define GLX_HEIGHT 3
#define GLX_SCREEN 4
#define GLX_TEXTURE_TARGET_EXT 5
#define GL_TEXTURE_2D 3553
#define GLX_TEXTURE_2D_EXT 6
#define GLX_TEXTURE_RECTANGLE_EXT 7
#define GLX_EVENT_MASK 8
#define GLX_FBCONFIG_ID 9
#define GLX_PRESERVED_CONTENTS 10
#define GLX_STEREO_TREE_EXT 11
#define GLX_DRAWABLE_TYPE 12
#define GLX_WINDOW_BIT 1
#define GLX_PIXMAP_BIT 2
#define GLX_PBUFFER_BIT 4
#define X_Reply 1
#define __GLX_DECLARE_SWAP_VARIABLES
#define __GLX_DECLARE_SWAP_ARRAY_VARIABLES
#define __GLX_SWAP_SHORT(pointer) ((void)(pointer))
#define __GLX_SWAP_INT(pointer) ((void)(pointer))
#define __GLX_SWAP_INT_ARRAY(pointer,length) ((void)(pointer),(void)(length))
typedef struct __GLXdrawable __GLXdrawable;
typedef struct { XID errorValue; int swapped,sequence; } ClientRec,*ClientPtr;
typedef struct { ClientPtr client; } __GLXclientState;
typedef struct { int fbconfigID; } __GLXconfig;
typedef struct Screen {
    int myNum;
    void (*DestroyPixmap)(void *);
} ScreenRec,*ScreenPtr;
typedef struct { XID id; int width,height; ScreenPtr pScreen; } DrawableRec,*DrawablePtr;
typedef DrawablePtr WindowPtr;
typedef void *PixmapPtr;
typedef struct __GLXscreen {
    ScreenPtr pScreen;
    __GLXdrawable *(*createDrawable)(ClientPtr,struct __GLXscreen *,DrawablePtr,XID,int,XID,__GLXconfig *);
} __GLXscreen;
'''
harness+=struct
harness+=r'''
typedef struct Context {
    struct Context *next;
    ClientPtr currentClient;
    __GLXdrawable *drawPriv,*readPriv;
    void (*loseCurrent)(struct Context *);
} __GLXcontext;
static __GLXcontext *glxAllContexts,*lastGLContext;
static void glFlush(void) {}
static int __glXDrawableRes=42;
static struct { XID id; __GLXdrawable *pointer; } resources[16];
static int resource_count,destroy_count,write_calls;
static DrawablePtr naked_window;
typedef struct { int type; unsigned short sequenceNumber; CARD32 length,numAttribs; } xGLXGetDrawableAttributesReply;
#define sz_xGLXGetDrawableAttributesReply sizeof(xGLXGetDrawableAttributesReply)
static xGLXGetDrawableAttributesReply reply;
static CARD32 attributes[20];
static void WriteToClient(ClientPtr client,size_t size,const void *data) {
    (void)client;
    if (write_calls++==0) {assert(size==sizeof(reply));memcpy(&reply,data,size);}
    else {assert(size<=sizeof(attributes));memcpy(attributes,data,size);}
}
static int dixLookupResourceByType(void **pointer,XID id,int type,ClientPtr client,int access) {
    (void)type;(void)client;(void)access;
    for (int i=0;i<resource_count;i++) if (resources[i].id==id) {*pointer=resources[i].pointer;return Success;}
    *pointer=NULL;return BadValue;
}
static int dixLookupWindow(WindowPtr *pointer,XID id,ClientPtr client,int access) {
    (void)client;(void)access;
    if (naked_window && naked_window->id==id) {*pointer=naked_window;return Success;}
    return BadWindow;
}
static Bool AddResource(XID id,int type,void *pointer) {
    (void)type;assert(resource_count<16);
    resources[resource_count].id=id;resources[resource_count++].pointer=pointer;return TRUE;
}
static void FreeResourceByType(XID id,int type,Bool skip) {
    (void)type;assert(skip);
    for (int i=0;i<resource_count;i++) if (resources[i].id==id) {
        resources[i]=resources[--resource_count];return;
    }
    assert(!"missing paired alias");
}
'''
harness+=validate+create+query+change+destroy+provider_drawable
harness+=r'''
static CARD32 attribute(CARD32 key) {
    for (unsigned i=0;i<reply.numAttribs;i++) if (attributes[2*i]==key) return attributes[2*i+1];
    assert(!"attribute missing");return 0;
}
static void counted_destroy(__GLXdrawable *drawable) {
    destroy_count++;lorieGlxDrawableDestroy(drawable);
}
static void release_resource(XID id) {
    for (int i=0;i<resource_count;i++) if (resources[i].id==id) {
        __GLXdrawable *pointer=resources[i].pointer;
        resources[i]=resources[--resource_count];
        DrawableGone(pointer,id);return;
    }
    assert(!"resource missing");
}
int main(void) {
    ClientRec client={.sequence=246};__GLXclientState cl={.client=&client};
    ScreenRec screen={.myNum=0};__GLXscreen glx={.pScreen=&screen,.createDrawable=lorieGlxCreateDrawable};
    __GLXconfig config={.fbconfigID=77};
    for (int order=0;order<2;order++) {
        XID xwindow=0x200005,glxwindow=0x400007;
        DrawableRec window={.id=xwindow,.width=1280,.height=720,.pScreen=&screen};
        naked_window=&window;
        assert(DoCreateGLXDrawable(&client,&glx,&config,&window,xwindow,glxwindow,GLX_DRAWABLE_WINDOW)==Success);
        assert(resource_count==2);
        __GLXdrawable *drawable=NULL;int error=0;
        assert(validGlxDrawable(&client,glxwindow,GLX_DRAWABLE_ANY,DixGetAttrAccess,&drawable,&error));
        assert(drawable->drawId==glxwindow && drawable->pDraw==&window);
        assert(drawable->destroy && drawable->swapBuffers && drawable->waitX && drawable->waitGL);
        drawable->destroy=counted_destroy;
        CARD32 mask[]={GLX_EVENT_MASK,0x04000000};
        assert(DoChangeDrawableAttributes(&client,glxwindow,1,mask)==Success);
        write_calls=0;
        assert(DoGetDrawableAttributes(&cl,glxwindow)==Success && write_calls==2);
        assert(reply.sequenceNumber==246);
        assert(attribute(GLX_WIDTH)==1280 && attribute(GLX_HEIGHT)==720);
        assert(attribute(GLX_SCREEN)==0 && attribute(GLX_FBCONFIG_ID)==77);
        assert(attribute(GLX_EVENT_MASK)==0x04000000 && attribute(GLX_DRAWABLE_TYPE)==GLX_WINDOW_BIT);
        /* Invalid aliases/types must remain rejected, not made valid by a workaround. */
        assert(!validGlxDrawable(&client,xwindow,GLX_DRAWABLE_WINDOW,DixGetAttrAccess,&drawable,&error));
        assert(error==__glXError(GLXBadWindow));
        assert(!validGlxDrawable(&client,glxwindow,GLX_DRAWABLE_PIXMAP,DixGetAttrAccess,&drawable,&error));
        assert(error==__glXError(GLXBadPixmap));
        assert(DoGetDrawableAttributes(&cl,0xabcdef)==__glXError(GLXBadDrawable));
        __GLXcontext context={.drawPriv=drawable,.readPriv=drawable};glxAllContexts=&context;
        release_resource(order?xwindow:glxwindow);
        assert(resource_count==0 && destroy_count==order+1);
        assert(!context.drawPriv && !context.readPriv);glxAllContexts=NULL;
    }
    /* Legacy GLX1.2 uses one XID for both names and still needs one destruction. */
    DrawableRec legacy={.id=0x101,.width=20,.height=30,.pScreen=&screen};
    naked_window=&legacy;
    assert(DoCreateGLXDrawable(&client,&glx,&config,&legacy,legacy.id,legacy.id,GLX_DRAWABLE_WINDOW)==Success);
    assert(resource_count==1);write_calls=0;
    assert(DoGetDrawableAttributes(&cl,legacy.id)==Success);
    assert(attribute(GLX_WIDTH)==20 && attribute(GLX_HEIGHT)==30);
    release_resource(legacy.id);assert(resource_count==0);
    puts("PASS: GLXWindow distinct IDs validate/query/event-mask; both destruction orders remove aliases; legacy identity preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='xserver-glx-drawable-') as temporary:
    test,binary=Path(temporary)/'test.c',Path(temporary)/'test'
    test.write_text(harness)
    subprocess.run([args.cc,'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined',str(test),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
