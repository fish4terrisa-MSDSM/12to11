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

#include <X11/extensions/XI2.h>

enum test_expect_event_kind
  {
    POINTER_ENTER_EVENT,
    POINTER_FRAME_EVENT,
    POINTER_MOTION_EVENT,
  };

struct test_expect_data
{
  /* The coordinates of the event.  */
  double x, y;

  /* What kind of event is being waited for.  */
  enum test_expect_event_kind kind;

  /* Whether or not the expected event arrived.  */
  bool arrived;
};

enum test_kind
  {
    MAP_WINDOW_KIND,
    TEST_ENTRY_KIND,
  };

static const char *test_names[] =
  {
    "map_window",
    "test_entry",
  };

#define LAST_TEST	        TEST_ENTRY_KIND
#define TEST_SOURCE_DEVICE	4500000

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

/* How many elements are in the current listener data.  */
static int num_listener_data;

/* The current listener data.  */
struct test_expect_data *current_listener_data;



/* Forward declarations.  */
static void submit_surface_damage (struct wl_surface *, int, int, int, int);
static void expect_surface_enter (double, double);
static void expect_surface_motion (double, double);



/* Get a timestamp suitable for use in events dispatched to the test
   seat.  */

static uint32_t
test_get_time (void)
{
  struct timespec timespec;

  clock_gettime (CLOCK_MONOTONIC, &timespec);

  return (timespec.tv_sec * 1000
	  + timespec.tv_nsec / 1000000);
}

/* Get the root window.  */

static Window
test_get_root (void)
{
  return DefaultRootWindow (display->x_display);
}



static void
test_single_step (enum test_kind kind)
{
  struct wl_buffer *buffer;

  test_log ("running test step: %s", test_names[kind]);

  switch (kind)
    {
    case MAP_WINDOW_KIND:
      buffer = load_png_image (display, "seat_test.png");

      if (!buffer)
	report_test_failure ("failed to load seat_test.png");

      wl_surface_attach (wayland_surface, buffer, 0, 0);
      submit_surface_damage (wayland_surface, 0, 0, 500, 500);
      wl_surface_commit (wayland_surface);
      wl_buffer_destroy (buffer);
      break;

    case TEST_ENTRY_KIND:
      /* Enter the 500x500 window.  The window is at 0, 0 relative to
	 the root window.  */
      test_seat_controller_dispatch_XI_Enter (display->seat->controller,
					      test_get_time (),
					      TEST_SOURCE_DEVICE,
					      XINotifyAncestor,
					      test_get_root (),
					      test_surface_window,
					      None,
					      wl_fixed_from_double (0.0),
					      wl_fixed_from_double (0.0),
					      wl_fixed_from_double (0.0),
					      wl_fixed_from_double (0.0),
					      XINotifyNormal,
					      False, True, NULL, NULL,
					      NULL);

      /* Expect an enter at 0.0 by 0.0.  */
      expect_surface_enter (0.0, 0.0);

      /* Now move the mouse a little.  */
      test_seat_controller_dispatch_XI_Motion (display->seat->controller,
					       test_get_time (),
					       TEST_SOURCE_DEVICE,
					       0,
					       test_get_root (),
					       test_surface_window,
					       None,
					       wl_fixed_from_double (1.0),
					       wl_fixed_from_double (2.0),
					       wl_fixed_from_double (1.0),
					       wl_fixed_from_double (2.0),
					       0,
					       NULL, NULL, NULL, NULL);

      /* Expect mouse motion at the specified coordinates.  */
      expect_surface_motion (1.0, 2.0);
      break;
    }

  if (kind == LAST_TEST)
    test_complete ();
}



static void
expect_surface_enter (double x, double y)
{
  struct test_expect_data data[2];

  test_log ("waiting for enter at %g, %g", x, y);

  memset (data, 0, sizeof data);

  data[0].x = x;
  data[0].y = y;
  data[0].kind = POINTER_ENTER_EVENT;
  data[1].kind = POINTER_FRAME_EVENT;

  /* Set the current listener data and do a roundtrip.  */
  current_listener_data = data;
  num_listener_data = 2;

  wl_display_roundtrip (display->display);
  current_listener_data = NULL;
  num_listener_data = 0;

  /* See whether or not the event arrived.  */
  if (!data[0].arrived || !data[1].arrived)
    report_test_failure ("expected events did not arrive");
  else
    test_log ("received enter followed by frame");
}

static void
expect_surface_motion (double x, double y)
{
  struct test_expect_data data[2];

  test_log ("waiting for motion at %g, %g", x, y);

  memset (data, 0, sizeof data);

  data[0].x = x;
  data[0].y = y;
  data[0].kind = POINTER_MOTION_EVENT;
  data[1].kind = POINTER_FRAME_EVENT;

  /* Set the current listener data and do a roundtrip.  */
  current_listener_data = data;
  num_listener_data = 2;

  wl_display_roundtrip (display->display);
  current_listener_data = NULL;
  num_listener_data = 0;

  /* See whether or not the event arrived.  */
  if (!data[0].arrived || !data[1].arrived)
    report_test_failure ("expected events did not arrive");
  else
    test_log ("received motion followed by frame");
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
  test_single_step (TEST_ENTRY_KIND);
}

static const struct test_surface_listener test_surface_listener =
  {
    handle_test_surface_mapped,
  };



/* Obtain the next test data record.  The events arriving are checked
   to be in the order in which they arrive in
   current_listener_data.  */

static struct test_expect_data *
get_next_expect_data (void)
{
  int i;

  for (i = 0; i < num_listener_data; ++i)
    {
      if (current_listener_data[i].arrived)
	continue;

      return &current_listener_data[i];
    }

  return NULL;
}

static void
handle_pointer_enter (void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, struct wl_surface *surface,
		      wl_fixed_t surface_x, wl_fixed_t surface_y)
{
  struct test_expect_data *test_data;

  test_data = get_next_expect_data ();

  if (!test_data)
    {
      test_log ("ignored enter event at %g %g",
		wl_fixed_to_double (surface_x),
		wl_fixed_to_double (surface_y));
      return;
    }

  if (test_data->kind != POINTER_ENTER_EVENT)
    return;

  test_log ("got enter event at %g, %g",
	    wl_fixed_to_double (surface_x),
	    wl_fixed_to_double (surface_y));

  if (test_data->x == wl_fixed_to_double (surface_x)
      && test_data->y == wl_fixed_to_double (surface_y))
    test_data->arrived = true;
  else
    report_test_failure ("missed enter event at %g %g",
			 test_data->x, test_data->y);
}

static void
handle_pointer_leave (void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, struct wl_surface *surface)
{
  /* ... */
}

static void
handle_pointer_motion (void *data, struct wl_pointer *wl_pointer,
		       uint32_t time, wl_fixed_t surface_x,
		       wl_fixed_t surface_y)
{
  struct test_expect_data *test_data;

  test_data = get_next_expect_data ();

  if (!test_data)
    {
      test_log ("ignored motion event at %g %g",
		wl_fixed_to_double (surface_x),
		wl_fixed_to_double (surface_y));
      return;
    }

  if (test_data->kind != POINTER_MOTION_EVENT)
    return;

  test_log ("got motion event at %g, %g",
	    wl_fixed_to_double (surface_x),
	    wl_fixed_to_double (surface_y));

  if (test_data->x == wl_fixed_to_double (surface_x)
      && test_data->y == wl_fixed_to_double (surface_y))
    test_data->arrived = true;
  else
    report_test_failure ("missed motion event at %g %g",
			 test_data->x, test_data->y);
}

static void
handle_pointer_button (void *data, struct wl_pointer *wl_pointer,
		       uint32_t serial, uint32_t time, uint32_t button,
		       uint32_t state)
{
  /* TODO... */
}

static void
handle_pointer_axis (void *data, struct wl_pointer *wl_pointer,
		     uint32_t time, uint32_t axis, wl_fixed_t value)
{
  /* TODO... */
}

static void
handle_pointer_frame (void *data, struct wl_pointer *wl_pointer)
{
  struct test_expect_data *test_data;

  test_data = get_next_expect_data ();

  if (!test_data)
    {
      test_log ("ignored frame event");
      return;
    }

  if (test_data->kind != POINTER_FRAME_EVENT)
    return;

  test_log ("got frame event");
  test_data->arrived = true;
}

static void
handle_pointer_axis_source (void *data, struct wl_pointer *wl_pointer,
			    uint32_t axis_source)
{
  /* TODO... */
}

static void
handle_pointer_axis_stop (void *data, struct wl_pointer *wl_pointer,
			  uint32_t time, uint32_t axis)
{
  /* TODO... */
}

static void
handle_pointer_axis_discrete (void *data, struct wl_pointer *wl_pointer,
			      uint32_t axis, int32_t discrete)
{
  /* TODO... */
}

static void
handle_pointer_axis_value120 (void *data, struct wl_pointer *wl_pointer,
			      uint32_t axis, int32_t value120)
{
  /* TODO... */
}

static const struct wl_pointer_listener pointer_listener =
  {
    handle_pointer_enter,
    handle_pointer_leave,
    handle_pointer_motion,
    handle_pointer_button,
    handle_pointer_axis,
    handle_pointer_frame,
    handle_pointer_axis_source,
    handle_pointer_axis_stop,
    handle_pointer_axis_discrete,
    handle_pointer_axis_value120,
  };



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

  /* Initialize the pointer listener.  */
  wl_pointer_add_listener (display->seat->pointer, &pointer_listener,
			   NULL);

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

  test_init_seat (display);
  run_test ();
}
