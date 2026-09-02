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

#include "compositor.h"

/* Array of supported atoms.  Free this with XFree.  */
static Atom *net_supported_atoms;

/* Number of elements in that array.  */
int n_supported_atoms;

static Window
GetWmCheckWindow (void)
{
  Window result;
  unsigned char *tmp_data;
  int rc, actual_format;
  unsigned long actual_size, bytes_remaining;
  Atom actual_type;

  tmp_data = NULL;
  rc = XGetWindowProperty (compositor.display,
			   DefaultRootWindow (compositor.display),
			   _NET_SUPPORTING_WM_CHECK,
			   0, 1, False, XA_WINDOW, &actual_type,
			   &actual_format, &actual_size,
			   &bytes_remaining, &tmp_data);

  if (rc != Success || actual_type != XA_WINDOW
      || actual_format != 32 || actual_size != 1
      || !tmp_data)
    {
      if (tmp_data)
	XFree (tmp_data);

      return None;
    }

  result = *(Window *) tmp_data;
  XFree (tmp_data);

  return result;
}

static Bool
IsValidWmCheckWindow (Window window)
{
  CatchXErrors ();
  XSelectInput (compositor.display, window,
		SubstructureNotifyMask);
  return !UncatchXErrors (NULL);
}

Bool
XLWmSupportsHint (Atom hint)
{
  Window wm_check_window;
  Bool errors;
  unsigned char *tmp_data;
  int rc, actual_format, i;
  unsigned long actual_size, bytes_remaining;
  Atom actual_type;

  /* Window manager restarts are not handled here, since the rest of
     the code cannot cope with that.  */

 start_check:
  if (net_supported_atoms)
    {
      for (i = 0; i < n_supported_atoms; ++i)
	{
	  if (net_supported_atoms[i] == hint)
	    return True;
	}

      return False;
    }

  wm_check_window = GetWmCheckWindow ();

  if (!IsValidWmCheckWindow (wm_check_window))
    return False;

  tmp_data = NULL;

  CatchXErrors ();
  rc = XGetWindowProperty (compositor.display,
			   DefaultRootWindow (compositor.display),
			   _NET_SUPPORTED, 0, 4096, False, XA_ATOM,
			   &actual_type, &actual_format, &actual_size,
			   &bytes_remaining, &tmp_data);
  errors = UncatchXErrors (NULL);

  if (rc != Success || actual_type != XA_ATOM || errors)
    {
      if (tmp_data)
	XFree (tmp_data);

      return False;
    }
  else
    {
      net_supported_atoms = (Atom *) tmp_data;
      n_supported_atoms = actual_size;

      goto start_check;
    }
}
