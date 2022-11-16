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
#include "xdg-activation-v1.h"

#include <X11/extensions/XInput2.h>

enum test_kind
  {
    XDG_ACTIVATION_KIND,
  };

static const char *test_names[] =
  {
    "test_activation_kind",
  };

#define LAST_TEST	        XDG_ACTIVATION_KIND
#define TEST_SOURCE_DEVICE	415000

/* The display.  */
static struct test_display *display;

/* The activation interface.  */
static struct xdg_activation_v1 *activation;

/* Test interfaces.  */
static struct test_interface test_interfaces[] =
  {
    { "xdg_activation_v1", &activation, &xdg_activation_v1_interface, 1, },
  };

/* The test surface window.  */
static Window test_surface_window;

/* The test surface and Wayland surface.  */
static struct test_surface *test_surface;
static struct wl_surface *wayland_surface;

/* The the last activation times.  */
static uint32_t last_activation_months;
static uint32_t last_activation_milliseconds;

/* The last activation surface.  */
static struct wl_surface *last_activation_surface;



/* Forward declarations.  */
static void submit_surface_damage (struct wl_surface *, int, int, int, int);
static void wait_for_map (void);



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
handle_xdg_activation_token_done (void *data,
				  struct xdg_activation_token_v1 *token,
				  const char *token_string)
{
  char **token_pointer;

  token_pointer = data;
  *token_pointer = strdup (token_string);
}

static const struct xdg_activation_token_v1_listener activation_token_listener =
  {
    handle_xdg_activation_token_done,
  };



static void
check_activation_with_serial (uint32_t serial, bool expect_success)
{
  struct xdg_activation_token_v1 *token;
  char *token_string;

  /* Set the last user time to 0, 1001.  */
  test_seat_controller_set_last_user_time (display->seat->controller,
					   0, 1001);

  /* Ask for an activation token.  */
  token = xdg_activation_v1_get_activation_token (activation);
  xdg_activation_token_v1_set_serial (token, serial, display->seat->seat);
  xdg_activation_token_v1_set_surface (token, wayland_surface);
  xdg_activation_token_v1_set_app_id (token, "xdg_activation_test");

  token_string = NULL;
  xdg_activation_token_v1_add_listener (token, &activation_token_listener,
					&token_string);
  xdg_activation_token_v1_commit (token);
  wl_display_roundtrip (display->display);

  if (!token_string)
    report_test_failure ("failed to obtain activation token");

  xdg_activation_token_v1_destroy (token);

  /* Now, try to activate the surface.  */
  last_activation_months = 0;
  last_activation_milliseconds = 0;
  xdg_activation_v1_activate (activation, token_string,
			      wayland_surface);
  wl_display_roundtrip (display->display);
  free (token_string);

  if (expect_success)
    {
      if (last_activation_months != 0
	  || last_activation_milliseconds != 1001)
	report_test_failure ("activation failed, wrong time or event not"
			     " received");

      if (last_activation_surface != wayland_surface)
	report_test_failure ("activation succeeded, but the activator"
			     " surface was wrong");
    }
  else if (last_activation_months || last_activation_milliseconds)
    report_test_failure ("activation succeeded unexpectedly");
}

static void
test_single_step (enum test_kind kind)
{
  struct wl_buffer *buffer;
  struct test_XIButtonState *button_state;
  uint32_t serial;

  test_log ("running test step: %s", test_names[kind]);

  switch (kind)
    {
    case XDG_ACTIVATION_KIND:
      buffer = load_png_image (display, "tiny.png");

      if (!buffer)
	report_test_failure ("failed to load tiny.png");

      wl_surface_attach (wayland_surface, buffer, 0, 0);
      submit_surface_damage (wayland_surface, 0, 0, 4, 4);
      wl_surface_commit (wayland_surface);
      wait_for_map ();

      /* First, dispatch a single enter and button press event and get
	 a serial after that event.  */

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

      button_state
	= test_seat_controller_get_XIButtonState (display->seat->controller);

      test_seat_controller_dispatch_XI_ButtonPress (display->seat->controller,
						    test_get_time (),
						    TEST_SOURCE_DEVICE,
						    2,
						    test_get_root (),
						    test_surface_window,
						    None,
						    wl_fixed_from_double (1.0),
						    wl_fixed_from_double (1.0),
						    wl_fixed_from_double (1.0),
						    wl_fixed_from_double (1.0),
						    0,
						    button_state,
						    NULL, NULL, NULL);
      test_XIButtonState_add_button (button_state, 2);

      serial = test_get_serial (display);

      /* Release the buttons.  */
      test_seat_controller_dispatch_XI_ButtonRelease (display->seat->controller,
						      test_get_time (),
						      TEST_SOURCE_DEVICE,
						      2,
						      test_get_root (),
						      test_surface_window,
						      None,
						      wl_fixed_from_double (1.0),
						      wl_fixed_from_double (1.0),
						      wl_fixed_from_double (1.0),
						      wl_fixed_from_double (1.0),
						      0,
						      button_state,
						      NULL, NULL, NULL);
      test_XIButtonState_remove_button (button_state, 2);

      /* Now, set the last user time and try to activate the surface
	 with the given serial.  */
      check_activation_with_serial (serial, true);

      /* Next, get a serial after the button release and try to
	 activate with that.  */
      serial = test_get_serial (display);
      check_activation_with_serial (serial, true);

      /* Finally, click the mouse button again.  Verify that using the
	 previously obtained serial for activation no longer
	 works.  */
      test_seat_controller_dispatch_XI_ButtonPress (display->seat->controller,
						    test_get_time (),
						    TEST_SOURCE_DEVICE,
						    2,
						    test_get_root (),
						    test_surface_window,
						    None,
						    wl_fixed_from_double (1.0),
						    wl_fixed_from_double (1.0),
						    wl_fixed_from_double (1.0),
						    wl_fixed_from_double (1.0),
						    0,
						    button_state,
						    NULL, NULL, NULL);
      test_XIButtonState_add_button (button_state, 2);
      check_activation_with_serial (serial, false);
      break;
    }

  if (kind == LAST_TEST)
    test_complete ();
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
handle_test_surface_mapped (void *data, struct test_surface *test_surface,
			    uint32_t xid, const char *display_string)
{
  test_surface_window = xid;
}

static void
handle_test_surface_activated (void *data, struct test_surface *test_surface,
			       uint32_t months, uint32_t milliseconds,
			       struct wl_surface *activator_surface)
{
  last_activation_months = months;
  last_activation_milliseconds = milliseconds;
  last_activation_surface = activator_surface;
}

static const struct test_surface_listener test_surface_listener =
  {
    handle_test_surface_mapped,
    handle_test_surface_activated,
  };



static void
wait_for_map (void)
{
  while (!test_surface_window)
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
  test_single_step (XDG_ACTIVATION_KIND);

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
