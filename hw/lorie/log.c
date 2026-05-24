#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdarg.h>
#include <stdio.h>

#include "os.h"
#include "log.h"

void
lorieLog(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    ErrorF("[Xlorie] ");
    VErrorF(format, args);
    va_end(args);
}
