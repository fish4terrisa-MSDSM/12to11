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

/* select_helper_multiple.c -- Perform a request for multiple
   selections.

   There must be at least three arguments: the name of the display,
   the timestamp at which the selection was acquired, and the
   target(s).  */

/* The display connected to.  */
static Display *display;

/* The selection transfer window.  */
static Window selection_transfer_window;

/* Various atoms.  */
static Atom CLIPBOARD, INCR, MULTIPLE, ATOM_PAIR, *target_atoms;

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
	      == MULTIPLE)
	  && (event->xselection.target
	      == MULTIPLE))
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
  char *atom_names[4];
  Atom atoms[4], actual_type, property, *parameters;
  XEvent event;
  int actual_format, i;
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

  atom_names[0] = (char *) "CLIPBOARD";
  atom_names[1] = (char *) "INCR";
  atom_names[2] = (char *) "MULTIPLE";
  atom_names[3] = (char *) "ATOM_PAIR";
  XInternAtoms (display, atom_names, 4, False, atoms);
  CLIPBOARD = atoms[0];
  INCR = atoms[1];
  MULTIPLE = atoms[2];
  ATOM_PAIR = atoms[3];

  /* Intern the target atoms.  */
  target_atoms = malloc (sizeof *target_atoms * argc - 3);
  XInternAtoms (display, &argv[3], argc - 3, False, target_atoms);

  /* Write each of the atoms as parameters before making the selection
     request.  */
  atoms[0] = target_atoms[0];
  atoms[1] = target_atoms[0];
  XChangeProperty (display, selection_transfer_window, MULTIPLE,
		   ATOM_PAIR, 32, PropModeReplace,
		   (unsigned char *) atoms, 2);

  for (i = 1; i < argc - 3; ++i)
    {
      atoms[0] = target_atoms[i];
      atoms[1] = target_atoms[i];
      XChangeProperty (display, selection_transfer_window, MULTIPLE,
		       ATOM_PAIR, 32, PropModeAppend,
		       (unsigned char *) atoms, 2);
    }

  /* Now, request the selection.  */
  XConvertSelection (display, CLIPBOARD, MULTIPLE,
		     MULTIPLE, selection_transfer_window,
		     timestamp);

  /* And wait for the SelectionNotify event.  */
  wait_for_selection_notify (&event);

  /* Selection conversion failed.  */
  if (event.xselection.property == None)
    return 1;

  /* Now, read the MULTIPLE property again.  See which conversions
     were completed.  */
  XGetWindowProperty (display, selection_transfer_window,
		      event.xselection.property, 0, (argc - 3) * 2,
		      True, ATOM_PAIR,
		      &actual_type, &actual_format, &nitems,
		      &bytes_after, &data);

  if (actual_format != 32 || actual_type != ATOM_PAIR
      || nitems != (argc - 3) * 2)
    return 1;

  parameters = (Atom *) data;

  for (i = 0; i < argc - 3; ++i)
    {
      if (!parameters[i * 2 + 1])
	continue;

      property = parameters[i * 2];

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
	      break;
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
	}
    }

  return 0;
}
