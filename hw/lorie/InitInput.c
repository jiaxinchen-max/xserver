#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <X11/X.h>
#include <X11/Xos.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>

#include "dix.h"
#include "exevents.h"
#include "inputstr.h"
#include "inpututils.h"
#include "mi.h"
#include "mipointer.h"
#include "os.h"
#include "scrnintstr.h"
#include "xkbsrv.h"
#include "xserver-properties.h"

#include "lorie.h"
#include "log.h"

static DeviceIntPtr lorieMouse;
static DeviceIntPtr lorieTouch;
DeviceIntPtr lorieKeyboard;
static int registeredInputFd = -1;

extern int ucs2keysym(long ucs);
void lorieKeysymKeyboardEvent(KeySym keysym, int down);

void
ProcessInputEvents(void)
{
    mieqProcessInputEvents();
}

void
DDXRingBell(int volume, int pitch, int duration)
{
}

static double
clampDouble(double value, double minValue, double maxValue)
{
    if (value < minValue)
        return minValue;
    if (value > maxValue)
        return maxValue;
    return value;
}

static ScreenPtr
lorieInputScreen(void)
{
    if (screenInfo.numScreens <= 0)
        return NULL;
    return screenInfo.screens[0];
}

static int
lorieKeybdProc(DeviceIntPtr pDevice, int onoff)
{
    DevicePtr pDev = (DevicePtr) pDevice;

    switch (onoff) {
    case DEVICE_INIT:
        InitKeyboardDeviceStruct(pDevice, NULL, NULL, NULL);
        break;
    case DEVICE_ON:
        pDev->on = TRUE;
        break;
    case DEVICE_OFF:
        pDev->on = FALSE;
        break;
    case DEVICE_CLOSE:
        break;
    default:
        return BadMatch;
    }
    return Success;
}

static Bool
lorieInitPointerButtons(DeviceIntPtr device)
{
#define NBUTTONS 10
    BYTE map[NBUTTONS + 1];
    Atom btn_labels[NBUTTONS] = { 0 };
    int i;

    for (i = 1; i <= NBUTTONS; i++)
        map[i] = i;

    btn_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
    btn_labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
    btn_labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
    btn_labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
    btn_labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
    btn_labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
    btn_labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);

    return InitButtonClassDeviceStruct(device, NBUTTONS, btn_labels, map);
#undef NBUTTONS
}

static int
lorieMouseProc(DeviceIntPtr device, int what)
{
#define NAXES 4
    Atom axes_labels[NAXES] = { 0 };

    switch (what) {
    case DEVICE_INIT:
        device->public.on = FALSE;

        axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_X);
        axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y);
        axes_labels[2] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_HWHEEL);
        axes_labels[3] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_WHEEL);

        if (!lorieInitPointerButtons(device) ||
            !InitValuatorClassDeviceStruct(device, NAXES, axes_labels,
                                           GetMotionHistorySize(), Relative) ||
            !InitValuatorAxisStruct(device, 0, axes_labels[0],
                                    NO_AXIS_LIMITS, NO_AXIS_LIMITS,
                                    0, 0, 0, Relative) ||
            !InitValuatorAxisStruct(device, 1, axes_labels[1],
                                    NO_AXIS_LIMITS, NO_AXIS_LIMITS,
                                    0, 0, 0, Relative) ||
            !InitValuatorAxisStruct(device, 2, axes_labels[2],
                                    NO_AXIS_LIMITS, NO_AXIS_LIMITS,
                                    0, 0, 0, Relative) ||
            !InitValuatorAxisStruct(device, 3, axes_labels[3],
                                    NO_AXIS_LIMITS, NO_AXIS_LIMITS,
                                    0, 0, 0, Relative) ||
            !SetScrollValuator(device, 2, SCROLL_TYPE_HORIZONTAL, 1.0,
                               SCROLL_FLAG_NONE) ||
            !SetScrollValuator(device, 3, SCROLL_TYPE_VERTICAL, 1.0,
                               SCROLL_FLAG_PREFERRED) ||
            !InitPtrFeedbackClassDeviceStruct(device,
                                              (PtrCtrlProcPtr) NoopDDA) ||
            !InitPointerAccelerationScheme(device, PtrAccelPredictable))
            return BadValue;
        return Success;

    case DEVICE_ON:
        device->public.on = TRUE;
        return Success;

    case DEVICE_OFF:
    case DEVICE_CLOSE:
        device->public.on = FALSE;
        return Success;
    default:
        return BadMatch;
    }
#undef NAXES
}

static int
lorieTouchProc(DeviceIntPtr device, int what)
{
#define NTOUCHPOINTS 20
#define NBUTTONS 1
#define NAXES 2
    BYTE map[NBUTTONS + 1] = { 0 };
    Atom btn_labels[NBUTTONS] = { 0 };
    Atom axes_labels[NAXES] = { 0 };

    switch (what) {
    case DEVICE_INIT:
        device->public.on = FALSE;

        map[1] = 1;
        btn_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
        axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_MT_POSITION_X);
        axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_ABS_MT_POSITION_Y);

        if (!InitValuatorClassDeviceStruct(device, NAXES, axes_labels,
                                           GetMotionHistorySize(), Absolute) ||
            !InitButtonClassDeviceStruct(device, NBUTTONS, btn_labels, map) ||
            !InitTouchClassDeviceStruct(device, NTOUCHPOINTS, XIDirectTouch,
                                        NAXES) ||
            !InitValuatorAxisStruct(device, 0, axes_labels[0],
                                    0, 0xffff, 10000, 0, 10000, Absolute) ||
            !InitValuatorAxisStruct(device, 1, axes_labels[1],
                                    0, 0xffff, 10000, 0, 10000, Absolute))
            return BadValue;
        return Success;

    case DEVICE_ON:
        device->public.on = TRUE;
        return Success;

    case DEVICE_OFF:
    case DEVICE_CLOSE:
        device->public.on = FALSE;
        return Success;
    default:
        return BadMatch;
    }
#undef NAXES
#undef NBUTTONS
#undef NTOUCHPOINTS
}

static void
drainBytes(int fd, size_t count)
{
    char buffer[256];

    while (count > 0) {
        size_t chunk = count > sizeof(buffer) ? sizeof(buffer) : count;
        ssize_t ret = read(fd, buffer, chunk);

        if (ret > 0) {
            count -= ret;
            continue;
        }
        if (ret < 0 && errno == EINTR)
            continue;
        break;
    }
}

static Bool
readBytes(int fd, void *buffer, size_t count)
{
    size_t offset = 0;

    while (offset < count) {
        ssize_t ret = read(fd, (char *) buffer + offset, count - offset);

        if (ret > 0) {
            offset += ret;
            continue;
        }
        if (ret < 0 && errno == EINTR)
            continue;
        return FALSE;
    }

    return TRUE;
}

static Bool
handleClipboardAnnounce(ClientPtr client, void *closure)
{
    (void) client;
    (void) closure;
    lorieHandleClipboardAnnounce();
    return TRUE;
}

static Bool
handleClipboardData(ClientPtr client, void *closure)
{
    (void) client;
    lorieHandleClipboardData(closure);
    return TRUE;
}

static void
handleTouchEvent(lorieEvent *event)
{
    ScreenPtr screen = lorieInputScreen();
    ValuatorMask mask;
    DDXTouchPointInfoPtr touch;
    double x, y;

    if (!screen || !lorieTouch)
        return;

    x = clampDouble(event->touch.x, 0, screen->width);
    y = clampDouble(event->touch.y, 0, screen->height);
    touch = TouchFindByDDXID(lorieTouch, event->touch.id, FALSE);

    if (event->touch.type == XI_TouchUpdate && (!touch || !touch->active))
        event->touch.type = XI_TouchBegin;
    if (event->touch.type == XI_TouchEnd && (!touch || !touch->active))
        return;

    valuator_mask_zero(&mask);
    valuator_mask_set_double(&mask, 0, x * 0xffff / (double) screen->width);
    valuator_mask_set_double(&mask, 1, y * 0xffff / (double) screen->height);
    QueueTouchEvents(lorieTouch, event->touch.type, event->touch.id, 0, &mask);
}

static void
handleMouseEvent(lorieEvent *event)
{
    ScreenPtr screen = lorieInputScreen();
    ValuatorMask mask;
    int flags;

    if (!screen || !lorieMouse)
        return;

    valuator_mask_zero(&mask);

    switch (event->mouse.detail) {
    case 0:
        flags = event->mouse.relative ?
            POINTER_RELATIVE | POINTER_ACCELERATE :
            POINTER_ABSOLUTE | POINTER_SCREEN | POINTER_NORAW;
        if (!event->mouse.relative) {
            event->mouse.x = clampDouble(event->mouse.x, 0, screen->width);
            event->mouse.y = clampDouble(event->mouse.y, 0, screen->height);
        }
        valuator_mask_set_double(&mask, 0, event->mouse.x);
        valuator_mask_set_double(&mask, 1, event->mouse.y);
        QueuePointerEvents(lorieMouse, MotionNotify, 0, flags, &mask);
        break;
    case 1:
    case 2:
    case 3:
        QueuePointerEvents(lorieMouse,
                           event->mouse.down ? ButtonPress : ButtonRelease,
                           event->mouse.detail, POINTER_RELATIVE, NULL);
        break;
    case 4:
        if (event->mouse.x) {
            valuator_mask_set_double(&mask, 2, event->mouse.x / 120.0);
            QueuePointerEvents(lorieMouse, MotionNotify, 0,
                               POINTER_RELATIVE, &mask);
        }
        if (event->mouse.y) {
            valuator_mask_zero(&mask);
            valuator_mask_set_double(&mask, 3, event->mouse.y / 120.0);
            QueuePointerEvents(lorieMouse, MotionNotify, 0,
                               POINTER_RELATIVE, &mask);
        }
        break;
    default:
        break;
    }
}

static void
lorieInputNotify(int fd, int ready, void *data)
{
    lorieEvent event;

    if (ready & X_NOTIFY_ERROR) {
        lorieInputUnregister();
        return;
    }

    while (read(fd, &event, sizeof(event)) == sizeof(event)) {
        switch (event.type) {
        case EVENT_TOUCH:
            handleTouchEvent(&event);
            break;
        case EVENT_MOUSE:
            handleMouseEvent(&event);
            break;
        case EVENT_KEY:
            if (lorieKeyboard)
                QueueKeyboardEvents(lorieKeyboard,
                                    event.key.state ? KeyPress : KeyRelease,
                                    event.key.key);
            break;
        case EVENT_UNICODE:
            if (lorieKeyboard) {
                int keysym = ucs2keysym((long) event.unicode.code);

                if (keysym != -1) {
                    lorieKeysymKeyboardEvent((KeySym) keysym, TRUE);
                    lorieKeysymKeyboardEvent((KeySym) keysym, FALSE);
                }
            }
            break;
        case EVENT_SCREEN_SIZE:
            lorieConfigureNotify(event.screenSize.width,
                                  event.screenSize.height,
                                  event.screenSize.framerate,
                                  0, NULL);
            drainBytes(fd, event.screenSize.name_size);
            break;
        case EVENT_CLIPBOARD_ENABLE:
            lorieEnableClipboardSync(event.clipboardEnable.enable);
            break;
        case EVENT_CLIPBOARD_ANNOUNCE:
            QueueWorkProc(handleClipboardAnnounce, NULL, NULL);
            break;
        case EVENT_CLIPBOARD_SEND: {
            char *data;

            if (event.clipboardSend.count > 16 * 1024 * 1024) {
                drainBytes(fd, event.clipboardSend.count);
                break;
            }
            data = calloc(1, event.clipboardSend.count + 1);
            if (!data) {
                drainBytes(fd, event.clipboardSend.count);
                break;
            }
            if (!readBytes(fd, data, event.clipboardSend.count)) {
                free(data);
                lorieInputUnregister();
                return;
            }
            data[event.clipboardSend.count] = '\0';
            QueueWorkProc(handleClipboardData, NULL, data);
            break;
        }
        case EVENT_STOP_RENDER:
            lorieInputUnregister();
            return;
        default:
            break;
        }

        int pending = 0;
        if (ioctl(fd, FIONREAD, &pending) < 0 ||
            pending < (int) sizeof(event))
            break;
    }
}

int
lorieInputRegisterFd(int fd)
{
    if (fd < 0)
        return -1;

    lorieInputUnregister();
    if (!SetNotifyFd(fd, lorieInputNotify, X_NOTIFY_READ, NULL)) {
        lorieLog("failed to register input fd %d\n", fd);
        return -1;
    }

    registeredInputFd = fd;
    return 0;
}

void
lorieInputUnregister(void)
{
    lorieEnableClipboardSync(FALSE);

    if (registeredInputFd >= 0) {
        RemoveNotifyFd(registeredInputFd);
        registeredInputFd = -1;
    }
}

void
InitInput(int argc, char *argv[])
{
    Atom xiclass;
    int fd;

    lorieMouse = AddInputDevice(serverClient, lorieMouseProc, TRUE);
    lorieTouch = AddInputDevice(serverClient, lorieTouchProc, TRUE);
    lorieKeyboard = AddInputDevice(serverClient, lorieKeybdProc, TRUE);

    xiclass = MakeAtom(XI_MOUSE, sizeof(XI_MOUSE) - 1, TRUE);
    AssignTypeAndName(lorieMouse, xiclass, "Lorie mouse");
    xiclass = MakeAtom(XI_TOUCHSCREEN, sizeof(XI_TOUCHSCREEN) - 1, TRUE);
    AssignTypeAndName(lorieTouch, xiclass, "Lorie touch");
    xiclass = MakeAtom(XI_KEYBOARD, sizeof(XI_KEYBOARD) - 1, TRUE);
    AssignTypeAndName(lorieKeyboard, xiclass, "Lorie keyboard");

    ActivateDevice(lorieMouse, FALSE);
    ActivateDevice(lorieTouch, FALSE);
    ActivateDevice(lorieKeyboard, FALSE);
    EnableDevice(lorieMouse, TRUE);
    EnableDevice(lorieTouch, TRUE);
    EnableDevice(lorieKeyboard, TRUE);
    AttachDevice(NULL, lorieMouse, inputInfo.pointer);
    AttachDevice(NULL, lorieTouch, inputInfo.pointer);
    AttachDevice(NULL, lorieKeyboard, inputInfo.keyboard);

    (void) mieqInit();

    fd = lorieRenderInputFd();
    if (fd >= 0)
        lorieInputRegisterFd(fd);
}

void
CloseInput(void)
{
    lorieInputUnregister();
    mieqFini();
}
