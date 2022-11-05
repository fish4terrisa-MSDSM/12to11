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

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <iconv.h>

#include "compositor.h"
#include "xdg-shell.h"
#include "xdg-decoration-unstable-v1.h"

#define ToplevelFromRoleImpl(impl) ((XdgToplevel *) (impl))

typedef struct _XdgToplevel XdgToplevel;
typedef struct _XdgDecoration XdgDecoration;
typedef struct _ToplevelState ToplevelState;
typedef struct _PropMotifWmHints PropMotifWmHints;
typedef struct _XdgUnmapCallback XdgUnmapCallback;

typedef enum _How How;
typedef enum _DecorationMode DecorationMode;

enum
  {
    StateIsMapped		    = 1,
    StatePendingMaxSize		    = (1 << 1),
    StatePendingMinSize		    = (1 << 2),
    StatePendingAckMovement	    = (1 << 3),
    StatePendingResize		    = (1 << 4),
    StatePendingConfigureSize	    = (1 << 5),
    StatePendingConfigureStates	    = (1 << 6),
    StateDecorationModeDirty	    = (1 << 7),
    StateEverMapped		    = (1 << 8),
    StateNeedDecorationConfigure    = (1 << 9),
    StateWaitingForInitialConfigure = (1 << 10),
  };

enum
  {
    SupportsWindowMenu = 1,
    SupportsMaximize   = (1 << 2),
    SupportsFullscreen = (1 << 3),
    SupportsMinimize   = (1 << 4),
  };

enum
  {
    MwmHintsDecorations = (1L << 1),
    MwmDecorAll		= (1L << 0),
  };

enum _How
  {
    Remove = 0,
    Add	   = 1,
    Toggle = 2,
  };

enum _DecorationMode
  {
    DecorationModeClient	= 0,
    DecorationModeWindowManager = 1,
  };

struct _XdgUnmapCallback
{
  /* Function run when the toplevel is unmapped or detached.  */
  void (*unmap) (void *);

  /* Data for that function.  */
  void *data;

  /* Next and last callbacks in this list.  */
  XdgUnmapCallback *next, *last;
};

struct _PropMotifWmHints
{
  unsigned long flags;
  unsigned long functions;
  unsigned long decorations;
  long input_mode;
  unsigned long status;
};

struct _ToplevelState
{
  /* The surface is maximized. The window geometry specified in the
     configure event must be obeyed by the client.

     The client should draw without shadow or other decoration outside
     of the window geometry.  */
  Bool maximized : 1;

  /* The surface is fullscreen.  The window geometry specified in the
     configure event is a maximum; the client cannot resize beyond
     it. For a surface to cover the whole fullscreened area, the
     geometry dimensions must be obeyed by the client. For more
     details, see xdg_toplevel.set_fullscreen.  */
  Bool fullscreen : 1;

  /* Client window decorations should be painted as if the window is
     active. Do not assume this means that the window actually has
     keyboard or pointer focus.  */
  Bool activated : 1;
};

struct _XdgToplevel
{
  /* The parent role implementation.  */
  XdgRoleImplementation impl;

  /* The role associated with this toplevel.  */
  Role *role;

  /* The wl_resource associated with this toplevel.  */
  struct wl_resource *resource;

  /* The Motif window manager hints associated with this toplevel.  */
  PropMotifWmHints motif;

  /* The current window manager state.  */
  ToplevelState toplevel_state;

  /* All resize callbacks currently posted.  */
  XLList *resize_callbacks;

  /* Timer for completing window state changes.  The order of
     _NET_WM_STATE changes and ConfigureNotify events is not
     predictable, so we batch up both kinds of events with a 0.01
     second delay by default, before sending the resulting
     ConfigureNotify event.  However, if drag-to-resize is in
     progress, no such delay is effected.  */
#define DefaultStateDelayNanoseconds 10000000
  Timer *configuration_timer;

  /* List of callbacks run upon unmapping.  The callbacks are then
     deleted.  */
  XdgUnmapCallback unmap_callbacks;

  /* The parent toplevel.  */
  XdgToplevel *transient_for;

  /* The unmap callback for the parent toplevel.  */
  XdgUnmapCallback *parent_callback;

  /* Any decoration resource associated with this toplevel.  */
  XdgDecoration *decoration;

  /* Various geometries before a given state change.

     width00/height00 mean the size when the toplevel was neither
     maximized nor fullscreen.

     width01/height01 mean the size when the toplevel was not
     maximized but not fullscreen.

     width10/height10 mean the size when the toplevel was fullscreen
     but not maximized.

     width11/height11 mean the size when the toplevel was
     maximized both maximized and fullscreen.

     These values are used to guess how the state changed when
     handling the ConfigureNotify event preceeding the PropertyNotify
     event for _NET_WM_STATE.  */
  int width01, height01, width10, height10;
  int width00, height00, width11, height11;

  /* Minimum size of this toplevel.  */
  int min_width, min_height;

  /* Maximim size of this toplevel.  */
  int max_width, max_height;

  /* Pending values.  */
  int pending_max_width, pending_max_height;
  int pending_min_height, pending_min_width;

  /* How much to move upon the next ack_configure.  Used to resize a
     window westwards or northwards.  */
  int ack_west, ack_north;

  /* The width, height, west and north motion of the next resize.  */
  int resize_width, resize_height, resize_west, resize_north;

  /* Mask of what this toplevel is allowed to do.  It is first set
     based on _NET_SUPPORTED upon toplevel creation, and then
     _NET_WM_ALLOWED_ACTIONS.  */
  int supported;

  /* The number of references to this toplevel.  */
  int refcount;

  /* Some state associated with this toplevel.  */
  int state;

  /* The serial of the last configure sent.  */
  uint32_t conf_serial;

  /* Whether or not we are waiting for a reply to a configure
     event.  */
  Bool conf_reply;

  /* The current width and height of this toplevel as received in the
     last ConfigureNotify event.  */
  int width, height;

  /* The width and height used by that timer if
     StatePendingConfigureSize is set.  */
  int configure_width, configure_height;

  /* The number of seats that currently have this surface focused.  */
  int focus_seat_count;

  /* Array of states.  */
  struct wl_array states;

  /* The decoration mode.  */
  DecorationMode decor;

  /* X Windows size hints.  */
  XSizeHints size_hints;
};

struct _XdgDecoration
{
  /* The associated resource.  */
  struct wl_resource *resource;

  /* The associated toplevel.  */
  XdgToplevel *toplevel;
};

enum
  {
    NetWmPingMask = 1,
  };

/* iconv context used to convert between UTF-8 and Latin-1.  */
static iconv_t latin_1_cd;

/* Whether or not to work around state changes being desynchronized
   with configure events.  */
static Bool apply_state_workaround;

/* Whether or not to batch together state and size changes that arrive
   at almost the same time.  */
static Bool batch_state_changes;

/* Mask consisting of currently enabled window manager protocols.  */
static int window_manager_protocols;

static XdgUnmapCallback *
RunOnUnmap (XdgToplevel *toplevel, void (*unmap) (void *),
	    void *data)
{
  XdgUnmapCallback *callback;

  XLAssert (toplevel->state & StateIsMapped
	    && toplevel->role);

  callback = XLMalloc (sizeof *callback);
  callback->next = toplevel->unmap_callbacks.next;
  callback->last = &toplevel->unmap_callbacks;
  toplevel->unmap_callbacks.next->last = callback;
  toplevel->unmap_callbacks.next = callback;

  callback->data = data;
  callback->unmap = unmap;

  return callback;
}

static void
CancelUnmapCallback (XdgUnmapCallback *callback)
{
  callback->next->last = callback->last;
  callback->last->next = callback->next;

  XLFree (callback);
}

static void
RunUnmapCallbacks (XdgToplevel *toplevel)
{
  XdgUnmapCallback *first, *last;

  first = toplevel->unmap_callbacks.next;

  while (first != &toplevel->unmap_callbacks)
    {
      last = first;
      first = first->next;

      last->unmap (last->data);
      XLFree (last);
    }

  /* Re-initialize the sentinel node for the list of unmap
     callbacks.  */
  toplevel->unmap_callbacks.next = &toplevel->unmap_callbacks;
  toplevel->unmap_callbacks.last = &toplevel->unmap_callbacks;
}

static Bool
IsWindowMapped (Role *role, XdgRoleImplementation *impl)
{
  XdgToplevel *toplevel;

  toplevel = ToplevelFromRoleImpl (impl);
  return toplevel->state & StateIsMapped;
}

static void
WriteHints (XdgToplevel *toplevel)
{
  XChangeProperty (compositor.display,
		   XLWindowFromXdgRole (toplevel->role),
		   _MOTIF_WM_HINTS, _MOTIF_WM_HINTS, 32,
		   PropModeReplace,
		   (unsigned char *) &toplevel->motif, 5);
}

static void
SetDecorated (XdgToplevel *toplevel, Bool decorated)
{
  toplevel->motif.flags |= MwmHintsDecorations;

  if (decorated)
    toplevel->motif.decorations = MwmDecorAll;
  else
    toplevel->motif.decorations = 0;

  if (toplevel->role)
    WriteHints (toplevel);
}

static void
DestroyBacking (XdgToplevel *toplevel)
{
  if (--toplevel->refcount)
    return;

  /* If there is a pending configuration timer, remove it.  */
  if (toplevel->configuration_timer)
    RemoveTimer (toplevel->configuration_timer);

  if (toplevel->parent_callback)
    CancelUnmapCallback (toplevel->parent_callback);

  XLListFree (toplevel->resize_callbacks,
	      XLSeatCancelResizeCallback);

  wl_array_release (&toplevel->states);
  XLFree (toplevel);
}

static void
AddState (XdgToplevel *toplevel, uint32_t state)
{
  uint32_t *data;

  data = wl_array_add (&toplevel->states, sizeof *data);
  *data = state;
}

static void
SendDecorationConfigure1 (XdgToplevel *toplevel)
{
  XLAssert (toplevel->decoration != NULL);

#define ServerSide ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
#define ClientSide ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE

  if (toplevel->decor == DecorationModeClient)
    zxdg_toplevel_decoration_v1_send_configure (toplevel->decoration->resource,
					        ClientSide);
  else
    zxdg_toplevel_decoration_v1_send_configure (toplevel->decoration->resource,
						ServerSide);

#undef ServerSide
#undef ClientSide

  /* This means that the decoration should be reapplied upon the next
     commit.  */
  toplevel->state |= StateDecorationModeDirty;
}

static void
SendConfigure (XdgToplevel *toplevel, unsigned int width,
	       unsigned int height)
{
  uint32_t serial;

  serial = wl_display_next_serial (compositor.wl_display);
  xdg_toplevel_send_configure (toplevel->resource, width, height,
			       &toplevel->states);

  /* If a toplevel decoration resource is created and
     SetMode/UnsetMode is called before the initial toplevel commit,
     then the toplevel decoration mode must be sent here instead.  */

  if (toplevel->state & StateNeedDecorationConfigure
      && toplevel->decoration)
    SendDecorationConfigure1 (toplevel);
  toplevel->state &= ~StateNeedDecorationConfigure;

  XLXdgRoleSendConfigure (toplevel->role, serial);

  toplevel->conf_reply = True;
  toplevel->conf_serial = serial;
}

static void
SendDecorationConfigure (XdgToplevel *toplevel)
{
  uint32_t serial;

  /* This should never be NULL when called!  */
  XLAssert (toplevel->decoration != NULL);

  serial = wl_display_next_serial (compositor.wl_display);

  SendDecorationConfigure1 (toplevel);
  XLXdgRoleSendConfigure (toplevel->role, serial);

  toplevel->conf_reply = True;
  toplevel->conf_serial = serial;
}

/* Forward declaration.  */

static void SendStates (XdgToplevel *);
static void WriteStates (XdgToplevel *);

static void
NoteConfigureTime (Timer *timer, void *data, struct timespec time)
{
  XdgToplevel *toplevel;
  int width, height, effective_width, effective_height;

  toplevel = data;

  /* If only the window state changed, call SendStates.  */
  if (!(toplevel->state & StatePendingConfigureSize))
    SendStates (toplevel);
  else
    {
      /* If the states changed, write them.  */
      if (toplevel->state & StatePendingConfigureStates)
	WriteStates (toplevel);

      effective_width = toplevel->configure_width;
      effective_height = toplevel->configure_height;

      /* toplevel->role->surface should not be NULL here, as the timer
	 is cancelled upon role detachment.  */
      TruncateScaleToSurface (toplevel->role->surface,
			      effective_width, effective_height,
			      &effective_width, &effective_height);

      /* Compute the geometry for the configure event based on the
	 current size of the toplevel.  */
      XLXdgRoleCalcNewWindowSize (toplevel->role,
				  effective_width,
				  effective_height,
				  &width, &height);

      /* Send the configure event.  */
      SendConfigure (toplevel, width, height);
    }

  /* Clear the pending size and state flags.  */
  toplevel->state &= ~StatePendingConfigureSize;
  toplevel->state &= ~StatePendingConfigureStates;

  /* Cancel and clear the timer.  */
  RemoveTimer (timer);
  toplevel->configuration_timer = NULL;
}

static void
FlushConfigurationTimer (XdgToplevel *toplevel)
{
  if (!toplevel->configuration_timer)
    return;

  /* Cancel the configuration timer and flush pending state to the
     state array.  It is assumed that a configure event will be sent
     immediately afterwards.  */

  if (toplevel->state & StatePendingConfigureStates)
    WriteStates (toplevel);

  /* Clear the pending size and state flags.  */
  toplevel->state &= ~StatePendingConfigureSize;
  toplevel->state &= ~StatePendingConfigureStates;

  /* Cancel and clear the timer.  */
  RemoveTimer (toplevel->configuration_timer);
  toplevel->configuration_timer = NULL;
}

static Bool
MaybePostDelayedConfigure (XdgToplevel *toplevel, int flag)
{
  XLList *tem;

  if (!batch_state_changes)
    return False;

  toplevel->state |= flag;

  if (toplevel->configuration_timer)
    {
      /* The timer is already ticking... */
      RetimeTimer (toplevel->configuration_timer);
      return True;
    }

  /* If some seat is being resized, return False.  */
  for (tem = live_seats; tem; tem = tem->next)
    {
      if (XLSeatResizeInProgress (tem->data))
	return False;
    }

  toplevel->configuration_timer
    = AddTimer (NoteConfigureTime, toplevel,
		MakeTimespec (0, DefaultStateDelayNanoseconds));
  return True;
}

static void
WriteStates (XdgToplevel *toplevel)
{
  toplevel->states.size = 0;

  if (toplevel->toplevel_state.maximized)
    AddState (toplevel, XDG_TOPLEVEL_STATE_MAXIMIZED);

  if (toplevel->toplevel_state.fullscreen)
    AddState (toplevel, XDG_TOPLEVEL_STATE_FULLSCREEN);

  if (toplevel->toplevel_state.activated)
    AddState (toplevel, XDG_TOPLEVEL_STATE_ACTIVATED);

  if (toplevel->resize_callbacks)
    AddState (toplevel, XDG_TOPLEVEL_STATE_RESIZING);
}

static void
CurrentWindowGeometry (XdgToplevel *toplevel,
		       int *width, int *height)
{
  /* Calculate the current window geometry for sending a configure
     event.  */

  TruncateScaleToSurface (toplevel->role->surface,
			  toplevel->width,
			  toplevel->height,
			  width, height);

  XLXdgRoleCalcNewWindowSize (toplevel->role, *width,
			      *height, width, height);
}

static void
SendStates (XdgToplevel *toplevel)
{
  int width, height;

  WriteStates (toplevel);

  /* toplevel->role->surface should not be NULL here.  */

  CurrentWindowGeometry (toplevel, &width, &height);
  SendConfigure (toplevel, width, height);
}

static void
RecordStateSize (XdgToplevel *toplevel)
{
  Bool a, b;
  int width, height;

  if (!toplevel->role->surface)
    /* We can't get the scale factor in this case.  */
    return;

  /* Record the last known size of a toplevel before its state is
     changed.  That way, we can send xdg_toplevel::configure with the
     right state, should the window manager send ConfigureNotify
     before changing the state.  */

  a = toplevel->toplevel_state.maximized;
  b = toplevel->toplevel_state.fullscreen;

  if (XLWmSupportsHint (_GTK_FRAME_EXTENTS))
    {
      /* Note that if _GTK_FRAME_EXTENTS is supported, the window
	 manager will elect to send us the old window geometry instead
	 upon minimization.  */
      XLXdgRoleGetCurrentGeometry (toplevel->role, NULL, NULL,
				   &width, &height);

      /* Scale the width and height to window dimensions.  */
      TruncateScaleToWindow (toplevel->role->surface, width, height,
			     &width, &height);
    }
  else
    {
      width = toplevel->width;
      height = toplevel->height;
    }

  if (!a && !b) /* 00 */
    {
      toplevel->width00 = width;
      toplevel->height00 = height;
    }

  if (!a && b) /* 10 */
    {
      toplevel->width10 = width;
      toplevel->height10 = height;
    }

  if (a && !b) /* 01 */
    {
      toplevel->width01 = width;
      toplevel->height01 = height;
    }

  if (a && b) /* 11 */
    {
      toplevel->width11 = width;
      toplevel->height11 = height;
    }
}

static void
HandleWmStateChange (XdgToplevel *toplevel)
{
  unsigned long actual_size;
  unsigned long bytes_remaining;
  int rc, actual_format, i;
  Atom actual_type, *states;
  unsigned char *tmp_data;
  Window window;
  ToplevelState *state, old;

  tmp_data = NULL;
  window = XLWindowFromXdgRole (toplevel->role);
  state = &toplevel->toplevel_state;

  rc = XGetWindowProperty (compositor.display, window,
			   _NET_WM_STATE, 0, 65536,
			   False, XA_ATOM, &actual_type,
			   &actual_format, &actual_size,
			   &bytes_remaining, &tmp_data);

  if (rc != Success || !tmp_data
      || actual_type != XA_ATOM || actual_format != 32
      || bytes_remaining)
    goto empty_states;

  states = (Atom *) tmp_data;

  /* First, reset relevant states.  */

  memcpy (&old, state, sizeof *state);
  state->maximized = False;
  state->fullscreen = False;
  state->activated = False;

  /* Then loop through and enable any states that are set.  */

  for (i = 0; i < actual_size; ++i)
    {
      if (states[i] == _NET_WM_STATE_FULLSCREEN)
	state->fullscreen = True;

      if (states[i] == _NET_WM_STATE_FOCUSED)
	state->activated = True;

      if (states[i] == _NET_WM_STATE_MAXIMIZED_HORZ
	  || states[i] == _NET_WM_STATE_MAXIMIZED_VERT)
	state->maximized = True;
    }

  if (memcmp (&old, &state, sizeof *state)
      && !MaybePostDelayedConfigure (toplevel,
				     StatePendingConfigureStates))
    /* Finally, send states if they changed.  */
    SendStates (toplevel);

  /* And free the atoms.  */

  if (tmp_data)
    XFree (tmp_data);

  return;

 empty_states:

  /* Retrieving the EWMH state failed.  Clear all states.  */

  state->maximized = False;
  state->fullscreen = False;
  state->activated = False;

  if (tmp_data)
    XFree (tmp_data);

  SendStates (toplevel);
}

static void
SendWmCapabilities (XdgToplevel *toplevel)
{
  struct wl_array array;
  uint32_t *data;

  wl_array_init (&array);

  if (toplevel->supported & SupportsWindowMenu)
    {
      data = wl_array_add (&array, sizeof *data);
      *data = XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU;
    }

  if (toplevel->supported & SupportsMinimize)
    {
      data = wl_array_add (&array, sizeof *data);
      *data = XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE;
    }

  if (toplevel->supported & SupportsMaximize)
    {
      data = wl_array_add (&array, sizeof *data);
      *data = XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE;
    }

  if (toplevel->supported & SupportsFullscreen)
    {
      data = wl_array_add (&array, sizeof *data);
      *data = XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN;
    }

  xdg_toplevel_send_wm_capabilities (toplevel->resource, &array);
  wl_array_release (&array);
}

static void
HandleAllowedActionsChange (XdgToplevel *toplevel)
{
  unsigned long actual_size;
  unsigned long bytes_remaining;
  int rc, actual_format, i;
  Atom actual_type, *states;
  unsigned char *tmp_data;
  Window window;
  int old;

  tmp_data = NULL;
  window = XLWindowFromXdgRole (toplevel->role);

  rc = XGetWindowProperty (compositor.display, window,
			   _NET_WM_ALLOWED_ACTIONS, 0, 65536,
			   False, XA_ATOM, &actual_type,
			   &actual_format, &actual_size,
			   &bytes_remaining, &tmp_data);

  if (rc != Success || !tmp_data
      || actual_type != XA_ATOM || actual_format != 32
      || bytes_remaining)
    goto empty_states;

  states = (Atom *) tmp_data;

  /* First, reset the actions that we will change.  */

  old = toplevel->supported;
  toplevel->supported &= ~SupportsMaximize;
  toplevel->supported &= ~SupportsMinimize;
  toplevel->supported &= ~SupportsFullscreen;

  /* Then loop through and enable any states that are set.  */

  for (i = 0; i < actual_size; ++i)
    {
      if (states[i] == _NET_WM_ACTION_FULLSCREEN)
        toplevel->supported |= SupportsFullscreen;

      if (states[i] == _NET_WM_ACTION_MAXIMIZE_HORZ
	  || states[i] == _NET_WM_ACTION_MAXIMIZE_VERT)
	toplevel->supported |= SupportsMaximize;

      if (states[i] == _NET_WM_ACTION_MINIMIZE)
	toplevel->supported |= SupportsMinimize;
    }

  if (toplevel->supported != old)
    /* Finally, send states if they changed.  */
    SendStates (toplevel);

  /* And free the atoms.  */

  if (tmp_data)
    XFree (tmp_data);

  return;

 empty_states:

  /* Retrieving the action list failed.  Ignore this PropertyNotify,
     but free the data if it was set.  */

  if (tmp_data)
    XFree (tmp_data);
}

static void
ApplyGtkFrameExtents (XdgToplevel *toplevel, int x, int y,
		      int x2, int y2)
{
  long cardinals[4];
  Window window;

  cardinals[0] = x;
  cardinals[1] = x2;
  cardinals[2] = y;
  cardinals[3] = y2;

  window = XLWindowFromXdgRole (toplevel->role);

  XChangeProperty (compositor.display, window,
		   _GTK_FRAME_EXTENTS, XA_CARDINAL,
		   32, PropModeReplace,
		   (unsigned char *) cardinals, 4);
}

static void
HandleWindowGeometryChange (XdgToplevel *toplevel)
{
  XSizeHints *hints;
  int width, height, dx, dy, x, y;
  Subcompositor *subcompositor;
  View *view;

  if (!toplevel->role || !toplevel->role->surface)
    return;

  view = toplevel->role->surface->view;
  subcompositor = ViewGetSubcompositor (view);

  XLXdgRoleGetCurrentGeometry (toplevel->role, &x, &y,
			       &width, &height);
  TruncateScaleToWindow (toplevel->role->surface, width, height,
			 &width, &height);
  TruncateSurfaceToWindow (toplevel->role->surface, x, y, &x, &y);

  dx = SubcompositorWidth (subcompositor) - width;
  dy = SubcompositorHeight (subcompositor) - height;

  ApplyGtkFrameExtents (toplevel, x, y, dx - x, dy - y);

  hints = &toplevel->size_hints;

  hints->flags |= PMinSize | PSize;

  /* Initially, specify PSize.  After the first MapNotify, also
     specify PPosition so that subsurfaces won't move the window.  */

  /* First, make hints->min_width and hints->min_height the min width
     in terms of the window coordinate system.  Then, add deltas. */
  TruncateScaleToWindow (toplevel->role->surface, toplevel->min_width,
			 toplevel->min_height, &hints->min_width,
			 &hints->min_height);

  /* Add deltas.  */
  hints->min_width += dx;
  hints->min_height += dy;

  if (toplevel->max_width)
    {
      /* Do the same with the max width.  */
      TruncateScaleToWindow (toplevel->role->surface, toplevel->max_width,
			     toplevel->max_height, &hints->max_width,
			     &hints->max_height);

      hints->max_width += dx;
      hints->max_height += dy;
      hints->flags |= PMaxSize;
    }
  else
    hints->flags &= ~PMaxSize;

  /* If a scale factor is set, also set the resize increment to the
     scale factor.  */

  if (toplevel->role->surface->factor != 1)
    {
      /* Take the ceiling value, there is no good way of dealing with
	 cases where the scale ends up a non-integer value.  */
      hints->width_inc = ceil (toplevel->role->surface->factor);
      hints->height_inc = ceil (toplevel->role->surface->factor);
      hints->flags |= PResizeInc;
    }
  else
    hints->flags &= ~PResizeInc;

  XSetWMNormalHints (compositor.display,
		     XLWindowFromXdgRole (toplevel->role),
		     hints);
}

static Bool
GetClientMachine (XTextProperty *client_machine)
{
  struct addrinfo template, *result;
  int rc;
  long host_name_max;
  char *hostname;

  host_name_max = sysconf (_SC_HOST_NAME_MAX);

  if (host_name_max == -1)
    /* The maximum host name is indeterminate.  Use a sane limit like
       _POSIX_HOST_NAME_MAX.  */
    host_name_max = _POSIX_HOST_NAME_MAX + 1;
  else
    host_name_max += 1;

  /* Allocate the buffer holding the hostname.  */
  hostname = alloca (host_name_max + 1);

  /* Get the hostname.  */
  if (gethostname (hostname, host_name_max + 1))
    /* Obtaining the hostname failed.  */
    return False;

  /* NULL-terminate the hostname.  */
  hostname[host_name_max] = '\0';

  /* Now find the fully-qualified domain name.  */
  memset (&template, 0, sizeof template);
  template.ai_family = AF_UNSPEC;
  template.ai_socktype = SOCK_STREAM;
  template.ai_flags = AI_CANONNAME;

  rc = getaddrinfo (hostname, NULL, &template, &result);

  if (rc || !result)
    return False;

  /* Copy it to the client machine text property.  */
  client_machine->value
    = (unsigned char *) XLStrdup (result->ai_canonname);
  client_machine->encoding = XA_STRING;
  client_machine->nitems = strlen (result->ai_canonname);
  client_machine->format = 8;

  /* Free the result.  */
  freeaddrinfo (result);
  return True;
}

static void
WriteCredentialProperties (XdgToplevel *toplevel)
{
  struct wl_client *client;
  pid_t pid;
  Window window;
  unsigned long process_id;
  XTextProperty client_machine;

  /* Write credential properties such as _NET_WM_PID and
     WM_CLIENT_MACHINE.  The PID is obtained from the Wayland
     connection.  */

  client = wl_resource_get_client (toplevel->resource);

  /* Get the credentials of the client.  If the Wayland library cannot
     obtain those credentials, the client is simply disallowed from
     connecting to this server.  */
  wl_client_get_credentials (client, &pid, NULL, NULL);

  /* Write the _NET_WM_PID property.  */
  window = XLWindowFromXdgRole (toplevel->role);
  process_id = pid;
  XChangeProperty (compositor.display, window, _NET_WM_PID,
		   XA_CARDINAL, 32, PropModeReplace,
		   (unsigned char *) &process_id, 1);

  /* First, let Xlib write WM_CLIENT_MACHINE and WM_LOCALE_NAME.  */
  XSetWMProperties (compositor.display, window, NULL, NULL,
                    NULL, 0, NULL, NULL, NULL);

  /* Next, write the fully-qualified client machine if it can be
     obtained.  */
  if (GetClientMachine (&client_machine))
    {
      XSetWMClientMachine (compositor.display, window,
			   &client_machine);
      XLFree (client_machine.value);
      return;
    }
}

static void
Attach (Role *role, XdgRoleImplementation *impl)
{
  Atom protocols[2];
  XdgToplevel *toplevel;
  int nproto;
  XWMHints wmhints;
  Window window;

  toplevel = ToplevelFromRoleImpl (impl);
  toplevel->refcount++;
  toplevel->role = role;

  nproto = 0;
  window = XLWindowFromXdgRole (role);

  protocols[nproto++] = WM_DELETE_WINDOW;

  /* _NET_WM_PING should be disabled when the window manager kills
     clients using XKillClient.  */
  if (window_manager_protocols & NetWmPingMask)
    protocols[nproto++] = _NET_WM_PING;

  if (XLFrameClockSyncSupported ())
    protocols[nproto++] = _NET_WM_SYNC_REQUEST;

  XSetWMProtocols (compositor.display,
		   window, protocols, nproto);

  WriteHints (toplevel);

  /* Write credential properties: _NET_WM_PID, WM_CLIENT_MACHINE,
     etc.  */
  WriteCredentialProperties (toplevel);

  /* This tells the window manager not to override size choices made
     by the client.  */
  toplevel->size_hints.flags |= PSize;

  /* Apply the surface's window geometry.  */
  HandleWindowGeometryChange (toplevel);

  /* First, initialize toplevel->supported, should the resource be new
     enough.  */
  toplevel->supported = 0;

  if (wl_resource_get_version (toplevel->resource) >= 5)
    {
      /* Assume iconification is always supported, until we get
	 _NET_WM_ALLOWED_ACTIONS.  */
      toplevel->supported |= SupportsMinimize;

      /* Then, populate toplevel->supported based on
	 _NET_SUPPORTED.  */
      if (XLWmSupportsHint (_NET_WM_STATE_FULLSCREEN))
	toplevel->supported |= SupportsFullscreen;

      if (XLWmSupportsHint (_NET_WM_STATE_MAXIMIZED_HORZ)
	  || XLWmSupportsHint (_NET_WM_STATE_MAXIMIZED_VERT))
	toplevel->supported |= SupportsMaximize;

      if (XLWmSupportsHint (_GTK_SHOW_WINDOW_MENU))
	toplevel->supported |= SupportsWindowMenu;

      /* Finally, send the initial capabilities to the client.  */
      SendWmCapabilities (toplevel);
    }

  /* Set the input hint, without placing WM_TAKE_FOCUS in
     WM_PROTOCOLS.  This asks the window manager to manage our focus
     state.  */
  wmhints.flags = InputHint;
  wmhints.input = True;

  XSetWMHints (compositor.display, window, &wmhints);

  /* Write the XdndAware property.  */
  XLDndWriteAwarenessProperty (window);
}

/* Forward declaration.  */

static void Unmap (XdgToplevel *);

static void
Detach (Role *role, XdgRoleImplementation *impl)
{
  XdgToplevel *toplevel;

  toplevel = ToplevelFromRoleImpl (impl);

  /* First, unmap the toplevel.  */
  if (toplevel->state & StateIsMapped)
    Unmap (toplevel);

  /* Next, undo everything that we changed on the window.  */
  toplevel->role = NULL;

  XSetWMProtocols (compositor.display,
		   XLWindowFromXdgRole (role),
		   NULL, 0);

  DestroyBacking (toplevel);
}

/* Forward declaration.  */

static void UpdateParent (XdgToplevel *, XdgToplevel *);

static void
Unmap (XdgToplevel *toplevel)
{
  Window window;

  toplevel->state &= ~StateIsMapped;

  window = XLWindowFromXdgRole (toplevel->role);

  XUnmapWindow (compositor.display, window);

  /* Unmapping an xdg_toplevel means that the surface cannot be shown
     by the compositor until it is explicitly mapped again. All active
     operations (e.g., move, resize) are canceled and all attributes
     (e.g. title, state, stacking, ...) are discarded for an
     xdg_toplevel surface when it is unmapped. The xdg_toplevel
     returns to the state it had right after xdg_surface.get_toplevel.
     The client can re-map the toplevel by perfoming a commit without
     any buffer attached, waiting for a configure event and handling
     it as usual (see xdg_surface description).  */

  /* Clear all the state.  */

  toplevel->state = StateWaitingForInitialConfigure;
  toplevel->conf_reply = False;
  toplevel->conf_serial = 0;
  toplevel->states.size = 0;
  toplevel->width = 0;
  toplevel->height = 0;
  toplevel->min_width = 0;
  toplevel->min_height = 0;

  memset (&toplevel->toplevel_state, 0,
	  sizeof toplevel->toplevel_state);

  /* If there is a pending configure timer, remove it.  */
  if (toplevel->configuration_timer)
    RemoveTimer (toplevel->configuration_timer);
  toplevel->configuration_timer = NULL;

  XLListFree (toplevel->resize_callbacks,
	      XLSeatCancelResizeCallback);
  toplevel->resize_callbacks = NULL;

  memset (&toplevel->size_hints, 0, sizeof toplevel->size_hints);
  XSetWMNormalHints (compositor.display, window,
		     &toplevel->size_hints);

  /* Clear the parent.  */
  UpdateParent (toplevel, NULL);

  /* Run unmap callbacks.  */
  RunUnmapCallbacks (toplevel);
}

static void
Map (XdgToplevel *toplevel)
{
  /* We can't guarantee that the toplevel contents will be preserved
     at this point.  */
  SubcompositorGarbage (XLSubcompositorFromXdgRole (toplevel->role));

  toplevel->state |= StateIsMapped | StateEverMapped;
  toplevel->state &= ~StateWaitingForInitialConfigure;

  /* Update the width and height from the xdg_surface bounds.  */
  toplevel->width = XLXdgRoleGetWidth (toplevel->role);
  toplevel->height = XLXdgRoleGetHeight (toplevel->role);

  /* Resize the window to those bounds beforehand as well.  */
  XLXdgRoleResizeForMap (toplevel->role);

  /* Now, map the window.  */
  XMapWindow (compositor.display,
	      XLWindowFromXdgRole (toplevel->role));
}

static void
AckConfigure (Role *role, XdgRoleImplementation *impl, uint32_t serial)
{
  XdgToplevel *toplevel;

  toplevel = ToplevelFromRoleImpl (impl);

  if (serial == toplevel->conf_serial)
    toplevel->conf_reply = False;
}

static void
SendOutputBounds (XdgToplevel *toplevel)
{
  int x_min, y_min, x_max, y_max;

  XLGetMaxOutputBounds (&x_min, &y_min, &x_max, &y_max);
  xdg_toplevel_send_configure_bounds (toplevel->resource,
				      x_max - x_min + 1,
				      y_max - y_min + 1);
}

static void
Commit (Role *role, Surface *surface, XdgRoleImplementation *impl)
{
  XdgToplevel *toplevel;

  toplevel = ToplevelFromRoleImpl (impl);

  /* Apply any pending min or max size.  */

  if (toplevel->state & StatePendingMinSize)
    {
      toplevel->min_width = toplevel->pending_min_width;
      toplevel->min_height = toplevel->pending_min_height;
    }

  if (toplevel->state & StatePendingMaxSize)
    {
      toplevel->max_width = toplevel->pending_max_width;
      toplevel->max_height = toplevel->pending_max_height;
    }

  if (toplevel->state & (StatePendingMaxSize | StatePendingMinSize))
    {
      HandleWindowGeometryChange (toplevel);

      toplevel->state &= ~StatePendingMaxSize;
      toplevel->state &= ~StatePendingMinSize;
    }

  if (!surface->current_state.buffer
      /* Whenever any commit happens without the toplevel being
	 mapped, send the initial configure event.  This is because
	 some clients attach an initial 1x1 buffer and expect the
	 compositor to do its thing.  */
      || (toplevel->state & StateWaitingForInitialConfigure))
    {
      /* Stop waiting for initial configure.  */
      toplevel->state &= ~StateWaitingForInitialConfigure;

      /* No buffer was attached, unmap the window and send an empty
	 configure event.  */
      if (toplevel->state & StateIsMapped)
	Unmap (toplevel);

      FlushConfigurationTimer (toplevel);

      if (wl_resource_get_version (toplevel->resource) >= 4)
	/* Send the maximum bounds of the window to the client.  It
	   isn't possible to predict where the window will be mapped,
	   so unfortunately the precise output bounds can't be used
	   here.  */
	SendOutputBounds (toplevel);

      SendConfigure (toplevel, 0, 0);
    }
  else if (!toplevel->conf_reply)
    {
      /* Configure reply received, so map the toplevel.  */
      if (!(toplevel->state & StateIsMapped))
	Map (toplevel);
    }

  if (!toplevel->conf_reply
      && toplevel->state & StateDecorationModeDirty)
    {
      /* The decoration is dirty and all configure events were
	 aknowledged; apply the new decoration.  */

      if (toplevel->decor == DecorationModeWindowManager)
	SetDecorated (toplevel, True);
      else
	SetDecorated (toplevel, False);

      toplevel->state &= ~StateDecorationModeDirty;
    }
}

static void
PostResize1 (XdgToplevel *toplevel, int west_motion, int north_motion,
	     int new_width, int new_height)
{
  /* FIXME: the two computations below are still not completely
     right.  */

  if (new_width < toplevel->min_width)
    {
      west_motion += toplevel->min_width - new_width;

      /* Don't move too far west.  */
      if (west_motion > 0)
	west_motion = 0;

      new_width = toplevel->min_width;
    }

  if (new_height < toplevel->min_height)
    {
      north_motion += toplevel->min_height - new_height;

      /* Don't move too far north.  */
      if (north_motion > 0)
	north_motion = 0;

      new_height = toplevel->min_height;
    }

  if (!(toplevel->state & StatePendingAckMovement))
    {
      FlushConfigurationTimer (toplevel);
      SendConfigure (toplevel, new_width, new_height);

      toplevel->ack_west += west_motion;
      toplevel->ack_north += north_motion;
      toplevel->state |= StatePendingAckMovement;

      /* Clear extra state flags that are no longer useful.  */
      toplevel->state &= ~StatePendingResize;
      toplevel->resize_west = 0;
      toplevel->resize_north = 0;
      toplevel->resize_width = 0;
      toplevel->resize_height = 0;
    }
  else
    {
      /* A configure event has been posted but has not yet been fully
	 processed.  Accumulate the new width, height, west and north
	 values, and send another configure event once it really does
	 arrive, and the previous changes have been committed.  */

      toplevel->state |= StatePendingResize;

      toplevel->resize_west += west_motion;
      toplevel->resize_north += north_motion;
      toplevel->resize_width = new_width;
      toplevel->resize_height = new_height;
    }
}

static void
CommitInsideFrame (Role *role, XdgRoleImplementation *impl)
{
  XdgToplevel *toplevel;

  toplevel = ToplevelFromRoleImpl (impl);

  if (!toplevel->conf_reply
      && toplevel->state & StatePendingAckMovement)
    {
      XLXdgRoleMoveBy (role, toplevel->ack_west,
		       toplevel->ack_north);

      toplevel->ack_west = 0;
      toplevel->ack_north = 0;
      toplevel->state &= ~StatePendingAckMovement;

      /* This pending movement has completed.  Apply postponed state,
	 if there is any.  */
      if (toplevel->state & StatePendingResize)
	PostResize1 (toplevel, toplevel->resize_west,
		     toplevel->resize_north,
		     toplevel->resize_width,
		     toplevel->resize_height);
    }
}

static Bool
RestoreStateTo (XdgToplevel *toplevel, int width, int height)
{
  if (width == toplevel->width11 && height == toplevel->height11)
    return False;

  if (width == toplevel->width00 && height == toplevel->height00)
    {
      /* Neither fullscreen nor maximized.  Clear both flags.  */
      toplevel->toplevel_state.fullscreen = False;
      toplevel->toplevel_state.maximized = False;

      return True;
    }

  if (width == toplevel->width10 && height == toplevel->height10)
    {
      if (width == toplevel->width01 && height == toplevel->height01)
	/* Ambiguous, punt.  */
	return False;

      /* Fullscreen, not maximized.  Clear any maximized flag that was
	 set.  */
      toplevel->toplevel_state.maximized = False;
      return True;
    }

  if (width == toplevel->width01 && height == toplevel->height01)
    {
      if (width == toplevel->width01 && height == toplevel->height01)
	/* Ambiguous, punt.  */
	return False;

      /* Maximized, but not fullscreen.  Clear any fullscreen flag
	 that was set.  */
      toplevel->toplevel_state.fullscreen = False;
      return True;
    }

  return False;
}

static Bool
HandleConfigureEvent (XdgToplevel *toplevel, XEvent *event)
{
  int width, height, configure_width, configure_height;

  if (event->xconfigure.send_event)
    /* Handle only synthetic events, since that's what the
       window manager sends upon movement.  */
    XLXdgRoleNoteConfigure (toplevel->role, event);
  else
    XLXdgRoleReconstrain (toplevel->role, event);

  if (event->xconfigure.width == toplevel->width
      && event->xconfigure.height == toplevel->height)
    {
      if (!toplevel->configuration_timer)
	/* No configure event will be generated in the future.
	   Unfreeze the frame clock.  */
	XLXdgRoleNoteRejectedConfigure (toplevel->role);

      return True;
    }

  /* Try to guess if the window state was restored to some earlier
     value, and set it now, to avoid race conditions when some clients
     continue trying to stay maximized or fullscreen.  */

  if (apply_state_workaround
      && RestoreStateTo (toplevel, event->xconfigure.width,
			 event->xconfigure.height))
    WriteStates (toplevel);

  /* Set toplevel->width and toplevel->height correctly.  */
  toplevel->width = event->xconfigure.width;
  toplevel->height = event->xconfigure.height;

  /* Also set the bounds width and height to avoid resizing the
     window.  */
  XLXdgRoleSetBoundsSize (toplevel->role,
			  toplevel->width,
			  toplevel->height);

  if (!MaybePostDelayedConfigure (toplevel, StatePendingConfigureSize))
    {
      /* Scale the configure event width and height to the
	 surface.  */
      TruncateScaleToSurface (toplevel->role->surface,
			      event->xconfigure.width,
			      event->xconfigure.height,
			      &configure_width,
			      &configure_height);

      /* Calculate the new window size.  */
      XLXdgRoleCalcNewWindowSize (toplevel->role,
				  configure_width,
				  configure_height,
				  &width, &height);

      SendConfigure (toplevel, width, height);
    }

  /* Now set toplevel->configure_width and
     toplevel->configure_height.  */
  toplevel->configure_width = toplevel->width;
  toplevel->configure_height = toplevel->height;

  RecordStateSize (toplevel);

  return True;
}

static Bool
WindowResizedPredicate (Display *display, XEvent *event, XPointer data)
{
  Role *role;
  XdgToplevel *toplevel;
  Window target_window;

  toplevel = (XdgToplevel *) data;
  role = toplevel->role;
  target_window = XLWindowFromXdgRole (role);

  if (event->type == ConfigureNotify
      && event->xconfigure.window == target_window)
    /* Extract the event from the event queue.  */
    return True;

  return False;
}

static int
IfEvent (XEvent *event_return, Bool (*predicate) (Display *,
						  XEvent *,
						  XPointer),
	 XPointer arg, struct timespec timeout)
{
  struct timespec current_time, target;
  int fd;
  fd_set fds;

  fd = ConnectionNumber (compositor.display);
  current_time = CurrentTimespec ();
  target = TimespecAdd (current_time, timeout);

  /* Check if an event is already in the queue.  If it is, avoid
     syncing.  */
  if (XCheckIfEvent (compositor.display, event_return,
		     predicate, arg))
    return 0;

  while (true)
    {
      /* Get events into the queue.  */
      XSync (compositor.display, False);

      /* Look for an event again.  */
      if (XCheckIfEvent (compositor.display, event_return,
			 predicate, arg))
	return 0;

      /* Calculate the timeout.  */
      current_time = CurrentTimespec ();
      timeout = TimespecSub (target, current_time);

      /* If not, wait for some input to show up on the X connection,
	 or for the timeout to elapse.  */
      FD_ZERO (&fds);
      FD_SET (fd, &fds);

      /* If this fails due to an IO error, XSync will call the IO
	 error handler.  */
      pselect (fd + 1, &fds, NULL, NULL, &timeout, NULL);

      /* Timeout elapsed.  */
      current_time = CurrentTimespec ();
      if (TimespecCmp (target, current_time) < 0)
	return 1;
    }
}

static void
NoteSize (Role *role, XdgRoleImplementation *impl,
	  int width, int height)
{
  XdgToplevel *toplevel;

  toplevel = ToplevelFromRoleImpl (impl);

  toplevel->width = width;
  toplevel->height = height;
}

static void
NoteWindowPreResize (Role *role, XdgRoleImplementation *impl,
		     int width, int height)
{
  int gwidth, gheight, dx, dy, x, y;
  XdgToplevel *toplevel;

  toplevel = ToplevelFromRoleImpl (impl);

  if (!toplevel->role || !toplevel->role->surface)
    return;

  /* Set the GTK frame immediately before a resize.  This prevents the
     window manager from constraining us by the old values.  */

  XLXdgRoleGetCurrentGeometry (toplevel->role, &x, &y,
			       &gwidth, &gheight);

  /* Scale the window geometry to window dimensions.  */
  TruncateScaleToWindow (toplevel->role->surface, gwidth, gheight,
			 &gwidth, &gheight);
  TruncateSurfaceToWindow (toplevel->role->surface, x, y, &x, &y);

  dx = width - gwidth;
  dy = height - gheight;

  ApplyGtkFrameExtents (toplevel, x, y, dx - x, dy - y);
}

static void
NoteWindowResized (Role *role, XdgRoleImplementation *impl,
		   int width, int height)
{
  XEvent event;
  int rc;
  XdgToplevel *toplevel;

  toplevel = ToplevelFromRoleImpl (impl);

  /* The window resized.  Don't allow ConfigureNotify events to pile
     up and mess up our view of what the window dimensions are by
     waiting for the next ConfigureNotify event.  */

  XFlush (compositor.display);

  rc = IfEvent (&event, WindowResizedPredicate, (XPointer) impl,
		/* Wait at most 0.5 ms in case the window system doesn't
		   send a reply.  */
		MakeTimespec (0, 500000000));

  if (!rc)
    {
      /* Make these values right.  It can happen that the window
	 manager doesn't respect the width and height (the main
	 culprit seems to be height) chosen by us.  */
      toplevel->width = event.xconfigure.width;
      toplevel->height = event.xconfigure.height;

      if (event.xconfigure.send_event)
	XLXdgRoleNoteConfigure (toplevel->role, &event);
      else
	XLXdgRoleReconstrain (toplevel->role, &event);

      RecordStateSize (toplevel);
    }
}

static void
PostResize (Role *role, XdgRoleImplementation *impl, int west_motion,
	    int north_motion, int new_width, int new_height)
{
  XdgToplevel *toplevel;

  toplevel = ToplevelFromRoleImpl (impl);
  PostResize1 (toplevel, west_motion, north_motion,
	       new_width, new_height);
}

static void
HandleGeometryChange (Role *role, XdgRoleImplementation *impl)
{
  XdgToplevel *toplevel;

  toplevel = ToplevelFromRoleImpl (impl);
  HandleWindowGeometryChange (toplevel);
}

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  XdgToplevel *toplevel;

  toplevel = wl_resource_get_user_data (resource);
  toplevel->resource = NULL;

  /* If there is an attached decoration resource, detach it.  */
  if (toplevel->decoration)
    toplevel->decoration->toplevel = NULL;

  DestroyBacking (toplevel);
}

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  XdgToplevel *toplevel;

  toplevel = wl_resource_get_user_data (resource);

  if (toplevel->role)
    XLXdgRoleDetachImplementation (toplevel->role,
				   &toplevel->impl);

  /* If the resource still has a decoration applied, then this is an
     error.  */
  if (toplevel->decoration)
    wl_resource_post_error (resource,
			    ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED,
			    "the attached decoration would be orphaned by"
			    " the destruction of this resource");
  else
    wl_resource_destroy (resource);
}

static void
HandleParentUnmapped (void *data)
{
  XdgToplevel *child, *new_parent;

  child = data;
  new_parent = child->transient_for->transient_for;

  /* Clear child->transient_for etc so UpdateParent doesn't delete the
     callback twice.  */
  child->transient_for = NULL;
  child->parent_callback = NULL;

  /* If parent is child itself, then it might not be mapped.  */
  if (new_parent && !(new_parent->state & StateIsMapped))
    new_parent = NULL;

  /* Set the new parent of child.  */
  UpdateParent (child, new_parent);
}

static void
UpdateWmTransientForProperty (XdgToplevel *child)
{
  Window window, parent;

  window = XLWindowFromXdgRole (child->role);

  if (child->transient_for)
    parent = XLWindowFromXdgRole (child->transient_for->role);

  if (!child->transient_for)
    XDeleteProperty (compositor.display, window,
		     WM_TRANSIENT_FOR);
  else
    XChangeProperty (compositor.display, window,
		     WM_TRANSIENT_FOR, XA_WINDOW,
		     32, PropModeReplace,
		     (unsigned char *) &parent, 1);
}

static void
UpdateParent (XdgToplevel *child, XdgToplevel *parent)
{
  if (parent == child->transient_for)
    return;

  if (child->transient_for)
    {
      CancelUnmapCallback (child->parent_callback);

      child->transient_for = NULL;
      child->parent_callback = NULL;
    }

  if (parent)
    {
      child->transient_for = parent;
      child->parent_callback
	= RunOnUnmap (parent, HandleParentUnmapped, child);
    }

  UpdateWmTransientForProperty (child);
}

static void
SetParent (struct wl_client *client, struct wl_resource *resource,
	   struct wl_resource *parent_resource)
{
  XdgToplevel *child, *parent;

  child = wl_resource_get_user_data (resource);

  if (!child->role)
    return;

  if (parent_resource)
    parent = wl_resource_get_user_data (parent_resource);
  else
    parent = NULL;

  if (parent && !(parent->state & StateIsMapped))
    UpdateParent (child, NULL);
  else
    UpdateParent (child, parent);

  /* Now, verify that no circular loop has formed in the window
     hierarchy.  */

  parent = child->transient_for;
  for (; parent; parent = parent->transient_for)
    {
      /* If parent becomes child again, then we know there is a
	 circular reference somewhere.  In that case, post a fatal
	 protocol error.  */

      if (child == parent)
	wl_resource_post_error (resource, XDG_TOPLEVEL_ERROR_INVALID_PARENT,
				"trying to set parent in a circular fashion");
    }
}

static void
SetNetWmName (XdgToplevel *toplevel, const char *title)
{
  size_t length;

  length = strlen (title);

  /* length shouldn't be allowed to exceed the max-request-size of the
     display.  */
  if (length > SelectionQuantum ())
    length = SelectionQuantum ();

  /* Change the toplevel window's _NET_WM_NAME property.  */
  XChangeProperty (compositor.display,
		   XLWindowFromXdgRole (toplevel->role),
		   _NET_WM_NAME, UTF8_STRING, 8, PropModeReplace,
		   (unsigned char *) title, length);
}

static void
ConvertWmName (XdgToplevel *toplevel, const char *title)
{
  iconv_t cd;
  char *outbuf, *inbuf;
  char *outptr, *inptr;
  size_t outbytesleft, inbytesleft;

  /* Try to convert TITLE from UTF-8 to Latin-1, which is what X
     wants.  */
  cd = latin_1_cd;

  if (cd == (iconv_t) -1)
    /* The conversion could not take place for any number of
       reasons.  */
    return;

  /* Latin-1 is generally smaller than UTF-8.  */
  outbytesleft = strlen (title);
  inbytesleft = outbytesleft;
  outbuf = XLMalloc (outbytesleft);
  inbuf = (char *) title;
  inptr = inbuf;
  outptr = outbuf;

  /* latin_1_cd might already have been used.  Reset the iconv
     state.  */
  iconv (cd, NULL, NULL, &outptr, &outbytesleft);

  /* Restore outptr and outbytesleft to their old values.  */
  outptr = outbuf;
  outbytesleft = inbytesleft;

  /* No error checking is necessary when performing conversions from
     UTF-8 to Latin-1.  */
  iconv (cd, &inptr, &inbytesleft, &outptr, &outbytesleft);

  /* Write the converted string.  */
  XChangeProperty (compositor.display,
		   XLWindowFromXdgRole (toplevel->role),
		   WM_NAME, XA_STRING, 8, PropModeReplace,
		   (unsigned char *) outbuf,
		   /* Limit the size of the title to the amount of
		      data that can be transferred to the X
		      server.  */
		   MIN (SelectionQuantum (), outptr - outbuf));

  /* Free the output buffer.  */
  XLFree (outbuf);
}

static void
SetTitle (struct wl_client *client, struct wl_resource *resource,
	  const char *title)
{
  XdgToplevel *toplevel;

  toplevel = wl_resource_get_user_data (resource);

  if (!toplevel->role)
    return;

  SetNetWmName (toplevel, title);

  /* Also set WM_NAME, in addition for _NET_WM_NAME, for the benefit
     of old pagers and window managers.  */
  ConvertWmName (toplevel, title);
}

static void
SetAppId (struct wl_client *client, struct wl_resource *resource,
	  const char *app_id)
{
  XClassHint class_hints;
  XdgToplevel *toplevel;

  toplevel = wl_resource_get_user_data (resource);

  if (!toplevel->role)
    return;

  if (toplevel->state & StateIsMapped)
    /* The toplevel is already mapped.  Setting class hints in this
       situation is not possible under X.  */
    return;

  class_hints.res_name = (char *) app_id;
  class_hints.res_class = (char *) app_id;

  XSetClassHint (compositor.display,
		 XLWindowFromXdgRole (toplevel->role),
		 &class_hints);
}

static void
ShowWindowMenu (struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *seat_resource, uint32_t serial, int32_t x,
		int32_t y)
{
  int root_x, root_y;
  Seat *seat;
  XdgToplevel *toplevel;

  toplevel = wl_resource_get_user_data (resource);

  if (!toplevel->role)
    return;

  seat = wl_resource_get_user_data (seat_resource);

  if (XLSeatIsInert (seat))
    return;

  XLXdgRoleCurrentRootPosition (toplevel->role, &root_x, &root_y);

  XLSeatShowWindowMenu (seat, toplevel->role->surface,
			root_x + x, root_y + y);
}

static void
Move (struct wl_client *client, struct wl_resource *resource,
      struct wl_resource *seat_resource, uint32_t serial)
{
  XdgToplevel *toplevel;
  Seat *seat;

  seat = wl_resource_get_user_data (seat_resource);
  toplevel = wl_resource_get_user_data (resource);

  if (!toplevel->role || !toplevel->role->surface)
    return;

  XLMoveToplevel (seat, toplevel->role->surface, serial);
}

static void
HandleResizeDone (void *key, void *data)
{
  XdgToplevel *toplevel;

  toplevel = data;
  toplevel->resize_callbacks
    = XLListRemove (toplevel->resize_callbacks, key);

  if (!toplevel->resize_callbacks)
    SendStates (toplevel);
}

static void
Resize (struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *seat_resource, uint32_t serial,
	uint32_t edges)
{
  XdgToplevel *toplevel;
  Seat *seat;
  Bool ok;
  void *callback_key;

  if (edges > XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT)
    {
      wl_resource_post_error (resource,
			      XDG_TOPLEVEL_ERROR_INVALID_RESIZE_EDGE,
			      "not a resize edge");
      return;
    }

  seat = wl_resource_get_user_data (seat_resource);
  toplevel = wl_resource_get_user_data (resource);

  if (!toplevel->role || !toplevel->role->surface)
    return;

  ok = XLResizeToplevel (seat, toplevel->role->surface,
			 serial, edges);

  if (!ok)
    return;

  /* Now set up the special resizing state.  */
  callback_key = XLSeatRunAfterResize (seat, HandleResizeDone,
				       toplevel);
  toplevel->resize_callbacks
    = XLListPrepend (toplevel->resize_callbacks,
		     callback_key);

  /* And send it to the client.  */
  SendStates (toplevel);
}

static void
SetMaxSize (struct wl_client *client, struct wl_resource *resource,
	    int32_t width, int32_t height)
{
  XdgToplevel *toplevel;

  toplevel = wl_resource_get_user_data (resource);

  if (width < 0 || height < 0)
    {
      wl_resource_post_error (resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
			      "invalid max size %d %d", width, height);
      return;
    }

  toplevel->pending_max_width = width;
  toplevel->pending_max_height = height;

  if (toplevel->max_height != height
      || toplevel->max_width != width)
    toplevel->state |= StatePendingMaxSize;
}

static void
SetMinSize (struct wl_client *client, struct wl_resource *resource,
	    int32_t width, int32_t height)
{
  XdgToplevel *toplevel;

  toplevel = wl_resource_get_user_data (resource);

  if (width < 0 || height < 0)
    {
      wl_resource_post_error (resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
			      "invalid min size %d %d", width, height);
      return;
    }

  toplevel->pending_min_width = width;
  toplevel->pending_min_height = height;

  if (toplevel->min_width != width
      || toplevel->min_height != height)
    toplevel->state |= StatePendingMinSize;
}

static void
SetWmState (XdgToplevel *toplevel, Atom what, Atom what1, How how)
{
  XEvent event;

  if (!toplevel->role)
    return;

  memset (&event, 0, sizeof event);

  event.xclient.type = ClientMessage;
  event.xclient.window = XLWindowFromXdgRole (toplevel->role);
  event.xclient.message_type = _NET_WM_STATE;
  event.xclient.format = 32;

  event.xclient.data.l[0] = how;
  event.xclient.data.l[1] = what;
  event.xclient.data.l[2] = what1;
  event.xclient.data.l[3] = 1;

  XSendEvent (compositor.display,
	      DefaultRootWindow (compositor.display),
	      False,
	      SubstructureRedirectMask | SubstructureNotifyMask,
	      &event);
}

static void
SetMaximized (struct wl_client *client, struct wl_resource *resource)
{
  XdgToplevel *toplevel;

  toplevel = wl_resource_get_user_data (resource);
  SetWmState (toplevel, _NET_WM_STATE_MAXIMIZED_HORZ,
	      _NET_WM_STATE_MAXIMIZED_VERT, Add);
}

static void
UnsetMaximized (struct wl_client *client, struct wl_resource *resource)
{
  XdgToplevel *toplevel;

  toplevel = wl_resource_get_user_data (resource);
  SetWmState (toplevel, _NET_WM_STATE_MAXIMIZED_HORZ,
	      _NET_WM_STATE_MAXIMIZED_VERT, Remove);
}

static void
SetFullscreen (struct wl_client *client, struct wl_resource *resource,
	       struct wl_resource *output_resource)
{
  XdgToplevel *toplevel;

  /* Maybe also move the toplevel to the output?  */

  toplevel = wl_resource_get_user_data (resource);
  SetWmState (toplevel, _NET_WM_STATE_FULLSCREEN, None, Add);
}

static void
UnsetFullscreen (struct wl_client *client, struct wl_resource *resource)
{
  XdgToplevel *toplevel;

  toplevel = wl_resource_get_user_data (resource);
  SetWmState (toplevel, _NET_WM_STATE_FULLSCREEN, None, Remove);
}

static void
SetMinimized (struct wl_client *client, struct wl_resource *resource)
{
  XdgToplevel *toplevel;

  toplevel = wl_resource_get_user_data (resource);

  /* N.B. that this is very easy for us, since Wayland "conveniently"
     provides no way for the client to determine the iconification
     state of toplevels, or to deiconify them.  */

  if (!toplevel->role)
    return;

  XIconifyWindow (compositor.display,
		  XLWindowFromXdgRole (toplevel->role),
		  DefaultScreen (compositor.display));
}

static void
ReplyToPing (XEvent *event)
{
  XEvent copy;

  copy = *event;

  /* Reply to the ping message by sending it back to the window
     manager.  */
  copy.xclient.window = DefaultRootWindow (compositor.display);
  XSendEvent (compositor.display, copy.xclient.window,
	      False, (SubstructureRedirectMask
		      | SubstructureNotifyMask), &copy);
}

static void
NoteFocus (Role *role, XdgRoleImplementation *impl, FocusMode mode)
{
  XdgToplevel *toplevel;
  int old_focus;

  toplevel = ToplevelFromRoleImpl (impl);
  old_focus = toplevel->focus_seat_count;

  /* Increase or decrease the number of seats that currently have this
     surface under input focus.  */
  switch (mode)
    {
    case SurfaceFocusIn:
      toplevel->focus_seat_count++;
      break;

    case SurfaceFocusOut:
      toplevel->focus_seat_count
	= MAX (toplevel->focus_seat_count - 1, 0);
      break;
    }

  /* Now, change the toplevel state accordingly.  */
  if (old_focus && !toplevel->focus_seat_count)
    {
      /* The surface should no longer be activated.  */
      toplevel->toplevel_state.activated = False;
      WriteStates (toplevel);
      SendStates (toplevel);
    }
  else
    {
      /* The surface should now be activated.  */
      toplevel->toplevel_state.activated = True;
      WriteStates (toplevel);
      SendStates (toplevel);
    }
}

static void
OutputsChanged (Role *role, XdgRoleImplementation *impl)
{
  XdgToplevel *toplevel;
  int width, height;

  toplevel = ToplevelFromRoleImpl (impl);

  /* The list of outputs changed.  Send the new bounds to the
     client.  */
  if (toplevel->resource)
    {
      if (wl_resource_get_version (toplevel->resource) < 4)
	/* The client is too old to accept configure_bounds.  */
	return;

      /* Send the updated bounds to the toplevel.  */
      SendOutputBounds (toplevel);
      CurrentWindowGeometry (toplevel, &width, &height);
      SendConfigure (toplevel, width, height);
    }
}

static const struct xdg_toplevel_interface xdg_toplevel_impl =
  {
    .destroy = Destroy,
    .set_parent = SetParent,
    .set_title = SetTitle,
    .set_app_id = SetAppId,
    .show_window_menu = ShowWindowMenu,
    .move = Move,
    .resize = Resize,
    .set_max_size = SetMaxSize,
    .set_min_size = SetMinSize,
    .set_maximized = SetMaximized,
    .unset_maximized = UnsetMaximized,
    .set_fullscreen = SetFullscreen,
    .unset_fullscreen = UnsetFullscreen,
    .set_minimized = SetMinimized,
  };

void
XLGetXdgToplevel (struct wl_client *client, struct wl_resource *resource,
		  uint32_t id)
{
  XdgToplevel *toplevel;
  Role *role;

  toplevel = XLSafeMalloc (sizeof *toplevel);
  role = wl_resource_get_user_data (resource);

  if (!toplevel)
    {
      wl_client_post_no_memory (client);
      return;
    }

  memset (toplevel, 0, sizeof *toplevel);

  toplevel->resource
    = wl_resource_create (client, &xdg_toplevel_interface,
			  wl_resource_get_version (resource),
			  id);

  if (!toplevel->resource)
    {
      XLFree (toplevel);
      wl_client_post_no_memory (client);

      return;
    }

  /* Set this flag for some buggy clients.  See the comment above the
     part of Commit that checks this flag for more details.  */
  toplevel->state |= StateWaitingForInitialConfigure;

  toplevel->impl.funcs.attach = Attach;
  toplevel->impl.funcs.commit = Commit;
  toplevel->impl.funcs.detach = Detach;

  toplevel->impl.funcs.ack_configure = AckConfigure;
  toplevel->impl.funcs.note_size = NoteSize;
  toplevel->impl.funcs.note_window_resized = NoteWindowResized;
  toplevel->impl.funcs.note_window_pre_resize = NoteWindowPreResize;
  toplevel->impl.funcs.handle_geometry_change = HandleGeometryChange;
  toplevel->impl.funcs.post_resize = PostResize;
  toplevel->impl.funcs.commit_inside_frame = CommitInsideFrame;
  toplevel->impl.funcs.is_window_mapped = IsWindowMapped;
  toplevel->impl.funcs.outputs_changed = OutputsChanged;

  if (!XLWmSupportsHint (_NET_WM_STATE_FOCUSED))
    /* If _NET_WM_STATE_FOCUSED is unsupported, fall back to utilizing
       focus in and focus out events to determine the focus state.  */
    toplevel->impl.funcs.note_focus = NoteFocus;

  /* Set up the sentinel node for the list of unmap callbacks.  */
  toplevel->unmap_callbacks.next = &toplevel->unmap_callbacks;
  toplevel->unmap_callbacks.last = &toplevel->unmap_callbacks;

  wl_array_init (&toplevel->states);

  wl_resource_set_implementation (toplevel->resource, &xdg_toplevel_impl,
				  toplevel, HandleResourceDestroy);
  toplevel->refcount++;

  /* Wayland surfaces are by default undecorated.  Removing
     decorations will (or rather ought to) also cause the window
     manager to empty the frame window's input region, which allows
     the surface-specified input region to work correctly.  */
  SetDecorated (toplevel, False);

  XLXdgRoleAttachImplementation (role, &toplevel->impl);
}

Bool
XLHandleXEventForXdgToplevels (XEvent *event)
{
  XdgToplevel *toplevel;
  XdgRoleImplementation *impl;

  if (event->type == ClientMessage)
    {
      impl = XLLookUpXdgToplevel (event->xclient.window);

      if (!impl)
	return False;

      toplevel = ToplevelFromRoleImpl (impl);

      if (event->xclient.message_type == WM_PROTOCOLS)
	{
	  if (event->xclient.data.l[0] == WM_DELETE_WINDOW
	      && toplevel->resource)
	    {
	      xdg_toplevel_send_close (toplevel->resource);

	      return True;
	    }
	  else if (event->xclient.data.l[0] == _NET_WM_PING)
	    /* _NET_WM_PING arrived.  Record the event and send ping
	       to the client.  toplevel->role should be non-NULL
	       here.  */
	    XLXdgRoleHandlePing (toplevel->role, event, ReplyToPing);

	  return False;
	}

      return (toplevel->role->surface
	      ? XLDndFilterClientMessage (toplevel->role->surface,
					  event)
	      : False);
    }

  if (event->type == MapNotify)
    {
      /* Always pass through MapNotify events.  */

      impl = XLLookUpXdgToplevel (event->xclient.window);

      if (!impl)
	return False;

      toplevel = ToplevelFromRoleImpl (impl);

      if (toplevel)
	{
	  toplevel->size_hints.flags |= PPosition;

	  XSetWMNormalHints (compositor.display,
			     event->xmap.window,
			     &toplevel->size_hints);
	}

      return False;
    }

  if (event->type == ConfigureNotify)
    {
      impl = XLLookUpXdgToplevel (event->xclient.window);

      if (!impl)
	return False;

      toplevel = ToplevelFromRoleImpl (impl);

      if (toplevel && toplevel->role
	  && toplevel->role->surface
	  && toplevel->state & StateIsMapped)
	return HandleConfigureEvent (toplevel, event);

      return False;
    }

  if (event->type == PropertyNotify)
    {
      if (event->xproperty.atom == _NET_WM_STATE)
	{
	  impl = XLLookUpXdgToplevel (event->xclient.window);

	  if (!impl)
	    return False;

	  toplevel = ToplevelFromRoleImpl (impl);

	  if (toplevel && toplevel->role
	      && toplevel->role->surface)
	    HandleWmStateChange (toplevel);

	  return True;
	}

      if (event->xproperty.atom == _NET_WM_ALLOWED_ACTIONS)
	{
	  impl = XLLookUpXdgToplevel (event->xclient.window);

	  if (!impl)
	    return False;

	  toplevel = ToplevelFromRoleImpl (impl);

	  if (toplevel && toplevel->role
	      && toplevel->role->surface
	      && (wl_resource_get_version (toplevel->resource) >= 5))
	    HandleAllowedActionsChange (toplevel);

	  return True;
	}

      return False;
    }

  return False;
}

static void
ReadWmProtocolsString (const char **string_return)
{
  XrmDatabase rdb;
  XrmName namelist[3];
  XrmClass classlist[3];
  XrmValue value;
  XrmRepresentation type;

  rdb = XrmGetDatabase (compositor.display);

  if (!rdb)
    return;

  namelist[1] = XrmStringToQuark ("wmProtocols");
  namelist[0] = app_quark;
  namelist[2] = NULLQUARK;

  classlist[1] = XrmStringToQuark ("WmProtocols");
  classlist[0] = resource_quark;
  classlist[2] = NULLQUARK;

  if (XrmQGetResource (rdb, namelist, classlist,
		       &type, &value)
      && type == QString)
    *string_return = (const char *) value.addr;
}

static int
ParseWmProtocols (const char *string)
{
  const char *end, *sep;
  char *buffer;
  int wm_protocols;

  wm_protocols = 0;
  end = string + strlen (string);

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

      if (!strcmp (buffer, "netWmPing"))
	wm_protocols |= NetWmPingMask;
      else
	fprintf (stderr, "Warning: encountered invalid window manager "
		 "protocol: %s\n", buffer);

      string = sep + 1;
    }

  return wm_protocols;
}

void
XLInitXdgToplevels (void)
{
  const char *wm_protocols;

  latin_1_cd = iconv_open ("ISO-8859-1", "UTF-8");
  apply_state_workaround = (getenv ("APPLY_STATE_WORKAROUND") != NULL);
  batch_state_changes = !getenv ("DIRECT_STATE_CHANGES");

  /* Determine which window manager protocols are to be enabled.  */
  wm_protocols = "netWmPing,";
  ReadWmProtocolsString (&wm_protocols);
  window_manager_protocols = ParseWmProtocols (wm_protocols);
}

Bool
XLIsXdgToplevel (Window window)
{
  return XLLookUpXdgToplevel (window) != NULL;
}



static void
DestroyDecoration (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
SetMode (struct wl_client *client, struct wl_resource *resource,
	 uint32_t mode)
{
  XdgDecoration *decoration;

  decoration = wl_resource_get_user_data (resource);

  if (!decoration->toplevel)
    return;

  switch (mode)
    {
    case ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE:
      decoration->toplevel->decor = DecorationModeClient;
      break;

    case ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE:
      decoration->toplevel->decor = DecorationModeWindowManager;
      break;

    default:
      wl_resource_post_error (resource, WL_DISPLAY_ERROR_IMPLEMENTATION,
			      "trying to set bogus decoration mode %u",
			      mode);
      return;
    }

  /* According to #wayland the configure event shouldn't be sent for
     partially initialized surfaces.  */
  if (decoration->toplevel->state & StateEverMapped)
    SendDecorationConfigure (decoration->toplevel);
  else
    decoration->toplevel->state |= StateNeedDecorationConfigure;
}

static void
UnsetMode (struct wl_client *client, struct wl_resource *resource)
{
  XdgDecoration *decoration;

  decoration = wl_resource_get_user_data (resource);

  if (!decoration->toplevel)
    return;

  /* Default to using window manager decorations.  */
  decoration->toplevel->decor = DecorationModeWindowManager;

  /* According to #wayland the configure event shouldn't be sent for
     partially initialized surfaces.  */
  if (decoration->toplevel->state & StateEverMapped)
    SendDecorationConfigure (decoration->toplevel);
  else
    decoration->toplevel->state |= StateNeedDecorationConfigure;
}

static struct zxdg_toplevel_decoration_v1_interface decoration_impl =
  {
    .destroy = DestroyDecoration,
    .set_mode = SetMode,
    .unset_mode = UnsetMode,
  };

static void
HandleDecorationResourceDestroy (struct wl_resource *resource)
{
  XdgDecoration *decoration;

  decoration = wl_resource_get_user_data (resource);

  /* Detach the decoration from the toplevel if the latter still
     exists.  */
  if (decoration->toplevel)
    {
      decoration->toplevel->decoration = NULL;

      /* Clear StateNeedDecorationConfigure, as not doing so may
	 result in decoration->toplevel (NULL) being dereferenced by
	 SendDecorationConfigure1.  */
      decoration->toplevel->state &= ~StateNeedDecorationConfigure;
    }

  /* Free the decoration.  */
  XLFree (decoration);
}

void
XLXdgToplevelGetDecoration (XdgRoleImplementation *impl,
			    struct wl_resource *resource,
			    uint32_t id)
{
  XdgToplevel *toplevel;
  XdgDecoration *decoration;

  toplevel = ToplevelFromRoleImpl (impl);

  /* See if a decoration object is already attached.  */
  if (toplevel->decoration)
    {
#define AlreadyConstructed ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED
      wl_resource_post_error (resource, AlreadyConstructed,
			      "the given toplevel already has a decoration"
			      "object.");
#undef AlreadyConstructed
      return;
    }

  /* See if a buffer is already attached.  */
  if (toplevel->role->surface
      && toplevel->role->surface->current_state.buffer)
    {
#define UnconfiguredBuffer ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER
      wl_resource_post_error (resource, UnconfiguredBuffer,
			      "given toplevel already has attached buffer");
#undef UnconfiguredBuffer
      return;
    }

  decoration = XLSafeMalloc (sizeof *decoration);

  if (!decoration)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (decoration, 0, sizeof *decoration);
  decoration->resource
    = wl_resource_create (wl_resource_get_client (resource),
			  &zxdg_toplevel_decoration_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!decoration->resource)
    {
      XLFree (decoration);
      wl_resource_post_no_memory (resource);
      return;
    }

  /* Now attach the decoration to the toplevel and vice versa.  */
  toplevel->decoration = decoration;
  decoration->toplevel = toplevel;

  /* And set the implementation.  */
  wl_resource_set_implementation (decoration->resource, &decoration_impl,
				  decoration, HandleDecorationResourceDestroy);
}
