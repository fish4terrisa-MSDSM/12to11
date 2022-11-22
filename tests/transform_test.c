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

/* Tests for buffer transforms.  The relevant requests tested are:

     wl_surface.set_buffer_transform (90)
     wl_surface.set_buffer_transform (180)
     wl_surface.set_buffer_transform (270)
     wl_surface.set_buffer_transform (FLIPPED)
     wl_surface.set_buffer_transform (FLIPPED_90)
     wl_surface.set_buffer_transform (FLIPPED_180)
     wl_surface.set_buffer_transform (FLIPPED_270)

   Each test is run in order, with both damage_buffer and damage being
   used to compute buffer damage.  */

enum test_kind
  {
    MAP_WINDOW_KIND,
    BUFFER_TRANSFORM_90_KIND,
    BUFFER_TRANSFORM_180_KIND,
    BUFFER_TRANSFORM_270_KIND,
    BUFFER_TRANSFORM_FLIPPED_KIND,
    BUFFER_TRANSFORM_FLIPPED_90_KIND,
    BUFFER_TRANSFORM_FLIPPED_180_KIND,
    BUFFER_TRANSFORM_FLIPPED_270_KIND,
  };

static const char *test_names[] =
  {
    "map_window",
    "buffer_transform_90",
    "buffer_transform_180",
    "buffer_transform_270",
    "buffer_transform_flipped",
    "buffer_transform_flipped_90",
    "buffer_transform_flipped_180",
    "buffer_transform_flipped_270",
  };

#define LAST_TEST       BUFFER_TRANSFORM_FLIPPED_270_KIND

/* The display.  */
static struct test_display *display;

/* Test interfaces.  */
static struct test_interface test_interfaces[] =
  {
    /* No interfaces yet.  */
  };

/* The test surface window.  */
static Window test_surface_window;

/* The test surface and Wayland surface.  */
static struct test_surface *test_surface;
static struct wl_surface *wayland_surface;



/* Forward declarations.  */
static void wait_frame_callback (struct wl_surface *);
static void submit_surface_damage (struct wl_surface *, int, int, int, int);



static void
do_transform_damage_test (int surface_x, int surface_y,
			  int surface_width,
			  int surface_height,
			  enum wl_output_transform transform,
			  const char *dump_1_name,
			  const char *dump_2_name)
{
  struct wl_buffer *buffer, *damaged_buffer;

  buffer = load_png_image (display, "basic_test_card.png");

  if (!buffer)
    report_test_failure ("failed to load basic_test_card.png");

  damaged_buffer = load_png_image (display, "basic_damage.png");

  if (!damaged_buffer)
    report_test_failure ("failed to load basic_damage.png");

  wl_surface_attach (wayland_surface, buffer, 0, 0);
  wl_surface_set_buffer_transform (wayland_surface, transform);
  submit_surface_damage (wayland_surface, 0, 0,
			 INT_MAX, INT_MAX);
  wait_frame_callback (wayland_surface);

  /* Verify the image without any damage applied.  */
  verify_image_data (display, test_surface_window, dump_1_name);

  /* Now, load the damaged buffer and apply surface damage.  */
  wl_surface_attach (wayland_surface, damaged_buffer, 0, 0);
  submit_surface_damage (wayland_surface, surface_x, surface_y,
			 surface_width, surface_height);
  wait_frame_callback (wayland_surface);

  /* Verify the image with damage applied.  */
  verify_image_data (display, test_surface_window, dump_2_name);

  /* Now, load the untransformed buffer and apply buffer
     damage.  */
  wl_surface_attach (wayland_surface, buffer, 0, 0);
  wl_surface_damage_buffer (wayland_surface, 49, 26, 57, 48);
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
      submit_surface_damage (wayland_surface, 0, 0,
			     INT_MAX, INT_MAX);
      wl_surface_commit (wayland_surface);
      wl_buffer_destroy (buffer);
      break;

    case BUFFER_TRANSFORM_90_KIND:
      /* Perform the test.  */
      do_transform_damage_test (5, 49, 48, 57,
				WL_OUTPUT_TRANSFORM_90,
				"transform_test_90_1.dump",
				"transform_test_90_2.dump");

      /* Run the next test.  */
      test_single_step (BUFFER_TRANSFORM_180_KIND);
      break;

    case BUFFER_TRANSFORM_180_KIND:
      /* Perform the test.  */
      do_transform_damage_test (7, 5, 57, 48,
				WL_OUTPUT_TRANSFORM_180,
				"transform_test_180_1.dump",
				"transform_test_180_2.dump");

      /* Run the next test.  */
      test_single_step (BUFFER_TRANSFORM_270_KIND);
      break;

    case BUFFER_TRANSFORM_270_KIND:
      /* Perform the test.  */
      do_transform_damage_test (26, 7, 48, 57,
				WL_OUTPUT_TRANSFORM_270,
				"transform_test_270_1.dump",
				"transform_test_270_2.dump");

      /* Run the next test.  */
      test_single_step (BUFFER_TRANSFORM_FLIPPED_KIND);
      break;

    case BUFFER_TRANSFORM_FLIPPED_KIND:
      /* Perform the test.  */
      do_transform_damage_test (7, 26, 57, 48,
				WL_OUTPUT_TRANSFORM_FLIPPED,
				"transform_test_flipped_1.dump",
				"transform_test_flipped_2.dump");

      /* Run the next test.  */
      test_single_step (BUFFER_TRANSFORM_FLIPPED_90_KIND);
      break;

    case BUFFER_TRANSFORM_FLIPPED_90_KIND:
      /* Perform the test.  */
      do_transform_damage_test (26, 49, 48, 57,
				WL_OUTPUT_TRANSFORM_FLIPPED_90,
				"transform_test_flipped_90_1.dump",
				"transform_test_flipped_90_2.dump");

      /* Run the next test.  */
      test_single_step (BUFFER_TRANSFORM_FLIPPED_180_KIND);
      break;

    case BUFFER_TRANSFORM_FLIPPED_180_KIND:
      /* Perform the test.  */
      do_transform_damage_test (49, 5, 57, 48,
				WL_OUTPUT_TRANSFORM_FLIPPED_180,
				"transform_test_flipped_180_1.dump",
				"transform_test_flipped_180_2.dump");

      /* Run the next test.  */
      test_single_step (BUFFER_TRANSFORM_FLIPPED_270_KIND);
      break;

    case BUFFER_TRANSFORM_FLIPPED_270_KIND:
      /* Perform the test.  */
      do_transform_damage_test (5, 7, 48, 57,
				WL_OUTPUT_TRANSFORM_FLIPPED_270,
				"transform_test_flipped_270_1.dump",
				"transform_test_flipped_270_2.dump");
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
  test_single_step (BUFFER_TRANSFORM_90_KIND);
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
submit_surface_damage (struct wl_surface *surface, int x, int y, int width,
		       int height)
{
  test_log ("damaging surface by %d, %d, %d, %d", x, y, width,
	    height);

  wl_surface_damage (surface, x, y, width, height);
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
