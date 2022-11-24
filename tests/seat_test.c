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
    POINTER_AXIS_VALUE120_EVENT,
    KEYBOARD_ENTER_EVENT,
    KEYBOARD_KEY_EVENT,
    KEYBOARD_MODIFIERS_EVENT,
    SURFACE_RESIZE_FINISHED_EVENT,
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

  /* The serial.  */
  uint32_t serial;
};

struct test_recorded_axis_value120_event
{
  /* The event header.  */
  struct test_recorded_event header;

  /* The axis.  */
  uint32_t axis;

  /* The value120.  */
  int32_t value120;
};

struct test_recorded_keyboard_enter_event
{
  /* The event header.  */
  struct test_recorded_event header;

  /* The event surface.  */
  struct wl_surface *surface;

  /* The keys.  */
  uint32_t *keys;

  /* The number of keys in that array.  */
  size_t num_keys;
};

struct test_recorded_keyboard_key_event
{
  /* The event header.  */
  struct test_recorded_event header;

  /* The key.  */
  uint32_t key;

  /* And the key state.  */
  uint32_t state;
};

struct test_recorded_keyboard_modifiers_event
{
  /* The event header.  */
  struct test_recorded_event header;

  /* The modifiers.  */
  uint32_t base, latched, locked;

  /* The group.  */
  uint32_t group;
};

struct test_recorded_resize_finished_event
{
  /* The event header.  */
  struct test_recorded_event header;
};

struct test_subsurface
{
  /* The subsurface itself.  */
  struct wl_subsurface *subsurface;

  /* The associated surface.  */
  struct wl_surface *surface;
};

enum test_kind
  {
    MAP_WINDOW_KIND,
    TEST_ENTRY_KIND,
    TEST_CLICK_KIND,
    TEST_GRAB_KIND,
    TEST_VALUATOR_KIND,
    TEST_KEY_KIND,
    TEST_RESIZE_KIND,
  };

static const char *test_names[] =
  {
    "map_window",
    "test_entry",
    "test_click",
    "test_grab",
    "test_valuator",
    "test_key",
    "test_resize",
  };

#define LAST_TEST	        TEST_RESIZE_KIND
#define TEST_SOURCE_DEVICE	4500000

/* The display.  */
static struct test_display *display;

/* The subcompositor.  */
static struct wl_subcompositor *subcompositor;

/* Test interfaces.  */
static struct test_interface test_interfaces[] =
  {
    { "wl_subcompositor", &subcompositor, &wl_subcompositor_interface, 1, },
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
static void wait_frame_callback (struct wl_surface *);
static void expect_surface_enter (double, double);
static void expect_surface_motion (double, double);
static void record_events (void);
static void expect_frame_event (void);
static void expect_enter_event (struct wl_surface *, double, double);
static void expect_motion_event (double, double);
static void expect_leave_event (void);
static uint32_t expect_button_event (int, int);
static void expect_axis_value120_event (uint32_t, int32_t);
static void expect_keyboard_enter_event (struct wl_surface *, uint32_t *,
					 size_t);
static void expect_keyboard_key_event (uint32_t, uint32_t);
static void expect_keyboard_modifiers_event (uint32_t, uint32_t,
					     uint32_t, uint32_t);
static void expect_resize_finished_event (void);
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



static struct test_subsurface *
make_test_subsurface (void)
{
  struct test_subsurface *subsurface;

  subsurface = malloc (sizeof *subsurface);

  if (!subsurface)
    goto error_1;

  subsurface->surface
    = wl_compositor_create_surface (display->compositor);

  if (!subsurface->surface)
    goto error_2;

  subsurface->subsurface
    = wl_subcompositor_get_subsurface (subcompositor,
				       subsurface->surface,
				       wayland_surface);

  if (!subsurface->subsurface)
    goto error_3;

  return subsurface;

 error_3:
  wl_surface_destroy (subsurface->surface);
 error_2:
  free (subsurface);
 error_1:
  return NULL;
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
run_grab_test (struct test_XIButtonState *button_state,
	       struct test_subsurface *child)
{
  /* First, get all events that should already have arrived.  */
  wl_display_roundtrip (display->display);

  /* Next, dispatch all of the events.  */
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
  test_seat_controller_dispatch_XI_Motion (display->seat->controller,
					   test_get_time (),
					   TEST_SOURCE_DEVICE,
					   0,
					   test_get_root (),
					   test_surface_window,
					   None,
					   wl_fixed_from_double (0.0),
					   wl_fixed_from_double (0.0),
					   wl_fixed_from_double (0.0),
					   wl_fixed_from_double (0.0),
					   0,
					   button_state,
					   NULL, NULL, NULL);
  test_seat_controller_dispatch_XI_Motion (display->seat->controller,
					   test_get_time (),
					   TEST_SOURCE_DEVICE,
					   0,
					   test_get_root (),
					   test_surface_window,
					   None,
					   wl_fixed_from_double (150.0),
					   wl_fixed_from_double (150.0),
					   wl_fixed_from_double (150.0),
					   wl_fixed_from_double (150.0),
					   0,
					   button_state,
					   NULL, NULL, NULL);
  test_seat_controller_dispatch_XI_ButtonPress (display->seat->controller,
						test_get_time (),
						TEST_SOURCE_DEVICE,
						1,
						test_get_root (),
						test_surface_window,
						None,
						wl_fixed_from_double (150.0),
						wl_fixed_from_double (150.0),
						wl_fixed_from_double (150.0),
						wl_fixed_from_double (150.0),
						0,
						button_state,
						NULL, NULL, NULL);
  test_XIButtonState_add_button (button_state, 1);
  test_seat_controller_dispatch_XI_Motion (display->seat->controller,
					   test_get_time (),
					   TEST_SOURCE_DEVICE,
					   0,
					   test_get_root (),
					   test_surface_window,
					   None,
					   wl_fixed_from_double (95.0),
					   wl_fixed_from_double (95.0),
					   wl_fixed_from_double (95.0),
					   wl_fixed_from_double (95.0),
					   0,
					   button_state,
					   NULL, NULL, NULL);
  test_seat_controller_dispatch_XI_ButtonRelease (display->seat->controller,
						  test_get_time (),
						  TEST_SOURCE_DEVICE,
						  1,
						  test_get_root (),
						  test_surface_window,
						  None,
						  wl_fixed_from_double (95.0),
						  wl_fixed_from_double (90.0),
						  wl_fixed_from_double (95.0),
						  wl_fixed_from_double (95.0),
						  0,
						  button_state,
						  NULL, NULL, NULL);
  test_XIButtonState_remove_button (button_state, 1);

  /* Now, verify the events that arrive.  */
  record_events ();
  expect_frame_event ();
  expect_enter_event (wayland_surface, 95.0, 95.0);
  expect_frame_event ();
  expect_leave_event ();
  expect_frame_event ();
  expect_button_event (BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
  expect_frame_event ();
  expect_motion_event (-5.0, -5.0);
  expect_frame_event ();
  expect_button_event (BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
  expect_frame_event ();
  expect_motion_event (50.0, 50.0);
  expect_frame_event ();
  expect_enter_event (child->surface, 50.0, 50.0);
  expect_frame_event ();
  expect_leave_event ();
  expect_frame_event ();
  expect_motion_event (0.0, 0.0);
  expect_frame_event ();
  expect_enter_event (wayland_surface, 0.0, 0.0);
}

static void
run_valuator_test (void)
{
  struct test_XIDeviceInfo *info;
  struct test_XIValuatorState *valuator_state;

  /* First, create the device info.  */
  info
    = test_device_controller_get_device_info (display->seat->device_controller);

  /* Send the first leave event.  */
  test_seat_controller_dispatch_XI_Leave (display->seat->controller,
					  test_get_time (),
					  TEST_SOURCE_DEVICE,
					  XINotifyAncestor,
					  test_get_root (),
					  test_surface_window,
					  None,
					  wl_fixed_from_double (-1.0),
					  wl_fixed_from_double (-1.0),
					  wl_fixed_from_double (-1.0),
					  wl_fixed_from_double (-1.0),
					  XINotifyNormal,
					  False, True,
					  NULL, NULL, NULL);

  /* Set the device ID and add the valuators.  */
  test_XIDeviceInfo_set_device_id (info, display->seat->device_id);
  test_XIDeviceInfo_set_use (info, XIMasterPointer);
  test_XIDeviceInfo_set_attachment (info, display->seat->controller);
  test_XIDeviceInfo_set_name (info, "Test virtual pointer");
  test_XIDeviceInfo_set_enabled (info, 1);
  test_XIDeviceInfo_add_XIScrollClassInfo (info,
					   TEST_SOURCE_DEVICE,
					   1,
					   XIScrollTypeVertical,
					   wl_fixed_from_double (1.0),
					   XIScrollFlagPreferred);
  test_XIDeviceInfo_add_XIScrollClassInfo (info,
					   TEST_SOURCE_DEVICE,
					   2,
					   XIScrollTypeHorizontal,
					   wl_fixed_from_double (2.0),
					   XIScrollFlagPreferred);
  test_XIDeviceInfo_add_XIValuatorClassInfo (info,
					     TEST_SOURCE_DEVICE,
					     1,
					     "Rel Scroll Vertical",
					     wl_fixed_from_double (0.0),
					     wl_fixed_from_double (0.0),
					     wl_fixed_from_double (0.0),
					     1,
					     XIModeRelative);
  test_XIDeviceInfo_add_XIValuatorClassInfo (info,
					     TEST_SOURCE_DEVICE,
					     2,
					     "Rel Scroll Horizontal",
					     wl_fixed_from_double (0.0),
					     wl_fixed_from_double (0.0),
					     wl_fixed_from_double (0.0),
					     1,
					     XIModeRelative);
  test_device_controller_add_device_info (display->seat->device_controller,
					  info);
  test_XIDeviceInfo_destroy (info);

  /* Dispatch the first entry event.  */
  test_seat_controller_dispatch_XI_Enter (display->seat->controller,
					  test_get_time (),
					  TEST_SOURCE_DEVICE,
					  XINotifyAncestor,
					  test_get_root (),
					  test_surface_window,
					  None,
					  wl_fixed_from_double (1.0),
					  wl_fixed_from_double (1.0),
					  wl_fixed_from_double (1.0),
					  wl_fixed_from_double (1.0),
					  XINotifyNormal,
					  False, True, NULL, NULL,
					  NULL);

  /* Create the valuator state.  */
  valuator_state
    = test_seat_controller_get_XIValuatorState (display->seat->controller);

  if (!valuator_state)
    report_test_failure ("failed to create valuator state");

  test_XIValuatorState_add_valuator (valuator_state, 1,
				     wl_fixed_from_double (1.0));
  test_XIValuatorState_add_valuator (valuator_state, 2,
				     wl_fixed_from_double (1.0));

  /* Dispatch the first motion event.  */
  test_seat_controller_dispatch_XI_Motion (display->seat->controller,
					   test_get_time (),
					   TEST_SOURCE_DEVICE,
					   0,
					   test_get_root (),
					   test_surface_window,
					   None,
					   wl_fixed_from_double (2.0),
					   wl_fixed_from_double (2.0),
					   wl_fixed_from_double (2.0),
					   wl_fixed_from_double (2.0),
					   0,
					   NULL,
					   valuator_state,
					   NULL, NULL);
  test_XIValuatorState_destroy (valuator_state);

  /* Dispatch the second motion event.  */
  valuator_state
    = test_seat_controller_get_XIValuatorState (display->seat->controller);

  if (!valuator_state)
    report_test_failure ("failed to create valuator state");

  test_XIValuatorState_add_valuator (valuator_state, 1,
				     wl_fixed_from_double (1.1));
  test_XIValuatorState_add_valuator (valuator_state, 2,
				     wl_fixed_from_double (2.6));
  test_seat_controller_dispatch_XI_Motion (display->seat->controller,
					   test_get_time (),
					   TEST_SOURCE_DEVICE,
					   0,
					   test_get_root (),
					   test_surface_window,
					   None,
					   wl_fixed_from_double (2.0),
					   wl_fixed_from_double (2.0),
					   wl_fixed_from_double (2.0),
					   wl_fixed_from_double (2.0),
					   0,
					   NULL,
					   valuator_state,
					   NULL, NULL);
  test_XIValuatorState_destroy (valuator_state);

  /* Now, verify the events that arrive.  */
  record_events ();
  expect_frame_event ();
  expect_axis_value120_event (WL_POINTER_AXIS_VERTICAL_SCROLL, 12);
  expect_axis_value120_event (WL_POINTER_AXIS_HORIZONTAL_SCROLL, 96);
  expect_frame_event ();
  expect_motion_event (2.0, 2.0);
  expect_frame_event ();
  expect_enter_event (wayland_surface, 1.0, 1.0);
  expect_frame_event ();
  expect_leave_event ();
}

static void
run_key_test (void)
{
  test_seat_controller_dispatch_XI_FocusIn (display->seat->controller,
					    test_get_time (),
					    TEST_SOURCE_DEVICE,
					    XINotifyAncestor,
					    test_get_root (),
					    test_surface_window,
					    None,
					    wl_fixed_from_double (2.0),
					    wl_fixed_from_double (2.0),
					    wl_fixed_from_double (2.0),
					    wl_fixed_from_double (2.0),
					    XINotifyNonlinear,
					    0,
					    1,
					    NULL, NULL, NULL);
  test_seat_controller_dispatch_XI_RawKeyPress (display->seat->controller,
						test_get_time (),
						TEST_SOURCE_DEVICE,
						67,
						0,
						NULL);
  test_seat_controller_dispatch_XI_KeyPress (display->seat->controller,
					     test_get_time (),
					     TEST_SOURCE_DEVICE,
					     67,
					     test_get_root (),
					     test_surface_window,
					     None,
					     wl_fixed_from_double (2.0),
					     wl_fixed_from_double (2.0),
					     wl_fixed_from_double (2.0),
					     wl_fixed_from_double (2.0),
					     0,
					     NULL, NULL, NULL, NULL);
  test_seat_controller_dispatch_XI_RawKeyRelease (display->seat->controller,
						  test_get_time (),
						  TEST_SOURCE_DEVICE,
						  67,
						  0,
						  NULL);
  test_seat_controller_dispatch_XI_KeyRelease (display->seat->controller,
					       test_get_time (),
					       TEST_SOURCE_DEVICE,
					       67,
					       test_get_root (),
					       test_surface_window,
					       None,
					       wl_fixed_from_double (2.0),
					       wl_fixed_from_double (2.0),
					       wl_fixed_from_double (2.0),
					       wl_fixed_from_double (2.0),
					       0,
					       NULL, NULL, NULL, NULL);

  /* Now, verify the events as they arrive.  */
  record_events ();
  expect_keyboard_key_event (59, WL_KEYBOARD_KEY_STATE_RELEASED);
  expect_keyboard_key_event (59, WL_KEYBOARD_KEY_STATE_PRESSED);
  expect_keyboard_modifiers_event (0, 0, 0, 0);
  expect_keyboard_enter_event (wayland_surface, NULL, 0);
}

static void
run_resize_test (struct test_XIButtonState *button_state)
{
  uint32_t serial;

  test_seat_controller_dispatch_XI_ButtonPress (display->seat->controller,
						test_get_time (),
						TEST_SOURCE_DEVICE,
						1,
						test_get_root (),
						test_surface_window,
						None,
						wl_fixed_from_double (2.0),
						wl_fixed_from_double (2.0),
						wl_fixed_from_double (2.0),
						wl_fixed_from_double (2.0),
						0,
						button_state,
						NULL, NULL, NULL);
  test_XIButtonState_add_button (button_state, 1);
  record_events ();

  /* Obtain the serial.  */
  expect_frame_event ();
  serial = expect_button_event (BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
  expect_no_events ();

  /* Now, start resize.  */
  test_surface_move_resize (test_surface, TEST_MANAGER_RESIZE_EDGE_MOVE,
			    serial, display->seat->seat);

  /* And dispatch the button release.  */
  test_seat_controller_dispatch_XI_ButtonRelease (display->seat->controller,
						  test_get_time (),
						  TEST_SOURCE_DEVICE,
						  1,
						  test_get_root (),
						  test_surface_window,
						  None,
						  wl_fixed_from_double (2.0),
						  wl_fixed_from_double (2.0),
						  wl_fixed_from_double (2.0),
						  wl_fixed_from_double (2.0),
						  0,
						  button_state,
						  NULL, NULL, NULL);
  test_XIButtonState_remove_button (button_state, 1);

  record_events ();
  expect_frame_event ();
  expect_enter_event (wayland_surface, 2.0, 2.0);
  expect_resize_finished_event ();
  expect_frame_event ();
  expect_leave_event ();
}

static void
test_single_step (enum test_kind kind)
{
  struct wl_buffer *buffer, *child_buffer;
  struct test_XIButtonState *button_state;
  struct test_subsurface *child;

  button_state = NULL;

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
      kind = TEST_GRAB_KIND;
      goto again;

    case TEST_GRAB_KIND:
      /* Test subsurface grabbing.  Create a 100x100 child of the
	 parent surface, and move it to 100, 100.  Then, enter the
	 parent at 0, 0, and dispatch a motion event there.  Dispatch
	 a motion event to 150, 150 (inside the child).  Press button
	 1.  Dispatch a motion event to 95, 95.  Finally, release
	 button 1.  Verify that only the following events are sent in
	 the given order:

           enter (SERIAL, PARENT, 0.0, 0.0)
           frame ()
	   motion (TIME, 0.0, 0.0)
	   frame ()
	   leave ()
	   frame ()
	   enter (SERIAL, CHILD, 50.0, 50.0)
	   frame ()
	   motion (TIME, 50.0, 50.0)
	   frame ()
           button (SERIAL, TIME, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED)
           frame ()
	   motion (TIME, -5.0, -5.0)
	   frame ()
	   button (SERIAL, TIME, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELASED)
	   frame ()
	   leave ()
	   frame ()
	   enter (SERIAL, PARENT, 95.0, 95.0)
	   frame ()  */

      child = make_test_subsurface ();

      if (!child)
	report_test_failure ("failed to create test subsurface");

      child_buffer = load_png_image (display, "seat_child.png");

      if (!child_buffer)
	report_test_failure ("failed to load seat_child.png");

      wl_surface_attach (child->surface, child_buffer, 0, 0);
      wl_surface_commit (child->surface);
      wl_subsurface_set_position (child->subsurface, 100, 100);

      /* wait_frame_callback is necessary because input regions are
	 not updated until the update completes.  */
      wait_frame_callback (wayland_surface);

      /* Run the test.  */
      run_grab_test (button_state, child);
      kind = TEST_VALUATOR_KIND;
      goto again;

    case TEST_VALUATOR_KIND:
      /* Dispatch a leave event at -1, -1, and then attach the following
	 scroll valuator information to the seat:

	   type
	     ScrollClass
	   number
	     1
	   scroll_type
	     Vertical
	   flags
	     Preferred
	   increment
	     1.0

	   type
	     ScrollClass
	   number
	     2
	   scroll_type
	     Horizontal
	   flags
	     Preferred
	   increment
	     2.0

	   type
	     ValuatorClass
	   sourceid
	     TEST_SOURCE_DEVICE
	   number
	     1
	   label
	     Rel Scroll Vertical
	   min
	     0.0
	   max
	     0.0
	   resolution
	     1
	   mode
	     Relative
	   value
	     0.0

	   type
	     ValuatorClass
	   sourceid
	     TEST_SOURCE_DEVICE
	   number
	     2
	   label
	     Rel Scroll Horizontal
	   min
	     0.0
	   max
	     0.0
	   resolution
	     1
	   mode
	     Relative
	   value
	     0.0

       then, dispatch an entry event at 1, 1, followed by two motion
       events at 2, 2 with the following valuators:

         1, 1.0, 2, 1.0
	 1, 1.1, 2, 2.6

       verify that the following events arrive in the specified order:

       leave ()
       frame ()
       enter (SERIAL, PARENT, 1.0, 1.0)
       frame ()       

       motion (TIME, 2.0, 2.0) (this motion event should arrive because
                                the entry event happened after
				the scroll valuator information was
				recorded.  The first motion event to
				arrive after that should be used to obtain
				the current value of the valuator, and
				not for calculating scroll deltas.)
       frame ();
       axis_value120 (WL_POINTER_AXIS_HORIZONTAL_SCROLL, 96)
       axis_value120 (WL_POINTER_AXIS_VERTICAL_SCROLL, 12)
       frame ();  */

      run_valuator_test ();
      kind = TEST_KEY_KIND;
      goto again;

    case TEST_KEY_KIND:
      /* Test simple key press and key release.  Dispatch an
	 XI_FocusIn event to the test surface.  Then, press and
	 release the keycode 67, generating both raw and device
	 events.  Verify that the following events are sent to the
	 keyboard.

           enter (SERIAL, SURFACE, array[0])
	   modifiers (SERIAL, 0, 0, 0, 0)
           key (SERIAL, TIME, 59, WL_KEYBOARD_KEY_STATE_PRESSED)
	   key (SERIAL, TIME, 59, WL_KEYBOARD_KEY_STATE_RELEASED) */

      run_key_test ();
      kind = TEST_RESIZE_KIND;
      goto again;

    case TEST_RESIZE_KIND:
      /* Test simple resize (or rather movement).  Dispatch a
	 ButtonPress event, obtain the serial of said event, start
	 resize, and dispatch a ButtonRelease event.  Verify that the
	 following events are sent to the pointer and surface.

           button (SERIAL, TIME, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED)
           frame ()
           leave ()
           resize_finished ()
	   frame ();
	   enter (SERIAL, SURFACE, 2.0, 2.0)
	   frame (); */

      run_resize_test (button_state);
      break;
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
expect_enter_event (struct wl_surface *surface, double x, double y)
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

      if (enter->x == x && enter->y == y
	  && (!surface || (surface == enter->surface)))
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

static uint32_t
expect_button_event (int button, int state)
{
  struct test_recorded_event *event;
  struct test_recorded_button_event *button_event;
  uint32_t serial;

  event = record_tail;

  if (!event)
    report_test_failure ("expected event not sent");

  record_tail = event->last;

  if (event->kind == POINTER_BUTTON_EVENT)
    {
      button_event = (struct test_recorded_button_event *) event;

      if (button_event->button == button && button_event->state == state)
	{
	  serial = button_event->serial;
	  free (event);

	  return serial;
	}
      else
	report_test_failure ("expected button event received "
			     "with incorrect parameters");
    }
  else
    report_test_failure ("expected button event, but it was not received");
}

static void
expect_axis_value120_event (uint32_t axis, int32_t value120)
{
  struct test_recorded_event *event;
  struct test_recorded_axis_value120_event *axis_value120_event;

  event = record_tail;

  if (!event)
    report_test_failure ("expected event not sent");

  record_tail = event->last;

  if (event->kind == POINTER_AXIS_VALUE120_EVENT)
    {
      axis_value120_event
	= (struct test_recorded_axis_value120_event *) event;

      if (axis_value120_event->axis == axis
	  && axis_value120_event->value120 == value120)
	free (event);
      else
	report_test_failure ("expected axis_value120 event received "
			     "with incorrect parameters (axis: %"PRIu32","
			     " value120: %"PRIi32")", axis, value120);
    }
  else
    report_test_failure ("expected axis_value120 event, but it was not "
			 "received");
}

static void
expect_keyboard_enter_event (struct wl_surface *surface, uint32_t *keys,
			     size_t num_keys)
{
  struct test_recorded_event *event;
  struct test_recorded_keyboard_enter_event *keyboard_enter_event;

  event = record_tail;

  if (!event)
    report_test_failure ("expected event not sent");

  record_tail = record_tail->last;

  if (event->kind == KEYBOARD_ENTER_EVENT)
    {
      keyboard_enter_event
	= (struct test_recorded_keyboard_enter_event *) event;

      if (keyboard_enter_event->num_keys != num_keys
	  || surface != keyboard_enter_event->surface
	  || memcmp (keyboard_enter_event->keys, keys,
		     num_keys * sizeof *keys))
	report_test_failure ("expected keyboard_enter event passed"
			     " with invalid parameters");
      else
	{
	  free (keyboard_enter_event->keys);
	  free (event);
	}
    }
  else
    report_test_failure ("expected keyboard_enter_event, but it was"
			 " not received");
}

static void
expect_keyboard_key_event (uint32_t key, uint32_t state)
{
  struct test_recorded_event *event;
  struct test_recorded_keyboard_key_event *keyboard_key_event;

  event = record_tail;

  if (!event)
    report_test_failure ("expected event not sent");

  record_tail = record_tail->last;

  if (event->kind == KEYBOARD_KEY_EVENT)
    {
      keyboard_key_event
	= (struct test_recorded_keyboard_key_event *) event;

      if (key != keyboard_key_event->key
	  || state != keyboard_key_event->state)
	report_test_failure ("expected keyboard_key passed with"
			     " invalid parameters");
      else
	free (event);
    }
  else
    report_test_failure ("expected keyboard_key_event, but it was"
			 " not received");
}

static void
expect_keyboard_modifiers_event (uint32_t base, uint32_t latched,
				 uint32_t locked, uint32_t group)
{
  struct test_recorded_event *event;
  struct test_recorded_keyboard_modifiers_event *keyboard_modifiers_event;

  event = record_tail;

  if (!event)
    return;

  record_tail = record_tail->last;

  if (event->kind == KEYBOARD_MODIFIERS_EVENT)
    {
      keyboard_modifiers_event
	= (struct test_recorded_keyboard_modifiers_event *) event;

      if (keyboard_modifiers_event->base != base
	  || keyboard_modifiers_event->latched != latched
	  || keyboard_modifiers_event->locked != locked
	  || keyboard_modifiers_event->group != group)
	report_test_failure ("expected keyboard_modifiers passed with"
			     " invalid parameters");
      else
	free (event);
    }
  else
    report_test_failure ("expected keyboard_modifiers_event, but it was"
			 " not received");
}

static void
expect_no_events (void)
{
  if (record_tail)
    report_test_failure ("expected there to be no more events, "
			 "yet some arrived");
}

static void
expect_resize_finished_event (void)
{
  struct test_recorded_event *event;

  event = record_tail;

  if (!event)
    return;

  record_tail = record_tail->last;

  if (event->kind == SURFACE_RESIZE_FINISHED_EVENT)
    free (event);
  else
    report_test_failure ("expected resize_finished_event, but it was"
			 " not received");
}

static void
expect_surface_enter (double x, double y)
{
  /* Record events.  */
  record_events ();

  /* Expect an enter event, followed by a frame event.  */
  expect_frame_event ();
  expect_enter_event (NULL, x, y);
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

static void
handle_test_surface_committed (void *data, struct test_surface *surface,
			       uint32_t presentation_hint)
{

}

static void
handle_test_surface_resize_finished (void *data, struct test_surface *surface)
{
  struct test_recorded_resize_finished_event *event;

  if (!recording_events)
    {
      test_log ("ignored resize finish event");
      return;
    }

  event = malloc (sizeof *event);

  if (!event)
    report_test_failure ("failed to record event");

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif

  event->header.kind = SURFACE_RESIZE_FINISHED_EVENT;
  event->header.last = record_tail;
  record_tail = &event->header;

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}

static const struct test_surface_listener test_surface_listener =
  {
    handle_test_surface_mapped,
    NULL,
    handle_test_surface_committed,
    handle_test_surface_resize_finished,
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
  event->serial = serial;

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
  struct test_recorded_axis_value120_event *event;

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

  event->header.kind = POINTER_AXIS_VALUE120_EVENT;
  event->header.last = record_tail;
  record_tail = &event->header;

  event->axis = axis;
  event->value120 = value120;

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
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
handle_keyboard_keymap (void *data, struct wl_keyboard *keyboard,
			uint32_t format, int32_t fd, uint32_t size)
{
  close (fd);
}

static void
handle_keyboard_enter (void *data, struct wl_keyboard *keyboard,
		       uint32_t serial, struct wl_surface *surface,
		       struct wl_array *keys)
{
  struct test_recorded_keyboard_enter_event *event;

  if (!recording_events)
    {
      test_log ("ignored keyboard enter event");
      return;
    }

  event = malloc (sizeof *event);

  if (!event)
    report_test_failure ("failed to record event");

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif

  event->header.kind = KEYBOARD_ENTER_EVENT;
  event->header.last = record_tail;
  record_tail = &event->header;

  if (keys->size % sizeof (uint32_t))
    report_test_failure ("reported key size modulo"
			 " uint32_t!");

  event->keys = malloc (keys->size);

  if (!event->keys)
    report_test_failure ("failed to allocate key array");

  memcpy (event->keys, keys->data, keys->size);
  event->num_keys = keys->size / sizeof (uint32_t);
  event->surface = surface;

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}

static void
handle_keyboard_leave (void *data, struct wl_keyboard *keyboard,
		       uint32_t serial, struct wl_surface *surface)
{

}

static void
handle_keyboard_key (void *data, struct wl_keyboard *keyboard,
		     uint32_t serial, uint32_t time, uint32_t key,
		     uint32_t state)
{
  struct test_recorded_keyboard_key_event *event;

  if (!recording_events)
    {
      test_log ("ignored keyboard key event");
      return;
    }

  event = malloc (sizeof *event);

  if (!event)
    report_test_failure ("failed to record event");

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif

  event->header.kind = KEYBOARD_KEY_EVENT;
  event->header.last = record_tail;
  record_tail = &event->header;

  event->key = key;
  event->state = state;

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}

static void
handle_keyboard_modifiers (void *data, struct wl_keyboard *keyboard,
			   uint32_t serial, uint32_t mods_depressed,
			   uint32_t mods_latched, uint32_t mods_locked,
			   uint32_t group)
{
  struct test_recorded_keyboard_modifiers_event *event;

  if (!recording_events)
    {
      test_log ("ignored modifiers event");
      return;
    }

  event = malloc (sizeof *event);

  if (!event)
    report_test_failure ("failed to record modifiers event");

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif

  event->header.kind = KEYBOARD_MODIFIERS_EVENT;
  event->header.last = record_tail;
  record_tail = &event->header;

  event->base = mods_depressed;
  event->latched = mods_latched;
  event->locked = mods_locked;
  event->group = group;

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}

static void
handle_keyboard_repeat_info (void *data, struct wl_keyboard *keyboard,
			     int32_t rate, int32_t delay)
{
  
}

static const struct wl_keyboard_listener keyboard_listener =
  {
    handle_keyboard_keymap,
    handle_keyboard_enter,
    handle_keyboard_leave,
    handle_keyboard_key,
    handle_keyboard_modifiers,
    handle_keyboard_repeat_info,
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

  /* Initialize the pointer listener.  */
  wl_pointer_add_listener (display->seat->pointer, &pointer_listener,
			   NULL);

  /* And the keyboard listener.  */
  wl_keyboard_add_listener (display->seat->keyboard, &keyboard_listener,
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
