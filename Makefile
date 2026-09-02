# Makefile for 12to11.
#
# make EGL=0     build without the EGL renderer
# make ANALYZE=0 build without -fanalyzer (on by default with GCC 10+)

BUILDDIR	:= build
PROTO_DIR	:= protocols
DATA_DIR	:= data
SRC_DIR		:= src

CC		?= cc

PREFIX		?= /usr/local
BINDIR		?= $(PREFIX)/bin
MANDIR		?= $(PREFIX)/share/man

PROG_PKGS	:= xcb xcb-shm xcb-dri3 xcb-shape xcb-randr \
		   x11 x11-xcb xext xrandr xrender xfixes xi \
		   xkbfile xcursor xpresent xshmfence \
		   pixman-1 libdrm wayland-server

CPPFLAGS	+= $(shell pkg-config --cflags $(PROG_PKGS)) \
		   -D_GNU_SOURCE -U_BSD_SOURCE -U_SVID_SOURCE \
		   -DPortFile=\"port_gnu.h\" \
		   -I$(BUILDDIR)
CFLAGS		?= $(WARN_CFLAGS) -g3 -O2
LDFLAGS		+= -pthread
LDLIBS		+= $(shell pkg-config --libs $(PROG_PKGS)) -lm

WARN_CFLAGS	:= -fno-common -Wall -Warith-conversion -Wdate-time \
-Wdisabled-optimization -Wdouble-promotion -Wduplicated-cond -Wextra \
-Wformat-signedness -Winit-self -Winvalid-pch -Wlogical-op \
-Wmissing-declarations -Wmissing-include-dirs -Wmissing-prototypes \
-Wnested-externs -Wnull-dereference -Wold-style-definition \
-Wopenmp-simd -Wpacked -Wpointer-arith -Wstrict-prototypes \
-Wsuggest-attribute=format -Wsuggest-attribute=noreturn \
-Wsuggest-final-methods -Wsuggest-final-types -Wuninitialized \
-Wunknown-pragmas -Wunused-macros -Wvariadic-macros \
-Wvector-operation-performance -Wwrite-strings -Warray-bounds=2 \
-Wattribute-alias=2 -Wformat=2 -Wformat-truncation=2 \
-Wimplicit-fallthrough=5 -Wshift-overflow=2 -Wuse-after-free=3 \
-Wvla-larger-than=4031 -Wredundant-decls -Wno-missing-field-initializers \
-Wno-override-init -Wno-sign-compare -Wno-type-limits \
-Wno-unused-parameter -Wno-format-nonliteral

GCC_MAJOR	:= $(shell $(CC) -dumpfullversion -dumpversion | \
			   cut -d. -f1)
ifeq ($(shell test $(GCC_MAJOR) -ge 10 && echo yes),yes)
ANALYZE		?= 1
else
ANALYZE		?= 0
endif

ifeq ($(ANALYZE),1)
CFLAGS		+= -fanalyzer
endif

PROTOCOLS	:= linux-dmabuf-unstable-v1 xdg-shell \
		   primary-selection-unstable-v1 \
		   linux-explicit-synchronization-unstable-v1 \
		   viewporter xdg-decoration-unstable-v1 \
		   text-input-unstable-v3 single-pixel-buffer-v1 \
		   drm-lease-v1 pointer-constraints-unstable-v1 \
		   relative-pointer-unstable-v1 \
		   keyboard-shortcuts-inhibit-unstable-v1 \
		   idle-inhibit-unstable-v1 pointer-gestures-unstable-v1 \
		   12to11-test xdg-activation-v1 tearing-control-v1 \
		   cursor-shape-v1 wlr-layer-shell-unstable-v1 \
		   ext-image-capture-source-v1 \
		   ext-image-copy-capture-v1

PROTO_HDRS	:= $(addprefix $(BUILDDIR)/,$(addsuffix .h,$(PROTOCOLS)))
PROTO_SRCS	:= $(addprefix $(BUILDDIR)/,$(addsuffix .c,$(PROTOCOLS)))
PROTO_OBJS	:= $(PROTO_SRCS:.c=.o)

# test_seat.c is not compiled directly; seat.c includes it.

SRCS		:= $(filter-out test_seat.c egl.c,$(notdir $(wildcard $(SRC_DIR)/*.c)))
OBJS		:= $(addprefix $(BUILDDIR)/,$(SRCS:.c=.o))

GEN_HEADERS	:= $(BUILDDIR)/transfer_atoms.h $(BUILDDIR)/drm_modifiers.h

DRM_INCLUDEDIR	:= $(patsubst -I%,%,$(firstword \
			   $(filter -I%,$(shell pkg-config --cflags libdrm))))
DRMFOURCCH	:= $(DRM_INCLUDEDIR)/drm_fourcc.h

ifneq ($(EGL),0)
CPPFLAGS	+= -DHaveEglSupport
CFLAGS		+= $(shell pkg-config --cflags egl glesv2)
LDLIBS		+= $(shell pkg-config --libs egl glesv2)
OBJS		+= $(BUILDDIR)/egl.o
GEN_HEADERS	+= $(BUILDDIR)/shaders.h
endif

DEPS		:= $(OBJS:.o=.d) $(PROTO_OBJS:.o=.d)

all: $(BUILDDIR)/12to11

$(BUILDDIR):
	mkdir -p $@

$(BUILDDIR)/short_types.txt: $(DATA_DIR)/media_types.txt | $(BUILDDIR)
	sed '/application\/vnd/d' $< > $@

$(BUILDDIR)/transfer_atoms.h: $(BUILDDIR)/short_types.txt \
			      $(DATA_DIR)/mime0.awk $(DATA_DIR)/mime1.awk \
			      $(DATA_DIR)/mime2.awk $(DATA_DIR)/mime3.awk \
			      $(DATA_DIR)/mime4.awk
	gawk -f $(DATA_DIR)/mime0.awk $< >  $@
	gawk -f $(DATA_DIR)/mime1.awk $< >> $@
	gawk -f $(DATA_DIR)/mime2.awk $< >> $@
	gawk -f $(DATA_DIR)/mime3.awk $< >> $@
	gawk -f $(DATA_DIR)/mime4.awk $< >> $@

$(BUILDDIR)/drm_modifiers.h: $(DATA_DIR)/modifiers.awk $(DRMFOURCCH) \
			     | $(BUILDDIR)
	gawk -f $(DATA_DIR)/modifiers.awk $(DRMFOURCCH) > $@

$(BUILDDIR)/shaders.h: $(DATA_DIR)/shaders.txt $(DATA_DIR)/shaders.awk \
		       | $(BUILDDIR)
	gawk -f $(word 2,$^) $< > $@

$(BUILDDIR)/%.h: $(PROTO_DIR)/%.xml | $(BUILDDIR)
	wayland-scanner server-header $< $@

$(BUILDDIR)/%.c: $(PROTO_DIR)/%.xml | $(BUILDDIR)
	wayland-scanner private-code $< $@

$(OBJS): $(BUILDDIR)/%.o: $(SRC_DIR)/%.c $(GEN_HEADERS) $(PROTO_HDRS) \
			 | $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(PROTO_OBJS): $(BUILDDIR)/%.o: $(BUILDDIR)/%.c $(PROTO_HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/12to11: $(OBJS) $(PROTO_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

tests:
	$(MAKE) -C tests

install: $(BUILDDIR)/12to11
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)/man1
	install -m 755 $(BUILDDIR)/12to11 $(DESTDIR)$(BINDIR)
	install -m 644 man/12to11.man $(DESTDIR)$(MANDIR)/man1/12to11.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/12to11 $(DESTDIR)$(MANDIR)/man1/12to11.1

clean:
	rm -rf $(BUILDDIR)
	$(MAKE) -C tests clean

distclean: clean

git_archive:
	git archive --prefix=12to11/ -o 12to11.tar HEAD

.PHONY: all tests install uninstall clean distclean git_archive

-include $(DEPS)
