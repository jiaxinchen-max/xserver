bin_PROGRAMS = Xlorie
bin_SCRIPTS = xlorie-startxfce4

AM_CFLAGS = -DHAVE_DIX_CONFIG_H \
	    -I$(top_srcdir)/hw/lorie \
	    -I$(top_srcdir)/dri3 \
	    $(DIX_CFLAGS)

SRCS =	InitInput.c \
	InputXKB.c \
	InitOutput.c \
	clipboard.c \
	render-adapter.c \
	log.c \
	log.h \
	lorie.h \
	$(top_srcdir)/hw/xquartz/keysym2ucs.c \
	$(top_srcdir)/hw/xquartz/keysym2ucs.h \
	$(top_srcdir)/mi/miinitext.c \
	$(top_srcdir)/mi/miinitext.h

# GPU path (glamor + EGL/GLES into the shared AHardwareBuffer). See lorie-egl.c
# and lorie-glamor.c. All GL/EGL dispatch goes through libepoxy (pulled in by
# libglamor's GLAMOR_LIBS), and lorie-egl.c provides glamor_egl_screen_init so we
# do not link libglamor_egl_stubs.
if GLAMOR
AM_CFLAGS += -DLORIE_HAVE_GLAMOR -I$(top_srcdir)/glamor
SRCS += lorie-egl.c lorie-egl.h lorie-glamor.c lorie-glamor.h
# libglamor.la is a real file (valid make prerequisite); GLAMOR_LIBS are linker
# flags (-L.../-lepoxy) that must only go into LDADD, never DEPENDENCIES.
GLAMOR_LINK = $(top_builddir)/glamor/libglamor.la
GLAMOR_EXTRA_LIBS = $(GLAMOR_LIBS)
else
GLAMOR_LINK =
GLAMOR_EXTRA_LIBS =
endif

Xlorie_SOURCES = $(SRCS)

XLORIE_LIBS = \
	@XLORIE_LIBS@ \
	$(MAIN_LIB) \
	$(XSERVER_LIBS) \
	$(top_builddir)/Xi/libXistubs.la \
	$(GLAMOR_LINK)

Xlorie_LDADD = $(XLORIE_LIBS) $(GLAMOR_EXTRA_LIBS) $(XLORIE_SYS_LIBS) $(XSERVER_SYS_LIBS)
Xlorie_DEPENDENCIES = $(XLORIE_LIBS)
Xlorie_LDFLAGS = $(LD_EXPORT_SYMBOLS_FLAG)

EXTRA_DIST = $(bin_SCRIPTS)

relink:
	$(AM_V_at)rm -f Xlorie$(EXEEXT) && $(MAKE) Xlorie$(EXEEXT)
