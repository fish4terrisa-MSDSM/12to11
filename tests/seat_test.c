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
#include <linux/input-event-codes.h>

enum test_expect_event_kind
  {
    POINTER_ENTER_EVENT,
    POINTER_FRAME_EVENT,
    POINTER_MOTION_EVENT,
    POINTER_LEAVE_EVENT,
    POINTER_BUTTON_EVENT,
  };

struct test_recorded_event
{
  /* What kind of event this is.  */
  enum test_expect_event_kind kind;

  /* The last event.  */
  struct test_recorded_event *last;
};

struct test_recorded_enter_event
{
  /* The event header.  */
  struct test_recorded_event header;

  /* The surface of the event.  */
  struct wl_surface *surface;

  /* The coordinates of the event.  */
  double x, y;
};

struct test_recorded_frame_event
{
  /* The event header.  */
  struct test_recorded_event header;
};

struct test_recorded_motion_event
{
  /* The event header.  */
  struct test_recorded_event header;

  /* The coordinates of the event.  */
  double x, y;
};

struct test_recorded_leave_event
{
  /* The event header.  */
  struct test_recorded_event header;
};

struct test_recorded_button_event
{
  /* The event header.  */
  struct test_recorded_event header;

  /* The button and state.  */
  uint32_t button, state;
};

enum test_kind
  {
    MAP_WINDOW_KIND,
    TEST_ENTRY_KIND,
    TEST_CLICK_KIND,
  };

static const char *test_names[] =
  {
    "map_window",
    "test_entry",
    "test_click",
  };

#define LAST_TEST	        TEST_CLICK_KIND
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

/* Whether or not events are being recorded.  */
static bool recording_events;

/* If so, the tail of the event list.  */
static struct test_recorded_event *record_tail;



/* Forward declarations.  */
static void submit_surface_damage (struct wl_surface *, int, int, int, int);
static void expect_surface_enter (double, double);
static void expect_surface_motion (double, double);
static void record_events (void);
static void expect_frame_event (void);
static void expect_enter_event (double, double);
static void expect_motion_event (double, double);
static void expect_leave_event (void);
static void expect_button_event (int, int);
static void expect_no_events (void);



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
run_click_test (struct test_XIButtonState *button_state)
{
  /* First, get all events that should already have arrived.  */
  wl_display_roundtrip (display->display);

  /* Next, dispatch the button press events.  */
  test_seat_controller_dispatch_XI_ButtonPress (display->seat->controller,
						test_get_time (),
						TEST_SOURCE_DEVICE,
						1,
						test_get_root (),
						test_surface_window,
						None,
						wl_fixed_from_double (1.0),
						wl_fixed_from_double (2.0),
						wl_fixed_from_double (1.0),
						wl_fixed_from_double (2.0),
						0,
						button_state,
						NULL, NULL, NULL);
  test_XIButtonState_add_button (button_state, 1);

  test_seat_controller_dispatch_XI_ButtonPress (display->seat->controller,
						test_get_time (),
						TEST_SOURCE_DEVICE,
						2,
						test_get_root (),
						test_surface_window,
						None,
						wl_fixed_from_double (1.0),
						wl_fixed_from_double (2.0),
						wl_fixed_from_double (1.0),
						wl_fixed_from_double (2.0),
						0,
						button_state,
						NULL, NULL, NULL);
  test_XIButtonState_add_button (button_state, 2);

  /* Now, dispatch the motion and leave events.  */
  test_seat_controller_dispatch_XI_Motion (display->seat->controller,
					   test_get_time (),
					   TEST_SOURCE_DEVICE,
					   0,
					   test_get_root (),
					   test_surface_window,
					   None,
					   wl_fixed_from_double (550.0),
					   wl_fixed_from_double (550.0),
					   wl_fixed_from_double (550.0),
					   wl_fixed_from_double (550.0),
					   0,
					   button_state,
					   NULL, NULL, NULL);
  test_seat_controller_dispatch_XI_Leave (display->seat->controller,
					  test_get_time (),
					  TEST_SOURCE_DEVICE,
					  XINotifyAncestor,
					  test_get_root (),
					  test_surface_window,
					  None,
					  wl_fixed_from_double (550.0),
					  wl_fixed_from_double (550.0),
					  wl_fixed_from_double (550.0),
					  wl_fixed_from_double (550.0),
					  XINotifyNormal,
					  False, True,
					  button_state, NULL, NULL);

  /* And release the buttons.  */
  test_seat_controller_dispatch_XI_ButtonRelease (display->seat->controller,
						  test_get_time (),
						  TEST_SOURCE_DEVICE,
						  1,
						  test_get_root (),
						  test_surface_window,
						  None,
						  wl_fixed_from_double (550.0),
						  wl_fixed_from_double (550.0),
						  wl_fixed_from_double (550.0),
						  wl_fixed_from_double (550.0),
						  0,
						  button_state,
						  NULL, NULL, NULL);
  test_XIButtonState_remove_button (button_state, 2);

  test_seat_controller_dispatch_XI_ButtonRelease (display->seat->controller,
						  test_get_time (),
						  TEST_SOURCE_DEVICE,
						  2,
						  test_get_root (),
						  test_surface_window,
						  None,
						  wl_fixed_from_double (550.0),
						  wl_fixed_from_double (550.0),
						  wl_fixed_from_double (550.0),
						  wl_fixed_from_double (550.0),
						  0,
						  button_state,
						  NULL, NULL, NULL);
  test_XIButtonState_remove_button (button_state, 1);

  /* Send the ungrab leave event.  */
  test_seat_controller_dispatch_XI_Leave (display->seat->controller,
					  test_get_time (),
					  TEST_SOURCE_DEVICE,
					  XINotifyAncestor,
					  test_get_root (),
					  test_surface_window,
					  None,
					  wl_fixed_from_double (550.0),
					  wl_fixed_from_double (550.0),
					  wl_fixed_from_double (550.0),
					  wl_fixed_from_double (550.0),
					  XINotifyUngrab,
					  False, True,
					  button_state, NULL, NULL);

  /* Now, verify the events that arrive.  */
  record_events ();
  expect_frame_event ();
  expect_leave_event ();
  expect_frame_event ();
  expect_button_event (BTN_MIDDLE, WL_POINTER_BUTTON_STATE_RELEASED);
  expect_frame_event ();
  expect_button_event (BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
  expect_frame_event ();
  expect_motion_event (550.0, 550.0);
  expect_frame_event ();
  expect_button_event (BTN_MIDDLE, WL_POINTER_BUTTON_STATE_PRESSED);
  expect_frame_event ();
  expect_button_event (BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
  expect_no_events ();
}

static void
test_single_step (enum test_kind kind)
{
  struct wl_buffer *buffer;
  struct test_XIButtonState *button_state;

 again:
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

      /* Run the click test.  */
      kind = TEST_CLICK_KIND;
      goto again;

    case TEST_CLICK_KIND:
      /* Test clicking and grab processing.  Press buttons 1 and 2,
	 dispatch a motion event at 550, 550, a NotifyNormal leave
	 event at 550, 550, and finally button release events for both
	 buttons followed by a NotifyUngrab leave event.  Verify that
	 only the following events are sent in the given order:

	   button (SERIAL, TIME, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED)
	   frame ()
	   button (SERIAL, TIME, BTN_MIDDLE, WL_POINTER_BUTTON_STATE_PRESSED)
	   frame ()
	   motion (TIME, 550.0, 550.0)
	   frame ()
	   button (SERIAL, TIME, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED)
	   frame ()
	   button (SERIAL, TIME, BTN_MIDDLE, WL_POINTER_BUTTON_STATE_RELEASED)
	   frame ()
	   leave ()
	   frame ()  */

      /* Initialize the button state.  */
      button_state
	= test_seat_controller_get_XIButtonState (display->seat->controller);

      if (!button_state)
	report_test_failure ("failed to obtain button state resource");

      run_click_test (button_state);
    }

  if (kind == LAST_TEST)
    test_complete ();
}



/* Record events previously sent to the seat's pointer or keyboard
   device, and place them into `record_tail'.  */

static void
record_events (void)
{
  recording_events = true;
  wl_display_roundtrip (display->display);
  recording_events = false;
}

static void
expect_frame_event (void)
{
  struct test_recorded_event *event;

  event = record_tail;

  if (!event)
    report_test_failure ("expected event not sent");

  record_tail = event->last;

  if (event->kind == POINTER_FRAME_EVENT)
    free (event);
  else
    report_test_failure ("a frame event was expected, but not received");
}

static void
expect_enter_event (double x, double y)
{
  struct test_recorded_event *event;
  struct test_recorded_enter_event *enter;

  event = record_tail;

  if (!event)
    report_test_failure ("expected event not sent");

  record_tail = event->last;

  if (event->kind == POINTER_ENTER_EVENT)
    {
      enter = (struct test_recorded_enter_event *) event;

      if (enter->x == x && enter->y == y)
	free (event);
      else
	report_test_failure ("expected enter event received "
			     "with incorrect coordinates");
    }
  else
    report_test_failure ("expected enter event, but it was not received");
}

static void
expect_motion_event (double x, double y)
{
  struct test_recorded_event *event;
  struct test_recorded_motion_event *motion;

  event = record_tail;

  if (!event)
    report_test_failure ("expected event not sent");

  record_tail = event->last;

  if (event->kind == POINTER_MOTION_EVENT)
    {
      motion = (struct test_recorded_motion_event *) event;

      if (motion->x == x && motion->y == y)
	free (event);
      else
	report_test_failure ("expected motion event received "
			     "with incorrect coordinates");
    }
  else
    report_test_failure ("expected motion event, but it was not received");
}

static void
expect_leave_event (void)
{
  struct test_recorded_event *event;

  event = record_tail;

  if (!event)
    report_test_failure ("expected event not sent");

  record_tail = event->last;

  if (event->kind == POINTER_LEAVE_EVENT)
    free (event);
  else
    report_test_failure ("a leave event was expected, but not received");
}

static void
expect_button_event (int button, int state)
{
  struct test_recorded_event *event;
  struct test_recorded_button_event *button_event;

  event = record_tail;

  if (!event)
    report_test_failure ("expected event not sent");

  record_tail = event->last;

  if (event->kind == POINTER_BUTTON_EVENT)
    {
      button_event = (struct test_recorded_button_event *) event;

      if (button_event->button == button && button_event->state == state)
	free (event);
      else
	report_test_failure ("expected button event received "
			     "with incorrect parameters");
    }
  else
    report_test_failure ("expected button event, but it was not received");
}

static void
expect_no_events (void)
{
  if (record_tail)
    report_test_failure ("expected there to be no more events, "
			 "yet some arrived");
}

static void
expect_surface_enter (double x, double y)
{
  /* Record events.  */
  record_events ();

  /* Expect an enter event, followed by a frame event.  */
  expect_frame_event ();
  expect_enter_event (x, y);
  expect_no_events ();
}

static void
expect_surface_motion (double x, double y)
{
  /* Record events.  */
  record_events ();

  /* Expect a motion event followed by a frame event.  */
  expect_frame_event ();
  expect_motion_event (x, y);
  expect_no_events ();
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



static void
handle_pointer_enter (void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, struct wl_surface *surface,
		      wl_fixed_t surface_x, wl_fixed_t surface_y)
{
  struct test_recorded_enter_event *event;

  if (!recording_events)
    {
      test_log ("ignored enter event at %g %g",
		wl_fixed_to_double (surface_x),
		wl_fixed_to_double (surface_y));
      return;
    }

  event = malloc (sizeof *event);

  if (!event)
    report_test_failure ("failed to record event");

  event->header.kind = POINTER_ENTER_EVENT;
  event->header.last = record_tail;
  record_tail = &event->header;

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif

  event->x = wl_fixed_to_double (surface_x);
  event->y = wl_fixed_to_double (surface_x);
  event->surface = surface;

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}

static void
handle_pointer_leave (void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, struct wl_surface *surface)
{
  struct test_recorded_leave_event *event;

  if (!recording_events)
    {
      test_log ("ignored leave event");
      return;
    }

  event = malloc (sizeof *event);

  if (!event)
    report_test_failure ("failed to record event");

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif

  event->header.kind = POINTER_LEAVE_EVENT;
  event->header.last = record_tail;
  record_tail = &event->header;

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}

static void
handle_pointer_motion (void *data, struct wl_pointer *wl_pointer,
		       uint32_t time, wl_fixed_t surface_x,
		       wl_fixed_t surface_y)
{
  struct test_recorded_motion_event *event;

  if (!recording_events)
    {
      test_log ("ignored motion event at %g %g",
		wl_fixed_to_double (surface_x),
		wl_fixed_to_double (surface_y));
      return;
    }

  event = malloc (sizeof *event);

  if (!event)
    report_test_failure ("failed to record event");

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif

  event->header.kind = POINTER_MOTION_EVENT;
  event->header.last = record_tail;
  record_tail = &event->header;

  event->x = wl_fixed_to_double (surface_x);
  event->y = wl_fixed_to_double (surface_y);

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}

static void
handle_pointer_button (void *data, struct wl_pointer *wl_pointer,
		       uint32_t serial, uint32_t time, uint32_t button,
		       uint32_t state)
{
  struct test_recorded_button_event *event;

  if (!recording_events)
    {
      test_log ("ignored button event");
      return;
    }

  event = malloc (sizeof *event);

  if (!event)
    report_test_failure ("failed to record event");

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif

  event->header.kind = POINTER_BUTTON_EVENT;
  event->header.last = record_tail;
  record_tail = &event->header;

  event->button = button;
  event->state = state;

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
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
  struct test_recorded_frame_event *event;

  if (!recording_events)
    {
      test_log ("ignored frame event");
      return;
    }

  event = malloc (sizeof *event);

  if (!event)
    report_test_failure ("failed to record event");

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif

  event->header.kind = POINTER_FRAME_EVENT;
  event->header.last = record_tail;
  record_tail = &event->header;

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
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
