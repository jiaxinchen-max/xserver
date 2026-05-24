#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xproto.h>

#include "dix.h"
#include "dixstruct.h"
#include "property.h"
#include "propertyst.h"
#include "selection.h"
#include "windowstr.h"
#include "xace.h"

#include "lorie.h"
#include "log.h"

static int (*origProcSendEvent)(ClientPtr) = NULL;
static int (*origProcConvertSelection)(ClientPtr) = NULL;
static Atom xaTIMESTAMP, xaTEXT, xaCLIPBOARD, xaTARGETS, xaSTRING, xaUTF8_STRING;
static Bool clipboardEnabled = FALSE;
static char *cachedData;

typedef struct LorieDataTarget {
    ClientPtr client;
    Atom selection;
    Atom target;
    Atom property;
    Window requestor;
    CARD32 time;
    struct LorieDataTarget *next;
} LorieDataTarget;

static LorieDataTarget *lorieDataTargetHead;

void
lorieEnableClipboardSync(Bool enable)
{
    clipboardEnabled = enable;
}

static void
lorieConvertLF(const char *src, char *dst, size_t bytes)
{
    size_t i, j = 0;

    for (i = 0; i < bytes; i++)
        if (src[i] != '\r')
            dst[j++] = src[i];
}

static void
lorieLatin1ToUTF8(unsigned char *out, const unsigned char *in)
{
    while (*in) {
        if (*in < 128) {
            *out++ = *in++;
        } else {
            *out++ = 0xc2 + (*in > 0xbf);
            *out++ = (*in++ & 0x3f) + 0x80;
        }
    }
}

static int
lorieCheckUTF8(const unsigned char *utf, size_t size)
{
    size_t ix = 0;

    while (ix < size && utf[ix]) {
        unsigned char c = utf[ix];

        if (!(c & 0x80)) {
            ix++;
            continue;
        }

        if (ix + 1 >= size || (utf[ix + 1] & 0xc0) != 0x80)
            return 0;

        if ((c & 0xe0) == 0xe0) {
            if (ix + 2 >= size || (utf[ix + 2] & 0xc0) != 0x80)
                return 0;
            if ((c & 0xf0) == 0xf0) {
                if ((c & 0xf8) != 0xf0 || ix + 3 >= size ||
                    (utf[ix + 3] & 0xc0) != 0x80)
                    return 0;
                ix += 4;
            } else {
                ix += 3;
            }
        } else {
            ix += 2;
        }
    }

    return 1;
}

static size_t
lorieUtf8ToUCS4(const char *src, size_t max, unsigned *dst)
{
    size_t count, consumed = 1;

    *dst = 0xfffd;
    if (max == 0)
        return 0;

    if ((*src & 0x80) == 0) {
        *dst = *src;
        count = 0;
    } else if ((*src & 0xe0) == 0xc0) {
        *dst = *src & 0x1f;
        count = 1;
    } else if ((*src & 0xf0) == 0xe0) {
        *dst = *src & 0x0f;
        count = 2;
    } else if ((*src & 0xf8) == 0xf0) {
        *dst = *src & 0x07;
        count = 3;
    } else {
        src++;
        max--;
        while (max-- > 0 && ((*src++ & 0xc0) == 0x80))
            consumed++;
        return consumed;
    }

    src++;
    max--;
    while (count--) {
        consumed++;
        if (max == 0 || (*src & 0xc0) != 0x80) {
            *dst = 0xfffd;
            return consumed;
        }
        *dst = (*dst << 6) | (*src & 0x3f);
        src++;
        max--;
    }

    if (*dst >= 0xd800 && *dst < 0xe000)
        *dst = 0xfffd;

    return consumed;
}

static char *
lorieUtf8ToLatin1(const char *src)
{
    const char *in = src;
    size_t inLen = strlen(src);
    size_t outLen = 0;
    char *out;

    while (*in) {
        unsigned ucs;
        size_t len = lorieUtf8ToUCS4(in, inLen, &ucs);

        in += len;
        inLen -= len;
        outLen++;
    }

    out = calloc(outLen + 1, 1);
    if (!out)
        return NULL;

    in = src;
    inLen = strlen(src);
    outLen = 0;
    while (*in) {
        unsigned ucs;
        size_t len = lorieUtf8ToUCS4(in, inLen, &ucs);

        in += len;
        inLen -= len;
        out[outLen++] = ucs > 0xff ? '?' : (char) ucs;
    }

    return out;
}

static void
lorieSelectionRequest(Atom selection, Atom target)
{
    Selection *pSel;

    if (!clipboardEnabled ||
        dixLookupSelection(&pSel, selection, serverClient,
                           DixGetAttrAccess) != Success)
        return;

    xEvent event = {0};
    event.u.u.type = SelectionRequest;
    event.u.selectionRequest.owner = pSel->window;
    event.u.selectionRequest.time = currentTime.milliseconds;
    event.u.selectionRequest.requestor = screenInfo.screens[0]->root->drawable.id;
    event.u.selectionRequest.selection = selection;
    event.u.selectionRequest.target = target;
    event.u.selectionRequest.property = target;
    WriteEventsToClient(pSel->client, 1, &event);
}

static Bool
lorieHasAtom(Atom atom, const Atom list[], size_t size)
{
    size_t i;

    for (i = 0; i < size; i++)
        if (list[i] == atom)
            return TRUE;
    return FALSE;
}

static void
lorieHandleSelection(Atom target)
{
    PropertyPtr prop;
    WindowPtr root = screenInfo.screens[0]->root;

    if (target != xaTARGETS && target != xaSTRING && target != xaUTF8_STRING)
        return;

    if (dixLookupProperty(&prop, root, target, serverClient,
                          DixReadAccess) != Success)
        return;

    if (target == xaTARGETS && prop->type == XA_ATOM && prop->format == 32) {
        if (lorieHasAtom(xaUTF8_STRING, (const Atom *) prop->data, prop->size))
            lorieSelectionRequest(xaCLIPBOARD, xaUTF8_STRING);
        else if (lorieHasAtom(xaSTRING, (const Atom *) prop->data, prop->size))
            lorieSelectionRequest(xaCLIPBOARD, xaSTRING);
    } else if (target == xaSTRING && prop->type == xaSTRING &&
               prop->format == 8) {
        char *filtered, *utf8;

        if (prop->size > UINT32_MAX / 2 - 1)
            return;

        filtered = calloc(prop->size + 1, 1);
        utf8 = calloc((prop->size + 1) * 2, 1);
        if (!filtered || !utf8) {
            free(filtered);
            free(utf8);
            return;
        }

        lorieConvertLF(prop->data, filtered, prop->size);
        lorieLatin1ToUTF8((unsigned char *) utf8,
                          (unsigned char *) filtered);
        lorieSendClipboardData(utf8);
        free(filtered);
        free(utf8);
    } else if (target == xaUTF8_STRING && prop->type == xaUTF8_STRING &&
               prop->format == 8) {
        char *filtered;

        if (!lorieCheckUTF8(prop->data, prop->size)) {
            lorieLog("invalid UTF-8 sequence in clipboard\n");
            return;
        }

        filtered = calloc(prop->size + 1, 1);
        if (!filtered)
            return;

        lorieConvertLF(prop->data, filtered, prop->size);
        lorieSendClipboardData(filtered);
        free(filtered);
    }
}

static int
lorieProcSendEvent(ClientPtr client)
{
    REQUEST(xSendEventReq);
    REQUEST_SIZE_MATCH(xSendEventReq);

    if (clipboardEnabled &&
        stuff->event.u.u.type == SelectionNotify &&
        stuff->event.u.selectionNotify.requestor ==
            screenInfo.screens[0]->root->drawable.id &&
        stuff->event.u.selectionNotify.selection == xaCLIPBOARD &&
        stuff->event.u.selectionNotify.target ==
            stuff->event.u.selectionNotify.property)
        lorieHandleSelection(stuff->event.u.selectionNotify.target);

    return origProcSendEvent(client);
}

static void
lorieSelectionCallback(CallbackListPtr *callbacks, void *data, void *args)
{
    SelectionInfoRec *info = args;

    (void) callbacks;
    (void) data;

    if (clipboardEnabled && info->selection->selection == xaCLIPBOARD &&
        info->kind == SelectionSetOwner)
        lorieSelectionRequest(xaCLIPBOARD, xaTARGETS);
}

static int
lorieConvertSelection(ClientPtr client, Atom selection, Atom target,
                      Atom property, Window requestor, CARD32 time,
                      const char *data)
{
    Selection *pSel;
    WindowPtr pWin;
    Atom realProperty = property == None ? target : property;
    xEvent event = {0};
    int rc;

    rc = dixLookupSelection(&pSel, selection, client, DixGetAttrAccess);
    if (rc != Success)
        return rc;

    rc = dixLookupWindow(&pWin, requestor, client, DixSetAttrAccess);
    if (rc != Success)
        return rc;

    if (target == xaTARGETS) {
        Atom targets[] = { xaTARGETS, xaTIMESTAMP, xaSTRING, xaTEXT,
                           xaUTF8_STRING };

        rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                     XA_ATOM, 32, PropModeReplace,
                                     sizeof(targets) / sizeof(targets[0]),
                                     targets, TRUE);
        if (rc != Success)
            return rc;
    } else if (target == xaTIMESTAMP) {
        rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                     XA_INTEGER, 32, PropModeReplace, 1,
                                     &pSel->lastTimeChanged.milliseconds,
                                     TRUE);
        if (rc != Success)
            return rc;
    } else if (data == NULL) {
        LorieDataTarget *ldt;

        if (target != xaSTRING && target != xaTEXT && target != xaUTF8_STRING)
            return BadMatch;

        ldt = calloc(1, sizeof(*ldt));
        if (!ldt)
            return BadAlloc;

        ldt->client = client;
        ldt->selection = selection;
        ldt->target = target;
        ldt->property = property;
        ldt->requestor = requestor;
        ldt->time = time;
        ldt->next = lorieDataTargetHead;
        lorieDataTargetHead = ldt;

        lorieRequestClipboard();
        return Success;
    } else if (target == xaSTRING || target == xaTEXT) {
        char *latin1 = lorieUtf8ToLatin1(data);

        if (!latin1)
            return BadAlloc;

        rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                     XA_STRING, 8, PropModeReplace,
                                     strlen(latin1), latin1, TRUE);
        free(latin1);
        if (rc != Success)
            return rc;
    } else if (target == xaUTF8_STRING) {
        rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                     xaUTF8_STRING, 8, PropModeReplace,
                                     strlen(data), data, TRUE);
        if (rc != Success)
            return rc;
    } else {
        return BadMatch;
    }

    event.u.u.type = SelectionNotify;
    event.u.selectionNotify.time = time;
    event.u.selectionNotify.requestor = requestor;
    event.u.selectionNotify.selection = selection;
    event.u.selectionNotify.target = target;
    event.u.selectionNotify.property = property;
    WriteEventsToClient(client, 1, &event);
    return Success;
}

static int
lorieProcConvertSelection(ClientPtr client)
{
    Bool paramsOkay;
    WindowPtr pWin;
    Selection *pSel;
    int rc;

    REQUEST(xConvertSelectionReq);
    REQUEST_SIZE_MATCH(xConvertSelectionReq);

    rc = dixLookupWindow(&pWin, stuff->requestor, client, DixSetAttrAccess);
    if (rc != Success)
        return rc;

    paramsOkay = ValidAtom(stuff->selection) && ValidAtom(stuff->target);
    paramsOkay &= (stuff->property == None) || ValidAtom(stuff->property);
    if (!paramsOkay) {
        client->errorValue = stuff->property;
        return BadAtom;
    }

    rc = dixLookupSelection(&pSel, stuff->selection, client, DixReadAccess);
    if (rc == Success && pSel->client == serverClient &&
        pSel->window == screenInfo.screens[0]->root->drawable.id) {
        rc = lorieConvertSelection(client, stuff->selection, stuff->target,
                                   stuff->property, stuff->requestor,
                                   stuff->time, cachedData);
        if (rc != Success) {
            xEvent event = {0};

            event.u.u.type = SelectionNotify;
            event.u.selectionNotify.time = stuff->time;
            event.u.selectionNotify.requestor = stuff->requestor;
            event.u.selectionNotify.selection = stuff->selection;
            event.u.selectionNotify.target = stuff->target;
            event.u.selectionNotify.property = None;
            WriteEventsToClient(client, 1, &event);
        }
        return Success;
    }

    return origProcConvertSelection(client);
}

static int
lorieOwnSelection(Atom selection)
{
    Selection *pSel;
    SelectionInfoRec info;
    WindowPtr root = screenInfo.screens[0]->root;
    int rc;

    rc = dixLookupSelection(&pSel, selection, serverClient, DixSetAttrAccess);
    if (rc == Success) {
        if (pSel->client && pSel->client != serverClient) {
            xEvent event = {0};

            event.u.u.type = SelectionClear;
            event.u.selectionClear.time = currentTime.milliseconds;
            event.u.selectionClear.window = pSel->window;
            event.u.selectionClear.atom = pSel->selection;
            WriteEventsToClient(pSel->client, 1, &event);
        }
    } else if (rc == BadMatch) {
        pSel = dixAllocateObjectWithPrivates(Selection, PRIVATE_SELECTION);
        if (!pSel)
            return BadAlloc;

        pSel->selection = selection;
        rc = XaceHookSelectionAccess(serverClient, &pSel,
                                     DixCreateAccess | DixSetAttrAccess);
        if (rc != Success) {
            free(pSel);
            return rc;
        }

        pSel->next = CurrentSelections;
        CurrentSelections = pSel;
    } else {
        return rc;
    }

    pSel->lastTimeChanged = currentTime;
    pSel->window = root->drawable.id;
    pSel->pWin = root;
    pSel->client = serverClient;

    info.selection = pSel;
    info.client = serverClient;
    info.kind = SelectionSetOwner;
    CallCallbacks(&SelectionCallback, &info);

    return Success;
}

void
lorieHandleClipboardAnnounce(void)
{
    free(cachedData);
    cachedData = NULL;

    if (lorieOwnSelection(xaCLIPBOARD) != Success)
        lorieLog("could not set CLIPBOARD selection\n");
}

void
lorieHandleClipboardData(const char *data)
{
    LorieDataTarget *next;

    free(cachedData);
    cachedData = (char *) data;

    while (lorieDataTargetHead) {
        xEvent event = {0};
        int rc;

        rc = lorieConvertSelection(lorieDataTargetHead->client,
                                   lorieDataTargetHead->selection,
                                   lorieDataTargetHead->target,
                                   lorieDataTargetHead->property,
                                   lorieDataTargetHead->requestor,
                                   lorieDataTargetHead->time, cachedData);
        if (rc != Success) {
            event.u.u.type = SelectionNotify;
            event.u.selectionNotify.time = lorieDataTargetHead->time;
            event.u.selectionNotify.requestor = lorieDataTargetHead->requestor;
            event.u.selectionNotify.selection = lorieDataTargetHead->selection;
            event.u.selectionNotify.target = lorieDataTargetHead->target;
            event.u.selectionNotify.property = None;
            WriteEventsToClient(lorieDataTargetHead->client, 1, &event);
        }

        next = lorieDataTargetHead->next;
        free(lorieDataTargetHead);
        lorieDataTargetHead = next;
    }
}

void
lorieInitClipboard(void)
{
#define ATOM(name) xa##name = MakeAtom(#name, strlen(#name), TRUE)
    ATOM(TIMESTAMP);
    ATOM(TEXT);
    ATOM(CLIPBOARD);
    ATOM(TARGETS);
    ATOM(STRING);
    ATOM(UTF8_STRING);
#undef ATOM

    if (!origProcConvertSelection) {
        origProcConvertSelection = ProcVector[X_ConvertSelection];
        ProcVector[X_ConvertSelection] = lorieProcConvertSelection;
    }

    if (!origProcSendEvent) {
        origProcSendEvent = ProcVector[X_SendEvent];
        ProcVector[X_SendEvent] = lorieProcSendEvent;
    }

    if (!AddCallback(&SelectionCallback, lorieSelectionCallback, NULL))
        FatalError("Adding SelectionCallback failed\n");
}
