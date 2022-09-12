/* Wayland compositor running on top of an X server.

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

#include "compositor.h"

#include <X11/extensions/XInput.h>

/* X error handling routines.  The entry point into this code is
   CatchXErrors, which starts catching X errors in the following code,
   and UncatchXErrors, which syncs (if necessary) and saves any
   received XErrorEvent into a provided buffer.

   This code is not reentrant since it doesn't have to take care of
   many complicated scenarios that the Emacs code needs.  */

/* First request from which errors should be caught.  -1 if we are not
   currently catching errors.  */

static unsigned long first_error_req;

/* XErrorEvent any error that happens while errors are being caught
   will be saved in.  */

static XErrorEvent error;

/* True if any error was caught.  */

static Bool error_caught;

void
CatchXErrors (void)
{
  first_error_req = XNextRequest (compositor.display);
  error_caught = False;
}

Bool
UncatchXErrors (XErrorEvent *event)
{
  /* Try to avoid syncing to obtain errors if we know none could have
     been generated, because either no request has been made, or all
     requests have been processed.  */
  if ((LastKnownRequestProcessed (compositor.display)
       != XNextRequest (compositor.display) - 1)
      && (NextRequest (compositor.display)
	  > first_error_req))
    /* If none of those conditions apply, catch errors now.  */
    XSync (compositor.display, False);

  first_error_req = -1;

  if (!event)
    return error_caught;

  if (!error_caught)
    return False;

  *event = error;
  return True;
}

static int
ErrorHandler (Display *display, XErrorEvent *event)
{
  char buf[256];

  if (first_error_req != -1
      && event->serial >= first_error_req)
    {
      error = *event;
      error_caught = True;

      return 0;
    }

  if (XLHandleErrorForDmabuf (event))
    return 0;

  if (event->error_code == (xi_first_error + XI_BadDevice))
    /* Various XI requests can result in XI_BadDevice errors if the
       device has been removed on the X server, but we have not yet
       processed the corresponding hierarchy events.  */
    return 0;

  XGetErrorText (display, event->error_code, buf, sizeof buf);
  fprintf (stderr, "X protocol error: %s on protocol request %d\n",
	   buf, event->request_code);
  exit (70);
}

void
InitXErrors (void)
{
  first_error_req = -1;
  XSetErrorHandler (ErrorHandler);

  /* Allow debugging by setting an environment variable.  */
  if (getenv ("SYNCHRONIZE"))
    XSynchronize (compositor.display, True);
}
