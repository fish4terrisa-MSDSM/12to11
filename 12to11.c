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

#include <time.h>

#include "compositor.h"


/* Globals.  */
Compositor compositor;

static void
DetermineServerTime (void)
{
  Time server_time;
  struct timespec clock_spec, server_spec, diff;

  /* Try to determine if the X server time is the same as the
     monotonic time.  If it is not, certain features such as "active"
     frame synchronization will not be available.  */

  clock_gettime (CLOCK_MONOTONIC, &clock_spec);
  server_time = XLGetServerTimeRoundtrip ();
  server_spec.tv_sec = server_time / 1000;
  server_spec.tv_nsec = ((server_time - server_time / 1000 * 1000)
			 * 1000000);

  diff = TimespecSub (server_spec, clock_spec);

  if (TimespecCmp (diff, MakeTimespec (0, 50000000)) <= 0
      || TimespecCmp (diff, MakeTimespec (0, -50000000)) <= 0)
    /* Since the difference between the server time and the monotonic
       time is less than 50 ms, the server time is the monotonic
       time.  */
    compositor.server_time_monotonic = True;
  else
    {
      compositor.server_time_monotonic = False;
      fprintf (stderr, "Warning: the X server time does not seem to"
	       " be synchronized with the monotonic time.  Multiple"
	       " subsurfaces may be displayed at a reduced maximum"
	       " frame rate.\n");
    }
}

static void
XLMain (int argc, char **argv)
{
  Display *dpy;
  struct wl_display *wl_display;
  const char *socket;

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

  compositor.display = dpy;
  compositor.conn = XGetXCBConnection (dpy);
  compositor.wl_display = wl_display;
  compositor.wl_socket = socket;
  compositor.wl_event_loop
    = wl_display_get_event_loop (wl_display);

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
