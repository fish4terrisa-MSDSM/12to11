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

/* Tests for buffer scaling.  */

enum test_kind
  {
    MAP_WINDOW_KIND,
    BUFFER_SCALE_1_KIND,
    BUFFER_SCALE_2_KIND,
    BUFFER_SCALE_1_2_KIND,
    BUFFER_SCALE_2_2_KIND,
    BUFFER_SCALE_2_5_KIND,
  };

static const char *test_names[] =
  {
    "map_window",
    "buffer_scale_1",
    "buffer_scale_2",
    "buffer_scale_1_2",
    "buffer_scale_2_2",
    "buffer_scale_2_5",
  };

#define LAST_TEST       BUFFER_SCALE_2_5_KIND

/* The display.  */
static struct test_display *display;

/* Test interfaces.  */
static struct test_interface test_interfaces[] =
  {
    /* No interfaces.  */
  };

/* The test surface window.  */
static Window test_surface_window;

/* The test surface and Wayland surface.  */
static struct test_surface *test_surface;
static struct wl_surface *wayland_surface;



/* Forward declarations.  */
static void wait_frame_callback (struct wl_surface *);



static void
do_verify_window_size (int scale, int output_scale)
{
  int scale_factor;
  double buffer_factor;

  /* Verify the window size.  The window size is determined by first
     calculating a scale factor, which is the output scale minus the
     scale.  The scale factor describes how many times to scale down
     the buffer contents.  */
  scale_factor = scale - output_scale;

  /* Next, compute how much the buffer should actually be scaled
     by.  */
  buffer_factor = (scale_factor > 0
		   ? 1.0 / (scale_factor + 1)
		   : -scale_factor + 1);

  /* And verify the window size.  */
  verify_window_size (display, test_surface_window,
		      ceil (500 * buffer_factor),
		      ceil (500 * buffer_factor));
}

static void
do_scale_damage_test (int scale, const char *dump_1_name,
		      const char *dump_2_name)
{
  struct wl_buffer *buffer, *damaged_buffer;
  int scaled_x1, scaled_y1, scaled_x2, scaled_y2;

  buffer = load_png_image (display, "scale.png");

  if (!buffer)
    report_test_failure ("failed to load scale.png");

  damaged_buffer = load_png_image (display, "scale_damage.png");

  if (!damaged_buffer)
    report_test_failure ("failed to load scale_damage.png");

  wl_surface_set_buffer_scale (wayland_surface, scale);
  wl_surface_attach (wayland_surface, buffer, 0, 0);
  wl_surface_damage (wayland_surface, 0, 0, INT_MAX, INT_MAX);
  wait_frame_callback (wayland_surface);

  /* Verify the image without any damage applied.  */
  verify_image_data (display, test_surface_window, dump_1_name);

  /* Now, load the damaged buffer and apply buffer damage.  */
  wl_surface_attach (wayland_surface, damaged_buffer, 0, 0);
  wl_surface_damage_buffer (wayland_surface, 25, 25, 450, 450);
  wait_frame_callback (wayland_surface);

  /* Verify the image with damage applied.  */
  verify_image_data (display, test_surface_window, dump_2_name);

  /* Now, reattach the undamaged buffer and apply surface damage.  */
  wl_surface_attach (wayland_surface, buffer, 0, 0);
  scaled_x1 = floor (25.0 / scale);
  scaled_y1 = floor (25.0 / scale);
  scaled_x2 = ceil ((25 + 450.0) / scale);
  scaled_y2 = ceil ((25 + 450.0) / scale);
  wl_surface_damage (wayland_surface, scaled_x1, scaled_y1,
		     scaled_x2 - scaled_x1, scaled_y2 - scaled_y1);
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

    case BUFFER_SCALE_1_KIND:
      do_scale_damage_test (1, "buffer_scale_1_1.dump",
			    "buffer_scale_1_2.dump");
      do_verify_window_size (1, 1);
      test_single_step (BUFFER_SCALE_2_KIND);
      break;

    case BUFFER_SCALE_2_KIND:
      do_scale_damage_test (2, "buffer_scale_2_1.dump",
			    "buffer_scale_2_2.dump");
      do_verify_window_size (2, 1);
      test_single_step (BUFFER_SCALE_1_2_KIND);
      break;

    case BUFFER_SCALE_1_2_KIND:
      /* Now the buffer should be scaled up to 1000x1000.  */
      test_set_scale (display, 2);
      do_scale_damage_test (1, "buffer_scale_1_2_1.dump",
			    "buffer_scale_1_2_2.dump");
      do_verify_window_size (1, 2);
      test_single_step (BUFFER_SCALE_2_2_KIND);
      break;

    case BUFFER_SCALE_2_2_KIND:
      /* And the buffer should not be scaled at all.  */
      test_set_scale (display, 2);
      do_scale_damage_test (2, "buffer_scale_2_2_1.dump",
			    "buffer_scale_2_2_2.dump");
      do_verify_window_size (2, 2);
      test_single_step (BUFFER_SCALE_2_5_KIND);
      break;

    case BUFFER_SCALE_2_5_KIND:
      /* The buffer should be made three times larger.  */
      test_set_scale (display, 5);
      do_scale_damage_test (2, "buffer_scale_2_5_1.dump",
			    "buffer_scale_2_5_2.dump");
      do_verify_window_size (2, 5);
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
  test_single_step (BUFFER_SCALE_1_KIND);
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
