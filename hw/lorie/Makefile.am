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

Xlorie_SOURCES = $(SRCS)

XLORIE_LIBS = \
	@XLORIE_LIBS@ \
	$(MAIN_LIB) \
	$(XSERVER_LIBS) \
	$(top_builddir)/Xi/libXistubs.la

Xlorie_LDADD = $(XLORIE_LIBS) $(XLORIE_SYS_LIBS) $(XSERVER_SYS_LIBS)
Xlorie_DEPENDENCIES = $(XLORIE_LIBS)
Xlorie_LDFLAGS = $(LD_EXPORT_SYMBOLS_FLAG)

EXTRA_DIST = $(bin_SCRIPTS)

relink:
	$(AM_V_at)rm -f Xlorie$(EXEEXT) && $(MAKE) Xlorie$(EXEEXT)
