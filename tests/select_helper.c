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

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

/* select_helper.c -- Read the data of the clipboard selection and
   print them to stdout.

   There must be three arguments: the name of the display, the
   timestamp at which the selection was acquired, and the target.  */

/* The display connected to.  */
static Display *display;

/* The selection transfer window.  */
static Window selection_transfer_window;

/* Various atoms.  */
static Atom CLIPBOARD, target_atom, INCR;

static void
wait_for_selection_notify (XEvent *event)
{
  while (true)
    {
      XNextEvent (display, event);

      if (event->type == SelectionNotify
	  && (event->xselection.requestor
	      == selection_transfer_window)
	  && (event->xselection.selection
	      == CLIPBOARD)
	  && (event->xselection.property
	      == target_atom)
	  && (event->xselection.target
	      == target_atom))
	return;
    }
}

static void
wait_for_new_value (XEvent *event, Atom property)
{
  while (true)
    {
      XNextEvent (display, event);

      if (event->type == PropertyNotify
	  && event->xproperty.atom == property
	  && event->xproperty.state == PropertyNewValue)
	return;
    }
}

static size_t
get_size_for_format (int format)
{
  switch (format)
    {
    case 32:
      return sizeof (long);

    case 16:
      return sizeof (short int);

    case 8:
      return sizeof (char);
    }

  /* Should not actually happen.  */
  return 0;
}

int
main (int argc, char **argv)
{
  XSetWindowAttributes attrs;
  unsigned long flags, timestamp;
  char *atom_names[3];
  Atom atoms[3], actual_type, property;
  XEvent event;
  int actual_format;
  unsigned long nitems, bytes_after;
  unsigned char *data;

  if (argc < 4)
    /* Not enough arguments were specified.  */
    return 1;

  display = XOpenDisplay (argv[1]);

  if (!display)
    return 1;

  /* Make the window used to transfer selection data.  */
  attrs.override_redirect = True;
  attrs.event_mask = PropertyChangeMask;
  flags = CWEventMask | CWOverrideRedirect;

  selection_transfer_window
    = XCreateWindow (display, DefaultRootWindow (display),
		     -1, -1, 1, 1, 0, CopyFromParent, InputOnly,
		     CopyFromParent, flags, &attrs);

  /* Get the time.  */
  timestamp = strtoul (argv[2], NULL, 10);

  atom_names[0] = argv[3];
  atom_names[1] = (char *) "CLIPBOARD";
  atom_names[2] = (char *) "INCR";
  XInternAtoms (display, atom_names, 3, False, atoms);
  target_atom = atoms[0];
  CLIPBOARD = atoms[1];
  INCR = atoms[2];

  /* Now ask for CLIPBOARD.  */
  XConvertSelection (display, CLIPBOARD, target_atom,
		     target_atom, selection_transfer_window,
		     timestamp);

  /* And wait for the SelectionNotify event.  */
  wait_for_selection_notify (&event);

  /* Selection conversion failed.  */
  if (event.xselection.property == None)
    return 1;

  property = event.xselection.property;

  XGetWindowProperty (display, selection_transfer_window,
		      property, 0, 0xffffffff, True, AnyPropertyType,
		      &actual_type, &actual_format, &nitems, &bytes_after,
		      &data);

  if (!data || bytes_after)
    return 1;

  if (actual_type == INCR)
    {
      while (true)
	{
	  XFree (data);

	  wait_for_new_value (&event, property);
	  XGetWindowProperty (display, selection_transfer_window, property, 0,
			      0xffffffff, True, AnyPropertyType, &actual_type,
			      &actual_format, &nitems, &bytes_after, &data);

	  if (!data)
	    return 0;

	  if (nitems)
	    {
	      /* Write the selection data to stdout.  */
	      if (fwrite (data, get_size_for_format (actual_format),
			  nitems, stdout) != nitems)
		return 1;

	      continue;
	    }

	  /* Selection transfer is complete.  */
	  fflush (stdout);
	  return 0;
	}
    }
  else
    {
      /* Write the selection data to stdout.  */
      if (fwrite (data, get_size_for_format (actual_format),
		  nitems, stdout) != nitems)
	return 1;

      /* Return success.  */
      fflush (stdout);
      return 0;
    }
}
