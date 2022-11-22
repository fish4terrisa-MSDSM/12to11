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
#include "single-pixel-buffer-v1.h"
#include "viewporter.h"

/* Tests for single pixel buffers.  */

enum test_kind
  {
    MAP_WINDOW_KIND,
    SINGLE_PIXEL_BUFFER_KIND,
    SINGLE_PIXEL_BUFFER_VIEWPORT_KIND,
  };

static const char *test_names[] =
  {
    "map_window",
    "single_pixel_buffer",
    "single_pixel_buffer_viewport",
  };

#define LAST_TEST       SINGLE_PIXEL_BUFFER_VIEWPORT_KIND

/* The display.  */
static struct test_display *display;

/* The buffer manager.  */
static struct wp_single_pixel_buffer_manager_v1 *manager;

/* The viewporter.  */
static struct wp_viewporter *viewporter;

/* Test interfaces.  */
static struct test_interface test_interfaces[] =
  {
    { "wp_single_pixel_buffer_manager_v1", &manager,
      &wp_single_pixel_buffer_manager_v1_interface, 1, },
    { "wp_viewporter", &viewporter, &wp_viewporter_interface,
      1, },
  };

/* The test surface window.  */
static Window test_surface_window;

/* The test surface and Wayland surface.  */
static struct test_surface *test_surface;
static struct wl_surface *wayland_surface;

/* The single pixel buffer.  */
static struct wl_buffer *single_pixel_buffer;

/* The viewport.  */
static struct wp_viewport *viewport;



/* Forward declarations.  */
static void wait_frame_callback (struct wl_surface *);



static void
test_single_step (enum test_kind kind)
{
  test_log ("running test step: %s", test_names[kind]);

  switch (kind)
    {
    case MAP_WINDOW_KIND:
      wl_surface_attach (wayland_surface, single_pixel_buffer, 0, 0);
      wl_surface_damage (wayland_surface, 0, 0, 1, 1);
      wl_surface_commit (wayland_surface);
      break;

    case SINGLE_PIXEL_BUFFER_KIND:
      wait_frame_callback (wayland_surface);
      verify_image_data (display, test_surface_window,
			 "single_pixel_buffer.dump");
      test_single_step (SINGLE_PIXEL_BUFFER_VIEWPORT_KIND);
      break;

    case SINGLE_PIXEL_BUFFER_VIEWPORT_KIND:
      wp_viewport_set_source (viewport,
			      wl_fixed_from_double (0.0),
			      wl_fixed_from_double (0.0),
			      wl_fixed_from_double (1.0),
			      wl_fixed_from_double (1.0));
      wp_viewport_set_destination (viewport, 275, 275);
      wait_frame_callback (wayland_surface);

      verify_image_data (display, test_surface_window,
			 "single_pixel_buffer_viewport.dump");
      break;
    }

  if (kind == LAST_TEST)
    test_complete ();
}



static void
handle_test_surface_mapped (void *data, struct test_surface *surface,
			    uint32_t xid, const char *display_string)
{
  /* Sleep for 1 second to ensure that the window is exposed and
     redirected.  */
  sleep (1);

  /* Start the test.  */
  test_surface_window = xid;

  /* Run the test again.  */
  test_single_step (SINGLE_PIXEL_BUFFER_KIND);
}

static void
handle_test_surface_committed (void *data, struct test_surface *surface,
			       uint32_t presentation_hint)
{

}

static const struct test_surface_listener test_surface_listener =
  {
    handle_test_surface_mapped,
    NULL,
    handle_test_surface_committed,
  };



static void
handle_wl_callback_done (void *data, struct wl_callback *callback,
			   uint32_t callback_data)
{
  bool *flag;

  wl_callback_destroy (callback);

  /* Now tell wait_frame_callback to break out of the loop.  */
  flag = data;
  *flag = true;
}

static const struct wl_callback_listener wl_callback_listener =
  {
    handle_wl_callback_done,
  };



static void
wait_frame_callback (struct wl_surface *surface)
{
  struct wl_callback *callback;
  bool flag;

  /* Commit surface and wait for a frame callback.  */

  callback = wl_surface_frame (surface);
  flag = false;

  wl_callback_add_listener (callback, &wl_callback_listener,
			    &flag);
  wl_surface_commit (surface);

  while (!flag)
    {
      if (wl_display_dispatch (display->display) == -1)
        die ("wl_display_dispatch");
    }
}

static void
run_test (void)
{
  if (!make_test_surface (display, &wayland_surface,
			  &test_surface))
    report_test_failure ("failed to create test surface");

  test_surface_add_listener (test_surface, &test_surface_listener,
			     NULL);

  /* Create the single pixel buffer.  */
  single_pixel_buffer
    = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer (manager,
								0xffffffff,
								0xffffffff,
								0x00000000,
								0xffffffff);
  if (!single_pixel_buffer)
    report_test_failure ("failed to create single pixel buffer");

  /* And the viewport.  */
  viewport = wp_viewporter_get_viewport (viewporter, wayland_surface);

  if (!viewport)
    report_test_failure ("failed to get viewport");

  test_single_step (MAP_WINDOW_KIND);

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
