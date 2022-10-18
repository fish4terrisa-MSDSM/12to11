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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/errno.h>

#include <iconv.h>
#include <locale.h>
#include <langinfo.h>
#include <ctype.h>

#include "compositor.h"
#include "text-input-unstable-v3.h"

#include <X11/extensions/XInput2.h>

/* X Input Method (XIM) support.

   The X input method has a client-server architecture; the connection
   between the client and server is abstracted away by Xlib, and
   always results in an XIM object being produced.  The connection
   itself can take many forms: the IM server could be linked into the
   X library, another X client on the same server, running on another
   machine over a TCP/IP connection, or even a DECnet node.

   The XIM object will be assigned an arbitrary seat (usually the
   virtual core keyboard), which will be the only seat that can
   utilize input methods.

   Each text input will have a corresponding input context (XIC) for
   every focused window.  The XIC handles state logically associated
   with a single text entry area, such as currently composed text, the
   currently focused surface, the position of the cursor, and text
   surrounding the cursor.

   When the previously assigned seat's focus moves to a surface with
   an associated XIC, and the text input is enabled, focus is given to
   the XIC.  Subsequent extension key events are translated into core
   ones, then sent to the input context; then, should the input
   context chose to discard the event, the event is simply discarded.
   Otherwise, XmbLookupString is called on the event, and any keysym
   or string returned is looked up and committed or sent to the
   surface.

   The X library synthesizes fake client-side events to represent
   XIM_COMMIT events, and saves the committed text for XmbLookupString
   to return.  These events do not contain the information necessary
   to determine the XIC that sent the event which caused it to be
   generated.  Similarly, events sent with XIM_FORWARD_EVENT do not
   contain enough information to attribute them to the XIC that sent
   them.

   That means it is impossible to attribute forwarded events or
   committed text to the correct XIC, and thus it is impossible to
   look up which seat's TextInput resource an event is actually bound
   for.  If one day we move to our own implementation of the XIM
   protocol, then it will become possible to properly support
   multi-seat setups, with one XIC per-client and per-seat.

   Further more, the XIM has its own locale, which is not guaranteed
   to have the same coded character set as the character set used in
   the Wayland protocol, namely UTF-8.  During XIM creation, its
   locale's coded character set is obtained and used to create a
   character conversion context.  All text obtained from the XIM
   callbacks is then converted with that context, and character
   indices provided by the XIM are converted to byte indices into the
   converted string before being sent to the client.

   This code has many inherent race conditions, just like the
   zwp_text_input_v3 protocol itself.  And as described above, it only
   supports one seat due to limitations of the Xlib XIM wrapper.  */

typedef struct _TextInputClientInfo TextInputClientInfo;
typedef struct _TextInput TextInput;
typedef struct _TextInputState TextInputState;
typedef struct _TextPosition TextPosition;
typedef struct _PreeditBuffer PreeditBuffer;

typedef enum _XimStyleKind XimStyleKind;

enum _XimStyleKind
  {
    XimStyleNone,
    XimOverTheSpot,
    XimOffTheSpot,
    XimRootWindow,
    XimOnTheSpot,
  };

enum
  {
    PendingEnabled	   = 1,
    PendingCursorRectangle = (1 << 1),
    PendingSurroundingText = (1 << 2),
  };

struct _PreeditBuffer
{
  /* Buffer data.  */
  char *buffer;

  /* The locale.  */
  char *locale;

  /* Buffer size in bytes.  */
  size_t size;

  /* Buffer size in characters.  */
  int total_characters;
};

struct _TextPosition
{
  /* Byte position.  */
  ptrdiff_t bytepos;

  /* Character position.  */
  int charpos;
};

struct _TextInputState
{
  /* What is defined; alternatively, what is pending.  */
  int pending;

  /* Whether or not this text input is enabled.  */
  Bool enabled;

  /* Cursor rectangle.  */
  int cursor_x, cursor_y, cursor_width, cursor_height;

  /* Surrounding text.  This is allocated with XLMalloc and is made
     NULL upon commit in the pending state.  */
  char *surrounding_text;

  /* Character and byte positions of the cursor in the surrounding
     text.  */
  TextPosition cursor;
};

struct _TextInput
{
  /* The TextInputClientInfo associated with this text input.  */
  TextInputClientInfo *client_info;

  /* The wl_resource associated with this text input.  */
  struct wl_resource *resource;

  /* The next and last TextInputs.  */
  TextInput *next, *last;

  /* The XIC associated with this text input.  */
  XIC xic;

  /* The current pre-edit buffer, or NULL.  */
  PreeditBuffer *buffer;

  /* The position of the preedit caret in characters.  */
  int caret;

  /* The style of the caret.  */
  XIMCaretStyle caret_style;

  /* The pending state.  */
  TextInputState pending_state;

  /* The current state.  */
  TextInputState current_state;

  /* Number of commit requests performed.  */
  uint32_t serial;
};

/* Structure describing a list of TextInput resources associated with
   a given client.  */
struct _TextInputClientInfo
{
  /* The next and last objects in this list.  */
  TextInputClientInfo *next, *last;

  /* The associated seat.  */
  Seat *seat;

  /* The key associated with the seat destruction callback.  */
  void *seat_key;

  /* The associated Wayland client.  */
  struct wl_client *client;

  /* The list of Wayland client info objects.  */
  TextInput inputs;

  /* The current focused surface.  */
  Surface *focus_surface;
};

/* List of all TextInputClientInfos.  */
static TextInputClientInfo all_client_infos;

/* The text input manager global.  */
static struct wl_global *text_input_manager_global;

/* The IM fontset.  */
static XFontSet im_fontset;

#if defined DEBUG
#define DebugPrint(format, args...)				\
  fprintf (stderr, "%s: " format "\n", __FUNCTION__, ## args)
#else
#define DebugPrint(fmt, ...) ((void) 0)
#endif

/* The XIM currently in use, or NULL.  */
static XIM current_xim;

/* The conversion context for that XIM.  */
static iconv_t current_cd;

/* The preferred XIM style.  */
static XIMStyle xim_style;

/* The order in which XIM input styles will be searched for.  */
static XimStyleKind xim_style_order[5];

static int
CurrentCursorX (TextInput *input)
{
  int x, y;

  XLAssert (input->client_info->focus_surface != NULL);

  /* Scale these coordinates into window coordinates.  */
  TruncateSurfaceToWindow (input->client_info->focus_surface,
			   input->current_state.cursor_x,
			   input->current_state.cursor_y,
			   &x, &y);

  return x;
}

static int
CurrentCursorY (TextInput *input)
{
  int x, y;

  XLAssert (input->client_info->focus_surface != NULL);

  /* Scale these coordinates into window coordinates.  */
  TruncateSurfaceToWindow (input->client_info->focus_surface,
			   input->current_state.cursor_x,
			   input->current_state.cursor_y,
			   &x, &y);

  return y;
}

static int
CurrentCursorWidth (TextInput *input)
{
  int width, height;

  XLAssert (input->client_info->focus_surface != NULL);

  /* Scale these coordinates into window coordinates.  */
  TruncateScaleToWindow (input->client_info->focus_surface,
			 input->current_state.cursor_width,
			 input->current_state.cursor_height,
			 &width, &height);

  return width;
}

static int
CurrentCursorHeight (TextInput *input)
{
  int width, height;

  XLAssert (input->client_info->focus_surface != NULL);

  /* Scale these coordinates into window coordinates.  */
  TruncateScaleToWindow (input->client_info->focus_surface,
			 input->current_state.cursor_width,
			 input->current_state.cursor_height,
			 &width, &height);

  return height;
}


/* Byte-text position conversion.  */

static int
CountOctets (char byte)
{
  /* Given the start of a UTF-8 sequence, return how many following
     bytes are in the current sequence.  */
  return (!(byte & 0x80) ? 1
	  : !(byte & 0x20) ? 2
	  : !(byte & 0x10) ? 3
	  : !(byte & 0x08) ? 4
	  : 5);
}

static TextPosition
TextPositionFromBytePosition (const char *string, size_t length,
			      ptrdiff_t byte_position)
{
  const char *start;
  TextPosition position;

  start = string;
  position.charpos = 0;
  position.bytepos = byte_position;

  if (!byte_position)
    return position;

  if (byte_position > length)
    goto invalid;

  while (start < string + length)
    {
      if (start + CountOctets (*start) > string + length)
	goto invalid;

      start += CountOctets (*start);
      position.charpos++;
      position.bytepos = start - string;

      if (position.bytepos == byte_position)
	return position;
      else if (position.bytepos > byte_position)
	goto invalid;
    }

 invalid:
  /* Return the invalid text position.  */
  position.bytepos = -1;
  position.charpos = -1;
  return position;
}

static TextPosition
TextPositionFromCharPosition (const char *string, size_t length,
			      int char_position)
{
  const char *start;
  TextPosition position;

  start = string;
  position.charpos = 0;
  position.bytepos = 0;

  if (!char_position)
    return position;

  while (position.charpos < char_position)
    {
      if (start + CountOctets (*start) > string + length)
	goto invalid;

      start += CountOctets (*start);
      position.charpos++;
      position.bytepos = start - string;
    }

  /* Return the resulting text position.  */
  return position;

 invalid:
  /* Return the invalid text position.  */
  position.bytepos = -1;
  position.charpos = -1;
  return position;
}



/* Forward declaration.  */
static void CreateIC (TextInput *);

static void
DestroyTextInput (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
Enable (struct wl_client *client, struct wl_resource *resource)
{
  TextInput *input;

  input = wl_resource_get_user_data (resource);

  /* If there is already a string, free it, as the pending flag will
     be clobbered below.  */
  if (input->pending_state.surrounding_text)
    XLFree (input->pending_state.surrounding_text);
  input->pending_state.surrounding_text = NULL;

  /* Set the pending state.  */
  input->pending_state.pending = PendingEnabled;
  input->pending_state.enabled = True;
}

static void
Disable (struct wl_client *client, struct wl_resource *resource)
{
  TextInput *input;

  input = wl_resource_get_user_data (resource);

  /* If there is already a string, free it, as the pending flag will
     be clobbered below.  */
  if (input->pending_state.surrounding_text)
    XLFree (input->pending_state.surrounding_text);
  input->pending_state.surrounding_text = NULL;

  /* Set the pending state.  */
  input->pending_state.pending = PendingEnabled;
  input->pending_state.enabled = False;
}

static void
SetSurroundingText (struct wl_client *client, struct wl_resource *resource,
		    const char *text, int cursor, int anchor)
{
  TextInput *input;

  input = wl_resource_get_user_data (resource);

  /* If there is already a string, free it.  */
  if (input->pending_state.surrounding_text)
    XLFree (input->pending_state.surrounding_text);

  /* Set the surrounding text and cursor position.  */
  input->pending_state.surrounding_text = XLStrdup (text);
  input->pending_state.cursor
    = TextPositionFromBytePosition (text, strlen (text),
				    cursor);
  input->pending_state.pending |= PendingSurroundingText;
}

static void
SetTextChangeCause (struct wl_client *client, struct wl_resource *resource,
		    uint32_t cause)
{
  /* Not supported.  */
}

static void
SetContentType (struct wl_client *client, struct wl_resource *resource,
		uint32_t hint, uint32_t purpose)
{
  /* Not supported.  */
}

static void
SetCursorRectangle (struct wl_client *client, struct wl_resource *resource,
		    int32_t x, int32_t y, int32_t width, int32_t height)
{
  TextInput *input;

  input = wl_resource_get_user_data (resource);

  if ((input->current_state.pending & PendingCursorRectangle
       /* PendingEnabled will clear the current state's cursor
	  rectangle.  */
       && !input->pending_state.pending & PendingEnabled)
      && x == input->current_state.cursor_x
      && y == input->current_state.cursor_y
      && width == input->current_state.cursor_width
      && height == input->current_state.cursor_height)
    /* Nothing changed, return.  */
    return;

  input->pending_state.pending |= PendingCursorRectangle;
  input->pending_state.cursor_x = x;
  input->pending_state.cursor_y = y;
  input->pending_state.cursor_width = width;
  input->pending_state.cursor_height = height;
}

static TextInput *
FindEnabledTextInput (TextInputClientInfo *info)
{
  TextInput *input;

  input = info->inputs.next;
  while (input != &info->inputs)
    {
      if (input->current_state.enabled)
	return input;

      input = input->next;
    }

  return NULL;
}

static void
FitRect (XRectangle *input, int view_width, int view_height,
	 int caret_x, int caret_y, int caret_width, int caret_height)
{
  XRectangle r1, r2, copy;

  copy = *input;

  /* Try to fit the given dimensions into the view.  First,
     start with the width:

     right edge of view
     ^    |------suggested size-------|
     ^ caret X, Y + HEIGHT            ^ left edge of view

     If the suggested size does not fit, like so:

     ^    |------suggested size------^--|
     ^ caret X, Y + HEIGHT

     Move the caret to X 0 so it does, if the suggested size is wider
     than half the view:

     ^|------suggested size---------|^
     ^ start X, Y + HEIGHT

     Otherwise, move the rectangle leftwards until it fits.

     If it still does not fit, limit the width to that of the
     the view.

     Next, fit the height.  Start by placing the rectangle
     below the caret.  If that is too small, try placing the
     rectangle so that its bottom touches the caret.  If that
     still does not fit, then use the tallest of the following
     two rectangles:

     CARET_Y + CARET_HEIGHT by (BOTTOM_X - CARET_X + CARET_HEIGHT + 1)
     0 by CARET_Y.  */

  /* Do the width.  Input should already be placed at the bottom right
     corner of the caret.  */

  if (input->x + input->width >= view_width)
    {
      if (input->width > view_width / 2)
	/* Flip x to 0.  */
	input->x = 0;
      else
	/* Move the rect left until it fits.  */
	input->x -= (input->x + input->width - 1
		     - view_width);

      /* If it still doesn't fit, set x to 0 and width to
	 view_width.  */
      if (input->x + input->width >= view_width)
	{
	  input->x = 0;
	  input->width = view_width;
	}
    }

  /* Do the height.  */
  if (input->y + input->height >= view_height)
    {
      /* Flip the caret upwards, so the last scanline of the preedit
	 area is immediately above the first scanline of the
	 caret.  */
      input->y = caret_y - input->height;

      /* If the rectangle is still too small, use the rectangle formed
	 between the top of the view and the top of the caret, or that
	 between the bottom of the view and the bottom of the caret,
	 whichever is larger.  */
      if (input->y < 0 || input->y + input->height >= view_height)
	{
	  r1.y = 0;
	  r1.height = caret_y;

	  r2.y = caret_y + caret_height;
	  r2.height = view_height - r2.y;

	  if (r1.height > r2.height)
	    {
	      input->y = r1.y;
	      input->height = r1.height;
	    }
	  else
	    {
	      input->y = r2.y;
	      input->height = r2.height;
	    }
	}
    }

  /* If the rectangle is still invalid, just fall back to the old
     one.  */
  if (input->width <= 0 || input->height <= 0)
    *input = copy;
}

static void
DoGeometryAllocation (TextInput *input)
{
  XPoint spot;
  XRectangle area, *needed;
  XVaNestedList attr;
  View *view;
  char *rc;

  DebugPrint ("doing geometry allocation for %p", input);

  if (!input->xic)
    return;

  XLAssert (input->client_info->focus_surface != NULL);
  view = input->client_info->focus_surface->view;

  if (xim_style & XIMPreeditPosition)
    {
      DebugPrint ("IM wants spot values for preedit window");

      if (input->current_state.pending & PendingCursorRectangle)
	{
	  spot.x = CurrentCursorX (input);
	  spot.y = (CurrentCursorY (input)
		    + CurrentCursorHeight (input));
	}
      else
	{
	  spot.x = 0;
	  spot.y = 1;
	}

      DebugPrint ("using spot: %d, %d", spot.x, spot.y);
      attr = XVaCreateNestedList (0, XNSpotLocation, &spot, NULL);
      XSetICValues (input->xic, XNPreeditAttributes, attr, NULL);
      XFree (attr);
    }
  else if (xim_style & XIMPreeditArea)
    {
      DebugPrint ("IM wants geometry negotiation");

      /* Suggest no size to the input method.  */
      area.x = area.y = area.width = area.height = 0;

      attr = XVaCreateNestedList (0, XNAreaNeeded, &area, NULL);
      XSetICValues (input->xic, XNPreeditAttributes, attr, NULL);
      XFree (attr);

      /* Get the size from the input method.  */
      attr = XVaCreateNestedList (0, XNAreaNeeded, &needed, NULL);
      rc = XGetICValues (input->xic, XNPreeditAttributes, attr, NULL);
      XFree (attr);

      if (!rc)
	{
	  DebugPrint ("IM suggested the given size: %d %d",
		      needed->width, needed->height);

	  /* Place the rectangle below and to the right of the
	     caret.  */

	  if (input->current_state.pending & PendingCursorRectangle)
	    {
	      needed->x = (CurrentCursorX (input)
			   + CurrentCursorWidth (input));
	      needed->y = (CurrentCursorY (input)
			   + CurrentCursorHeight (input));

	      FitRect (needed, ViewWidth (view), ViewHeight (view),
		       CurrentCursorX (input), CurrentCursorY (input),
		       CurrentCursorWidth (input),
		       CurrentCursorHeight (input));

	      DebugPrint ("filled rectangle: %d %d %d %d",
			  needed->x, needed->y, needed->width,
			  needed->height);
	    }
	  else
	    {
	      /* No caret was specified... Place the preedit window on
		 the bottom left corner of the view.  */
	      needed->x = 0;
	      needed->y = ViewHeight (view) - needed->height;

	      DebugPrint ("placed rectangle: %d %d %d %d",
			  needed->x, needed->y, needed->width,
			  needed->height);
	    }

	  /* Set the geometry.  */
	  attr = XVaCreateNestedList (0, XNArea, needed, NULL);
	  XSetICValues (input->xic, XNPreeditAttributes, attr, NULL);
	  XFree (attr);

	  /* Free the rectangle returned.  */
	  XFree (needed);
	}
    }

  if (xim_style & XIMStatusArea)
    {
      DebugPrint ("IM wants geometry negotiation for status area");

      /* Suggest no size to the input method.  */
      area.x = area.y = area.width = area.height = 0;

      attr = XVaCreateNestedList (0, XNAreaNeeded, &area, NULL);
      XSetICValues (input->xic, XNStatusAttributes, attr, NULL);
      XFree (attr);

      /* Get the size from the input method.  */
      attr = XVaCreateNestedList (0, XNAreaNeeded, &needed, NULL);
      rc = XGetICValues (input->xic, XNStatusAttributes, attr, NULL);
      XFree (attr);

      if (!rc)
	{
	  DebugPrint ("IM suggested the given size: %d %d",
		      needed->width, needed->height);

	  /* Place the rectangle at the bottom of the window.  */
	  needed->x = ViewWidth (view) - needed->width;
	  needed->y = ViewHeight (view) - needed->height;

	  DebugPrint ("placed rectangle at bottom right: %d %d %d %d",
		      needed->x, needed->y, needed->width,
		      needed->height);

	  /* Set the geometry.  */
	  attr = XVaCreateNestedList (0, XNArea, needed, NULL);
	  XSetICValues (input->xic, XNStatusAttributes, attr, NULL);
	  XFree (attr);

	  /* Free the needed rectangle.  */
	  XFree (needed);
	}
    }
}

static void
Commit (struct wl_client *client, struct wl_resource *resource)
{
  TextInput *input, *enabled;

  input = wl_resource_get_user_data (resource);
  input->serial++;

  if (!input->client_info)
    /* The text input has no more associated seat.  */
    return;

  if (!input->client_info->focus_surface)
    /* The text input has no more associated surface.  */
    return;

  if (input->pending_state.pending & PendingEnabled)
    {
      if (input->pending_state.enabled)
	{
	  /* Check if there is another enabled text input in the same
	     client info structure.  */
	  enabled = FindEnabledTextInput (input->client_info);

	  if (enabled && enabled != input)
	    /* Return, as the spec says we should ignore requests to
	       enable a text input.  */
	    return;
	}

      /* Free any surrounding text in the current state.  */
      if (input->current_state.surrounding_text)
	XLFree (input->current_state.surrounding_text);

      /* Copy the pending state wholesale.  */
      input->current_state = input->pending_state;

      /* Clear the surrounding text.  */
      input->pending_state.surrounding_text = NULL;
      input->pending_state.pending = 0;

      if (input->current_state.surrounding_text)
	DebugPrint ("surrounding text early change: %s[%d]",
		    input->current_state.surrounding_text,
		    input->current_state.cursor.charpos);

      if (input->current_state.enabled)
	{
	  DebugPrint ("text input %p enabled, state: %2b", input,
		      (unsigned int) input->current_state.pending);

	  /* Maybe create or reset and then focus the IC.  */
	  if (!input->xic)
	    CreateIC (input);
	  else
	    XFree (XmbResetIC (input->xic));

	  /* Perform geometry/position allocation on the IC.  */
	  DoGeometryAllocation (input);

	  if (input->xic)
	    XSetICFocus (input->xic);
	}
      else
	{
	  DebugPrint ("text input %p disabled", input);

	  if (input->xic)
	    XUnsetICFocus (input->xic);
	}
    }
  else
    {
      /* Apply the pending state piecemeal.  */
      if (input->pending_state.pending & PendingCursorRectangle)
	{
	  DebugPrint ("cursor rectangle changed to: %d %d %d %d",
		      input->pending_state.cursor_x,
		      input->pending_state.cursor_y,
		      input->pending_state.cursor_width,
		      input->pending_state.cursor_height);

	  input->current_state.cursor_x
	    = input->pending_state.cursor_x;
	  input->current_state.cursor_y
	    = input->pending_state.cursor_y;
	  input->current_state.cursor_width
	    = input->pending_state.cursor_width;
	  input->current_state.cursor_height
	    = input->pending_state.cursor_height;

	  input->current_state.pending |= PendingCursorRectangle;

	  if (input->current_state.enabled && input->xic)
	    /* Perform geometry/position allocation on the IC.  */
	    DoGeometryAllocation (input);
	}

      if (input->pending_state.pending & PendingSurroundingText)
	{
	  DebugPrint ("surrounding text changed to: %s[%d]",
		      input->pending_state.surrounding_text,
		      input->pending_state.cursor.charpos);

	  if (input->current_state.surrounding_text)
	    XLFree (input->current_state.surrounding_text);

	  /* Surrounding text changed.  Move the surrounding text and
	     cursor position over.  */
	  input->current_state.surrounding_text
	    = input->pending_state.surrounding_text;
	  input->current_state.cursor = input->pending_state.cursor;

	  /* Clear the surrounding text on the pending state.  */
	  input->pending_state.surrounding_text = NULL;

	  /* And add the flag to the current state.  */
	  input->current_state.pending |= PendingSurroundingText;
	}

      /* Clear the pending state mask.  */
      input->pending_state.pending = 0;
    }
}

static const struct zwp_text_input_v3_interface input_impl =
  {
    .destroy = DestroyTextInput,
    .enable = Enable,
    .disable = Disable,
    .set_surrounding_text = SetSurroundingText,
    .set_text_change_cause = SetTextChangeCause,
    .set_content_type = SetContentType,
    .set_cursor_rectangle = SetCursorRectangle,
    .commit = Commit,
  };

/* Forward declarations.  */
static void FreePreeditBuffer (PreeditBuffer *);
static void UpdatePreedit (TextInput *);

static void
HandleICDestroyed (TextInput *input)
{
  /* Destroy the preedit buffer and update the preedit state.  */
  if (input->buffer)
    {
      FreePreeditBuffer (input->buffer);
      input->buffer = NULL;

      /* Send changes to the client.  */
      UpdatePreedit (input);
    }
}

static void
InputDoLeave (TextInput *input, Surface *old_surface)
{
  /* Destroy any XIC that was created.  */
  if (input->xic)
    {
      XDestroyIC (input->xic);
      input->xic = NULL;
      HandleICDestroyed (input);
    }

  /* Clear the input state.  */

  if (input->current_state.surrounding_text)
    XLFree (input->current_state.surrounding_text);

  memset (&input->current_state, 0, sizeof input->current_state);
}

static void
InputDoEnter (TextInput *input, Surface *new_surface)
{
  /* If there is still a preedit buffer, destroy it.  */
  if (input->buffer)
    {
      FreePreeditBuffer (input->buffer);

      /* Set it to NULL.  */
      input->buffer = NULL;
      UpdatePreedit (input);
    }
}

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  TextInput *input;

  input = wl_resource_get_user_data (resource);

  /* Now, if the client info is attached, unlink the text input.  */
  if (input->client_info)
    {
      input->last->next = input->next;
      input->next->last = input->last;

      /* If the client info is now empty, destroy the client info as
	 well.  */
      if (input->client_info->inputs.next
	  == &input->client_info->inputs)
	{
	  XLSeatCancelDestroyListener (input->client_info->seat_key);
	  input->client_info->last->next = input->client_info->next;
	  input->client_info->next->last = input->client_info->last;

	  XLFree (input->client_info);
	}
    }

  /* If an XIC still exists, destroy it.  */
  if (input->xic)
    XDestroyIC (input->xic);

  /* If there is a surrounding text string, free it.  */
  if (input->pending_state.surrounding_text)
    XLFree (input->pending_state.surrounding_text);
  if (input->current_state.surrounding_text)
    XLFree (input->current_state.surrounding_text);

  /* If there is still a preedit buffer, destroy it.  */
  if (input->buffer)
    FreePreeditBuffer (input->buffer);

  /* Free the text input itself.  */
  XLFree (input);
}



static void
HandleSeatDestroyed (void *data)
{
  TextInputClientInfo *info;
  TextInput *input;

  /* The seat associated with the given TextInputClientInfo was
     destroyed.  Detach every TextInput object.  */
  info = data;
  input = info->inputs.next;

  while (input != &info->inputs)
    {
      input->client_info = NULL;

      /* client_info is now NULL, meaning this text input is inert.
	 So destroy the XIC, as it's not being destroyed later.  */
      if (input->xic)
	{
	  XDestroyIC (input->xic);
	  input->xic = NULL;
	  HandleICDestroyed (input);
	}

      input = input->next;
    }

  /* Next, unlink and free the client info.  */
  info->last->next = info->next;
  info->next->last = info->last;
  XLFree (info);
}

static void
NoticeEnter (TextInputClientInfo *info, Surface *surface)
{
  TextInput *input;

  DebugPrint ("client info: %p, surface: %p",
	      info, surface);

  if (info->focus_surface == surface)
    /* The focus surface did not change.  */
    return;

  input = info->inputs.next;
  while (input != &info->inputs)
    {
      /* If a previous surface exists, also send a leave event.  */
      if (info->focus_surface)
	{
	  DebugPrint ("sending leave to text input %p", input);

	  XLAssert (info->focus_surface->resource != NULL);
	  zwp_text_input_v3_send_leave (input->resource,
					info->focus_surface->resource);

	  InputDoLeave (input, info->focus_surface);
	}

      DebugPrint ("sending enter to text input %p", input);

      /* Send the enter event to each text input.  */
      zwp_text_input_v3_send_enter (input->resource,
				    surface->resource);
      InputDoEnter (input, surface);

      input = input->next;
    }

  /* Record the focus surface.  Note that this surface should always
     be removed by ClearFocusSurface in the seat upon destruction, so
     there is no need for a callback to be registered here as well!

     If that invariant is broken, strange bugs will follow.  */
  info->focus_surface = surface;
}

static void
NoticeLeave (TextInputClientInfo *info)
{
  TextInput *input;

  /* If there is already no focus surface, return.  */
  if (!info->focus_surface)
    return;

  DebugPrint ("client info: %p", info);

      input = info->inputs.next;
      while (input != &info->inputs)
	{
	  DebugPrint ("sending leave to text input %p", input);

	  /* Otherwise, if info->focus_surface->resource is still
	     there, send the leave event to each text input.  */
	  if (info->focus_surface->resource)
	    /* Send the enter event to each text input.  */
	    zwp_text_input_v3_send_leave (input->resource,
					  info->focus_surface->resource);
	  InputDoLeave (input, info->focus_surface);

	  input = input->next;
	}

  /* And clear the focus surface.  */
  info->focus_surface = NULL;
}

static TextInputClientInfo *
GetClientInfo (struct wl_client *client, Seat *seat, Bool create)
{
  TextInputClientInfo *info;

  /* First, look through the list of client infos.  */
  info = all_client_infos.next;

  while (info != &all_client_infos)
    {
      if (info->seat == seat && info->client == client)
	return info;

      info = info->next;
    }

  if (!create)
    return NULL;

  /* If none was found, create one and link it onto the list.  */
  info = XLCalloc (1, sizeof *info);
  info->seat = seat;
  info->client = client;
  info->next = all_client_infos.next;
  info->last = &all_client_infos;
  all_client_infos.next->last = info;
  all_client_infos.next = info;

  /* Then, attach the seat destruction listener and initialize the
     list of text input objects.  */
  info->seat_key
    = XLSeatRunOnDestroy (seat, HandleSeatDestroyed, info);
  info->inputs.next = &info->inputs;

  /* And return info.  */
  return info;
}

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

/* Forward declaration.  */
static void FocusInCallback (Seat *, Surface *);

static void
GetTextInput (struct wl_client *client, struct wl_resource *resource,
	      uint32_t id, struct wl_resource *seat_resource)
{
  Seat *seat;
  struct wl_resource *dummy;
  TextInput *input;
  TextInputClientInfo *info;

  seat = wl_resource_get_user_data (seat_resource);

  /* If the seat is inert, we cannot rely on destroy callbacks being
     run.  In that case, we make a dummy text input resource with no
     data attached.  */
  if (XLSeatIsInert (seat))
    {
      dummy = wl_resource_create (client, &zwp_text_input_v3_interface,
				  wl_resource_get_version (resource), id);

      if (!dummy)
	wl_resource_post_no_memory (resource);
      else
	wl_resource_set_implementation (dummy, &input_impl, NULL, NULL);

      return;
    }

  /* Create the text input.  */
  input = XLSafeMalloc (sizeof *input);

  if (!input)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (input, 0, sizeof *input);
  input->resource
    = wl_resource_create (client, &zwp_text_input_v3_interface,
			  wl_resource_get_version (resource), id);

  if (!input->resource)
    {
      XLFree (input);
      wl_resource_post_no_memory (resource);
      return;
    }

  /* Obtain the client info.  */
  info = GetClientInfo (client, seat, True);

  /* Set the implementation.  N.B. that HandleResourceDestroy will
     free the client info structure once all references are gone.  */
  wl_resource_set_implementation (input->resource, &input_impl,
				  input, HandleResourceDestroy);

  /* Initialize and link the text input.  */
  input->client_info = info;
  input->next = info->inputs.next;
  input->last = &info->inputs;
  info->inputs.next->last = input;
  info->inputs.next = input;

  /* If there is already a focused surface on the seat belonging to
     the client, focus it now.  */
  if (info->focus_surface)
    {
      DebugPrint ("focusing newly created text input %p", input);

      /* The info already existed and already has a focus surface
	 set.  */
      zwp_text_input_v3_send_enter (input->resource,
				    info->focus_surface->resource);
      InputDoEnter (input, info->focus_surface);
    }
  else if (XLSeatIsClientFocused (seat, client))
    {
      DebugPrint ("focusing newly created text input with info %p", input);

      /* The info did not previously exist, but the client created a
	 surface that is the seat's input focus.  */
      FocusInCallback (seat, XLSeatGetFocus (seat));
    }
}

static const struct zwp_text_input_manager_v3_interface manager_impl =
  {
    .destroy = Destroy,
    .get_text_input = GetTextInput,
  };

static void
HandleBind (struct wl_client *client, void *data, uint32_t version,
	    uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client,
				 &zwp_text_input_manager_v3_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &manager_impl,
				  NULL, NULL);
}



static PreeditBuffer *
MakePreeditBuffer (const char *locale)
{
  PreeditBuffer *buffer;

  buffer = XLCalloc (1, sizeof *buffer);
  buffer->locale = XLStrdup (locale);

  return buffer;
}

static void
FreePreeditBuffer (PreeditBuffer *buffer)
{
  XLFree (buffer->buffer);
  XLFree (buffer->locale);
  XLFree (buffer);
}

static Bool
PreeditDeleteChars (PreeditBuffer *buffer, int start_char,
		    int length)
{
  wchar_t wc;
  char *oldlocale, *start, *end;
  int rc, chars, old_chars;

  /* Note that preedit buffers operate on text encoded with the IM
     locale's charset.  Record the old locale.  */
  oldlocale = XLStrdup (setlocale (LC_CTYPE, NULL));

  /* Switch to the new locale.  */
  if (!setlocale (LC_CTYPE, buffer->locale))
    {
      XLFree (oldlocale);
      return False;
    }

  start = buffer->buffer;
  chars = 0;

  /* Increase start until we reach start_char.  */
  while (chars < start_char)
    {
      if (start >= buffer->buffer + buffer->size)
	{
	  DebugPrint ("start %p out of bounds %p",
		      start, buffer->buffer + buffer->size);

	  /* start is out of bounds.  */
	  goto failure;
	}

      /* After this, rc should either be -1 (meaning failure) or the
	 number of bytes read.  */
      rc = mbtowc (&wc, start, buffer->buffer + buffer->size - start);
      chars++;

      DebugPrint ("mbtowc gave (calculating start) %d", rc);

      /* If rc is not -1, move start forward by that much.  */
      if (rc != -1)
	start += rc;
      else
        goto failure;
    }

  DebugPrint ("chars: %d, start, %p", chars, start);

  /* Now, start is the first byte of the area we want to delete.
     Count forward by length.  */
  end = start;
  old_chars = chars;

  while (chars < old_chars + length)
    {
      if (end >= buffer->buffer + buffer->size)
	{
	  DebugPrint ("end %p out of bounds %p",
		      end, buffer->buffer + buffer->size);

	  /* end is out of bounds.  */
	  goto failure;
	}

      /* After this, rc should either be -1 (meaning failure) or the
	 number of bytes read.  */
      rc = mbtowc (&wc, end, buffer->buffer + buffer->size - end);
      chars++;

      DebugPrint ("mbtowc gave (calculating end) %d", rc);

      /* If rc is not -1, move start forward by that much.  */
      if (rc != -1)
        end += rc;
      else
        goto failure;
    }

  DebugPrint ("chars: %d, end, %p", chars, end);

  /* Now, delete the area between start and end, by moving the bytes
     between end and the end of the buffer to start.  */
  memmove (start, end, buffer->buffer + buffer->size - end);

  /* Resize the buffer.  */
  buffer->size -= end - start;
  buffer->total_characters -= length;
  XLAssert (buffer->size >= 0);

  buffer->buffer = XLRealloc (buffer->buffer,
			      buffer->size);

  /* Restore the locale and return success.  */
  setlocale (LC_CTYPE, oldlocale);
  XLFree (oldlocale);

  /* Reset the shift state.  */
  mbtowc (NULL, NULL, 0);
  return True;

 failure:
  setlocale (LC_CTYPE, oldlocale);
  XLFree (oldlocale);

  /* Reset the shift state.  */
  mbtowc (NULL, NULL, 0);
  return False;
}

static Bool
PreeditInsertChars (PreeditBuffer *buffer, int start_char,
		    const char *string, size_t length,
		    int char_length)
{
  wchar_t wc;
  char *oldlocale, *start;
  int rc, chars;

  /* Note that preedit buffers operate on text encoded with the IM
     locale's charset.  Record the old locale.  */
  oldlocale = XLStrdup (setlocale (LC_CTYPE, NULL));

  /* Switch to the new locale.  */
  if (!setlocale (LC_CTYPE, buffer->locale))
    {
      XLFree (oldlocale);
      return False;
    }

  /* Resize the buffer accordingly.  */
  buffer->buffer = XLRealloc (buffer->buffer,
			      buffer->size + length);

  start = buffer->buffer;
  chars = 0;

  /* Increase start until we reach start_char.  */
  while (chars < start_char)
    {
      if (start >= buffer->buffer + buffer->size)
	/* start is out of bounds.  */
	goto failure;

      /* After this, rc should either be -1 (meaning failure) or the
	 number of bytes read.  */
      rc = mbtowc (&wc, start, buffer->buffer + buffer->size - start);
      chars++;

      /* If rc is not -1, move start forward by that much.  */
      if (rc != -1)
	start += rc;
      else
        goto failure;
    }

  /* Move everything past start length away.  */
  memmove (start + length, start,
	   buffer->buffer + buffer->size - start);
  buffer->size += length;
  buffer->total_characters += char_length;

  /* Copy the text onto start.  */
  memcpy (start, string, length);

  setlocale (LC_CTYPE, oldlocale);
  XLFree (oldlocale);

  /* Reset the shift state.  */
  mbtowc (NULL, NULL, 0);
  return True;

 failure:
  setlocale (LC_CTYPE, oldlocale);
  XLFree (oldlocale);

  /* Reset the shift state.  */
  mbtowc (NULL, NULL, 0);
  return False;
}

/* Forward declarations.  */
static char *ConvertString (char *, size_t, size_t *);
static void PreeditString (TextInput *, const char *, size_t, ptrdiff_t);

static void
UpdatePreedit (TextInput *input)
{
  char *buffer;
  size_t new_text_size;
  TextPosition caret;

  if (input->buffer)
    {
      /* Convert the preedit text.  */
      buffer = ConvertString (input->buffer->buffer,
			      input->buffer->size,
			      &new_text_size);
      DebugPrint ("updated buffer %p", buffer);

      if (!buffer)
	goto no_buffer;

      /* Obtain the caret position.  */

      if (input->caret_style != XIMIsInvisible)
	caret = TextPositionFromCharPosition (buffer, new_text_size,
					      input->caret);
      else
	/* The caret is hidden, so don't send any caret position.  */
	caret.bytepos = -1, caret.charpos = -1;

      DebugPrint ("caret position is: char %d, byte: %td",
		  caret.charpos, caret.bytepos);

      PreeditString (input, buffer, new_text_size,
		     /* caret.bytepos will be -1 if obtaining the
			position failed or the caret is hidden.  */
		     caret.bytepos);
      XLFree (buffer);
    }
  else
    {
    no_buffer:
      DebugPrint ("no buffer");

      /* Clear the preedit string.  */
      zwp_text_input_v3_send_preedit_string (input->resource, NULL,
					     -1, -1);
      zwp_text_input_v3_send_done (input->resource, input->serial);
    }
}

static int
PreeditStartCallback (XIC ic, XPointer client_data, XPointer call_data)
{
  TextInput *input;
  const char *locale;

  XLAssert (current_xim != NULL);

  input = (TextInput *) client_data;
  locale = XLocaleOfIM (current_xim);

  DebugPrint ("text input: %p; locale: %s", input, locale);

  if (input->buffer)
    FreePreeditBuffer (input->buffer);

  /* Create the preedit buffer.  */
  input->buffer = MakePreeditBuffer (locale);

  /* Set the default caret style.  */
  input->caret_style = XIMIsPrimary;

  /* There should be no limit on the number of bytes in a preedit
     string.  We make the string fit in 4000 bytes ourselves.  */
  return -1;
}

static void
PreeditDoneCallback (XIC ic, XPointer client_data, XPointer call_data)
{
  TextInput *input;

  input = (TextInput *) client_data;
  DebugPrint ("text input: %p", input);

  if (input->buffer)
    FreePreeditBuffer (input->buffer);
  input->buffer = NULL;

  /* Send change to the client.  */
  UpdatePreedit (input);
}

static char *
ConvertWcharString (PreeditBuffer *buffer, const wchar_t *input,
		    size_t input_size, size_t *string_size)
{
  char *output, *oldlocale;
  int rc;
  size_t bytes;

  /* Since the text is intended for BUFFER, switch to BUFFER's
     locale.  */
  oldlocale = XLStrdup (setlocale (LC_CTYPE, NULL));

  /* Switch to the new locale.  */
  if (!setlocale (LC_CTYPE, buffer->locale))
    {
      /* Setting the locale failed.  Return an empty string.  */
      XLFree (oldlocale);
      *string_size = 0;
      return NULL;
    }

  output = XLCalloc (input_size + 1, MB_CUR_MAX);
  bytes = 0;

  while (input_size)
    {
      input_size--;
      rc = wctomb (output + bytes, *input++);

      if (rc == -1)
	/* Invalid wide character code.  */
	continue;

      /* Otherwise, move the string forward this much.  */
      bytes += rc;
    }

  /* Return the string and the number of bytes put in it.  */
  *string_size = bytes;

  /* Clear shift state.  */
  wctomb (NULL, L'\0');

  /* Restore the old locale.  */
  setlocale (LC_CTYPE, oldlocale);
  XLFree (oldlocale);
  return output;
}

static void
PreeditDrawCallback (XIC ic, XPointer client_data,
		     XIMPreeditDrawCallbackStruct *call_data)
{
  TextInput *input;
  size_t string_size;
  char *multi_byte_string;

  input = (TextInput *) client_data;
  DebugPrint ("text input: %p", input);

  if (!input->buffer)
    return;

  DebugPrint ("chg_first: %d, chg_length: %d",
	      call_data->chg_first,
	      call_data->chg_length);

  /* Delete text between chg_first and chg_first + chg_length.  */
  if (call_data->chg_length
      && !PreeditDeleteChars (input->buffer, call_data->chg_first,
			      call_data->chg_length))
    {
      DebugPrint ("text deletion failed");
      return;
    }

  if (call_data->text)
    {
      if (call_data->text->encoding_is_wchar)
	{
	  DebugPrint ("converting wide character string");

	  multi_byte_string
	    = ConvertWcharString (input->buffer,
				  call_data->text->string.wide_char,
				  call_data->text->length,
				  &string_size);
	}
      else
	{
	  /* The multibyte string should be NULL terminated.  */
	  string_size = strlen (call_data->text->string.multi_byte);
	  multi_byte_string = call_data->text->string.multi_byte;
	}

      DebugPrint ("inserting text of size %d, %zu",
		  call_data->text->length, string_size);

      /* Now, insert whatever text was specified at chg_first.  */
      if (!PreeditInsertChars (input->buffer, call_data->chg_first,
			       multi_byte_string, string_size,
			       call_data->text->length))
	DebugPrint ("insertion failed");

      if (call_data->text->encoding_is_wchar)
	/* We must free the conversion results.  */
	XLFree (multi_byte_string);
    }

  /* Now set the caret position.  */
  input->caret = call_data->caret;

  DebugPrint ("buffer text is now: %.*s, with the caret at %d",
	      (int) input->buffer->size, input->buffer->buffer,
	      input->caret);

  /* Send change to the client.  */
  UpdatePreedit (input);
}

static void
PreeditCaretCallback (XIC ic, XPointer client_data,
		      XIMPreeditCaretCallbackStruct *call_data)
{
  TextInput *input;

  input = (TextInput *) client_data;

  if (!input->buffer)
    return;

  DebugPrint ("text input: %p; direction: %u", input,
	      call_data->direction);

  switch (call_data->direction)
    {
    case XIMAbsolutePosition:
      input->caret = call_data->position;
      break;

    case XIMForwardChar:
      input->caret = MIN (input->caret + 1,
			  input->buffer->total_characters);
      break;

    case XIMBackwardChar:
      input->caret = MAX (input->caret - 1, 0);
      break;

      /* The rest cannot be implemented under Wayland as the text
	 input protocol is too limited.  */
    default:
      DebugPrint ("unsupported movement direction");
    }

  /* Return the caret position.  */
  call_data->position = input->caret;

  /* Set the caret style.  */
  input->caret_style = call_data->style;

  /* Send change to the client.  */
  UpdatePreedit (input);
}

static TextPosition
ScanForwardWord (const char *string, size_t string_size,
		 TextPosition caret, int factor)
{
  const char *start;
  Bool punct_found;
  TextPosition caret_before;

  start = string + caret.bytepos;

  /* Skip initial whitespace.  */
  while (start < string + string_size
	 /* Make sure the character has 0 trailing bytes.  */
	 && *start < 127 && *start >= 0
	 && (isspace (*start) || ispunct (*start)))
    {
      start++;
      caret.charpos++;
      caret.bytepos++;
    }
  
  while (start < string + string_size)
    {
      punct_found = False;
      caret_before = caret;

      start += CountOctets (*start);

      if (start >= string + string_size)
	{
	  /* The string is too big.  */
	  caret.bytepos = -1;
	  caret.charpos = -1;

	  return caret;
	}
      else
	caret.bytepos = start - string;

      caret.charpos++;

      /* Eat all punctuation.  */
      while (isspace (*start) || ispunct (*start))
	{
	  punct_found = True;

	  if (++start >= string + string_size)
	    /* We are now at the end of the string, so just return the
	       position of caret_before, which should be before this
	       extraneous punctuation.  */
	    return caret_before;

	  /* Move the caret forward.  */
	  caret.charpos++;
	  caret.bytepos++;
	}

      if (punct_found && !(--factor))
	{
	  /* Punctuation was seen and factor is now 0.  Return the
	     caret before the punctuation.  */
	  DebugPrint ("returning caret_before: char: %d byte: %td",
		      caret_before.charpos, caret_before.bytepos);
	  return caret_before;
	}

      /* Simply return the current position at the end of the
	 string.  */
      if (start == string + string_size - 1)
	{
	  DebugPrint ("returning caret_before at end of string: char: %d byte: %td",
		      caret_before.charpos, caret_before.bytepos);

	  return caret_before;
	}
    }

  return caret;
}

static Bool
IsLeading (char c)
{
  return (((unsigned char) c) & 0b11000000
	  || !(c >> 7));
}

static TextPosition
ScanBackwardWord (const char *string, size_t string_size,
		  TextPosition caret, int factor)
{
  TextPosition original, caret_before;
  const char *start;
  Bool punct_found;

  /* Record the original caret position.  */
  original = caret;

  if (!string_size)
    {
      /* The string is empty, so simply return the start of the
	 string.  */
      caret.charpos = 0;
      caret.bytepos = 0;
      return caret;
    }

  /* First, skip all whitespace.  */
  start = string + caret.bytepos;
  while (start >= string
	 /* Make sure the character has 0 trailing bytes.  */
	 && *start < 127 && *start >= 0
	 && (isspace (*start) || ispunct (*start)))
    {
      start--;
      caret.charpos--;
      caret.bytepos--;

      if (caret.charpos <= 0 || caret.bytepos <= 0)
	return original;
    }

  /* Next, look backwards.  Every time whitespace is encountered,
     gobble it up, and decrease factor.  Once factor is 0, return the
     caret position before the first whitespace character.  Otherwise,
     repeat.  */

  while (start >= string)
    {
      caret_before = caret;

      do
	{
	  if (--start < string)
	    {
	      /* Invalid UTF-8 data was found in STRING.  Just return
		 the start of the string in this case.  */
	      caret.charpos = 0;
	      caret.bytepos = 0;
	      return caret;
	    }

	  caret.bytepos--;
	}
      while (!IsLeading (*start));

      caret.charpos--;

      DebugPrint ("caret_before: char: %d byte: %td, new char: %c",
		  caret_before.charpos, caret_before.bytepos,
		  *start);

      /* We are now at the start of the last character.  If it is
	 whitespace, eat the whitespace and decrease factor.  */

      punct_found = False;

      while (isspace (*start) || ispunct (*start))
	{
	  do
	    {
	      if (--start < string)
		{
		  /* Invalid UTF-8 data was found in STRING.  Just return
		     the start of the string in this case.  */
		  caret.charpos = 0;
		  caret.bytepos = 0;
		  return caret;
		}

	      caret.bytepos--;
	    }
	  while (!IsLeading (*start));
	  caret.charpos--;

	  punct_found = True;
	}

      if (punct_found && !(--factor))
	{
	  /* Punctuation was seen and the factor is now 0.  Return the
	     caret before the punctuation.  */
	  DebugPrint ("returning caret_before: char: %d byte: %td",
		      caret_before.charpos, caret_before.bytepos);
	  return caret_before;
	}
    }
  
  return caret;
}

static void
FindTextSections (const char *string, size_t string_size,
		  TextPosition caret, XIMCaretDirection direction,
		  int factor, TextPosition *start_return,
		  TextPosition *end_return)
{
  TextPosition end;
  const char *found;

  switch (direction)
    {
    case XIMForwardChar:
      /* Move forward by factor.  */
      end = TextPositionFromCharPosition (string, string_size,
					  caret.charpos + factor);
      break;

    case XIMBackwardChar:
      /* Move backwards by factor.  */
      end = TextPositionFromCharPosition (string, string_size,
					  MAX (0, caret.charpos - factor));
      break;

    case XIMForwardWord:
      /* Move forwards by factor words.  */
      end = ScanForwardWord (string, string_size, caret, factor);
      break;

    case XIMBackwardWord:
      /* Move backwards by factor words.  */
      end = ScanBackwardWord (string, string_size, caret, factor);
      break;

    case XIMLineStart:
      /* Scan backwards for factor newline characters.  */

      found = string + caret.bytepos;
      DebugPrint ("start: found %p, found-string %td",
		  found, found - string);

      while (factor)
	{
	  found = memrchr (string, '\n', found - string);
	  DebugPrint ("LineStart processing found %p %zd", found,
		      found - string);

	  if (!found)
	    {
	      /* Use the beginning of the string. */
	      found = string - 1;

	      /* Exit the loop too.  */
	      goto end_line_start;
	    }

	  factor--;
	}

    end_line_start:
      DebugPrint ("found %p string %p found+1-string %td",
		  found, string, found + 1 - string);
      end = TextPositionFromBytePosition (string, string_size,
					  found + 1 - string);
      break;

    case XIMLineEnd:
      /* Scan forwards for factor newline characters.  */
      found = string + caret.bytepos;

      while (factor)
	{
	  found = memchr (found + 1, '\n',
			  (string + string_size - 1) - found + 1);

	  if (!found)
	    {
	      /* Use the end of the string.  */
	      found = string + string_size - 1;
	      goto end_line_end;
	    }

	  factor--;
	}

    end_line_end:
      end = TextPositionFromBytePosition (string, string_size,
					  found - 1 - string);
      break;

    default:
      DebugPrint ("unsuported string conversion direction: %u",
		  direction);
      end.bytepos = 0;
      end.charpos = 0;
    }

  DebugPrint ("end: char: %d byte: %td", end.charpos,
	      end.bytepos);

  if (caret.charpos > end.charpos)
    *start_return = end, *end_return = caret;
  else
    *start_return = caret, *end_return = end;
}

static Bool
MoveCaret (TextPosition *caret, const char *buffer, size_t buffer_size,
	   int by)
{
  const char *end, *start;
  int octets;

  XLAssert (caret->bytepos <= buffer_size);

  if (by > 0)
    {
      end = buffer + buffer_size;
      buffer += caret->bytepos;

      while (by && buffer < end)
	{
	  octets = CountOctets (*buffer);

	  /* Move the buffer and text position forwards.  */
	  buffer += octets;
	  caret->bytepos += octets;
	  caret->charpos++;
	  by--;
	}

      /* If caret->bytepos is too large, return failure.  */
      if (buffer > end)
	return False;
    }
  else if (by < 0)
    {
      /* Move the buffer and text position backwards.  */

      start = buffer + caret->bytepos;
      while (by && start >= buffer)
	{
	  do
	    {
	      start--;
	      caret->bytepos -= 1;

	      if (start < buffer)
		return False;
	    }
	  while (!IsLeading (*start));

	  caret->charpos--;
	  by--;
	}
    }

  return True;
}

static char *
EncodeIMString (const char *input, size_t input_size, int *chars)
{
  iconv_t cd;
  char *oldlocale, *locale;
  size_t rc;
  ptrdiff_t size;
  char *outbuf, *outptr, *end;
  size_t outsize, outbytesleft;
  int nchars, num_chars_read;
  wchar_t wc;
  char *inbuf;

  /* Encode the given input string in the IM coding system, and then
     return a NULL terminated buffer and the number of characters, or
     NULL if the conversion failed.  */
  DebugPrint ("encoding string %.*s", (int) input_size, input);

  /* Switch to the input method locale.  */
  locale = XLocaleOfIM (current_xim);
  oldlocale = XLStrdup (setlocale (LC_CTYPE, NULL));

  if (!setlocale (LC_CTYPE, locale))
    {
      /* Switching to the new locale failed.  */
      XLFree (oldlocale);
      return NULL;
    }

  /* First, try creating a conversion descriptor.  */
  cd = iconv_open (nl_langinfo (CODESET), "UTF-8");

  /* If creating the cd failed, bail out.  */
  if (cd == (iconv_t) -1)
    {
      /* Restore the old locale.  */
      if (!setlocale (LC_CTYPE, oldlocale))
	abort ();

      XLFree (oldlocale);
      return NULL;
    }

  /* Otherwise, start converting.  */
  outbuf = XLMalloc (BUFSIZ + 1);
  outptr = outbuf;
  outsize = BUFSIZ;
  outbytesleft = outsize;
  inbuf = (char *) input;

  while (input_size > 0)
    {
      rc = iconv (cd, &inbuf, &input_size, &outptr,
		  &outbytesleft);
      DebugPrint ("iconv gave: %tu", rc);

      if (rc == (size_t) -1)
	{
	  /* See what went wrong.  */
	  if (errno == E2BIG)
	    {
	      /* Reallocate the output buffer.  */
	      outbuf = XLRealloc (outbuf, outsize + BUFSIZ + 1);

	      /* Move the outptr to the right location in the new
		 outbuf.  */
	      outptr = outbuf + outsize - outbytesleft;

	      /* Expand outsize and outbytesleft.  */
	      outsize += BUFSIZ;
	      outbytesleft += BUFSIZ;

	      DebugPrint ("expanding outsize to %tu, outbytesleft now %tu",
			  outsize, outbytesleft);
	    }
	  else
	    {
	      /* An error occured while encoding the string.
		 Normally, this is not such a big deal, but the number
		 of characters in the string is later counted with
		 mbtowc.  So, simply bail out.  */
	      DebugPrint ("iconv failed: %s", strerror (errno));
	      XLFree (outbuf);
	      iconv_close (cd);
	      /* Restore the old locale.  */
	      if (!setlocale (LC_CTYPE, oldlocale))
		abort ();

	      XLFree (oldlocale);
	      return NULL;
	    }
	}
    }

  /* The conversion finished.  */
  DebugPrint ("conversion finished, size_out %tu",
	      outsize - outbytesleft);

  /* Now, count the number of multibyte characters.  */
  nchars = 0;
  end = outbuf;
  size = outsize - outbytesleft;

  while (end < outbuf + size)
    {
      num_chars_read = mbtowc (&wc, end, outbuf + size - end);
      nchars++;

      if (num_chars_read != -1)
	end += num_chars_read;
      else
	{
	  DebugPrint ("mbtowc failed");

	  XLFree (outbuf);
	  iconv_close (cd);
	  /* Restore the old locale.  */
	  if (!setlocale (LC_CTYPE, oldlocale))
	    abort ();

	  XLFree (oldlocale);
	  return NULL;
	}
    }

  /* Reset the shift state and return the number of characters.  */
  mbtowc (NULL, NULL, 0);
  *chars = nchars;

  /* Close the cd.  */
  iconv_close (cd);

  /* Restore the old locale.  */
  if (!setlocale (LC_CTYPE, oldlocale))
    abort ();
  XLFree (oldlocale);

  /* Return the output buffer.  */
  return outbuf;
}

static void
StringConversionCallback (XIC ic, XPointer client_data,
			  XIMStringConversionCallbackStruct *call_data)
{
  TextInput *input;
  TextPosition start, end, caret;
  short position;
  size_t length;
  char *buffer;
  int num_characters;
  int bytes_before, bytes_after;

  input = (TextInput *) client_data;

  /* Clear some members of the returned text structure.  */
  call_data->text->feedback = NULL;
  call_data->text->encoding_is_wchar = False;

  if (!(input->current_state.pending & PendingSurroundingText))
    return;

  DebugPrint ("string conversion; position: %d, factor: %d"
	      " operation: %u", (short) call_data->position,
	      call_data->factor, call_data->operation);

  /* Obtain the actual caret position.  */
  caret = input->current_state.cursor;
  DebugPrint ("current caret position: char: %d, byte: %td",
	      caret.charpos, caret.bytepos);

  if (caret.charpos < 0 || caret.bytepos < 0)
    goto failure;

  /* This is unsigned short in Xlib.h but the spec says it should be
     signed.  */
  position = (short) call_data->position;

  /* Move the caret by position.  */
  length = strlen (input->current_state.surrounding_text);

  /* If the string is too small, just fail.  */
  if (!length)
    goto failure;

  if (!MoveCaret (&caret, input->current_state.surrounding_text,
		  length, position))
    {
      DebugPrint ("failed to move caret position");
      goto failure;
    }

  if (call_data->factor < 1)
    goto failure;

  DebugPrint ("new caret position: char %d, byte: %td",
	      caret.charpos, caret.bytepos);

  /* Now, obtain the start and end of the text to return.  */
  FindTextSections (input->current_state.surrounding_text,
		    length, caret, call_data->direction,
		    call_data->factor, &start, &end);

  DebugPrint ("start: %d, %td, end: %d, %td",
	      start.charpos, start.bytepos, end.charpos,
	      end.bytepos);

  /* If either of those positions are invalid, signal failure.  */
  if (start.charpos < 0 || start.bytepos < 0
      || end.charpos < 0 || end.bytepos < 0)
    goto failure;

  /* Verify that some assumptions hold.  */
  XLAssert (start.bytepos <= end.bytepos && end.bytepos < length);

  /* Extract and encode the contents of the string.  */
  buffer = EncodeIMString ((input->current_state.surrounding_text
			    + start.bytepos),
			   end.bytepos - start.bytepos + 1,
			   &num_characters);

  /* Return those characters.  */

  if (buffer)
    {
      call_data->text->length = MIN (USHRT_MAX, num_characters);
      call_data->text->string.mbs = buffer;
    }
  else
    goto failure;

  DebugPrint ("returned text: %s", buffer);

  if (call_data->operation == XIMStringConversionSubstitution)
    {
      /* Also tell the client to delete the extracted part of the
	 buffer.  First, calculate how much start extends behind the
	 cursor.  This is an approximation; it assumes that the
	 portion of text to change always contains the caret, which is
	 not guaranteed to be the case if the IM specified an offset,
	 direction, and factor that resulted in none of the text
	 between start and end containing the caret.  */
      caret = input->current_state.cursor;

      if (start.bytepos < caret.bytepos)
	bytes_before = caret.bytepos - start.bytepos;
      else
	bytes_before = 0;

      if (end.bytepos > caret.bytepos)
	bytes_after = end.bytepos - caret.bytepos;
      else
	bytes_after = 0;

      DebugPrint ("deleting: %d %d", bytes_before, bytes_after);

      zwp_text_input_v3_send_delete_surrounding_text (input->resource,
						      bytes_before,
						      bytes_after);
      zwp_text_input_v3_send_done (input->resource, input->serial);
    }

  return;

 failure:
  /* Return a string of length 0.  This assumes XFree is able to free
     data allocated with our malloc wrapper.  */
  call_data->text->length = 0;
  call_data->text->string.mbs = XLMalloc (0);
}

static void
CreateIC (TextInput *input)
{
  XVaNestedList status_attr, preedit_attr;
  XPoint spot;
  XRectangle rect;
  Window window;
  XIMCallback preedit_start_callback;
  XIMCallback preedit_draw_callback;
  XIMCallback preedit_done_callback;
  XIMCallback preedit_caret_callback;
  XIMCallback string_conversion_callback;
  unsigned long additional_events;

  if (!current_xim)
    return;

  if (!input->client_info)
    return;

  if (!input->client_info->focus_surface)
    return;

  window = XLWindowFromSurface (input->client_info->focus_surface);

  if (!window)
    return;

  XLAssert (!input->xic);

  DebugPrint ("creating XIC for text input %p", input);

  status_attr = NULL;
  preedit_attr = NULL;

  /* Create an XIC for the given text input.  */
  if (xim_style & XIMPreeditPosition)
    {
      DebugPrint ("IM wants spot values for preedit window");

      if (input->current_state.pending & PendingCursorRectangle)
	{
	  spot.x = CurrentCursorX (input);
	  spot.y = (CurrentCursorY (input)
		    + CurrentCursorHeight (input));
	}
      else
	{
	  spot.x = 0;
	  spot.y = 1;
	}

      DebugPrint ("using spot: %d, %d", spot.x, spot.y);
      preedit_attr = XVaCreateNestedList (0, XNSpotLocation, &spot,
					  XNFontSet, im_fontset, NULL);
    }
  else if (xim_style & XIMPreeditArea)
    {
      DebugPrint ("IM wants geometry negotiation");

      /* Use some dummy values, and then negotiate geometry after the
	 XIC is created.  */
      rect.x = 0;
      rect.y = 0;
      rect.height = 1;
      rect.width = 1;

      preedit_attr = XVaCreateNestedList (0, XNArea, &rect, XNFontSet,
					  im_fontset, NULL);
    }
  else if (xim_style & XIMPreeditCallbacks)
    {
      DebugPrint ("IM wants preedit callbacks");

      preedit_start_callback.client_data = (XPointer) input;
      preedit_done_callback.client_data = (XPointer) input;
      preedit_draw_callback.client_data = (XPointer) input;
      preedit_caret_callback.client_data = (XPointer) input;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
      preedit_start_callback.callback = (XIMProc) PreeditStartCallback;
      preedit_done_callback.callback = (XIMProc) PreeditDoneCallback;
      preedit_draw_callback.callback = (XIMProc) PreeditDrawCallback;
      preedit_caret_callback.callback = (XIMProc) PreeditCaretCallback;
#pragma GCC diagnostic pop

      preedit_attr = XVaCreateNestedList (0, XNPreeditStartCallback,
					  &preedit_start_callback,
					  XNPreeditDoneCallback,
					  &preedit_done_callback,
					  XNPreeditDrawCallback,
					  &preedit_draw_callback,
					  XNPreeditCaretCallback,
					  &preedit_caret_callback,
					  NULL);
    }

  if (xim_style & XIMStatusArea)
    {
      DebugPrint ("IM wants geometry negotiation for status area");

      /* Use some dummy values, and then negotiate geometry after the
	 XIC is created.  */
      rect.x = 0;
      rect.y = 0;
      rect.height = 1;
      rect.width = 1;

      status_attr = XVaCreateNestedList (0, XNArea, &rect, XNFontSet,
					 im_fontset, NULL);
    }

  DebugPrint ("preedit attr: %p, status attr: %p",
	      preedit_attr, status_attr);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
  string_conversion_callback.client_data = (XPointer) input;
  string_conversion_callback.callback = (XIMProc) StringConversionCallback;
#pragma GCC diagnostic pop

  if (preedit_attr && status_attr)
    input->xic = XCreateIC (current_xim, XNInputStyle, xim_style,
			    XNClientWindow, window, XNFocusWindow,
			    window, XNStatusAttributes, status_attr,
			    XNPreeditAttributes, preedit_attr,
			    XNStringConversionCallback,
			    &string_conversion_callback,
			    NULL);
  else if (preedit_attr)
    input->xic = XCreateIC (current_xim, XNInputStyle, xim_style,
			    XNClientWindow, window, XNFocusWindow,
			    window, XNPreeditAttributes, preedit_attr,
			    XNStringConversionCallback,
			    &string_conversion_callback,
			    NULL);
  else if (status_attr)
    input->xic = XCreateIC (current_xim, XNInputStyle, xim_style,
			    XNClientWindow, window, XNFocusWindow,
			    window, XNStatusAttributes, status_attr,
			    XNStringConversionCallback,
			    &string_conversion_callback,
			    NULL);
  else
    input->xic = XCreateIC (current_xim, XNInputStyle, xim_style,
			    XNClientWindow, window, XNFocusWindow,
			    window, XNStringConversionCallback,
			    &string_conversion_callback,
			    NULL);

  /* Select for additional events should the IC have been successfully
     created.  Note that we do not deselect for the extra event mask
     anywhere; the events an input method makes us select for should
     be benign enough.  */

  if (input->xic)
    {
      additional_events = NoEventMask;

      if (!XGetICValues (input->xic, XNFilterEvents,
			 &additional_events, NULL)
	  && additional_events)
	{
	  DebugPrint ("selecting for additional event mask: %lx",
		      additional_events);

	  XLSurfaceSelectExtraEvents (input->client_info->focus_surface,
				      additional_events);
	}
    }

  /* Free the nested lists.  */
  if (status_attr)
    XFree (status_attr);

  if (preedit_attr)
    XFree (preedit_attr);

  DebugPrint ("created IC %p", input->xic);
}

static void
IMDestroyCallback (XIM im, XPointer client_data, XPointer call_data)
{
  TextInputClientInfo *info;
  TextInput *input;

  DebugPrint ("XIM %p destroyed", im);

  if (im != current_xim)
    /* Is this even possible?  */
    return;

  /* The XIM was destroyed, and all XICs have been freed.  Clear all
     fields still referencing XICs or XIMs.  */

  current_xim = NULL;

  /* Close the cd.  */
  if (current_cd != (iconv_t) -1)
    iconv_close (current_cd);
  current_cd = (iconv_t) -1;

  /* Clear the XIC field of each input.  */

  info = all_client_infos.next;
  while (info != &all_client_infos)
    {
      input = info->inputs.next;
      while (input != &info->inputs)
	{
	  /* Destroy the XIC of this one input.  */
	  if (input->xic)
	    {
	      input->xic = NULL;

	      /* Handle IC destruction.  */
	      HandleICDestroyed (input);
	    }

	  /* Move to the next input.  */
	  input = input->next;
	}

      /* Move to the next client info.  */
      info = info->next;
    }

  DebugPrint ("finished XIM destruction");
}

static XIMStyle
CheckStyle (XIMStyles *styles, XIMStyle preedit_style,
	    XIMStyle status_style)
{
  int i;

  /* Is this preedit & status style combination supported? */
  for (i = 0; i < styles->count_styles; i++)
    {
      if ((styles->supported_styles[i] & preedit_style)
	  && (styles->supported_styles[i] & status_style))
	return styles->supported_styles[i];
    }

  return 0;
}

static void
CheckStyles (XIM xim)
{
  XIMStyles *styles;
  XIMStyle style;
  int i;

  /* Pick a supported XIM style from the current input method.  The
     following input styles are supported:

     over-the-spot, where the preedit is displayed in a window at a
     given position.

     off-the-spot, where the preedit is displayed in a window
     somewhere inside the application window.

     root-window, where the preedit is displayed is displayed in a
     window that is a child of the root window.

     on-the-spot, where the preedit is displayed inside the
     application window.  */

  if (XGetIMValues (xim, XNQueryInputStyle, &styles, NULL))
    {
      /* An error occured; default to none.  */
      xim_style = XIMPreeditNone | XIMStatusNone;
      return;
    }

  /* Otherwise, find the best style in our order of preference.  */
  for (i = 0; xim_style_order[i] != XimStyleNone; ++i)
    {
      DebugPrint ("considering style: %u", xim_style_order[i]);

      switch (xim_style_order[i])
	{
	case XimOverTheSpot:
	  DebugPrint ("checking for over-the-spot");
	  style = CheckStyle (styles, XIMPreeditPosition,
			      XIMStatusArea | XIMStatusNothing | XIMStatusNone);
	  if (style)
	    goto done;
	  break;

	case XimOffTheSpot:
	  DebugPrint ("checking for off-the-spot");
	  style = CheckStyle (styles, XIMPreeditArea,
			      XIMStatusArea | XIMStatusNothing | XIMStatusNone);
	  if (style)
	    goto done;
	  break;

	case XimRootWindow:
	  DebugPrint ("checking for root-window");
	  style = CheckStyle (styles, XIMPreeditNothing,
			      XIMStatusNothing | XIMStatusNone);
	  if (style)
	    goto done;
	  break;

	case XimOnTheSpot:
	  DebugPrint ("checking for on-the-spot");
	  style = CheckStyle (styles, XIMPreeditCallbacks,
			      XIMStatusArea | XIMStatusNothing | XIMStatusNone);
	  if (style)
	    goto done;
	  break;

	case XimStyleNone:
	  /* This shouldn't happen.  */
	  abort ();
	}
    }

  DebugPrint ("checking for input method styles failed");
  /* No style could be found, so fall back to XIMPreeditNone and
     XIMStatusNone.  */
  style = XIMPreeditNone | XIMStatusNone;
 done:
  DebugPrint ("set styles to: %lu", (unsigned long) style);
  XFree (styles);
  xim_style = style;
}

static void
HandleNewIM (XIM xim)
{
  TextInputClientInfo *info;
  TextInput *input;
  const char *locale;
  char *oldlocale, *coding;
  iconv_t cd;
  XIMCallback destroy_callback;

  /* A new input method is available; destroy the XIC of every text
     input.  */
  info = all_client_infos.next;
  while (info != &all_client_infos)
    {
      input = info->inputs.next;
      while (input != &info->inputs)
	{
	  /* Destroy the XIC of this one input.  */
	  if (input->xic)
	    {
	      XDestroyIC (input->xic);
	      input->xic = NULL;

	      /* Handle IC destruction.  */
	      HandleICDestroyed (input);
	    }

	  /* Move to the next input.  */
	  input = input->next;
	}

      /* Move to the next client info.  */
      info = info->next;
    }

  /* Now, it is okay to delete the current XIM.  */
  if (current_xim)
    XCloseIM (current_xim);
  current_xim = NULL;

  /* And its cd.  */
  if (current_cd != (iconv_t) -1)
    iconv_close (current_cd);
  current_cd = (iconv_t) -1;

  /* Obtain the locale of the new input method.  */
  locale = XLocaleOfIM (xim);

  /* Temporarily switch to the new locale to determine its coded
     character set.  */
  oldlocale = XLStrdup (setlocale (LC_ALL, NULL));

  if (!setlocale (LC_ALL, locale))
    {
      /* The locale specified by the input method couldn't be set.  */
      XLFree (oldlocale);
      goto bad_locale;
    }

  /* Now we are in the input method locale.  Obtain the codeset.  */
  coding = XLStrdup (nl_langinfo (CODESET));

  /* Switch back to the new locale.  */
  if (!setlocale (LC_ALL, oldlocale))
    abort ();

  DebugPrint ("input method coding system is %s", coding);

  /* Create a character conversion context for input data.  */
  cd = iconv_open ("UTF-8", coding);

  /* Free the new data.  */
  XLFree (oldlocale);
  XLFree (coding);

  /* If cd creation failed, assume it isn't supported.  */
  if (cd == (iconv_t) -1)
    goto bad_locale;

  DebugPrint ("conversion descriptor created to UTF-8");

  /* Now enable the input method and create XICs for all text inputs.
     Then, restore previous state.  */
  current_xim = xim;
  current_cd = cd;

  /* Attach the destroy callback to the XIM.  */
  destroy_callback.client_data = NULL;
  destroy_callback.callback = IMDestroyCallback;
  XSetIMValues (xim, XNDestroyCallback, &destroy_callback,
		NULL);

  /* Initialize the styles supported by this input method.  */
  CheckStyles (xim);

  /* A new input method is available; destroy the XIC of every text
     input.  */
  info = all_client_infos.next;
  while (info != &all_client_infos)
    {
      input = info->inputs.next;
      while (input != &info->inputs)
	{
	  /* Try to create the IC for this one input.  */
	  if (input->current_state.enabled
	      /* If this is NULL, then the IC will only be created
		 upon the next commit after the focus is actually
		 transferred to the text input.  */
	      && input->client_info->focus_surface)
	    {
	      CreateIC (input);

	      /* Focus the IC and do geometry allocation.  */
	      if (input->xic)
		XSetICFocus (input->xic);
	      DoGeometryAllocation (input);
	    }

	  /* Move to the next input.  */
	  input = input->next;
	}

      /* Move to the next client info.  */
      info = info->next;
    }

  return;

 bad_locale:
  XCloseIM (xim);
}

static void
IMInstantiateCallback (Display *display, XPointer client_data,
		       XPointer call_data)
{
  XIM newim;

  DebugPrint ("input method instantiated");

  /* Open the input method.  */
  newim = XOpenIM (compositor.display,
		   XrmGetDatabase (compositor.display),
		   (char *) compositor.resource_name,
		   (char *) compositor.app_name);

  /* Obtain its locale.  */
  if (newim)
    {
      DebugPrint ("created input method with locale: %s",
		  XLocaleOfIM (newim));
      HandleNewIM (newim);
    }
  else
    DebugPrint ("input method creation failed");
}

static void
FocusInCallback (Seat *seat, Surface *surface)
{
  TextInputClientInfo *info, *start;

  DebugPrint ("seat %p, surface %p", seat, surface);

  info = GetClientInfo (wl_resource_get_client (surface->resource),
			seat, False);

  if (info)
    {
      DebugPrint ("found seat client info; sending events");
      NoticeEnter (info, surface);
    }

  start = info ? info : &all_client_infos;
  info = start->next;

  /* Now, leave all of the other infos on the same seat.  */
  while (info != start)
    {
      if (info->seat == seat)
	NoticeLeave (info);

      /* Note that info->seat will be NULL for the sentinel node, so
	 the above comparison can never be true.  */
      info = info->next;
    }
}

static void
FocusOutCallback (Seat *seat)
{
  TextInputClientInfo *info;

  DebugPrint ("seat %p", seat);

  info = all_client_infos.next;
  while (info != &all_client_infos)
    {
      /* Leave the info if this is the same seat.  */
      if (info->seat == seat)
	NoticeLeave (info);

      info = info->next;
    }
}

static void
ConvertKeyEvent (XIDeviceEvent *xev, XEvent *event)
{
  /* Input methods cannot understand extension events, so filter an
     equivalent core event instead.  */

  memset (event, 0, sizeof *event);

  if (xev->evtype == XI_KeyPress)
    event->xkey.type = KeyPress;
  else
    event->xkey.type = KeyRelease;

  event->xkey.serial = xev->serial;
  event->xkey.send_event = xev->send_event;
  event->xkey.display = compositor.display;
  event->xkey.window = xev->event;
  event->xkey.root = xev->root;
  event->xkey.subwindow = xev->child;
  event->xkey.time = xev->time;
  event->xkey.state = ((xev->mods.effective & ~(1 << 13 | 1 << 14))
		       | (xev->group.effective << 13));
  event->xkey.keycode = xev->detail;
  event->xkey.x = xev->event_x;
  event->xkey.y = xev->event_y;
  event->xkey.x_root = xev->root_x;
  event->xkey.y_root = xev->root_y;

  if (xev->root == DefaultRootWindow (compositor.display))
    event->xkey.same_screen = True;

  /* Wayland clients don't expect to receive repeated key events,
     while input methods do.  However, there is no way to stuff the
     XIKeyRepeat flag into a core event.  Our saving graces are that:

       - the high two bits of a valid XID are not set.

       - event->xkey.subwindow is unused by all input methods.

       - it cannot be valid to actually query information from the
         subwindow, since it may no longer exist by the time the event
         is forwarded to the input method.

     As a result, it becomes possible to record that information by
     setting the high bit of the event subwindow for repeated key
     events.  */

  if (xev->flags & XIKeyRepeat)
    event->xkey.subwindow |= (1U << 31);
}

static char *
ConvertString (char *buffer, size_t nbytes, size_t *size_out)
{
  char *outbuf, *outptr;
  size_t outsize, outbytesleft, rc;

  outbuf = XLMalloc (BUFSIZ + 1);
  outptr = outbuf;
  outsize = BUFSIZ;
  outbytesleft = outsize;

  DebugPrint ("converting string of size %tu", nbytes);

  /* Reset the cd state.  */
  iconv (current_cd, NULL, NULL, &outptr, &outbytesleft);

  /* Start converting.  */
  while (nbytes > 0)
    {
      rc = iconv (current_cd, &buffer, &nbytes,
		  &outptr, &outbytesleft);

      DebugPrint ("iconv gave: %tu", rc);

      if (rc == (size_t) -1)
	{
	  /* See what went wrong.  */
	  if (errno == E2BIG)
	    {
	      /* Reallocate the output buffer.  */
	      outbuf = XLRealloc (outbuf, outsize + BUFSIZ + 1);

	      /* Move the outptr to the right location in the new
		 outbuf.  */
	      outptr = outbuf + outsize - outbytesleft;

	      /* Expand outsize and outbytesleft.  */
	      outsize += BUFSIZ;
	      outbytesleft += BUFSIZ;

	      DebugPrint ("expanding outsize to %tu, outbytesleft now %tu",
			  outsize, outbytesleft);
	    }
	  else
	    goto finish;
	}
    }

 finish:
  DebugPrint ("conversion finished, size_out %tu",
	      outsize - outbytesleft);

  /* Return outbuf and the number of bytes put in it.  */
  if (size_out)
    *size_out = outsize - outbytesleft;

  /* NULL-terminate the string.  */
  outbuf[outsize - outbytesleft] = '\0';

  return outbuf;
}

static void
PreeditString (TextInput *input, const char *buffer,
	       size_t buffer_size, ptrdiff_t cursor)
{
  char chunk[4000];
  const char *start, *end;
  int skip;
  const char *buffer_end;
  int cursor_pos;

  start = buffer;
  buffer_end = buffer + buffer_size;

  /* The Wayland protocol limits strings to 4000 bytes (including the
     terminating NULL).  Send the text as valid substrings consisting
     of less than 4000 bytes each.  */

  while (start < buffer_end)
    {
      end = start;

      while (true)
	{
	  skip = CountOctets (*end);

	  DebugPrint ("skip %d (%p+%d)", skip, end, skip);

	  if (end + skip - start >= 3998)
	    break;

	  if (end >= buffer_end)
	    break;

	  end += skip;
	}

      DebugPrint ("end-start (%p-%p): %zd", end, start,
		  end - start);

      /* Now, start to end contain a UTF-8 sequence less than 4000
	 bytes in length.  */
      XLAssert (end - start < 3998);
      memcpy (chunk, start, end - start);

      /* NULL-terminate the buffer.  */
      chunk[end - start] = '\0';
      DebugPrint ("sending buffered string %s", chunk);

      /* Calculate the cursor position and whether or not it is in
	 this chunk.  */

      if (cursor == -1)
	cursor_pos = -1;
      else
	cursor_pos = cursor - (start - buffer);

      if (cursor_pos < 0)
	cursor_pos = -1;

      /* Send the sequence.  */
      zwp_text_input_v3_send_preedit_string (input->resource, chunk,
					     cursor_pos, cursor_pos);

      start = end;
    }

  /* Finish sending it.  */
  zwp_text_input_v3_send_done (input->resource, input->serial);
}

static void
CommitString (TextInput *input, const char *buffer,
	      size_t buffer_size)
{
  char chunk[4000];
  const char *start, *end;
  int skip;
  const char *buffer_end;

  start = buffer;
  buffer_end = buffer + buffer_size;

  /* The Wayland protocol limits strings to 4000 bytes (including the
     terminating NULL).  Send the text as valid substrings consisting
     of less than 4000 bytes each.  */

  while (start < buffer_end)
    {
      end = start;

      while (true)
	{
	  skip = CountOctets (*end);

	  DebugPrint ("skip %d (%p+%d)", skip, end, skip);

	  if (end + skip - start >= 3998)
	    break;

	  if (end >= buffer_end)
	    break;

	  end += skip;
	}

      DebugPrint ("end-start (%p-%p): %zd", end, start,
		  end - start);

      /* Now, start to end contain a UTF-8 sequence less than 4000
	 bytes in length.  */
      XLAssert (end - start < 3998);
      memcpy (chunk, start, end - start);

      /* NULL-terminate the buffer.  */
      chunk[end - start] = '\0';
      DebugPrint ("sending buffered string %s", chunk);

      /* Send the sequence.  */
      zwp_text_input_v3_send_commit_string (input->resource, chunk);

      start = end;
    }

  /* Finish sending it.  */
  zwp_text_input_v3_send_done (input->resource, input->serial);
}

static Bool
LookupString (TextInput *input, XEvent *event, KeySym *keysym_return)
{
  char *buffer;
  size_t nbytes, buffer_size;
  Status status;
  KeySym keysym;

  if (event->xkey.type != KeyPress)
    {
      DebugPrint ("ignoring key release event");
      return False;
    }

  /* First, do XmbLookupString with the default buffer size.  */
  buffer = alloca (256);
  nbytes = XmbLookupString (input->xic, &event->xkey,
			    buffer, 256, &keysym, &status);
  DebugPrint ("looked up %zu", nbytes);

  if (status == XBufferOverflow)
    {
      DebugPrint ("overflow to %zu", nbytes);

      /* Handle overflow by growing the buffer.  */
      buffer = alloca (nbytes + 1);
      nbytes = XmbLookupString (input->xic, &event->xkey,
				buffer, nbytes + 1,
				&keysym, &status);
    }

  DebugPrint ("status is: %d", (int) status);

  /* If no string was returned, return False.  Otherwise, convert the
     string to UTF-8 and commit it.  */
  if (status != XLookupChars && status != XLookupBoth)
    {
      if (status == XLookupKeySym && keysym_return)
	/* Return the keysym if it was looked up.  */
	*keysym_return = keysym;

      return False;
    }

  DebugPrint ("converting buffer of %zu", nbytes);

  /* current_xim should not be NULL.  */
  XLAssert (current_xim != NULL);

  /* Convert the string.  */
  buffer = ConvertString (buffer, nbytes, &buffer_size);

  /* If the string happens to consist of only 1 control character and
     a keysym was also found, give preference to the keysym.  */
  if (buffer_size == 1 && status == XLookupBoth
      && buffer[0] > 0 && buffer[0] < 32)
    {
      DebugPrint ("using keysym in preference to single control char");

      XFree (buffer);

      if (keysym_return)
	*keysym_return = keysym;

      return False;
    }

  if (buffer)
    CommitString (input, buffer, buffer_size);
  XFree (buffer);

  return True;
}

static Bool
FilterInputCallback (Seat *seat, Surface *surface, void *event,
		     KeySym *keysym)
{
  XIDeviceEvent *xev;
  XEvent xkey;
  TextInputClientInfo *info;
  TextInput *input;

  xev = event;

  DebugPrint ("seat %p, surface %p, detail: %d, event: %lx",
	      seat, surface, xev->detail, xev->event);

  /* Find the client info.  */
  info = GetClientInfo (wl_resource_get_client (surface->resource),
			seat, False);

  /* Find the enabled text input.  */
  if (info)
    {
      input = FindEnabledTextInput (info);

      /* If there is an enabled text input, start filtering the
	 event.  */
      if (input && input->xic)
	{
	  DebugPrint ("found enabled text input %p on client-seat info %p",
		      input, info);

	  /* Convert the extension event into a fake core event that
	     the input method can understand.  */
	  ConvertKeyEvent (xev, &xkey);

	  /* And return the result of filtering the event.  */
	  if (XFilterEvent (&xkey, XLWindowFromSurface (surface)))
	    return True;

	  /* Otherwise, call XmbLookupString.  If a keysym is
	     returned, return False.  Otherwise, commit the string
	     looked up and return True.  */
	  return LookupString (input, &xkey, keysym);
	}
    }

  /* Otherwise, do nothing.  */
  return False;
}



/* Seat input callbacks.  */
static TextInputFuncs input_funcs =
  {
    .focus_in = FocusInCallback,
    .focus_out = FocusOutCallback,
    .filter_input = FilterInputCallback,
  };

void
XLTextInputDispatchCoreEvent (Surface *surface, XEvent *event)
{
  Seat *im_seat;
  TextInputClientInfo *info;
  TextInput *input;
  KeySym keysym;

  DebugPrint ("dispatching core event to surface %p:\n"
	      "\ttype: %d\n"
	      "\tserial: %lu\n"
	      "\tsend_event: %d\n"
	      "\twindow: %lx\n"
	      "\troot: %lx\n"
	      "\tsubwindow: %lx\n"
	      "\ttime: %lu\n"
	      "\tstate: %x\n"
	      "\tkeycode: %x", surface,
	      event->xkey.type,
	      event->xkey.serial, event->xkey.send_event,
	      event->xkey.window, event->xkey.subwindow,
	      event->xkey.subwindow, event->xkey.time,
	      event->xkey.state, event->xkey.keycode);

  keysym = 0;

  /* Some of the events we want here are rather special.  They are put
     back on the event queue by the X internationalization library in
     response to an XIM_COMMIT event being received.  Other events are
     put back on the event queue in response to XIM_FORWARD_EVENT.
     First, find out which seat is the input method seat.  */

  im_seat = XLSeatGetInputMethodSeat ();

  if (!im_seat)
    return;

  /* Next, find the client info associated with the surface for that
     seat.  */
  info = GetClientInfo (wl_resource_get_client (surface->resource),
		        im_seat, False);

  if (!info)
    return;

  if (info->focus_surface != surface)
    /* The surface is no longer focused.  */
    return;

  if (info)
    {
      /* And look for an enabled text input.  */
      input = FindEnabledTextInput (info);

      if (input)
	{
	  DebugPrint ("found enabled input %p on info %p", input, info);

	  /* Now, try to dispatch the core event.  First, look up the
	     text string.  */
	  if (!input->xic || LookupString (input, event, &keysym))
	    return;

	  if (event->xkey.subwindow & (1U << 31))
	    DebugPrint ("lookup failed; not dispatching event because"
			" this is a key repeat");
	  else
	    {
	      /* Since that failed, dispatch the event to the seat.  */
	      DebugPrint ("lookup failed; dispatching event to seat; "
			  "keysym is: %lu", keysym);

	      XLSeatDispatchCoreKeyEvent (im_seat, surface, event, keysym);
	    }
	}
    }
}

static Bool
InitFontset (void)
{
  XrmDatabase rdb;
  XrmName namelist[3];
  XrmClass classlist[3];
  XrmValue value;
  XrmRepresentation type;
  char **missing_charset_list, *def_string;
  int missing_charset_count;

  rdb = XrmGetDatabase (compositor.display);

  if (!rdb)
    return False;

  DebugPrint ("initializing fontset");

  namelist[1] = XrmStringToQuark ("ximFont");
  namelist[0] = app_quark;
  namelist[2] = NULLQUARK;

  classlist[1] = XrmStringToQuark ("XimFont");
  classlist[0] = resource_quark;
  classlist[2] = NULLQUARK;

  if (XrmQGetResource (rdb, namelist, classlist,
		       &type, &value)
      && type == QString)
    {
      DebugPrint ("XIM fontset: %s", value.addr);

      im_fontset = XCreateFontSet (compositor.display,
				   (char *) value.addr,
				   &missing_charset_list,
				   &missing_charset_count,
				   &def_string);

      if (missing_charset_count)
	XFreeStringList (missing_charset_list);
      return True;
    }

  return False;
}

static void
InitInputStyles (void)
{
  XrmDatabase rdb;
  XrmName namelist[3];
  XrmClass classlist[3];
  XrmValue value;
  XrmRepresentation type;
  int i;
  char *string, *end, *sep, *buffer;

  rdb = XrmGetDatabase (compositor.display);

  if (!rdb)
    return;

  DebugPrint ("initializing input styles");

  namelist[1] = XrmStringToQuark ("ximStyles");
  namelist[0] = app_quark;
  namelist[2] = NULLQUARK;

  classlist[1] = XrmStringToQuark ("XimStyles");
  classlist[0] = resource_quark;
  classlist[2] = NULLQUARK;

  if (XrmQGetResource (rdb, namelist, classlist,
		       &type, &value)
      && type == QString)
    {
      DebugPrint ("XIM styles: %s", value.addr);
      string = value.addr;
      end = string + strlen (string);
      i = 0;
      
      while (string < end)
	{
	  /* Find the next comma.  */
	  sep = strchr (string, ',');

	  if (!sep)
	    sep = end;

	  /* Copy the text between string and sep into buffer.  */
	  buffer = alloca (sep - string + 1);
	  memcpy (buffer, string, sep - string);
	  buffer[sep - string] = '\0';

	  /* If the comparison is successful, populate the list.  */
	  DebugPrint ("considering: %s", buffer);

	  if (!strcmp (buffer, "overTheSpot"))
	    xim_style_order[i++] = XimOverTheSpot;
	  else if (!strcmp (buffer, "offTheSpot"))
	    xim_style_order[i++] = XimOffTheSpot;
	  else if (!strcmp (buffer, "rootWindow"))
	    xim_style_order[i++] = XimRootWindow;
	  else if (!strcmp (buffer, "onTheSpot"))
	    xim_style_order[i++] = XimOnTheSpot;
	  else
	    {
	      /* Invalid value encountered; stop parsing.  */
	      DebugPrint ("invalid value: %s", buffer);
	      return;
	    }

	  /* Return if i is now 4.  */
	  if (i == 4)
	    return;

	  string = sep + 1;
	}

      return;
    }
  else
    {
      /* Set up default values.  */

      xim_style_order[0] = XimOverTheSpot;
      xim_style_order[1] = XimOffTheSpot;
      xim_style_order[2] = XimRootWindow;
      xim_style_order[3] = XimOnTheSpot;

      DebugPrint ("set up default values for XIM style order");
    }

  return;
}

void
XLInitTextInput (void)
{
  const char *modifiers;
  char **missing_charset_list, *def_string;
  int missing_charset_count;

  current_cd = (iconv_t) -1;

  if (!XSupportsLocale ())
    {
      DebugPrint ("not initializing text input because the"
		  " locale is not supported by the X library");
      return;
    }

  /* Append the contents of XMODIFIERS to the locale modifiers
     list.  */
  modifiers = XSetLocaleModifiers ("");
  DebugPrint ("locale modifiers are: %s", modifiers);

  /* Prevent -Wunused-but-set-variable when not debug.  */
  ((void) modifiers);

  all_client_infos.next = &all_client_infos;
  all_client_infos.last = &all_client_infos;

  text_input_manager_global
    = wl_global_create (compositor.wl_display,
			&zwp_text_input_manager_v3_interface,
			1, NULL, HandleBind);

  /* Initialize the IM fontset.  */
  if (!InitFontset ())
    {
      im_fontset = XCreateFontSet (compositor.display,
				   "-*-*-*-R-*-*-*-120-*-*-*-*",
				   &missing_charset_list,
				   &missing_charset_count,
				   &def_string);
      if (missing_charset_count)
	XFreeStringList (missing_charset_list);
    }

  /* Initialize input styles.  */
  InitInputStyles ();

  if (im_fontset == NULL)
    fprintf (stderr, "Unable to load any usable fontset for input methods\n");

  /* Register the IM callback.  */
  XRegisterIMInstantiateCallback (compositor.display,
				  XrmGetDatabase (compositor.display),
				  (char *) compositor.resource_name,
				  (char *) compositor.app_name,
				  IMInstantiateCallback, NULL);

  /* Register the text input functions.  */
  XLSeatSetTextInputFuncs (&input_funcs);
}
