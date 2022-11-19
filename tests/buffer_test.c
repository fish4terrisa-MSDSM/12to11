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

/* Tests for buffer release.  */

enum test_kind
  {
    BUFFER_RELEASE_KIND,
    BUFFER_DESTROY_KIND,
  };

static const char *test_names[] =
  {
    "buffer_release",
    "buffer_destroy",
  };

#define LAST_TEST	BUFFER_DESTROY_KIND

/* The display.  */
static struct test_display *display;

/* Test interfaces.  */
static struct test_interface test_interfaces[] =
  {
    /* No interfaces yet... */
  };

/* The test surface and Wayland surface.  */
static struct test_surface *test_surface;
static struct wl_surface *wayland_surface;



/* Forward declarations.  */
static void wait_frame_callback (struct wl_surface *);



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
  struct test_buffer *buffers[1000];
  int i;

 again:

  test_log ("running test step: %s", test_names[kind]);

  switch (kind)
    {
    case BUFFER_RELEASE_KIND:
      /* Abuse the buffer release machinery.  Repeatedly commit 1000
	 buffers.  Then, wait for a frame callback on the 1000th
	 commit.

	 Verify that the first 999 have been released.  */

      for (i = 0; i < ARRAYELTS (buffers) - 1; ++i)
	{
	  buffers[i] = make_test_buffer ();
	  wl_surface_attach (wayland_surface, buffers[i]->buffer,
			     0, 0);
	  test_buffer_committed (buffers[i]);
	  wl_surface_commit (wayland_surface);
	}

      buffers[i] = make_test_buffer ();
      wl_surface_attach (wayland_surface, buffers[i]->buffer,
			 0, 0);
      test_buffer_committed (buffers[i]);
      wait_frame_callback (wayland_surface);

      for (i = 0; i < ARRAYELTS (buffers) - 1; ++i)
	verify_buffer_released (buffers[i]);

      kind = BUFFER_DESTROY_KIND;
      goto again;

    case BUFFER_DESTROY_KIND:
      /* Now do the same thing, but destroy every other wl_buffer.  */

      for (i = 0; i < ARRAYELTS (buffers) - 1; ++i)
	{
	  wl_surface_attach (wayland_surface, buffers[i]->buffer,
			     0, 0);
	  test_buffer_committed (buffers[i]);
	  wl_surface_commit (wayland_surface);

	  if (i % 2)
	    wl_buffer_destroy (buffers[i]->buffer);

	  /* buffers[i] is intentionally "leaked".  */
	  buffers[i] = NULL;
	}

      wl_surface_attach (wayland_surface, buffers[i]->buffer,
			 0, 0);
      test_buffer_committed (buffers[i]);
      wait_frame_callback (wayland_surface);

      for (i = 0; i < ARRAYELTS (buffers) - 1; ++i)
	{
	  if (buffers[i])
	    verify_buffer_released (buffers[i]);
	}
    }

  if (kind == LAST_TEST)
    test_complete ();
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

  test_single_step (BUFFER_RELEASE_KIND);

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
