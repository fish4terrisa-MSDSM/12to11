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

#include <byteswap.h>

#include <stdio.h>
#include <string.h>

#include "compositor.h"

typedef struct _IntegerValueListener IntegerValueListener;
typedef enum _SettingType SettingType;

enum _SettingType
  {
    Integer  = 0,
    String   = 1,
    RgbColor = 2,
  };

struct _IntegerValueListener
{
  /* Function called when the value of the setting changes.  */
  void (*new_value) (int);

  /* The last serial at which the value changed; -1 if the value is
     not yet known.  */
  long long last_change_serial;

  /* The name of the setting that this listener wants to react to.  */
  const char *setting_name;

  /* The next listener in this chain.  */
  IntegerValueListener *next;
};

/* The settings manager window.  */
static Window xsettings_window;

/* Manager selection atom.  */
static Atom xsettings_atom;

/* List of all listeners for integer setting changes.  */
IntegerValueListener *integer_listeners;

/* Key for selected input.  */
static RootWindowSelection *input_key;

#define PadValue(n, m) ((n + m - 1) & (~(m - 1)))

static void
Swap32 (unsigned char byteorder, uint32_t *cardinal)
{
#ifdef __BIG_ENDIAN__
  if (byteorder == MSBFirst)
    return;
#else
  if (byteorder == LSBFirst)
    return;
#endif

  *cardinal = bswap_32 (*cardinal);
}

static void
SwapI32 (unsigned char byteorder, int *cardinal)
{
#ifdef __BIG_ENDIAN__
  if (byteorder == MSBFirst)
    return;
#else
  if (byteorder == LSBFirst)
    return;
#endif

  *cardinal = bswap_32 (*cardinal);
}

static void
Swap16 (unsigned char byteorder, uint16_t *cardinal)
{
#ifdef __BIG_ENDIAN__
  if (byteorder == MSBFirst)
    return;
#else
  if (byteorder == LSBFirst)
    return;
#endif

  *cardinal = bswap_16 (*cardinal);
}

static void
HandleIntegerValue (char *name, int value, uint32_t last_change_serial)
{
  IntegerValueListener *listener;

  for (listener = integer_listeners; listener; listener = listener->next)
    {
      if (!strcmp (listener->setting_name, name)
	  && last_change_serial > listener->last_change_serial)
	{
	  listener->last_change_serial = last_change_serial;
	  listener->new_value (value);
	}
    }
}

static void
ReadSettingsData (void)
{
  unsigned char *prop_data, *read, *name_start, *value_start;
  Atom actual_type;
  int actual_format;
  Status rc;
  unsigned long nitems_return, bytes_after;
  unsigned char byteorder;
  uint32_t serial, n_settings, value_length, last_change_serial;
  uint16_t name_length;
  int i, value;
  uint8_t type;
  XRenderColor color;
  ptrdiff_t nitems;
  char *name_buffer;

  prop_data = NULL;
  name_buffer = NULL;

  /* Now read the actual property data.  */
  CatchXErrors ();
  rc = XGetWindowProperty (compositor.display, xsettings_window,
			   _XSETTINGS_SETTINGS, 0, LONG_MAX, False,
			   _XSETTINGS_SETTINGS, &actual_type,
			   &actual_format, &nitems_return, &bytes_after,
			   &prop_data);
  if (UncatchXErrors (NULL))
    {
      /* An error occured while reading property data.  This means
	 that the manager window is gone, so begin watching for it
	 again.  */
      if (prop_data)
	XFree (prop_data);

      XLInitXSettings ();
      return;
    }

  if (rc != Success || actual_type != _XSETTINGS_SETTINGS
      || actual_format != 8 || !nitems_return)
    {
      /* The property is invalid.  */
      if (prop_data)
	XFree (prop_data);

      return;
    }

  read = prop_data;
  nitems = nitems_return;

  /* Begin reading property data.  */
  if (nitems < 12)
    goto end;
  nitems -= 12;

  /* CARD8, byte-order.  */
  byteorder = prop_data[0];
  prop_data++;

  /* CARD8 + CARD16, padding.  */
  prop_data += 3;

  /* CARD32, serial.  */
  serial = ((uint32_t *) prop_data)[0];
  prop_data += 4;
  Swap32 (byteorder, &serial);

  /* CARD32, number of settings in the property.  */
  n_settings = ((uint32_t *) prop_data)[0];
  prop_data += 4;
  Swap32 (byteorder, &n_settings);

  /* Begin reading each entry.  */
  i = 0;
  while (i < n_settings)
    {
      if (nitems < 4)
	goto end;

      /* CARD8, settings type.  */
      type = prop_data[0];
      prop_data++;

      /* CARD8, padding.  */
      prop_data++;

      /* CARD16, name length.  */
      name_length = ((uint16_t *) prop_data)[0];
      prop_data += 2;
      Swap16 (byteorder, &name_length);

      if (nitems < PadValue (name_length, 4) +4)
	goto end;
      nitems -= PadValue (name_length, 4) + 4;

      /* NAME_LENGTH + padding, property name.  */
      name_start = prop_data;
      prop_data += PadValue (name_length, 4);

      /* CARD32, last-change-serial.  */
      last_change_serial = ((uint32_t *) prop_data)[0];
      prop_data += 4;

      switch (type)
	{
	case String:
	  if (nitems < 4)
	    goto end;
	  nitems -= 4;

	  /* CARD32, value length.  */
	  value_length = ((uint32_t *) prop_data)[0];
	  prop_data += 4;
	  Swap32 (byteorder, &value_length);

	  if (nitems < PadValue (value_length, 4))
	    goto end;
	  nitems -= PadValue (value_length, 4);

	  /* VALUE_LENGTH + padding, property value.  */
	  value_start = prop_data;
	  prop_data += PadValue (value_length, 4);

	  /* Note that string values are not yet handled.  */
	  (void) value_start;
	  break;

	case Integer:
	  if (nitems < 4)
	    goto end;
	  nitems -= 4;

	  /* INT32, value.  */
	  value = ((int32_t *) prop_data)[0];
	  prop_data += 4;
	  SwapI32 (byteorder, &value);

	  /* Now, write the name to the name buffer, with NULL
	     termination.  */
	  name_buffer = XLRealloc (name_buffer, name_length + 1);
	  memcpy (name_buffer, name_start, name_length);
	  name_buffer[name_length] = '\0';

	  /* And run any change handlers.  */
	  HandleIntegerValue (name_buffer, value, last_change_serial);
	  break;

	case RgbColor:
	  if (nitems < 8)
	    goto end;
	  nitems -= 8;

	  /* CARD16, red.  */
	  color.red = ((uint16_t *) prop_data)[0];
	  prop_data += 2;
	  Swap16 (byteorder, &color.red);

	  /* CARD16, green.  */
	  color.green = ((uint16_t *) prop_data)[0];
	  prop_data += 2;
	  Swap16 (byteorder, &color.green);

	  /* CARD16, blue.  */
	  color.blue = ((uint16_t *) prop_data)[0];
	  prop_data += 2;
	  Swap16 (byteorder, &color.blue);

	  /* CARD16, alpha.  */
	  color.alpha = ((uint16_t *) prop_data)[0];
	  prop_data += 2;
	  Swap16 (byteorder, &color.alpha);
	  break;
	}

      i++;
    }

 end:
  if (read)
    XFree (read);

  XFree (name_buffer);
}

Bool
XLHandleOneXEventForXSettings (XEvent *event)
{
  if (event->type == ClientMessage
      && event->xclient.message_type == MANAGER
      && event->xclient.data.l[1] == xsettings_atom)
    {
      /* Set the settings manager window, deselect for StructureNotify
	 on the root window, and read the new settings data.  */
      if (input_key)
	XLDeselectInputFromRootWindow (input_key);
      input_key = NULL;

      xsettings_window = event->xclient.data.l[2];

      CatchXErrors ();
      /* Also select for PropertyNotify on the settings window, so we
	 can get notifications once properties change.  */
      XSelectInput (compositor.display, xsettings_window,
		    PropertyChangeMask);
      if (UncatchXErrors (NULL))
	/* The settings window vanished; select for manager events
	   again until we obtain the new settings window.  */
	XLInitXSettings ();
      else
	/* Begin reading settings data.  */
	ReadSettingsData ();

      return True;
    }
  else if (event->type == PropertyNotify
	   && event->xproperty.window == xsettings_window
	   && event->xproperty.atom == _XSETTINGS_SETTINGS)
    {
      CatchXErrors ();
      /* Also select for PropertyNotify on the settings window, so we
	 can get notifications once properties change.  */
      XSelectInput (compositor.display, xsettings_window,
		    PropertyChangeMask);
      if (UncatchXErrors (NULL))
	/* The settings window vanished; select for manager events
	   again until we obtain the new settings window.  */
	XLInitXSettings ();
      else
	/* Begin reading settings data.  */
	ReadSettingsData ();

      return True;
    }
  else if (event->type == DestroyNotify
	   && event->xdestroywindow.window == xsettings_window)
    {
      xsettings_window = None;

      /* The settings window was destroyed; select for manager events
	 again until the settings window reappears.  */
      XLInitXSettings ();
    }

  return False;
}

void
XLListenToIntegerSetting (const char *name, void (*callback) (int))
{
  IntegerValueListener *listener;

  listener = XLMalloc (sizeof *listener);
  listener->last_change_serial = -1;
  listener->new_value = callback;
  listener->setting_name = name;
  listener->next = integer_listeners;
  integer_listeners = listener;
}

void
XLInitXSettings (void)
{
  IntegerValueListener *listener;
  char buffer[64];

  if (xsettings_atom == None)
    {
      /* Intern the manager selection atom, if it doesn't already
	 exist.  */
      sprintf (buffer, "_XSETTINGS_S%d",
	       DefaultScreen (compositor.display));
      xsettings_atom = XInternAtom (compositor.display, buffer,
				    False);
    }

  /* Reset the last change serial of all listeners, since the settings
     provider window has vanished.  */
  for (listener = integer_listeners; listener; listener = listener->next)
    listener->last_change_serial = -1;

  /* Grab the server, and get the value of the manager selection.  */
  XGrabServer (compositor.display);

  xsettings_window = XGetSelectionOwner (compositor.display,
					 xsettings_atom);

  /* If the settings window doesn't exist yet, select for MANAGER
     messages on the root window.  */

  if (!xsettings_window && !input_key)
    input_key = XLSelectInputFromRootWindow (StructureNotifyMask);

  /* If the settings window exists, then begin reading property
     data.  */
  if (xsettings_window != None)
    {
      /* Also select for PropertyNotify events.  */
      XSelectInput (compositor.display, xsettings_window,
		    PropertyChangeMask);

      ReadSettingsData ();
    }

  /* Finally, ungrab the X server.  */
  XUngrabServer (compositor.display);
}
