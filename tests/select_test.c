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

#include <pthread.h>
#include <sys/wait.h>

#include "test_harness.h"

#include <X11/Xatom.h>

enum test_kind
  {
    SELECT_STRING_KIND,
  };

static const char *test_names[] =
  {
    "select_string",
  };

#define LAST_TEST	        SELECT_STRING_KIND

/* The display.  */
static struct test_display *display;

/* The data device manager.  */
static struct wl_data_device_manager *data_device_manager;

/* Test interfaces.  */
static struct test_interface test_interfaces[] =
  {
    { "wl_data_device_manager", &data_device_manager,
      &wl_data_device_manager_interface, 3, },
  };

/* The data device.  */
static struct wl_data_device *data_device;

/* Whether or not the data source's send listener was called.  */
static bool send_called;

/* Sample text used.  */

#define SAMPLE_TEXT							\
  ("Lorem ipsum dolor sit amet, consectetur adipiscing elit"		\
   ", sed do eiusmod tempor incididunt ut labore et dolore"		\
   " magna aliqua. Ut enim ad minim veniam, quis nostrud"		\
   " exercitation ullamco laboris nisi ut aliquip ex ea commodo"	\
   " consequat.  Duis aute irure dolor in reprehenderit in"		\
   " voluptate velit esse cillum dolore eu fugiat nulla pariatur."	\
   "  Excepteur sint occaecat cupidatat non proident, sunt in"		\
   " culpa qui officia deserunt mollit anim id est laborum.")



static Bool
test_get_time_1 (Display *display, XEvent *event, XPointer arg)
{
  Atom *atom;

  atom = (Atom *) arg;

  if (event->type == PropertyNotify
      && event->xproperty.atom == *atom)
    return True;

  return False;
}

/* Get a timestamp suitable for use in events dispatched to the test
   seat.  */

static Time
test_get_time (void)
{
  Atom property_atom;
  XEvent event;
  Window window;
  unsigned char unused;
  XSetWindowAttributes attrs;

  attrs.event_mask = PropertyChangeMask;
  window = XCreateWindow (display->x_display,
			  DefaultRootWindow (display->x_display),
			  0, 0, 1, 1, 0, 0, InputOnly, CopyFromParent,
			  CWEventMask, &attrs);
  unused = '\0';
  property_atom = XInternAtom (display->x_display,
			       "_INTERNAL_SERVER_TIME_PROP",
			       False);
  XChangeProperty (display->x_display, window, property_atom,
		   XA_CARDINAL, 8, PropModeReplace, &unused, 1);
  XIfEvent (display->x_display, &event, test_get_time_1,
	    (XPointer) &property_atom);
  XDestroyWindow (display->x_display, window);
  return event.xproperty.time;
}



static void
handle_data_source_target (void *data, struct wl_data_source *data_source,
			   const char *mime_type)
{
  /* Nothing to do here.  */
}

static void *
handle_data_source_send_1 (void *data)
{
  int fd;

  fd = (intptr_t) data;

  write (fd, SAMPLE_TEXT, sizeof SAMPLE_TEXT - 1);
  close (fd);

  return NULL;
}

static void
handle_data_source_send (void *data, struct wl_data_source *data_source,
			 const char *mime_type, int fd)
{
  pthread_t thread;

  send_called = true;

  if (pthread_create (&thread, NULL, handle_data_source_send_1,
		      (void *) (intptr_t) fd))
    die ("pthread_create");
}

static void
handle_data_source_cancelled (void *data, struct wl_data_source *data_source)
{
  report_test_failure ("data source cancelled");
}

static const struct wl_data_source_listener data_source_listener =
  {
    handle_data_source_target,
    handle_data_source_send,
    handle_data_source_cancelled,
  };



static void
own_sample_text (void)
{
  struct wl_data_source *source;
  uint32_t display_serial;

  source = wl_data_device_manager_create_data_source (data_device_manager);
  display_serial = test_get_serial (display);

  if (!source)
    report_test_failure ("failed to create data source");

  wl_data_source_offer (source, "text/plain");
  wl_data_source_offer (source, "text/plain;charset=utf-8");
  wl_data_source_add_listener (source, &data_source_listener, NULL);
  wl_data_device_set_selection (data_device, source, display_serial);
}

static void
verify_sample_text (Time time)
{
  int pipefds[2], wstatus;
  pid_t pid;
  char *display_string;
  char time_buffer[45], buffer[sizeof SAMPLE_TEXT];
  ssize_t bytes_read;
  const char *sample_text_buffer;

  /* Run select_helper with the specified timestamp.  Wait until
     handle_data_source_send is called, and then begin reading from
     the pipe.  */

  if (pipe (pipefds) < 0)
    die ("pipe");

  display_string = DisplayString (display->x_display);
  sprintf (time_buffer, "%lu", time);
  pid = fork ();

  if (pid == -1)
    die ("fork");
  else if (!pid)
    {
      close (pipefds[0]);

      if (!dup2 (pipefds[1], 1))
	exit (1);

      execlp ("./select_helper", "./select_helper",
	      display_string, time_buffer, "STRING",
	      NULL);
      exit (1);
    }

  send_called = false;

  while (!send_called)
    {
      if (wl_display_dispatch (display->display) == -1)
        die ("wl_display_dispatch");
    }

  /* Now, start reading from the pipe and comparing the contents.  */
  sample_text_buffer = SAMPLE_TEXT;
  bytes_read = read (pipefds[0], buffer, sizeof SAMPLE_TEXT - 1);

  if (bytes_read != sizeof SAMPLE_TEXT - 1)
    report_test_failure ("wanted %zu bytes, but got %zd",
			 sizeof SAMPLE_TEXT - 1, bytes_read);

  waitpid (pid, &wstatus, 0);

  if (WEXITSTATUS (wstatus))
    report_test_failure ("child exited with failure: %d",
			 WEXITSTATUS (wstatus));

  /* Now compare the text.  */
  if (memcmp (buffer, sample_text_buffer, bytes_read))
    report_test_failure ("read text differs from sample text!");

  close (pipefds[0]);
  close (pipefds[1]);
}

static void
verify_sample_text_multiple (Time time)
{
  int pipefds[2], wstatus;
  pid_t pid;
  char *display_string;
  char time_buffer[45], buffer[sizeof SAMPLE_TEXT];
  ssize_t bytes_read;
  const char *sample_text_buffer;

  /* Run select_helper with the specified timestamp.  Wait until
     handle_data_source_send is called, and then begin reading from
     the pipe.  */

  if (pipe (pipefds) < 0)
    die ("pipe");

  display_string = DisplayString (display->x_display);
  sprintf (time_buffer, "%lu", time);
  pid = fork ();

  if (pid == -1)
    die ("fork");
  else if (!pid)
    {
      close (pipefds[0]);

      if (!dup2 (pipefds[1], 1))
	exit (1);

      execlp ("./select_helper_multiple", "./select_helper_multiple",
	      display_string, time_buffer, "STRING", "UTF8_STRING",
	      NULL);
      exit (1);
    }

  send_called = false;

  while (!send_called)
    {
      if (wl_display_dispatch (display->display) == -1)
        die ("wl_display_dispatch");
    }

  /* Now, start reading from the pipe and comparing the contents.  */
  sample_text_buffer = SAMPLE_TEXT;
  bytes_read = read (pipefds[0], buffer, sizeof SAMPLE_TEXT - 1);

  if (bytes_read != sizeof SAMPLE_TEXT - 1)
    report_test_failure ("wanted %zu bytes, but got %zd",
			 sizeof SAMPLE_TEXT - 1, bytes_read);

  /* Now compare the text.  */
  if (memcmp (buffer, sample_text_buffer, bytes_read))
    report_test_failure ("read text differs from sample text!");

  /* Compare the text again for the second target.  */
  bytes_read = read (pipefds[0], buffer, sizeof SAMPLE_TEXT - 1);

  if (bytes_read != sizeof SAMPLE_TEXT - 1)
    report_test_failure ("wanted %zu bytes, but got %zd",
			 sizeof SAMPLE_TEXT - 1, bytes_read);

  waitpid (pid, &wstatus, 0);

  if (WEXITSTATUS (wstatus))
    report_test_failure ("child exited with failure: %d",
			 WEXITSTATUS (wstatus));

  close (pipefds[0]);
  close (pipefds[1]);
}



static void
test_single_step (enum test_kind kind)
{
  Time time;

  test_log ("running test step: %s", test_names[kind]);

  switch (kind)
    {
    case SELECT_STRING_KIND:
      /* Set the last user time of the seat to the current X server
	 time.  */
      time = test_get_time ();
      test_seat_controller_set_last_user_time (display->seat->controller,
					       0, time);
      own_sample_text ();

      /* Do a roundtrip.  If selection ownership changes, then the
	 protocol translator will wait for selection ownership to be
	 confirmed.  */
      wl_display_roundtrip (display->display);

      /* Now, verify the selection contents.  */
      test_log ("verifying sample text normally");
      verify_sample_text (time);

      /* And verify the selection contents for MULTIPLE.  */
      test_log ("verifying sample text via MULTIPLE");
      verify_sample_text_multiple (time);
      break;
    }

  if (kind == LAST_TEST)
    test_complete ();
}



static void
run_test (void)
{
  test_single_step (SELECT_STRING_KIND);

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
  data_device
    = wl_data_device_manager_get_data_device (data_device_manager,
					      display->seat->seat);
  run_test ();
}
