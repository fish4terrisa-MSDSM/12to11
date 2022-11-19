/* Tests for the Wayland compositor running on the X server.

Copyright (C) 2022 to various contributors.

This file is part of 12to11.

12to11 is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

12to11 is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with 12to11.  If not, see <https://www.gnu.org/licenses/>.  */

#include "test_harness.h"
#include "tearing-control-v1.h"

/* Tests for buffer release.  */

enum test_kind
  {
    TEARING_CONTROL_KIND,
    TEARING_DESTROY_KIND,
  };

static const char *test_names[] =
  {
    "tearing_control",
    "tearing_destroy",
  };

#define LAST_TEST	TEARING_CONTROL_KIND

/* The display.  */
static struct test_display *display;

/* The tearing control manager.  */
static struct wp_tearing_control_manager_v1 *manager;

/* Test interfaces.  */
static struct test_interface test_interfaces[] =
  {
    { "wp_tearing_control_manager_v1", &manager,
      &wp_tearing_control_manager_v1_interface, 1, },
  };

/* The test surface and Wayland surface.  */
static struct test_surface *test_surface;
static struct wl_surface *wayland_surface;

/* The tearing control.  */
static struct wp_tearing_control_v1 *tearing_control;

/* The presentation hint used.  1 means async, and 0 means vsync.  */
static int used_presentation_mode;



/* Forward declarations.  */
static void verify_async_used (void);
static void verify_vsync_used (void);



static struct test_buffer *
make_test_buffer (void)
{
  struct wl_buffer *buffer;
  struct test_buffer *test_buffer;
  char *empty_data;
  size_t stride;

  stride = get_image_stride (display, 24, 1);

  if (!stride)
    report_test_failure ("unknown stride");

  empty_data = calloc (1, stride);

  if (!empty_data)
    report_test_failure ("failed to allocate buffer data");

  buffer = upload_image_data (display, empty_data, 1, 1, 24);
  free (empty_data);

  if (!buffer)
    report_test_failure ("failed to create single pixel buffer");

  test_buffer = get_test_buffer (display, buffer);

  if (!test_buffer)
    report_test_failure ("failed to create test buffer");

  return test_buffer;
}

static void
test_single_step (enum test_kind kind)
{
  struct test_buffer *buffer;

 again:

  test_log ("running test step: %s", test_names[kind]);

  switch (kind)
    {
    case TEARING_CONTROL_KIND:
#define VSYNC WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC
      wp_tearing_control_v1_set_presentation_hint (tearing_control,
						   VSYNC);
#undef VSYNC
      buffer = make_test_buffer ();

      /* Attach the buffer.  */
      wl_surface_attach (wayland_surface, buffer->buffer, 0, 0);
      wl_surface_commit (wayland_surface);

      /* Now see what kind of presentation was used.  */
      verify_vsync_used ();

#define ASYNC WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC
      wp_tearing_control_v1_set_presentation_hint (tearing_control,
						   ASYNC);
#undef ASYNC
      wl_surface_commit (wayland_surface);

      /* Now verify that async presentation was used.  */
      verify_async_used ();

      kind = TEARING_DESTROY_KIND;
      goto again;

    case TEARING_DESTROY_KIND:
      /* Destroy the tearing control resource.  */
      wp_tearing_control_v1_destroy (tearing_control);
      wl_surface_commit (wayland_surface);

      /* Verify that the tearing hint reverted to vsync.  */
      verify_vsync_used ();
      break;
    }

  if (kind == LAST_TEST)
    test_complete ();
}



static void
handle_test_surface_mapped (void *data, struct test_surface *test_surface,
			    uint32_t xid, const char *display_string)
{

}

static void
handle_test_surface_activated (void *data, struct test_surface *test_surface,
			       uint32_t months, uint32_t milliseconds,
			       struct wl_surface *activator_surface)
{

}

static void
handle_test_surface_committed (void *data, struct test_surface *test_surface,
			       uint32_t presentation_hint)
{
  used_presentation_mode = presentation_hint;
}

static const struct test_surface_listener test_surface_listener =
  {
    handle_test_surface_mapped,
    handle_test_surface_activated,
    handle_test_surface_committed,
  };

static void
verify_async_used (void)
{
  wl_display_roundtrip (display->display);

  if (used_presentation_mode != 1)
    report_test_failure ("async presentation not used where expected!");
}

static void
verify_vsync_used (void)
{
  wl_display_roundtrip (display->display);

  if (used_presentation_mode == 1)
    report_test_failure ("vsync presentation not used where expected!");
}



static void
run_test (void)
{
  if (!make_test_surface (display, &wayland_surface,
			  &test_surface))
    report_test_failure ("failed to create test surface");

  test_surface_add_listener (test_surface, &test_surface_listener,
			     NULL);

  tearing_control
    = wp_tearing_control_manager_v1_get_tearing_control (manager,
							 wayland_surface);

  if (!tearing_control)
    report_test_failure ("failed to create tearing control");

  test_single_step (TEARING_CONTROL_KIND);

  while (true)
    {
      if (wl_display_dispatch (display->display) == -1)
        die ("wl_display_dispatch");
    }
}

int
main (void)
{
  test_init ();
  display = open_test_display (test_interfaces,
			       ARRAYELTS (test_interfaces));

  if (!display)
    report_test_failure ("failed to open display");

  run_test ();
}
