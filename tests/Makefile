# Makefile for the 12to11 integration tests.

BUILDDIR	:= build
PROTO_DIR	:= ../protocols

CC		?= cc

BASE_PKGS	:= wayland-client x11 libpng libdrm
DMABUF_PKGS	:= gbm libdrm

CPPFLAGS	+= $(shell pkg-config --cflags $(BASE_PKGS)) \
		   -D_GNU_SOURCE -U_BSD_SOURCE -U_SVID_SOURCE \
		   -I$(BUILDDIR)
CFLAGS		?= -g3 -O2
LDLIBS		+= $(shell pkg-config --libs $(BASE_PKGS)) -lm

PROTOCOLS	:= 12to11-test viewporter linux-dmabuf-unstable-v1 \
		   xdg-activation-v1 single-pixel-buffer-v1 \
		   tearing-control-v1

PROTO_HDRS	:= $(addprefix $(BUILDDIR)/,$(addsuffix .h,$(PROTOCOLS)))
PROTO_SRCS	:= $(addprefix $(BUILDDIR)/,$(addsuffix .c,$(PROTOCOLS)))
PROTO_OBJS	:= $(PROTO_SRCS:.c=.o)

COMMON_OBJS	:= $(BUILDDIR)/test_harness.o $(PROTO_OBJS)

PROGRAMS	:= imgview simple_test damage_test transform_test \
		   viewporter_test subsurface_test scale_test seat_test \
		   dmabuf_test select_test select_helper \
		   select_helper_multiple xdg_activation_test \
		   single_pixel_buffer_test buffer_test \
		   tearing_control_test

COMMON_TESTS	:= imgview simple_test damage_test transform_test \
		   viewporter_test subsurface_test scale_test seat_test \
		   xdg_activation_test single_pixel_buffer_test \
		   buffer_test tearing_control_test

DEPS		:= $(COMMON_OBJS:.o=.d) $(PROGRAMS:%=$(BUILDDIR)/%.d)

all: $(addprefix $(BUILDDIR)/,$(PROGRAMS))

$(BUILDDIR):
	mkdir -p $@

$(BUILDDIR)/%.h: $(PROTO_DIR)/%.xml | $(BUILDDIR)
	wayland-scanner client-header $< $@

$(BUILDDIR)/%.c: $(PROTO_DIR)/%.xml | $(BUILDDIR)
	wayland-scanner private-code $< $@

$(PROTO_OBJS): $(BUILDDIR)/%.o: $(BUILDDIR)/%.c $(PROTO_HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

TEST_OBJS	:= $(PROGRAMS:%=$(BUILDDIR)/%.o) $(BUILDDIR)/test_harness.o

$(TEST_OBJS): $(BUILDDIR)/%.o: %.c test_harness.h $(PROTO_HDRS) | $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(COMMON_TESTS:%=$(BUILDDIR)/%): $(BUILDDIR)/%: $(BUILDDIR)/%.o \
				 $(COMMON_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/dmabuf_test: $(BUILDDIR)/dmabuf_test.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS) \
		$(shell pkg-config --libs $(DMABUF_PKGS))

$(BUILDDIR)/select_test: $(BUILDDIR)/select_test.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -pthread -o $@ $^ $(LDLIBS)

$(BUILDDIR)/select_helper: $(BUILDDIR)/select_helper.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ \
		$(shell pkg-config --libs x11)

$(BUILDDIR)/select_helper_multiple: $(BUILDDIR)/select_helper_multiple.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ \
		$(shell pkg-config --libs x11)

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean

-include $(DEPS)
