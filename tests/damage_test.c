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

enum test_kind
  {
    MAP_WINDOW_KIND,
    BASIC_TEST_CARD_IMAGE_KIND,
    BASIC_DAMAGE_KIND,
  };

static const char *test_names[] =
  {
    "map_window",
    "basic_test_card_image",
    "basic_damage",
  };

#define LAST_TEST	BASIC_DAMAGE_KIND

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
static void submit_frame_callback (struct wl_surface *, enum test_kind);
static void submit_surface_damage (struct wl_surface *, int, int, int, int);



static void
verify_single_step (enum test_kind kind)
{
  switch (kind)
    {
    case BASIC_TEST_CARD_IMAGE_KIND:
      verify_image_data (display, test_surface_window,
			 "damage_test_1.dump");
      break;

    case BASIC_DAMAGE_KIND:
      verify_image_data (display, test_surface_window,
			 "damage_test_2.dump");
      break;

    default:
      break;
    }

  if (kind == LAST_TEST)
    test_complete ();
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

    case BASIC_TEST_CARD_IMAGE_KIND:
      buffer = load_png_image (display, "basic_test_card.png");

      if (!buffer)
	report_test_failure ("failed to load basic_test_card.png");

      wl_surface_attach (wayland_surface, buffer, 0, 0);
      submit_frame_callback (wayland_surface, kind);
      submit_surface_damage (wayland_surface, 0, 0,
			     INT_MAX, INT_MAX);
      wl_surface_commit (wayland_surface);
      wl_buffer_destroy (buffer);
      break;

    case BASIC_DAMAGE_KIND:
      buffer = load_png_image (display, "basic_damage.png");

      if (!buffer)
	report_test_failure ("failed to load basic_damage.png");

      wl_surface_attach (wayland_surface, buffer, 0, 0);
      submit_frame_callback (wayland_surface, kind);
      submit_surface_damage (wayland_surface, 49, 26, 57, 48);
      wl_surface_commit (wayland_surface);
      wl_buffer_destroy (buffer);
      break;
    }
}

static void
test_next_step (enum test_kind kind)
{
  switch (kind)
    {
    case BASIC_TEST_CARD_IMAGE_KIND:
      test_single_step (BASIC_DAMAGE_KIND);
      break;

    default:
      break;
    }
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
  test_single_step (BASIC_TEST_CARD_IMAGE_KIND);
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
  enum test_kind kind;

  /* kind is not a pointer.  It is an enum test_kind stuffed into a
     pointer.  */
  kind = (intptr_t) data;

  wl_callback_destroy (callback);
  verify_single_step (kind);

  /* Now run the next test in this sequence.  */
  test_next_step (kind);
}

static const struct wl_callback_listener wl_callback_listener =
  {
    handle_wl_callback_done,
  };



static void
submit_frame_callback (struct wl_surface *surface, enum test_kind kind)
{
  struct wl_callback *callback;

  callback = wl_surface_frame (surface);
  wl_callback_add_listener (callback, &wl_callback_listener,
			    (void *) (intptr_t) kind);
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
