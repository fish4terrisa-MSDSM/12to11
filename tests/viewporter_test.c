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
#include "viewporter.h"

/* Tests for wp_viewporter.  The relevant requests tested are:

     wp_viewporter.set_source (-1, -1, -1, -1)
     wp_viewporter.set_destination (200, 150)

     wp_viewporter.set_source (50, 50, 200, 200)
     wp_viewporter.set_destination (-1, -1)

     wp_viewporter.set_source (50, 50, 200, 200)
     wp_viewporter.set_destination (500, 500)

     wp_viewporter.set_source (50, 50, 200, 200)
     wp_viewporter.set_destination (50, 75)

   Each test is run in order, with both damage_buffer and damage being
   used to compute buffer damage.  Finally, the following requests are
   tested with a buffer transform of 90:

     wp_viewporter.set_source (250, 50, 200, 200)
     wp_viewporter.set_destination (100, 100)  */

enum test_kind
  {
    MAP_WINDOW_KIND,
    VIEWPORT_DEST_200_150_KIND,
    VIEWPORT_SRC_50_50_200_200_KIND,
    VIEWPORT_SRC_50_50_200_200_DEST_500_500_KIND,
    VIEWPORT_SRC_50_50_200_200_DEST_50_75_KIND,
    VIEWPORT_SRC_250_50_200_200_DEST_50_75_90CW_KIND,
  };

static const char *test_names[] =
  {
    "map_window",
    "viewport_dest_250_150",
    "viewport_src_50_50_200_200",
    "viewport_src_50_50_200_200_dest_500_500",
    "viewport_src_50_50_200_200_dest_50_75",
    "viewport_src_250_50_200_200_dest_50_75_90cw",
  };

#define LAST_TEST	VIEWPORT_SRC_250_50_200_200_DEST_50_75_90CW_KIND

/* The display.  */
static struct test_display *display;

/* The viewporter interface.  */
static struct wp_viewporter *viewporter;

/* Test interfaces.  */
static struct test_interface test_interfaces[] =
  {
    { "wp_viewporter", &viewporter, &wp_viewporter_interface, 1, },
  };

/* The test surface window.  */
static Window test_surface_window;

/* The test surface and Wayland surface.  */
static struct test_surface *test_surface;
static struct wl_surface *wayland_surface;

/* The surface viewport.  */
static struct wp_viewport *viewport;



/* Forward declarations.  */
static void wait_frame_callback (struct wl_surface *);



static void
do_viewport_damage_test (double src_x, double src_y, double src_width,
			 double src_height, int dest_width,
			 int dest_height, const char *dump_1_name,
			 const char *dump_2_name)
{
  struct wl_buffer *buffer, *damaged_buffer;

  buffer = load_png_image (display, "viewporter_test.png");

  if (!buffer)
    report_test_failure ("failed to load viewporter_test.png");

  damaged_buffer = load_png_image (display, "viewporter_test_1.png");

  if (!damaged_buffer)
    report_test_failure ("failed to load viewporter_test_1.png");

  /* Set the viewport.  */
  wp_viewport_set_source (viewport,
			  wl_fixed_from_double (src_x),
			  wl_fixed_from_double (src_y),
			  wl_fixed_from_double (src_width),
			  wl_fixed_from_double (src_height));
  wp_viewport_set_destination (viewport, dest_width, dest_height);

  wl_surface_attach (wayland_surface, buffer, 0, 0);
  wl_surface_damage (wayland_surface, 0, 0, INT_MAX, INT_MAX);
  wait_frame_callback (wayland_surface);

  /* Verify the image without any damage applied.  */
  verify_image_data (display, test_surface_window, dump_1_name);

  /* Now, load the damaged buffer and apply buffer damage.  */
  wl_surface_attach (wayland_surface, damaged_buffer, 0, 0);
  wl_surface_damage_buffer (wayland_surface, 100, 100, 30, 30);
  wait_frame_callback (wayland_surface);

  /* Verify the image with damage applied.  */
  verify_image_data (display, test_surface_window, dump_2_name);

  /* Now, load the untransformed buffer and apply surface damage.  */
  wl_surface_attach (wayland_surface, buffer, 0, 0);
  wl_surface_damage_buffer (wayland_surface, 100, 100, 30, 30);
  wait_frame_callback (wayland_surface);

  /* Verify that the surface with the damage reverted is the same
     as the initial contents of the surface.  */
  verify_image_data (display, test_surface_window, dump_1_name);

  /* Free both buffers.  */
  wl_buffer_destroy (buffer);
  wl_buffer_destroy (damaged_buffer);
}

static void
test_single_step (enum test_kind kind)
{
  struct wl_buffer *buffer;

  test_log ("running test step: %s", test_names[kind]);

  switch (kind)
    {
    case MAP_WINDOW_KIND:
      buffer = load_png_image (display, "blue.png");

      if (!buffer)
	report_test_failure ("failed to load blue.png");

      wl_surface_attach (wayland_surface, buffer, 0, 0);
      wl_surface_damage (wayland_surface, 0, 0, INT_MAX, INT_MAX);
      wl_surface_commit (wayland_surface);
      wl_buffer_destroy (buffer);
      break;

    case VIEWPORT_DEST_200_150_KIND:
      do_viewport_damage_test (-1, -1, -1, -1, 200, 150,
			       "viewport_dest_200_150_1.dump",
			       "viewport_dest_200_150_2.dump");
      test_single_step (VIEWPORT_SRC_50_50_200_200_KIND);
      break;

    case VIEWPORT_SRC_50_50_200_200_KIND:
      do_viewport_damage_test (50, 50, 200, 200, -1, -1,
			       "viewport_src_50_50_200_200_1.dump",
			       "viewport_src_50_50_200_200_2.dump");
      test_single_step (VIEWPORT_SRC_50_50_200_200_DEST_500_500_KIND);
      break;

    case VIEWPORT_SRC_50_50_200_200_DEST_500_500_KIND:
      do_viewport_damage_test (50, 50, 200, 200, 500, 500,
			       "viewport_src_50_50_200_200_dest_500_500_1.dump",
			       "viewport_src_50_50_200_200_dest_500_500_2.dump");
      test_single_step (VIEWPORT_SRC_50_50_200_200_DEST_50_75_KIND);
      break;

    case VIEWPORT_SRC_50_50_200_200_DEST_50_75_KIND:
      do_viewport_damage_test (50, 50, 200, 200, 50, 75,
			       "viewport_src_50_50_200_200_dest_50_75_1.dump",
			       "viewport_src_50_50_200_200_dest_50_75_2.dump");
      test_single_step (VIEWPORT_SRC_250_50_200_200_DEST_50_75_90CW_KIND);
      break;

    case VIEWPORT_SRC_250_50_200_200_DEST_50_75_90CW_KIND:
      wl_surface_set_buffer_transform (wayland_surface,
				       WL_OUTPUT_TRANSFORM_90);
      do_viewport_damage_test (250, 50, 200, 200, 50, 75,
			       "viewport_src_250_50_200_200_dest_50_75"
			       "_90cw_1.dump",
			       "viewport_src_250_50_200_200_dest_50_75"
			       "_90cw_2.dump");
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
  test_single_step (VIEWPORT_DEST_200_150_KIND);
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

  viewport = wp_viewporter_get_viewport (viewporter,
					 wayland_surface);

  if (!viewport)
    report_test_failure ("failed to create viewport");

  test_surface_add_listener (test_surface, &test_surface_listener,
			     NULL);
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
