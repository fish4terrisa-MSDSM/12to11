/* Wayland compositor running on top of an X serer.

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#include <locale.h>

#include "compositor.h"


/* Globals.  */
Compositor compositor;

static void
DetermineServerTime (void)
{
  Time server_time;
  struct timespec clock_spec;
  uint64_t clock_ms, truncated;

  /* Try to determine if the X server time is the same as the
     monotonic time.  If it is not, certain features such as "active"
     frame synchronization will not be available.  */

  clock_gettime (CLOCK_MONOTONIC, &clock_spec);
  server_time = XLGetServerTimeRoundtrip ();

  /* Convert the monotonic time to milliseconds.  */

  if (IntMultiplyWrapv (clock_spec.tv_sec, 1000, &clock_ms))
    goto overflow;

  if (IntAddWrapv (clock_ms, clock_spec.tv_nsec / 1000000,
		   &clock_ms))
    goto overflow;

  /* Truncate the time to 32 bits.  */
  truncated = clock_ms;

  /* Compare the clock time with the server time.  */
  if (llabs ((long long) server_time - (long long) truncated) <= 5)
    /* Since the difference between the server time and the monotonic
       time is less than 5 ms, the server time is most likely the
       monotonic time.  */
    compositor.server_time_monotonic = True;
  else
    {
    overflow:
      compositor.server_time_monotonic = False;
      fprintf (stderr, "Warning: the X server time does not seem to"
	       " be synchronized with the monotonic time.  Multiple"
	       " subsurfaces may be displayed at a reduced maximum"
	       " frame rate.\n");
    }
}

static void
HandleCmdline (Display *dpy, int argc, char **argv)
{
  int i;
  XrmDatabase rdb, initial_rdb;

  /* Set the default resource and class names.  */
  compositor.resource_name = "12to11";
  compositor.app_name = "12to11";

  if (argc < 2)
    /* There are no arguments to handle.  */
    return;

  /* Obtain the resource database.  */
  initial_rdb = rdb = XrmGetDatabase (dpy);

  /* Determine the instance name based on the executable.  First,
     remove any leading directory separator from argv[0].  */
  compositor.app_name = strrchr (argv[0], '/');

  /* If no directory separator was present, just use it.  */
  if (!compositor.app_name)
    compositor.app_name = argv[0];
  else
    /* Otherwise, strip the trailing '/'.  */
    compositor.app_name = compositor.app_name + 1;

  for (i = 1; i < argc; ++i)
    {
      if (!strcmp (argv[i], "-help"))
	{
	print_usage:
	  fprintf (stderr,
		   "usage: %s [-name name] [-class class] [-xrm resourcestring...]\n",
		   argv[0]);
	  exit (!strcmp (argv[i], "-help") ? 0 : 1);
	}
      else if (!strcmp (argv[i], "-class"))
	{
	  if (i + 1 >= argc)
	    {
	      fprintf (stderr, "%s: option -class requires a value\n",
		       argv[0]);
	      exit (1);
	    }

	  compositor.resource_name = argv[++i];
	}
      else if (!strcmp (argv[i], "-name"))
	{
	  if (i + 1 >= argc)
	    {
	      fprintf (stderr, "%s: option -name requires a value\n",
		       argv[0]);
	      exit (1);
	    }

	  compositor.app_name = argv[++i];
	}
      else if (!strcmp (argv[i], "-xrm"))
	{
	  if (i + 1 >= argc)
	    {
	      fprintf (stderr, "%s: option -xrm requires a value\n",
		       argv[0]);
	      exit (1);
	    }

	  XrmPutLineResource (&rdb, argv[++i]);
	}
      else
	{
	  fprintf (stderr, "%s: bad command line option \"%s\"\n",
		   argv[0], argv[1]);
	  goto print_usage;
	}
    }

  /* In case XrmPutLineResource created a new database, set it as the
     display's resource database.  */
  if (rdb != initial_rdb)
    XrmSetDatabase (dpy, rdb);
}

static void
XLMain (int argc, char **argv)
{
  Display *dpy;
  struct wl_display *wl_display;
  const char *socket;

  /* Set the locale.  */
  setlocale (LC_ALL, "");

  dpy = XOpenDisplay (NULL);
  wl_display = wl_display_create ();

  if (!dpy || !wl_display)
    {
      fprintf (stderr, "Display initialization failed\n");
      exit (1);
    }

  socket = wl_display_add_socket_auto (wl_display);

  if (!socket)
    {
      fprintf (stderr, "Unable to add socket to Wayland display\n");
      exit (1);
    }

  /* Initialize Xlib threads.  */
  XInitThreads ();

  /* Call XGetDefault with some dummy values to have the resource
     database set up.  */
  XrmInitialize ();
  XGetDefault (dpy, "dummmy", "value");

  /* Parse command-line arguments.  */
  HandleCmdline (dpy, argc, argv);

  compositor.display = dpy;
  compositor.conn = XGetXCBConnection (dpy);
  compositor.wl_display = wl_display;
  compositor.wl_socket = socket;
  compositor.wl_event_loop
    = wl_display_get_event_loop (wl_display);

  /* Initialize server time tracking very early.  */
  InitTime ();

  InitXErrors ();
  SubcompositorInit ();
  InitSelections ();

  XLInitTimers ();
  XLInitAtoms ();

  /* Initialize renderers immediately after timers and atoms are set
     up.  */
  InitRenderers ();

  XLInitRROutputs ();
  XLInitCompositor ();
  XLInitSurfaces ();
  XLInitShm ();
  XLInitXdgWM ();
  XLInitXdgSurfaces ();
  XLInitXdgToplevels ();
  XLInitFrameClock ();
  XLInitSubsurfaces ();
  XLInitSeats ();
  XLInitDataDevice ();
  XLInitPopups ();
  XLInitDmabuf ();
  XLInitXData ();
  XLInitXSettings ();
  XLInitIconSurfaces ();
  XLInitPrimarySelection ();
  XLInitExplicitSynchronization ();
  XLInitWpViewporter ();
  XLInitDecoration ();
  XLInitTextInput ();
  XLInitSinglePixelBuffer ();
  XLInitDrmLease ();
  XLInitPointerConstraints ();
  XLInitRelativePointer ();
  XLInitKeyboardShortcutsInhibit ();
  XLInitIdleInhibit ();
  XLInitPointerGestures ();

  /* This has to come after the rest of the initialization.  */
  DetermineServerTime ();
  XLRunCompositor ();
}

int
main (int argc, char **argv)
{
  XLMain (argc, argv);
  return 0;
}
