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
#include <string.h>
#include <stdlib.h>

#include "compositor.h"

#include <xcb/shape.h>

/* This module implements the Xdnd protocol.

   Drags between Wayland clients are implemented in seat.c and
   data_device.c instead.  */

enum
  {
    XdndProtocolVersion = 5,
  };

typedef struct _DndState DndState;
typedef struct _DragState DragState;
typedef struct _WindowCache WindowCache;
typedef struct _WindowCacheEntry WindowCacheEntry;
typedef struct _WindowCacheEntryHeader WindowCacheEntryHeader;

enum
  {
    IsMapped	   = 1,
    IsDestroyed	   = (1 << 2),
    IsToplevel	   = (1 << 3),
    IsNotToplevel  = (1 << 4),
    IsPropertyRead = (1 << 5),
    IsShapeDirtied = (1 << 6),
  };

struct _DndState
{
  /* The source window.  */
  Window source_window;

  /* The target window.  */
  Window target_window;

  /* The seat that is being used.  */
  Seat *seat;

  /* The key for the seat destruction callback.  */
  void *seat_callback;

  /* Array of selection targets, which are MIME types in the Xdnd
     protocol, making our interaction with Wayland clients very
     convenient.  */
  char **targets;

  /* The timestamp to use for accessing selection data.  */
  Time timestamp;

  /* The toplevel or child surface the pointer is currently
     inside.  */
  Surface *child;

  /* The unmap callback for that child.  */
  UnmapCallback *unmap_callback;

  /* The protocol version in use.  */
  int proto;

  /* Number of targets in that array.  */
  int ntargets;

  /* Monotonically increasing counter.  */
  unsigned int serial;

  /* Whether or not non-default values should be used to respond to
     drag-and-drop events.  */
  Bool respond;

  /* The struct wl_resource (s) associated with this drag and drop
     operation.  */
  XLList *resources;

  /* The surface associated with this drag and drop session.  */
  Surface *surface;

  /* The destroy callback associated with that surface.  */
  DestroyCallback *callback;

  /* The source action mask.  */
  uint32_t source_actions;

  /* The supported action and preferred action.  */
  uint32_t supported_actions, preferred_action;

  /* The chosen DND action.  */
  uint32_t used_action;

  /* Whether or not something was accepted.  */
  Bool accepted;

  /* Whether or not the transfer finished.  */
  Bool finished;

  /* Whether or not the drop has already happened.  */
  Bool dropped;

  /* The version of the XDND protocol being used.  */
  int version;
};

enum
  {
    TypeListSet		 = 1,
    MoreThanThreeTargets = (1 << 2),
    WaitingForStatus	 = (1 << 3),
    PendingPosition	 = (1 << 4),
    PendingDrop		 = (1 << 5),
    WillAcceptDrop	 = (1 << 6),
    NeedMouseRect	 = (1 << 7),
    SelectionFailed	 = (1 << 8),
    SelectionSet	 = (1 << 9),
    ActionListSet	 = (1 << 10),
  };

struct _DragState
{
  /* The seat performing the drag.  */
  Seat *seat;

  /* The seat destroy callback.  */
  void *seat_key;

  /* The seat modifier callback.  */
  void *mods_key;

  /* The window cache.  */
  WindowCache *window_cache;

  /* The time at which ownership of the selection was obtained.  */
  Time timestamp;

  /* The selected action.  */
  Atom action;

  /* The last coordinates the pointer was seen at.  */
  int last_root_x, last_root_y;

  /* The last toplevel window the pointer entered, and the actual
     window client messages will be sent to.  */
  Window toplevel, target;

  /* The first 3 targets.  */
  Atom first_targets[3];

  /* The protocol version of the target.  */
  int version;

  /* Some flags.  */
  int flags;

  /* Rectangle within which further position events should not be
     sent.  */
  XRectangle mouse_rect;

  /* The modifiers currently held down.  */
  unsigned int modifiers;
};

struct _WindowCache
{
  /* The association table between windows and entries.  */
  XLAssocTable *entries;

  /* The root window.  */
  WindowCacheEntry *root_window;
};

struct _WindowCacheEntryHeader
{
  /* The next and last window cache entries.  Not set on the root
     window.  */
  WindowCacheEntry *next, *last;
};

struct _WindowCacheEntry
{
  /* The next and last window cache entries.  Not set on the root
     window.  */
  WindowCacheEntry *next, *last;

  /* The XID of the window.  */
  Window window;

  /* The XID of the parent.  */
  Window parent;

  /* Linked list of children.  The first node is a sentinel node that
     is really a WindowCacheEntryHeader.  */
  WindowCacheEntry *children;

  /* The XDND proxy window.  Usually None.  */
  Window dnd_proxy;

  /* The window cache.  */
  WindowCache *cache;

  /* The old event mask.  Not set on the root window.  */
  unsigned long old_event_mask;

  /* The key for input selection, if this is the root window.  */
  RootWindowSelection *input_key;

  /* The bounds of the window relative to its parents.  */
  int x, y, width, height;

  /* Some flags.  The protocol version is flags >> 16 & 0xff; 0 means
     XDND is not supported.  */
  int flags;

  /* The region describing its shape.  */
  pixman_region32_t shape;
};

/* The global drop state.  */
static DndState dnd_state;

/* The global drag state.  */
static DragState drag_state;

/* The DataSource to which XdndFinish events will be set.  */
static DataSource *finish_source;

/* The version of any XdndFinish event received.  */
static int finish_version;

/* The action selected at the time of receiving the XdndFinish
   event.  */
static Atom finish_action;

/* The destroy callback for that data source.  */
static void *finish_source_key;

/* The timeout for that data source.  */
static Timer *finish_timeout;

/* Forward declaration.  */

static void FinishDndEntry (void);

static Seat *
AssignSeat (void)
{
  /* Since the XDND protocol doesn't provide any way to determine the
     seat a drag-and-drop operation is originating from, simply return
     the first seat to be created.  */

  if (live_seats)
    return live_seats->data;

  return NULL;
}

static void
HandleSeatDestroy (void *data)
{
  dnd_state.seat = NULL;
  dnd_state.seat_callback = NULL;

  /* Since the seat has been destroyed, finish the drag and drop
     operation.  */
  FinishDndEntry ();
}

static uint32_t
TranslateAction (Atom action)
{
  if (action == XdndActionCopy)
    return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;

  if (action == XdndActionMove)
    return WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;

  if (action == XdndActionAsk)
    return WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;

  /* Wayland doesn't have an equivalent to XdndActionPrivate, so fall
     back to copy.  */
  if (action == XdndActionPrivate)
    return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;

  /* Otherwise, return None.  */
  return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}

/* Forward declarations.  */

static void SendStatus (void);
static void RespondToDndDrop (void);

static void
Accept (struct wl_client *client, struct wl_resource *resource,
	uint32_t serial, const char *mime_type)
{
  uint32_t sc;

  sc = (intptr_t) wl_resource_get_user_data (resource);

  if (sc < dnd_state.serial
      || dnd_state.source_window == None)
    /* This data offer is out of date.  */
    return;

  if (wl_resource_get_version (resource) <= 2)
    /* In version 2 and below, this doesn't affect anything.  */
    return;

  if (!mime_type)
    {
      if (dnd_state.accepted)
	/* The accepted state changed.  */
	SendStatus ();

      dnd_state.accepted = False;
    }
  else
    {
      if (!dnd_state.accepted)
	/* The accepted state changed.  */
	SendStatus ();

      dnd_state.accepted = True;
    }
}

static void
Receive (struct wl_client *client, struct wl_resource *resource,
	 const char *mime_type, int fd)
{
  uint32_t serial;

  serial = (intptr_t) wl_resource_get_user_data (resource);

  if (serial < dnd_state.serial
      || dnd_state.source_window == None)
    {
      /* This data offer is out of date.  */
      close (fd);
      return;
    }

  XLReceiveDataFromSelection (dnd_state.timestamp, XdndSelection,
			      InternAtom (mime_type), fd);
}

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
Finish (struct wl_client *client, struct wl_resource *resource)
{
  uint32_t serial;

  serial = (intptr_t) wl_resource_get_user_data (resource);

  if (serial < dnd_state.serial
      || !dnd_state.used_action
      || !dnd_state.accepted
      || dnd_state.finished)
    {
      wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
			      "finish called at inopportune moment");
      return;
    }

  dnd_state.finished = True;

  /* If XdndDrop was received, send the XdndFinished message.  */
  if (dnd_state.dropped)
    RespondToDndDrop ();
}

static Atom
ConvertAction (uint32_t action)
{
  if (action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
    return XdndActionCopy;

  if (action == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
    return XdndActionMove;

  if (action == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
    return XdndActionAsk;

  return None;
}

static void
SendStatus (void)
{
  XEvent event;

  if (dnd_state.dropped)
    return;

  memset (&event, 0, sizeof event);
  event.xclient.type = ClientMessage;
  event.xclient.window = dnd_state.source_window;
  event.xclient.message_type = XdndStatus;
  event.xclient.format = 32;

  event.xclient.data.l[0] = dnd_state.target_window;

  if (dnd_state.respond)
    {
      if ((dnd_state.used_action
	   != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE)
	  && dnd_state.accepted)
	event.xclient.data.l[1] = 1;

      if (dnd_state.version >= 3)
	event.xclient.data.l[4] = ConvertAction (dnd_state.used_action);
      else
	/* The version of the data device manager protocol spoken by
	   the client doesn't support actions.  Use XdndActionPrivate.  */
	event.xclient.data.l[4] = XdndActionPrivate;
    }

  CatchXErrors ();
  XSendEvent (compositor.display, dnd_state.source_window,
	      False, NoEventMask, &event);
  UncatchXErrors (NULL);
}

static void
UpdateUsedAction (void)
{
  uint32_t intersection, old;
  XLList *list;

  old = dnd_state.used_action;

  /* First, see if the preferred action is supported.  If it is,
     simply use it.  */
  if (dnd_state.source_actions & dnd_state.preferred_action)
    dnd_state.used_action = dnd_state.preferred_action;
  else
    {
      intersection = (dnd_state.supported_actions
		      & dnd_state.source_actions);

      /* Now, try the following actions in order.  */
      if (intersection & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
	dnd_state.used_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
      else if (intersection & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
	dnd_state.used_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
      else if (intersection & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
	dnd_state.used_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
      else
	dnd_state.used_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    }

  /* Send the updated action to clients if it changed.  */
  if (old != dnd_state.used_action)
    {
      for (list = dnd_state.resources; list; list = list->next)
	{
	  if (wl_resource_get_version (list->data) >= 3)
	    wl_data_offer_send_action (list->data,
				       dnd_state.used_action);
	}
    }

  /* Send an XdndStatus if the action changed.  */
  SendStatus ();
}

static void
SetActions (struct wl_client *client, struct wl_resource *resource,
	    uint32_t dnd_actions, uint32_t preferred_action)
{
  uint32_t serial;

  serial = (intptr_t) wl_resource_get_user_data (resource);

  if (serial < dnd_state.serial
      || !dnd_state.source_window)
    /* This data offer is out of date.  */
    return;

  if (dnd_actions & ~(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
		      | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
		      | WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
      || (preferred_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK
	  && preferred_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
	  && preferred_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
	  && preferred_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE))
    wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_ACTION,
			    "invalid action or action mask among: %u %u",
			    dnd_actions, preferred_action);

  /* Otherwise, update the DND state with the supported action.  */
  dnd_state.supported_actions = dnd_actions;
  dnd_state.preferred_action = preferred_action;

  /* And send the updated state.  */
  UpdateUsedAction ();
}

static const struct wl_data_offer_interface wl_data_offer_impl =
  {
    .accept = Accept,
    .receive = Receive,
    .destroy = Destroy,
    .finish = Finish,
    .set_actions = SetActions,
  };

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  uint32_t serial;

  serial = (intptr_t) wl_resource_get_user_data (resource);

  if (serial >= dnd_state.serial
      && dnd_state.source_window != None)
    {
      /* Send XdndFinish if it hasn't already been sent.  Since the
	 resource has been destroyed without previously completing,
	 signal an error if its version is 3 or later.  */

      if (wl_resource_get_version (resource) >= 3)
	dnd_state.accepted = False;

      if (dnd_state.dropped)
	RespondToDndDrop ();

      /* Remove the resource from the resource list.  */
      dnd_state.resources = XLListRemove (dnd_state.resources,
					  resource);

      /* If there are no more resources, finish the drag and drop
	 operation.  Note that this might've already been done by
	 RespondToDndDrop, but it is safe to call FinishDndEntry
	 twice.  */
      if (!dnd_state.resources)
	FinishDndEntry ();
    }
}

static struct wl_resource *
CreateOffer (struct wl_client *client, int version)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_data_offer_interface,
				 version, 0);

  if (!resource)
    return NULL;

  wl_resource_set_implementation (resource, &wl_data_offer_impl,
				  (void *) (intptr_t) dnd_state.serial,
				  HandleResourceDestroy);
  dnd_state.resources = XLListPrepend (dnd_state.resources,
				       resource);

  /* If version <= 2, then the drag-and-drop operation should always
     be accepted, no matter whether or not accept is called.  */
  if (version <= 2)
    dnd_state.accepted = True;

  if (!dnd_state.version || dnd_state.version > version)
    dnd_state.version = version;

  return resource;
}

static void
SendOffers (struct wl_resource *resource)
{
  int i;

  for (i = 0; i < dnd_state.ntargets; ++i)
    {
      if (dnd_state.targets[i])
	wl_data_offer_send_offer (resource,
				  dnd_state.targets[i]);
    }
}

static void
FinishDndEntry (void)
{
  int i;

  if (dnd_state.seat && dnd_state.resources
      /* Don't send leave if a drop already happened.  */
      && !dnd_state.dropped)
    XLDataDeviceSendLeave (dnd_state.seat, dnd_state.surface,
			   NULL);

  dnd_state.source_window = None;
  dnd_state.target_window = None;
  dnd_state.surface = NULL;
  dnd_state.proto = 0;

  if (dnd_state.callback)
    XLSurfaceCancelRunOnFree (dnd_state.callback);
  dnd_state.callback = NULL;

  if (dnd_state.seat)
    XLSeatCancelDestroyListener (dnd_state.seat_callback);
  dnd_state.seat = NULL;
  dnd_state.seat_callback = NULL;

  if (dnd_state.child)
    XLSurfaceCancelUnmapCallback (dnd_state.unmap_callback);
  dnd_state.child = NULL;
  dnd_state.unmap_callback = NULL;

  for (i = 0; i < dnd_state.ntargets; ++i)
    {
      if (dnd_state.targets[i])
	XFree (dnd_state.targets[i]);
    }

  XLFree (dnd_state.targets);
  dnd_state.ntargets = 0;
  dnd_state.targets = NULL;
  dnd_state.source_actions = 0;
  dnd_state.supported_actions = 0;
  dnd_state.preferred_action = 0;
  dnd_state.used_action = 0;
  dnd_state.version = 0;
  dnd_state.accepted = False;
  dnd_state.finished = False;
  dnd_state.dropped = False;
  dnd_state.timestamp = CurrentTime;

  /* The resources are not destroyed, since the client will do that
     later.  */
  XLListFree (dnd_state.resources, NULL);
  dnd_state.resources = NULL;
}

static void
RespondToDndDrop (void)
{
  XEvent event;

  memset (&event, 0, sizeof event);
  event.xclient.type = ClientMessage;
  event.xclient.window = dnd_state.source_window;
  event.xclient.message_type = XdndFinished;
  event.xclient.format = 32;

  event.xclient.data.l[0] = dnd_state.target_window;

  if (dnd_state.proto >= 5
      && dnd_state.used_action && dnd_state.accepted
      && dnd_state.seat && dnd_state.respond)
    {
      /* This determines whether or not the drag and drop operation
	 was accepted.  */
      event.xclient.data.l[1] = 1;

      if (dnd_state.version >= 3)
	/* And this specifies the action that was really taken.  */
	event.xclient.data.l[2] = ConvertAction (dnd_state.used_action);
      else
	/* The version of the data device manager protocol spoken by
	   the client doesn't support actions.  Use XdndActionPrivate.  */
	event.xclient.data.l[2] = XdndActionPrivate;
    }

  CatchXErrors ();
  XSendEvent (compositor.display, dnd_state.source_window,
	      False, NoEventMask, &event);
  UncatchXErrors (NULL);

  /* Now that XdndFinished has been sent, the drag and drop operation
     is complete.  */
  FinishDndEntry ();
}

static void
HandleSurfaceDestroy (void *data)
{
  dnd_state.surface = NULL;
  dnd_state.callback = NULL;
}

static void
HandleDndEntry (Surface *target, Window source, Atom *targets,
		int ntargets, int proto)
{
  int i;
  char **names;

  if (dnd_state.source_window)
    {
      fprintf (stderr, "XdndEnter received while a drag-and-drop operation"
	       " is in progress; overriding current drag-and-drop operation\n");
      FinishDndEntry ();
    }

  dnd_state.proto = proto;
  dnd_state.source_window = source;
  dnd_state.surface = target;
  dnd_state.callback = XLSurfaceRunOnFree (dnd_state.surface,
					   HandleSurfaceDestroy, NULL);

  /* Retrieve the atoms inside the targets list.  */
  names = XLCalloc (ntargets, sizeof *names);

  XGetAtomNames (compositor.display, targets,
		 ntargets, names);

  /* Enter the names of the targets into the atom table so that they
     can be interned without roundtrips in the future.  */
  for (i = 0; i < ntargets; ++i)
    {
      if (names[i])
	ProvideAtom (names[i], targets[i]);
    }

  /* Find a seat to use for this drag-and-drop operation.  */
  dnd_state.seat = AssignSeat ();

  /* If a seat was found, listen for its destruction.  After the
     initiating seat is destroyed (or if none was found), we reply to
     all future drag-and-drop messages with dummy values.  */
  if (dnd_state.seat)
    dnd_state.seat_callback = XLSeatRunOnDestroy (dnd_state.seat,
						  HandleSeatDestroy,
						  NULL);

  /* Initialize available data types from the atom names.  */
  dnd_state.targets = names;
  dnd_state.ntargets = ntargets;

  /* Initialize other drag-and-drop state.  */
  dnd_state.respond = False;

  /* There shouldn't be any leftovers from the last session.  */
  XLAssert (dnd_state.resources == NULL);

  /* Initialize the target window.  */
  dnd_state.target_window = XLWindowFromSurface (target);

  /* Increase the state counter to make all out-of-date data offers
     invalid.  */
  dnd_state.serial++;
}

static Atom *
ReadXdndTypeList (Window window, int *nitems_return)
{
  Atom actual_type;
  int rc, actual_format;
  unsigned long nitems, bytes_remaining;
  unsigned char *tmp_data;

  tmp_data = NULL;

  CatchXErrors ();
  rc = XGetWindowProperty (compositor.display, window,
			   XdndTypeList, 0, LONG_MAX,
			   False, XA_ATOM, &actual_type,
			   &actual_format, &nitems,
			   &bytes_remaining, &tmp_data);
  if (UncatchXErrors (NULL) || rc != Success || actual_format != 32
      || !tmp_data || actual_type != XA_ATOM || nitems < 1)
    {
      if (tmp_data)
	XFree (tmp_data);

      return NULL;
    }

  *nitems_return = nitems;
  return (Atom *) tmp_data;
}

static Bool
HandleXdndEnterEvent (Surface *surface, XEvent *event)
{
  Atom *targets;
  Atom builtin[3];
  int ntargets, proto;

  if (event->xclient.data.l[1] & 1)
    /* There are more than 3 targets; retrieve them from the
       XdndTypeList property.  */
    targets = ReadXdndTypeList (event->xclient.data.l[0],
				&ntargets);
  else
    {
      /* Otherwise, the first three properties contain the selection
	 targets.  */
      targets = builtin;
      ntargets = 0;

      if (event->xclient.data.l[2])
	builtin[ntargets++] = event->xclient.data.l[2];

      if (event->xclient.data.l[3])
	builtin[ntargets++] = event->xclient.data.l[3];

      if (event->xclient.data.l[4])
	builtin[ntargets++] = event->xclient.data.l[4];
    }

  if (!targets)
    /* For some reason we failed to retrieve XdndTypeList.  Ignore the
       XdndEnter event.  */
    return True;

  proto = MIN (event->xclient.data.l[1] >> 24,
	       XdndProtocolVersion);

  HandleDndEntry (surface, event->xclient.data.l[0],
		  targets, ntargets, proto);

  if (event->xclient.data.l[1] & 1)
    /* Now, free the type list, which was allocated by Xlib.  */
    XFree (targets);

  return True;
}

static void
HandleChildUnmap (void *data)
{
  /* The child was unmapped.  */

  if (dnd_state.seat)
    XLDataDeviceSendLeave (dnd_state.seat, dnd_state.child,
			   NULL);
  XLSurfaceCancelUnmapCallback (dnd_state.unmap_callback);

  dnd_state.child = NULL;
  dnd_state.unmap_callback = NULL;

  /* Free our record of the data offers introduced at entry time; it
     is assumed that the client will delete them too.  */
  XLListFree (dnd_state.resources, NULL);
  dnd_state.resources = NULL;
}

static Bool
HandleMotion (Surface *toplevel, int x, int y, uint32_t action,
	      int *x_out, int *y_out)
{
  Subcompositor *subcompositor;
  View *view;
  int x_off, y_off;
  Surface *child;
  DndOfferFuncs funcs;
  XLList *tem;

  subcompositor = ViewGetSubcompositor (toplevel->view);

  /* Find the view underneath the subcompositor.  */
  view = SubcompositorLookupView (subcompositor, x, y,
				  &x_off, &y_off);

  if (view)
    child = ViewGetData (view);
  else
    /* No child was found.  This should be impossible in theory, but
       other clients don't respect the window shape when sending DND
       events.  */
    child = NULL;

  /* Compute the surface-relative coordinates and scale them.  */

  if (child)
    /* x_out and y_out are only used if dnd_state.child ends up
       non-NULL.  */
    TruncateWindowToSurface (child, x - x_off, y - y_off,
			     x_out, y_out);

  if (dnd_state.child == child)
    /* If nothing changed, don't do anything.  */
    return False;

  /* If the pointer was previously in a different surface, leave
     it.  */
  if (dnd_state.child)
    {
      XLDataDeviceSendLeave (dnd_state.seat, dnd_state.child,
			     NULL);
      XLSurfaceCancelUnmapCallback (dnd_state.unmap_callback);

      dnd_state.child = NULL;
      dnd_state.unmap_callback = NULL;

      /* Free our record of the data offers introduced at entry time;
	 it is assumed that the client will delete them too.  */
      XLListFree (dnd_state.resources, NULL);
      dnd_state.resources = NULL;
      dnd_state.used_action = 0;
      dnd_state.preferred_action = 0;
      dnd_state.supported_actions = 0;
      dnd_state.accepted = False;

      if (dnd_state.version <= 2)
	dnd_state.accepted = True;
    }

  /* Now, enter the new surface.  */
  if (child)
    {
      dnd_state.child = child;
      dnd_state.unmap_callback
	= XLSurfaceRunAtUnmap (child, HandleChildUnmap, NULL);
      funcs.create = CreateOffer;
      funcs.send_offers = SendOffers;

      /* Create the offers and send data to the clients.  */
      XLDataDeviceMakeOffers (dnd_state.seat, funcs, child, *x_out,
			      *y_out);
      /* Send source actions to each resource created.  */
      for (tem = dnd_state.resources; tem; tem = tem->next)
	{
	  if (wl_resource_get_version (tem->data) >= 3)
	    wl_data_offer_send_source_actions (tem->data, action);
	}

      /* Now compute whether or not we should respond with actual
	 values.  */
      dnd_state.respond = (dnd_state.resources != NULL);
    }

  return child != NULL;
}

static uint32_t
ReadDndActionList (Window window)
{
  uint32_t mask;
  Atom actual_type, *atoms;
  int rc, actual_format;
  unsigned long nitems, bytes_remaining, i;
  unsigned char *tmp_data;

  mask = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
  tmp_data = NULL;

  CatchXErrors ();
  rc = XGetWindowProperty (compositor.display, window,
			   XdndActionList, 0, LONG_MAX,
			   False, XA_ATOM, &actual_type,
			   &actual_format, &nitems,
			   &bytes_remaining, &tmp_data);
  if (UncatchXErrors (NULL) || rc != Success || actual_format != 32
      || !tmp_data || actual_type != XA_ATOM || nitems < 1)
    {
      if (tmp_data)
	XFree (tmp_data);

      return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    }

  atoms = (Atom *) tmp_data;

  for (i = 0; i < nitems; ++i)
    mask |= TranslateAction (atoms[i]);

  return mask;
}

static Bool
HandleXdndPositionEvent (Surface *surface, XEvent *event)
{
  int root_x, root_y, x, y;
  Window child;
  XLList *tem;
  uint32_t action;
  Bool sent_actions;

  if (event->xclient.data.l[0] != dnd_state.source_window)
    /* The message is coming from the wrong window, or drag and drop
       has not yet been set up.  */
    return True;

  if (surface != dnd_state.surface)
    /* This message is being delivered to the wrong surface.  */
    return True;

  /* Extract the root X and root Y from the event.  */
  root_x = event->xclient.data.l[2] >> 16;
  root_y = event->xclient.data.l[2] & 0xffff;

  /* Translate the coordinates to the surface's window.  */
  XTranslateCoordinates (compositor.display,
			 DefaultRootWindow (compositor.display),
			 XLWindowFromSurface (surface),
			 root_x, root_y, &x, &y, &child);

  action = TranslateAction (event->xclient.data.l[4]);

  /* Handle mouse motion.  */
  sent_actions = HandleMotion (surface, x, y, action, &x, &y);

  if (action == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
    {
      if (!(action & dnd_state.source_actions))
	/* Fetch the list of available actions, and give that to the
	   client along with the regular action list, if XdndActionAsk
	   is being specified for the first time.  */
	action |= ReadDndActionList (dnd_state.source_window);
      else
	/* Otherwise, preserve the action list that was already
	   read.  */
	action |= dnd_state.source_actions;
    }
  /* Send actions from all data offers.  */
  if (dnd_state.resources && !sent_actions)
    {
      /* If action is different from the current source action, send
	 the new source action to the client.  */

      if (action != dnd_state.source_actions)
	{
	  /* Send source actions to each resource created.  */
	  for (tem = dnd_state.resources; tem; tem = tem->next)
	    {
	      if (wl_resource_get_version (tem->data) >= 3)
		wl_data_offer_send_source_actions (tem->data, action);
	    }

	  /* Update the chosen action.  */
	  UpdateUsedAction ();
	}
    }

  dnd_state.source_actions = action;
  dnd_state.timestamp = event->xclient.data.l[3];

  if (dnd_state.seat && dnd_state.child)
    XLDataDeviceSendMotion (dnd_state.seat, surface,
			    /* l[3] is the timestamp of the
			       movement.  */
			    x, y, event->xclient.data.l[3]);

  /* Send an XdndStatus event in response.  */
  SendStatus ();

  return True;
}

static Bool
HandleXdndLeaveEvent (Surface *surface, XEvent *event)
{
  if (event->xclient.data.l[0] != dnd_state.source_window)
    /* The message is coming from the wrong window, or drag and drop
       has not yet been set up.  */
    return True;

  if (surface != dnd_state.surface)
    /* This message is being delivered to the wrong surface.  */
    return True;

  FinishDndEntry ();

  return True;
}

static Bool
HandleXdndDropEvent (Surface *surface, XEvent *event)
{
  if (event->xclient.data.l[0] != dnd_state.source_window)
    /* The message is coming from the wrong window, or drag and drop
       has not yet been set up.  */
    return True;

  if (surface != dnd_state.surface)
    /* This message is being delivered to the wrong surface.  */
    return True;

  dnd_state.timestamp = event->xclient.data.l[2];
  
  XLDataDeviceSendDrop (dnd_state.seat, surface);

  /* If finish has already been called, send XdndFinish to the source,
     and complete the transfer.  */
  if (dnd_state.finished
      /* Also respond (but with default values) if the transfer cannot
	 continue because the seat has been destroyed.  */
      || !dnd_state.respond
      || !dnd_state.seat
      /* Also respond if the resource version is less than 3.  */
      || dnd_state.version <= 2)
    RespondToDndDrop ();

  /* Set dnd_state.dropped.  */
  dnd_state.dropped = True;

  return True;
}

void
XLDndWriteAwarenessProperty (Window window)
{
  unsigned long version;

  version = XdndProtocolVersion;
  XChangeProperty (compositor.display, window,
		   XdndAware, XA_ATOM, 32, PropModeReplace,
		   (unsigned char *) &version, 1);
}

/* Keep in mind that the given surface should be a toplevel surface
   with a subcompositor attached.  */

Bool
XLDndFilterClientMessage (Surface *surface, XEvent *event)
{
  if (event->xclient.message_type == XdndEnter)
    return HandleXdndEnterEvent (surface, event);
  else if (event->xclient.message_type == XdndPosition)
    return HandleXdndPositionEvent (surface, event);
  else if (event->xclient.message_type == XdndLeave)
    return HandleXdndLeaveEvent (surface, event);
  else if (event->xclient.message_type == XdndDrop)
    return HandleXdndDropEvent (surface, event);

  return False;
}

/* Window cache management.  This allows us to avoid looking up the
   window shape each time we encounter a window.  */


static void
AddAfter (WindowCacheEntry *entry, WindowCacheEntry *after)
{
  entry->next = after->next;
  entry->last = after;
  after->next->last = entry;
  after->next = entry;
}

/* Forward declaration.  */
static void AddChildren (WindowCacheEntry *, xcb_query_tree_reply_t *);

static void
InitRegionWithRects (pixman_region32_t *region,
		     xcb_shape_get_rectangles_reply_t *rects)
{
  pixman_box32_t *boxes;
  xcb_rectangle_t *rectangles;
  int nrects, i;

  nrects = xcb_shape_get_rectangles_rectangles_length (rects);

  if (nrects > 64)
    boxes = XLMalloc (sizeof *boxes * nrects);
  else
    boxes = alloca (sizeof *boxes * nrects);

  rectangles = xcb_shape_get_rectangles_rectangles (rects);

  for (i = 0; i < nrects; ++i)
    {
      /* Convert the X rectangles to pixman boxes.  Pixman (X server)
	 boxes have x2, y2, set to a value one pixel larger than the
	 actual maximum pixels set, which is why we do not subtract 1
	 from rect->x + rect->width.  */

      boxes[i].x1 = rectangles[i].x;
      boxes[i].y1 = rectangles[i].y;
      boxes[i].x2 = rectangles[i].x + rectangles[i].width;
      boxes[i].y2 = rectangles[i].y + rectangles[i].height;
    }

  /* Initialize the region with those boxes.  */
  pixman_region32_init_rects (region, boxes, nrects);

  if (nrects > 64)
    XLFree (boxes);
}

static void
IntersectRegionWith (pixman_region32_t *region,
		     xcb_shape_get_rectangles_reply_t *rects)
{
  pixman_box32_t *boxes;
  xcb_rectangle_t *rectangles;
  int nrects, i;
  pixman_region32_t temp;

  nrects = xcb_shape_get_rectangles_rectangles_length (rects);

  if (nrects > 64)
    boxes = XLMalloc (sizeof *boxes * nrects);
  else
    boxes = alloca (sizeof *boxes * nrects);

  rectangles = xcb_shape_get_rectangles_rectangles (rects);

  for (i = 0; i < nrects; ++i)
    {
      /* Convert the X rectangles to pixman boxes.  Pixman (X server)
	 boxes have x2, y2, set to a value one pixel larger than the
	 actual maximum pixels set, which is why we do not subtract 1
	 from rect->x + rect->width.  */

      boxes[i].x1 = rectangles[i].x;
      boxes[i].y1 = rectangles[i].y;
      boxes[i].x2 = rectangles[i].x + rectangles[i].width;
      boxes[i].y2 = rectangles[i].y + rectangles[i].height;
    }

  /* Initialize the temporary region with those boxes.  */
  pixman_region32_init_rects (&temp, boxes, nrects);

  if (nrects > 64)
    XLFree (boxes);

  /* Intersect the other region with this one.  */
  pixman_region32_intersect (region, region, &temp);

  /* Free the temporary region.  */
  pixman_region32_fini (&temp);
}

static void
AddChild (WindowCacheEntry *parent, Window window,
	  xcb_get_geometry_reply_t *geometry,
	  xcb_query_tree_reply_t *children,
	  xcb_get_window_attributes_reply_t *attributes,
	  xcb_shape_get_rectangles_reply_t *bounding,
	  xcb_shape_get_rectangles_reply_t *input)
{
  WindowCacheEntry *entry;
  unsigned long mask;

  entry = XLCalloc (1, sizeof *entry);

  entry->window = window;
  entry->parent = parent->window;
  entry->x = geometry->x;
  entry->y = geometry->y;
  entry->width = geometry->width;
  entry->height = geometry->height;
  entry->children = XLMalloc (sizeof (WindowCacheEntryHeader));
  entry->children->next = entry->children;
  entry->children->last = entry->children;

  InitRegionWithRects (&entry->shape, bounding);
  IntersectRegionWith (&entry->shape, input);

  entry->cache = parent->cache;
  entry->old_event_mask = attributes->your_event_mask;

  if (attributes->map_state != XCB_MAP_STATE_UNMAPPED)
    entry->flags |= IsMapped;

  mask = (entry->old_event_mask
	  | SubstructureNotifyMask
	  | PropertyChangeMask);

  /* Select for SubstructureNotifyMask, so hierarchy events can be
     received for it and its children.  X errors should be caught
     around here.  In addition, we also ask for PropertyNotifyMask, so
     that IsToplevel/IsNotToplevel can be cleared correctly in
     response to changes of the WM_STATE property.  */
  XSelectInput (compositor.display, window, mask);

  /* Select for ShapeNotify events as well.  This allows us to update
     the shapes of each toplevel window along the way.  */
  xcb_shape_select_input (compositor.conn, window, 1);

  /* Insert the child in front of the window list.  */
  AddAfter (entry, parent->children);

  /* Add this child to the assoc table.  */
  XLMakeAssoc (parent->cache->entries, window,
	       entry);

  /* Add this child's children.  */
  AddChildren (entry, children);
}

static void
AddChildren (WindowCacheEntry *entry, xcb_query_tree_reply_t *reply)
{
  xcb_window_t *windows;
  int n_children, i;
  xcb_get_geometry_cookie_t *geometries;
  xcb_query_tree_cookie_t *children;
  xcb_get_window_attributes_cookie_t *attributes;
  xcb_shape_get_rectangles_cookie_t *boundings;
  xcb_shape_get_rectangles_cookie_t *inputs;
  xcb_get_geometry_reply_t *geometry;
  xcb_query_tree_reply_t *tree;
  xcb_get_window_attributes_reply_t *attribute;
  xcb_shape_get_rectangles_reply_t *bounding;
  xcb_shape_get_rectangles_reply_t *input;
  xcb_generic_error_t *error, *error1, *error2, *error3, *error4;
  xcb_get_geometry_reply_t **all_geometries;
  xcb_query_tree_reply_t **all_trees;
  xcb_get_window_attributes_reply_t **all_attributes;
  xcb_shape_get_rectangles_reply_t **all_boundings;
  xcb_shape_get_rectangles_reply_t **all_inputs;

  error = NULL;
  error1 = NULL;
  error2 = NULL;
  error3 = NULL;
  error4 = NULL;

  windows = xcb_query_tree_children (reply);
  n_children = xcb_query_tree_children_length (reply);

  /* First, issue all the requests for necessary information.  */
  geometries = XLMalloc (sizeof *geometries * n_children);
  children = XLMalloc (sizeof *children * n_children);
  attributes = XLMalloc (sizeof *attributes * n_children);
  boundings = XLMalloc (sizeof *boundings * n_children);
  inputs = XLMalloc (sizeof *inputs * n_children);
  all_geometries = XLCalloc (n_children, sizeof *all_geometries);
  all_trees = XLCalloc (n_children, sizeof *all_trees);
  all_attributes = XLCalloc (n_children, sizeof *all_attributes);
  all_boundings = XLCalloc (n_children, sizeof *all_boundings);
  all_inputs = XLCalloc (n_children, sizeof *all_inputs);

  for (i = 0; i < n_children; ++i)
    {
      geometries[i] = xcb_get_geometry (compositor.conn,
					windows[i]);
      children[i] = xcb_query_tree (compositor.conn,
				    windows[i]);
      attributes[i] = xcb_get_window_attributes (compositor.conn,
						 windows[i]);
      boundings[i] = xcb_shape_get_rectangles (compositor.conn,
					       windows[i],
					       XCB_SHAPE_SK_BOUNDING);
      inputs[i] = xcb_shape_get_rectangles (compositor.conn,
					    windows[i],
					    XCB_SHAPE_SK_INPUT);
    }

  /* Next, retrieve selection replies.  */
  for (i = 0; i < n_children; ++i)
    {
      geometry = xcb_get_geometry_reply (compositor.conn,
					 geometries[i],
					 &error);
      tree = xcb_query_tree_reply (compositor.conn,
				   children[i],
				   &error1);
      attribute = xcb_get_window_attributes_reply (compositor.conn,
						   attributes[i],
						   &error2);
      bounding = xcb_shape_get_rectangles_reply (compositor.conn,
						 boundings[i],
						 &error3);
      input = xcb_shape_get_rectangles_reply (compositor.conn,
					      inputs[i],
					      &error4);

      if (error || error1 || error2 || error3 || error4
	  || !geometry || !tree || !attribute || !bounding || !input)
	{
	  if (error)
	    free (error);

	  if (error1)
	    free (error1);

	  if (error2)
	    free (error2);

	  if (error3)
	    free (error3);

	  if (error4)
	    free (error4);

	  if (geometry)
	    free (geometry);

	  if (tree)
	    free (tree);

	  if (attribute)
	    free (attribute);

	  if (bounding)
	    free (bounding);

	  if (input)
	    free (input);

	  /* If an error occured, don't save the window.  */
	  continue;
	}

      /* Save the geometry and tree replies.  */
      all_geometries[i] = geometry;
      all_trees[i] = tree;
      all_attributes[i] = attribute;
      all_boundings[i] = bounding;
      all_inputs[i] = input;
    }

  /* And prepend all of the windows for which we got valid
     replies.  */
  for (i = 0; i < n_children; ++i)
    {
      if (!all_geometries[i])
	continue;

      AddChild (entry, windows[i], all_geometries[i],
		all_trees[i], all_attributes[i],
		all_boundings[i], all_inputs[i]);

      free (all_geometries[i]);
      free (all_trees[i]);
      free (all_attributes[i]);
      free (all_boundings[i]);
      free (all_inputs[i]);
    }

  /* Free all the allocated temporary data.  */
  XLFree (geometries);
  XLFree (children);
  XLFree (attributes);
  XLFree (boundings);
  XLFree (inputs);
  XLFree (all_geometries);
  XLFree (all_trees);
  XLFree (all_attributes);
  XLFree (all_boundings);
  XLFree (all_inputs);
}

static void
MakeRootWindowEntry (WindowCache *cache)
{
  WindowCacheEntry *entry;
  xcb_get_geometry_cookie_t geometry_cookie;
  xcb_query_tree_cookie_t tree_cookie;
  Window root;
  xcb_get_geometry_reply_t *geometry;
  xcb_query_tree_reply_t *tree;

  root = DefaultRootWindow (compositor.display);

  entry = XLCalloc (1, sizeof *entry);
  entry->window = DefaultRootWindow (compositor.display);
  entry->parent = None;

  entry->children = XLMalloc (sizeof (WindowCacheEntryHeader));
  entry->children->next = entry->children;
  entry->children->last = entry->children;

  /* Obtain the geometry of the root window, and its children.  */
  geometry_cookie = xcb_get_geometry (compositor.conn, root);
  tree_cookie = xcb_query_tree (compositor.conn, root);

  /* Get the replies from those requests.  */
  geometry = xcb_get_geometry_reply (compositor.conn, geometry_cookie,
				     NULL);
  tree = xcb_query_tree_reply (compositor.conn, tree_cookie, NULL);

  if (!geometry || !tree)
    {
      /* This should not happen in principle.  */
      fprintf (stderr, "failed to obtain window geometry or tree"
	       " of root window");
      abort ();
    }

  entry->x = geometry->x;
  entry->y = geometry->y;
  entry->width = geometry->width;
  entry->height = geometry->height;
  entry->flags |= IsMapped;

  /* The root window shouldn't have an input shape.  */
  pixman_region32_init_rect (&entry->shape, entry->x,
			     entry->y, entry->width,
			     entry->height);

  /* Select for SubstructureNotifyMask on the root window.  */
  entry->input_key
    = XLSelectInputFromRootWindow (SubstructureNotifyMask);

  /* Attach the entry to the cache.  */
  entry->cache = cache;
  cache->root_window = entry;
  XLMakeAssoc (cache->entries, root, entry);

  /* Add children to this window cache.  */
  CatchXErrors ();
  AddChildren (entry, tree);
  UncatchXErrors (NULL);

  free (geometry);
  free (tree);
}

static WindowCache *
AllocWindowCache (void)
{
  WindowCache *cache;

  cache = XLMalloc (sizeof *cache);
  cache->entries = XLCreateAssocTable (2048);
  MakeRootWindowEntry (cache);

  return cache;
}

static void
FreeWindowCacheEntry (WindowCacheEntry *entry)
{
  WindowCacheEntry *next, *last;

  /* First, free all the children of the entry.  */
  next = entry->children->next;
  while (next != entry->children)
    {
      last = next;
      next = next->next;

      FreeWindowCacheEntry (last);
    }

  /* Remove the association.  */
  XLDeleteAssoc (entry->cache->entries,
		 entry->window);

  /* Free the sentinel node.  */
  XLFree (entry->children);

  if (entry->last)
    {
      /* Unlink the entry, unless it is the root window.  */
      entry->last->next = entry->next;
      entry->next->last = entry->last;

      if (!(entry->flags & IsDestroyed))
	{
	  /* Revert back to the old event mask.  */
	  XSelectInput (compositor.display, entry->window,
			entry->old_event_mask);

	  /* Also stop selecting for ShapeNotify events.  */
	  xcb_shape_select_input (compositor.conn,
				  entry->window, 0);
	}
    }
  else
    /* This is the root window; stop selecting for
       SubstructureNotifyMask.  */
    XLDeselectInputFromRootWindow (entry->input_key);

  /* Free the region.  */
  pixman_region32_fini (&entry->shape);

  /* Free the entry itself.  */
  XLFree (entry);
}

static void
FreeWindowCache (WindowCache *cache)
{
  /* This prevents BadWindow errors from trying to destroy a deleted
     entry.  */
  CatchXErrors ();

  /* Free the root window.  */
  FreeWindowCacheEntry (cache->root_window);

  UncatchXErrors (NULL);

  /* And the assoc table.  */
  XLDestroyAssocTable (cache->entries);

  /* Free the cache.  */
  XLFree (cache);
}

static void
UnlinkWindowCacheEntry (WindowCacheEntry *entry)
{
  entry->last->next = entry->next;
  entry->next->last = entry->last;
}

static void
HandleCirculateNotify (WindowCache *cache, XEvent *event)
{
  WindowCacheEntry *parent, *window;

  if (event->xcirculate.event == event->xcirculate.window)
    /* This is the result of StructureNotifyMask, and the parent
       window cannot be accessed through the event.  */
    return;

  parent = XLLookUpAssoc (cache->entries, event->xcirculate.event);

  if (!parent)
    return;

  window = XLLookUpAssoc (cache->entries, event->xcirculate.window);

  if (!window)
    return;

  XLAssert (window->parent == event->xcirculate.event);

  /* If the window has been recirculated to the top, relink it
     immediately after the list.  Otherwise, it has been recirculated
     to the bottom, so place it before the first element of the
     list.  */

  UnlinkWindowCacheEntry (window);

  if (event->xcirculate.place == PlaceOnTop)
    AddAfter (window->next, parent->children);
  else
    AddAfter (window->next, parent->children->last);
}

static void
HandleConfigureNotify (WindowCache *cache, XEvent *event)
{
  WindowCacheEntry *window, *parent, *next;

  if (event->xconfigure.event == event->xconfigure.window)
    /* This is the result of StructureNotifyMask, and the parent
       window cannot be accessed through the event.  */
    return;

  window = XLLookUpAssoc (cache->entries, event->xconfigure.window);
  parent = XLLookUpAssoc (cache->entries, event->xconfigure.event);

  /* Reinitialize the contents of the window with the new
     information.  */
  if (event->xconfigure.x != window->x
      || event->xconfigure.y != window->y
      || event->xconfigure.width != window->width
      || event->xconfigure.height != window->height)
    {
      window->x = event->xconfigure.x;
      window->y = event->xconfigure.y;
      window->width = event->xconfigure.width;
      window->height = event->xconfigure.height;

      /* If the window is unshaped, then the ConfigureNotify could've
	 changed the actual shape of the window.  Mark the shape as
	 dirty.  */
      pixman_region32_clear (&window->shape);
      window->flags |= IsShapeDirtied;
    }

  if (!parent)
    /* This is the root window or something like it.  */
    return;

  /* Move the window to the right place in the stacking order.  If
     event->xconfigure.above is None, this window is at the bottom.
     If it is anywhere else, move it there.  */
  if (event->xconfigure.above == None)
    {
      if (window->last == parent->children)
	/* This window is already at the bottom... */
	return;

      /* Unlink the window and relink it at the end of the parent.  */
      UnlinkWindowCacheEntry (window);

      /* Move the child to the end of the window list.  */
      AddAfter (window, parent->children->last);
    }
  else if (window->next == parent->children
	   || window->next->window != event->xconfigure.above)
    {
      /* Find the item corresponding to the sibling.  */
      next = parent->children->next;

      while (next != parent->children)
	{
	  if (next->window == event->xconfigure.above)
	    {
	      /* Move the item on top of next by placing it before
		 next.  */
	      UnlinkWindowCacheEntry (window);
	      AddAfter (window, next->last);
	      break;
	    }

	  next = next->next;
	}

      /* This shouldn't be reached if no entry was found.  I don't
	 know what to do in this case.  */
    }
}

static void
HandleCreateNotify (WindowCache *cache, XEvent *event)
{
  WindowCacheEntry *parent;
  xcb_get_geometry_cookie_t geometry_cookie;
  xcb_query_tree_cookie_t tree_cookie;
  xcb_get_window_attributes_cookie_t attributes_cookie;
  xcb_shape_get_rectangles_cookie_t bounding_cookie;
  xcb_shape_get_rectangles_cookie_t input_cookie;
  xcb_get_geometry_reply_t *geometry;
  xcb_query_tree_reply_t *tree;
  xcb_get_window_attributes_reply_t *attributes;
  xcb_shape_get_rectangles_reply_t *bounding;
  xcb_shape_get_rectangles_reply_t *input;
  xcb_generic_error_t *error, *error1, *error2, *error3, *error4;

  error = NULL;
  error1 = NULL;
  error2 = NULL;
  error3 = NULL;
  error4 = NULL;

  parent = XLLookUpAssoc (cache->entries, event->xcreatewindow.parent);

  if (!parent)
    return;

  /* If the window already exists (this can happen if AddWindow adds
     children before we get the CreateNotify event), just return.  */
  if (XLLookUpAssoc (cache->entries, event->xcreatewindow.window))
    return;

  /* Add the window in front of the parent.  */
  geometry_cookie = xcb_get_geometry (compositor.conn,
				      event->xcreatewindow.window);
  tree_cookie = xcb_query_tree (compositor.conn,
				event->xcreatewindow.window);
  attributes_cookie = xcb_get_window_attributes (compositor.conn,
						 event->xcreatewindow.window);
  bounding_cookie = xcb_shape_get_rectangles (compositor.conn,
					      event->xcreatewindow.window,
					      XCB_SHAPE_SK_BOUNDING);
  input_cookie = xcb_shape_get_rectangles (compositor.conn,
					   event->xcreatewindow.window,
					   XCB_SHAPE_SK_INPUT);

  /* Ask for replies from the X server.  */
  geometry = xcb_get_geometry_reply (compositor.conn, geometry_cookie,
				     &error);
  tree = xcb_query_tree_reply (compositor.conn, tree_cookie, &error1);
  attributes = xcb_get_window_attributes_reply (compositor.conn,
						attributes_cookie,
						&error2);
  bounding = xcb_shape_get_rectangles_reply (compositor.conn,
					     bounding_cookie,
					     &error3);
  input = xcb_shape_get_rectangles_reply (compositor.conn,
					  input_cookie, &error4);

  if (error || error1 || error2 || error3 || error4
      || !geometry || !tree || !attributes || !bounding || !input)
    {
      if (error)
	free (error);

      if (error1)
	free (error1);

      if (error2)
	free (error2);

      if (error3)
	free (error3);

      if (error4)
	free (error4);

      if (geometry)
	free (geometry);

      if (tree)
	free (tree);

      if (attributes)
	free (attributes);

      if (bounding)
	free (bounding);

      if (input)
	free (input);

      return;
    }

  /* Now, really add the window.  */
  CatchXErrors ();
  AddChild (parent, event->xcreatewindow.window, geometry,
	    tree, attributes, bounding, input);
  UncatchXErrors (NULL);

  /* And free the reply data.  */
  free (geometry);
  free (tree);
  free (attributes);
  free (bounding);
  free (input);
}

static void
HandleMapNotify (WindowCache *cache, XEvent *event)
{
  WindowCacheEntry *window;

  if (event->xmap.event == event->xmap.window)
    /* This is the result of StructureNotifyMask, and the parent
       window cannot be accessed through the event.  */
    return;

  window = XLLookUpAssoc (cache->entries, event->xmap.window);

  if (!window)
    return;

  window->flags |= IsMapped;
}

static void
HandleReparentNotify (WindowCache *cache, XEvent *event)
{
  WindowCacheEntry *parent, *window;

  if (event->xreparent.event == event->xreparent.window)
    /* This came from StructureNotifyMask... */
    return;

  parent = XLLookUpAssoc (cache->entries, event->xreparent.parent);

  if (!parent)
    return;

  window = XLLookUpAssoc (cache->entries, event->xreparent.window);

  if (!window)
    return;

  /* First, unlink window.  */
  UnlinkWindowCacheEntry (window);

  /* Next, change its parent.  */
  window->parent = event->xreparent.parent;

  /* Link it onto the new parent.  */
  AddAfter (window, parent->last);
}

static void
HandleUnmapNotify (WindowCache *cache, XEvent *event)
{
  WindowCacheEntry *window;

  if (event->xunmap.event == event->xunmap.window)
    /* This is the result of StructureNotifyMask, and the parent
       window cannot be accessed through the event.  */
    return;

  window = XLLookUpAssoc (cache->entries, event->xunmap.window);

  if (!window)
    return;

  window->flags &= ~IsMapped;
}

static void
HandleDestroyNotify (WindowCache *cache, XEvent *event)
{
  WindowCacheEntry *window;

  window = XLLookUpAssoc (cache->entries, event->xdestroywindow.window);

  if (!window)
    return;

  /* This tells FreeWindowCacheEntry to not bother restoring the old
     event mask.  */
  window->flags |= IsDestroyed;
  FreeWindowCacheEntry (window);
}

static void
HandlePropertyNotify (WindowCache *cache, XEvent *event)
{
  WindowCacheEntry *window;

  if (event->xproperty.atom != WM_STATE)
    return;

  window = XLLookUpAssoc (cache->entries, event->xproperty.window);

  if (!window)
    return;

  /* WM_STATE has changed.  Clear both IsToplevel and IsNotToplevel;
     don't set either of those flags based on event->xproperty.state,
     since it's not okay to read the property here.  */

  window->flags &= ~(IsToplevel | IsNotToplevel);
}

static void
EnsureShape (WindowCacheEntry *entry, Bool force)
{
  xcb_shape_get_rectangles_reply_t *bounding;
  xcb_shape_get_rectangles_reply_t *input;
  xcb_shape_get_rectangles_cookie_t bounding_cookie;
  xcb_shape_get_rectangles_cookie_t input_cookie;
  xcb_generic_error_t *error, *error1;

  error = NULL;
  error1 = NULL;

  if (!force && !(entry->flags & IsShapeDirtied))
    /* The shape is not dirty.  */
    return;

  /* Reinitialize the window shape.  */
  bounding_cookie = xcb_shape_get_rectangles (compositor.conn,
					      entry->window,
					      XCB_SHAPE_SK_BOUNDING);
  input_cookie = xcb_shape_get_rectangles (compositor.conn,
					   entry->window,
					   XCB_SHAPE_SK_INPUT);

  /* Ask for replies from the X server.  */
  bounding = xcb_shape_get_rectangles_reply (compositor.conn,
					     bounding_cookie,
					     &error);
  input = xcb_shape_get_rectangles_reply (compositor.conn,
					  input_cookie, &error1);

  if (error || error1 || !bounding || !input)
    {
      if (error)
	free (error);

      if (error1)
	free (error1);

      if (bounding)
	free (bounding);

      if (input)
	free (input);

      /* An error occured; the window has probably been destroyed, in
	 which case a DestroyNotify event will arrive shortly.  */
      return;
    }

  /* Clear the region.  */
  pixman_region32_fini (&entry->shape);

  /* Repopulate window->shape with the new shape.  */
  InitRegionWithRects (&entry->shape, bounding);
  IntersectRegionWith (&entry->shape, input);

  /* Free the replies from the X server.  */
  free (bounding);
  free (input);

  /* Clear the shape dirtied flag.  */
  entry->flags &= ~IsShapeDirtied;  
}

static void
HandleShapeNotify (WindowCache *cache, XEvent *event)
{
  WindowCacheEntry *window;

  /* event->xany.window is the same as ((XShapeEvent *)
     event)->window, so we don't have to include the shape extension
     header.  */

  window = XLLookUpAssoc (cache->entries, event->xany.window);

  if (!window)
    return;

  /* Obtain the new shape from the X server.  */
  EnsureShape (window, True);
}

static void
ProcessEventForWindowCache (WindowCache *cache, XEvent *event)
{
  switch (event->type)
    {
    case CirculateNotify:
      HandleCirculateNotify (cache, event);
      break;

    case ConfigureNotify:
      HandleConfigureNotify (cache, event);
      break;

    case CreateNotify:
      HandleCreateNotify (cache, event);
      break;

    case DestroyNotify:
      HandleDestroyNotify (cache, event);
      break;

    case MapNotify:
      HandleMapNotify (cache, event);
      break;

    case ReparentNotify:
      HandleReparentNotify (cache, event);
      break;

    case UnmapNotify:
      HandleUnmapNotify (cache, event);
      break;

    case PropertyNotify:
      HandlePropertyNotify (cache, event);
      break;
    }

  if (event->type == shape_base + XCB_SHAPE_NOTIFY)
    HandleShapeNotify (cache, event);
}

static Bool
IsToplevelWindow (WindowCacheEntry *entry)
{
  unsigned long actual_size;
  unsigned long bytes_remaining;
  int rc, actual_format;
  Atom actual_type;
  unsigned char *tmp_data;

  if (entry->flags & IsNotToplevel)
    /* We know this isn't a toplevel window.   */
    return False;

  if (entry->flags & IsToplevel)
    /* We know this is a toplevel window.  */
    return True;

  /* We have not yet determined whether or not this is a toplevel
     window.  Read the WM_STATE property to find out.  */
  tmp_data = NULL;

  CatchXErrors ();
  rc = XGetWindowProperty (compositor.display, entry->window, WM_STATE,
			   0, 2, False, WM_STATE, &actual_type,
			   &actual_format, &actual_size, &bytes_remaining,
			   &tmp_data);
  if (UncatchXErrors (NULL) || rc != Success
      || actual_type != WM_STATE || actual_format != 32
      || bytes_remaining)
    {
      /* This means the window is not a toplevel.  */
      entry->flags |= IsNotToplevel;

      if (tmp_data)
	XFree (tmp_data);
      return False;
    }

  entry->flags |= IsToplevel;
  if (tmp_data)
    XFree (tmp_data);
  return True;
}

static Window
FindToplevelWindow1 (WindowCacheEntry *entry, int x, int y)
{
  WindowCacheEntry *child;
  pixman_box32_t temp;

  child = entry->children->next;

  while (child != entry->children)
    {
      if (XLIsWindowIconSurface (child->window)
	  || !(child->flags & IsMapped))
	goto next;

      /* If the shape is dirtied, fetch the new shape.  */
      EnsureShape (child, False);

      /* Check if X and Y are contained by the child and its input
	 region.  */
      if (x >= child->x && x < child->x + child->width
	  && y >= child->y && y < child->y + child->height
	  && pixman_region32_contains_point (&child->shape, x - child->x,
					     y - child->y, &temp))
	{
	  /* If this child is already a toplevel, return it.  */
	  if (IsToplevelWindow (child))
	    return child->window;

	  /* Otherwise, keep looking.  */
	  return FindToplevelWindow1 (child, x - child->x,
				      y - child->y);
	}

    next:
      child = child->next;
    }

  /* No toplevel window was found.  */
  return None;
}

static Window
FindToplevelWindow (WindowCache *cache, int root_x, int root_y)
{
  /* Find a mapped toplevel window.  */
  return FindToplevelWindow1 (cache->root_window, root_x, root_y);
}

/* Drag-and-drop between Wayland and X.  */


/* Forward declaration.  */
static void SendLeave (void);

static void
FinishDrag (void)
{
  if (drag_state.seat)
    XLSeatCancelDestroyListener (drag_state.seat_key);

  if (drag_state.mods_key)
    XLSeatRemoveModifierCallback (drag_state.mods_key);

  drag_state.mods_key = NULL;

  /* Leave any surface that we entered.  */
  SendLeave ();

  drag_state.seat = NULL;
  drag_state.seat_key = NULL;

  if (drag_state.window_cache)
    {
      FreeWindowCache (drag_state.window_cache);
      drag_state.window_cache = NULL;
    }

  /* Delete the XdndTypeList property.  */
  XDeleteProperty (compositor.display, selection_transfer_window,
		   XdndTypeList);

  /* Delete the XdndActionList property.  */
  XDeleteProperty (compositor.display, selection_transfer_window,
		   XdndActionList);

  /* Clear flags.  */
  drag_state.flags = 0;

  /* Clear the toplevel and target.  */
  drag_state.toplevel = 0;
  drag_state.target = 0;

  /* Disown XdndSelection.  */
  DisownSelection (XdndSelection);
}

static void
HandleDragSeatDestroy (void *data)
{
  drag_state.seat = NULL;
  drag_state.seat_key = NULL;

  FinishDrag ();
}

static void
ReadProtocolProperties (Window window, int *version_return,
			Window *proxy_return)
{
  WindowCacheEntry *entry;
  xcb_get_property_cookie_t xdnd_proto_cookie;
  xcb_get_property_cookie_t xdnd_proxy_cookie;
  xcb_generic_error_t *error, *error1;
  xcb_get_property_reply_t *proto, *proxy;
  uint32_t *values;

  error = NULL;
  error1 = NULL;

  /* Get the window entry corresponding to window in the window
     cache.  */
  entry = XLLookUpAssoc (drag_state.window_cache->entries, window);

  if (!entry)
    {
      /* Return some suitable values for a window that isn't in the
	 window cache.  */
      *version_return = 0;
      *proxy_return = None;

      /* The entry is not in the window cache... */
      return;
    }

  if (entry->flags & IsPropertyRead)
    {
      /* The version and proxy window were already obtained.  */

      *version_return = (entry->flags >> 16) & 0xff;
      *proxy_return = entry->dnd_proxy;
      return;
    }

  xdnd_proto_cookie = xcb_get_property (compositor.conn, 0,
					window, XdndAware,
					XCB_ATOM_ATOM, 0, 1);
  xdnd_proxy_cookie = xcb_get_property (compositor.conn, 0,
					window, XdndProxy,
					XCB_ATOM_WINDOW, 0, 1);

  /* Ask for replies from the X server.  */
  proto = xcb_get_property_reply (compositor.conn, xdnd_proto_cookie,
				  &error);
  proxy = xcb_get_property_reply (compositor.conn, xdnd_proxy_cookie,
				  &error1);

  /* If any errors occured, bail out, while freeing any data
     allocated.  */
  if (error || error1 || !proto || !proxy)
    {
      if (error)
	free (error);

      if (error1)
	free (error1);

      if (proto)
	free (proto);

      if (proxy)
	free (proxy);

      /* Store some default values before returning.  */
      *proxy_return = None;
      *version_return = 0;
      return;
    }

  /* Otherwise, the properties were read.  Determine if they are
     valid.  */
  if (proto->format == 32 && proto->type == XCB_ATOM_ATOM
      && xcb_get_property_value_length (proto) == 4)
    {
      /* Save the protocol version into the window flags.  Truncate
	 values above 255.  */
      values = xcb_get_property_value (proto);
      entry->flags |= (values[0] & 0xff) << 16;

      /* Return the version to the caller.  */
      *version_return = values[0];
    }
  else
    *version_return = 0;

  free (proto);

  if (proxy->format == 32 && proxy->type == XCB_ATOM_WINDOW
      && xcb_get_property_value_length (proxy) == 4)
    {
      /* Save the proxy window ID into the window cache entry.  */
      values = xcb_get_property_value (proxy);
      entry->dnd_proxy = values[0];

      /* Return the proxy to the caller.  */
      *proxy_return = values[0];
    }
  else
    *proxy_return = None;

  free (proxy);

  /* Mark properties as having been read.  */
  entry->flags |= IsPropertyRead;
}

static void
WriteTypeList (void)
{
  DataSource *source;
  Atom *targets;
  int n_targets;

  source = XLSeatGetDragDataSource (drag_state.seat);

  /* If no data source was specified, then functions for handling
     external DND should not be called at all.  */
  XLAssert (source != NULL);

  n_targets = XLDataSourceTargetCount (source);
  targets = XLMalloc (sizeof *targets * n_targets);
  XLDataSourceGetTargets (source, targets);

  if (n_targets)
    drag_state.first_targets[0] = targets[0];
  else
    drag_state.first_targets[0] = None;

  if (n_targets > 1)
    drag_state.first_targets[1] = targets[1];
  else
    drag_state.first_targets[1] = None;

  if (n_targets > 2)
    drag_state.first_targets[2] = targets[2];
  else
    drag_state.first_targets[2] = None;

  if (n_targets > 3)
    {
      /* There are more than 3 targets.  Write the type list.  */
      XChangeProperty (compositor.display, selection_transfer_window,
		       XdndTypeList, XA_ATOM, 32, PropModeReplace,
		       (unsigned char *) targets, n_targets);
      drag_state.flags |= MoreThanThreeTargets;
    }

  /* Free the targets.  */
  XLFree (targets);

  /* Set the type setup flag.  */
  drag_state.flags |= TypeListSet;
}

static const char *
GetAskActionName (Atom action)
{
  if (action == XdndActionCopy)
    return "Copy";

  if (action == XdndActionMove)
    return "Move";

  if (action == XdndActionLink)
    return "Link";

  if (action == XdndActionAsk)
    return "Ask";

  abort ();
}

static void
WriteActionList (void)
{
  DataSource *source;
  uint32_t action_mask;
  Atom actions[2];
  int nactions;
  ptrdiff_t i, end, fill;
  char *ask_actions;
  const char *name;
  XTextProperty prop;

  drag_state.flags |= ActionListSet;

  source = XLSeatGetDragDataSource (drag_state.seat);
  action_mask = XLDataSourceGetSupportedActions (source);

  if (action_mask & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
    {
      /* Write XdndActionList.  */

      nactions = 0;

      if (action_mask & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
	actions[nactions++] = XdndActionCopy;

      if (action_mask & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
	actions[nactions++] = XdndActionMove;

      XChangeProperty (compositor.display, selection_transfer_window,
		       XdndActionList, XA_ATOM, 32, PropModeReplace,
		       (unsigned char *) actions, nactions);

      /* Write XdndActionDescription.  This is a list of strings,
	 terminated by NULL, describing the drag and drop actions.

         These strings are not actually used by any program, so it is
         OK to not translate.  */

      ask_actions = NULL;
      end = 0;

      for (i = 0; i < nactions; ++i)
	{
	  fill = end;
	  name = GetAskActionName (actions[i]);
	  end += strlen (name) + 1;

	  ask_actions = XLRealloc (ask_actions, end);
	  strncpy (ask_actions + fill, name, end - fill);
	}

      prop.value = (unsigned char *) ask_actions;
      prop.encoding = XA_STRING;
      prop.format = 8;
      prop.nitems = end;

      XSetTextProperty (compositor.display, selection_transfer_window,
			&prop, XdndActionDescription);
      XLFree (ask_actions);
    }
}

static void
SendEnter (void)
{
  XEvent message;

  if (drag_state.toplevel == None
      || drag_state.version < 3)
    return;

  if (!(drag_state.flags & TypeListSet))
    /* Set up the drag and drop type list now.  */
    WriteTypeList ();

  if (!(drag_state.flags & ActionListSet))
    /* Set up the drag and drop action list now.  */
    WriteActionList ();

  message.xclient.type = ClientMessage;
  message.xclient.message_type = XdndEnter;
  message.xclient.format = 32;
  message.xclient.window = drag_state.toplevel;
  message.xclient.data.l[0] = selection_transfer_window;
  message.xclient.data.l[1] = MIN (XdndProtocolVersion,
				   drag_state.version) << 24;

  if (drag_state.flags & MoreThanThreeTargets)
    message.xclient.data.l[1] |= 1;

  message.xclient.data.l[2] = drag_state.first_targets[0];
  message.xclient.data.l[3] = drag_state.first_targets[1];
  message.xclient.data.l[4] = drag_state.first_targets[2];

  CatchXErrors ();
  XSendEvent (compositor.display, drag_state.target,
	      False, NoEventMask, &message);
  UncatchXErrors (NULL);
}

static Atom
ConvertActionsLoosely (uint32_t actions)
{
  /* Use XdndActionAsk if ask was specified.  */
  if (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
    return XdndActionAsk;

  if (drag_state.modifiers & ShiftMask
      && actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
    /* Shift is pressed; default to XdndActionMove.  */
    return XdndActionMove;

  if (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
    return XdndActionCopy;

  if (actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
    return XdndActionMove;

  return XdndActionPrivate;
}

static void
SendPosition (short root_x, short root_y)
{
  XEvent message;
  DataSource *source;
  uint32_t action_mask;

  if (!drag_state.seat || drag_state.version < 3)
    return;

  /* If we are waiting for an XdndStatus event, wait for it to arrive
     before sending the position.  */
  if (drag_state.flags & WaitingForStatus)
    {
      if (!(drag_state.flags & PendingDrop))
	/* If the drop already happened, don't bother sending another
	   position event.  */
	drag_state.flags |= PendingPosition;

      return;
    }

  drag_state.flags &= ~PendingPosition;

  /* If this rectangle is within the mouse rectangle, do nothing.  */

  if (drag_state.flags & NeedMouseRect
      && root_x >= drag_state.mouse_rect.x
      && root_y >= drag_state.mouse_rect.y
      && root_x < (drag_state.mouse_rect.x
		   + drag_state.mouse_rect.width)
      && root_y < (drag_state.mouse_rect.y
		   + drag_state.mouse_rect.height))
    return;

  /* Otherwise, send the XdndPosition event now.  */
  message.xclient.type = ClientMessage;
  message.xclient.message_type = XdndPosition;
  message.xclient.format = 32;
  message.xclient.window = drag_state.toplevel;
  message.xclient.data.l[0] = selection_transfer_window;
  message.xclient.data.l[1] = 0;
  message.xclient.data.l[2] = (root_x << 16) | root_y;
  message.xclient.data.l[3] = 0;
  message.xclient.data.l[4] = 0;

  if (MIN (XdndProtocolVersion, drag_state.version) >= 3)
    message.xclient.data.l[3] = drag_state.timestamp;

  if (MIN (XdndProtocolVersion, drag_state.version) >= 4)
    {
      source = (finish_source
		/* Use the finish source if it is available.
		   drag_state.seat's source will be NULL by the time
		   this is called in response to a delayed drop.  */
		? finish_source
		: XLSeatGetDragDataSource (drag_state.seat));
      action_mask = XLDataSourceGetSupportedActions (source);

      /* A word about how converting actions between
	 wl_data_device_manager and XDND aware programs works.

         When dragging between two Wayland clients, version 3 sources
         can specify a mask of supported actions, which the compositor
         then compares with the supported actions announced by the
         drop target to determine a single selected action.

	 The compositor is also supposed to change the selected action
	 based on information such as the state of the keyboard
	 modifiers.

         Version 2 sources, on the other hand, do not support any
         specific drop actions.  Instead, wl_data_offer_accept conveys
         whether or not the source accepts a MIME type provided by the
         target.

         No matter what version of the wl_data_device protocol is
         spoken by the data source, it is difficult to convert between
         Wayland actions and XDND actions.  With version 3 sources, we
         default to looking through the supported actions in the
         following order:

	    - WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY (XdndActionCopy)
            - WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE (XdndActionMove)
            - (anything else)                        (XdndActionPrivate)

	 or the following order, if Shift is pressed:

	   - WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE (XdndActionMove)
	   - WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY (XdndActionCopy)
	   - (anything else)                        (XdndActionPrivate)

         and returning the action selected by the client (as described
         in the XdndStatus event sent by it in response).

         With version 2 sources, we always specify XdndActionPrivate,
         and call accept with the first MIME type specified.  */
      message.xclient.data.l[4] = ConvertActionsLoosely (action_mask);
    }

  CatchXErrors ();
  XSendEvent (compositor.display, drag_state.target,
	      False, NoEventMask, &message);
  UncatchXErrors (NULL);

  /* Now wait for an XdndStatus to be sent in reply.  */
  drag_state.flags |= WaitingForStatus;
}

static void
SendLeave (void)
{
  XEvent message;

  if (drag_state.toplevel == None
      || drag_state.version < 3)
    return;

  message.xclient.type = ClientMessage;
  message.xclient.message_type = XdndLeave;
  message.xclient.format = 32;

  /* Events have their window field set to drag_state.toplevel,
     regardless of whether or not a proxy was specified.  */

  message.xclient.window = drag_state.toplevel;

  /* selection_transfer_window is used, since it is the owner of
     XdndSelection.  */

  message.xclient.data.l[0] = selection_transfer_window;
  message.xclient.data.l[1] = 0;
  message.xclient.data.l[2] = 0;
  message.xclient.data.l[3] = 0;
  message.xclient.data.l[4] = 0;

  CatchXErrors ();
  XSendEvent (compositor.display, drag_state.target,
	      False, NoEventMask, &message);
  UncatchXErrors (NULL);
}

static const char *
PickMimeType (DataSource *source)
{
  XLList *list;

  list = XLDataSourceGetMimeTypeList (source);

  if (!list)
    return NULL;

  return list->data;
}

static void
ReportStateToSource (void)
{
  DataSource *source;
  struct wl_resource *resource;
  uint32_t action;

  source = XLSeatGetDragDataSource (drag_state.seat);

  if (!source)
    return;

  resource = XLResourceFromDataSource (source);

  /* If no data type was accepted, report that to the source.  */
  if (!(drag_state.flags & WillAcceptDrop))
    wl_data_source_send_target (resource, NULL);
  else
    wl_data_source_send_target (resource,
				PickMimeType (source));

  /* If the source is new enough, report the selected action to the
     source.  */
  if (wl_resource_get_version (resource) >= 3)
    {
      action = TranslateAction (drag_state.action);
      wl_data_source_send_action (resource, action);
    }
}

/* Forward declaration.  */
static void SendDrop (void);

static void
HandleXdndStatus (XEvent *event)
{
  unsigned long flags, rect, rect1;

  if (event->xclient.data.l[0] != drag_state.toplevel)
    /* This event is for a window other than the toplevel.  */
    return;

  /* Clear the waiting for status flag.  */
  drag_state.flags &= ~WaitingForStatus;

  /* Determine whether or not the target will accept the drop.  */
  flags = event->xclient.data.l[1];

  if (flags & 1)
    drag_state.flags |= WillAcceptDrop;
  else
    drag_state.flags &= ~WillAcceptDrop;

  /* Determine if the target wants a mouse rectangle.  */
  rect = event->xclient.data.l[2];
  rect1 = event->xclient.data.l[3];

  if (flags & 2 || !rect1)
    drag_state.flags &= ~NeedMouseRect;
  else
    {
      drag_state.flags |= NeedMouseRect;
      drag_state.mouse_rect.x = (rect & 0xffff0000) >> 16;
      drag_state.mouse_rect.y = (rect & 0xffff);
      drag_state.mouse_rect.width = (rect1 & 0xffff0000) >> 16;
      drag_state.mouse_rect.height = (rect1 & 0xffff);
    }

  /* Set the client's selected action.  */
  drag_state.action = event->xclient.data.l[4];

  ReportStateToSource ();

  /* Send any pending XdndPosition event.  */
  if (drag_state.flags & PendingPosition)
    SendPosition (drag_state.last_root_x,
		  drag_state.last_root_y);

  if (!(drag_state.flags & WaitingForStatus)
      && drag_state.flags & PendingDrop)
    {
      /* Send any pending XdndDrop event.  */
      drag_state.flags &= ~PendingDrop;

      if (!(drag_state.flags & WillAcceptDrop)
	  || drag_state.action == None)
	{
	  /* The status changed and is no longer eligible for
	     dropping.  Cancel.  */
	  SendLeave ();

	  /* Also tell the data source that this was cancelled.  */
	  XLDataSourceSendDropCancelled (finish_source);
	}
      else
	/* Otherwise, send the drop.  */
	SendDrop ();
    }
}

static void
HandleXdndFinished (XEvent *event)
{
  struct wl_resource *resource;
  Atom new_action;

  if (!finish_source)
    return;

  /* Send either cancel or performed to the source depending on
     whether or not the target accepted the drop.  */
  if (finish_version < 5 || event->xclient.data.l[0] & 1)
    {
      /* The drop was successful.  If the action changed, send it to
	 the data source, followed by finished.  */

      resource = XLResourceFromDataSource (finish_source);

      if (wl_resource_get_version (resource) >= 3
	  && finish_version >= 5)
	{
	  new_action = event->xclient.data.l[2];

	  if (new_action != finish_action)
	    wl_data_source_send_action (resource,
					TranslateAction (new_action));
	}

      if (wl_resource_get_version (resource) >= 3)
	wl_data_source_send_dnd_finished (resource);
    }
  else
    /* Send the drop cancelled event.  */
    XLDataSourceSendDropCancelled (finish_source);

  finish_source = NULL;
  XLDataSourceCancelDestroyCallback (finish_source_key);
  finish_source_key = NULL;

  RemoveTimer (finish_timeout);
  finish_timeout = NULL;

  /* Either way, finish dragging.  */
  FinishDrag ();
}

static void
HandleDataSourceDestroy (void *data)
{
  finish_source = NULL;
  finish_source_key = NULL;

  if (finish_timeout)
    RemoveTimer (finish_timeout);
  FinishDrag ();
}

static void
HandleTimerExpired (Timer *timer, void *data, struct timespec time)
{
  RemoveTimer (timer);

  if (finish_source)
    {
      /* Send cancelled to the data source.  */
      XLDataSourceSendDropCancelled (finish_source);

      finish_source = NULL;
      XLDataSourceCancelDestroyCallback (finish_source_key);
      finish_source_key = NULL;

      FinishDrag ();
    }
}

static void
SendDrop (void)
{
  XEvent message;

  if (drag_state.toplevel == None
      || drag_state.version < 3)
    return;

  message.xclient.type = ClientMessage;
  message.xclient.message_type = XdndDrop;
  message.xclient.format = 32;
  message.xclient.window = drag_state.toplevel;
  message.xclient.data.l[0] = selection_transfer_window;
  message.xclient.data.l[1] = 0;
  message.xclient.data.l[2] = drag_state.timestamp;
  message.xclient.data.l[3] = 0;
  message.xclient.data.l[4] = 0;

  /* First, send the event to the client.  */

  CatchXErrors ();
  XSendEvent (compositor.display, drag_state.target,
	      False, NoEventMask, &message);
  UncatchXErrors (NULL);

  /* Tell the source to start waiting for finish.  */
  XLDataSourceSendDropPerformed (finish_source);
}

static void
ProcessClientMessage (XEvent *event)
{
  if (event->xclient.message_type == XdndStatus)
    HandleXdndStatus (event);
  else if (event->xclient.message_type == XdndFinished)
    HandleXdndFinished (event);
}

static void
HandleModifiersChanged (unsigned int effective, void *data)
{
  drag_state.modifiers = effective;

  /* Report the new action to the client.  */
  SendPosition (drag_state.last_root_x, drag_state.last_root_y);
}

void
XLHandleOneXEventForDnd (XEvent *event)
{
  if (drag_state.window_cache)
    ProcessEventForWindowCache (drag_state.window_cache,
				event);

  if (drag_state.seat && event->type == ClientMessage)
    ProcessClientMessage (event);
}

void
XLDoDragLeave (Seat *seat)
{
  if (seat == drag_state.seat && drag_state.toplevel)
    {
      SendLeave ();

      drag_state.toplevel = None;
      drag_state.target = None;
      drag_state.version = 0;
      drag_state.action = None;

      /* Clear flags that are specific to each toplevel.  */
      drag_state.flags &= ~WillAcceptDrop;
      drag_state.flags &= ~NeedMouseRect;
      drag_state.flags &= ~PendingPosition;
      drag_state.flags &= ~PendingDrop;
      drag_state.flags &= ~WaitingForStatus;

      /* Report the changed state to the source.  */
      ReportStateToSource ();
    }
}

void
XLDoDragMotion (Seat *seat, double root_x, double root_y)
{
  Window toplevel, proxy, self;
  int version, proxy_version;
  Timestamp timestamp;

  if (finish_source || drag_state.flags & PendingDrop)
    /* A finish is pending.  */
    return;

  if (drag_state.seat && drag_state.seat != seat)
    /* The XDND protocol doesn't support MPX, so only allow one seat
       to drag out of Wayland at once.  */
    return;

  if (!drag_state.seat)
    {
      drag_state.seat = seat;
      drag_state.seat_key
	= XLSeatRunOnDestroy (seat, HandleDragSeatDestroy, NULL);
      drag_state.modifiers
	= XLSeatGetEffectiveModifiers (seat);
      drag_state.mods_key
	= XLSeatAddModifierCallback (seat, HandleModifiersChanged,
				     NULL);

      drag_state.last_root_x = INT_MIN;
      drag_state.last_root_y = INT_MIN;
    }

  if (drag_state.flags & SelectionFailed)
    /* We do not have ownership over XdndSelection.  */
    return;

  if (root_x == drag_state.last_root_x
      && root_y == drag_state.last_root_y)
    /* Ignore subpixel movement.  */
    return;

  drag_state.last_root_x = root_x;
  drag_state.last_root_y = root_y;

  /* Try to own XdndSelection with the last user time.  */
  if (!(drag_state.flags & SelectionSet))
    {
      timestamp = XLSeatGetLastUserTime (seat);
      drag_state.timestamp = timestamp.milliseconds;

      if (!XLOwnDragSelection (drag_state.timestamp,
			       XLSeatGetDragDataSource (seat)))
	{
	  /* We could not obtain ownership over XdndSelection.  */
	  drag_state.flags |= SelectionFailed;
	  return;
	}
      else
	drag_state.flags |= SelectionSet;
    }

  /* Also initialize the window cache.  */
  if (!drag_state.window_cache)
    drag_state.window_cache = AllocWindowCache ();

  toplevel = FindToplevelWindow (drag_state.window_cache,
				 root_x, root_y);

  if (XLIsXdgToplevel (toplevel))
    /* If this one of our own surfaces, ignore it.  */
    toplevel = None;

  if (toplevel && toplevel != drag_state.toplevel)
    {
      /* Try to determine whether or not the given toplevel supports
	 XDND, and whether or not a proxy is set.  */
      ReadProtocolProperties (toplevel, &version, &proxy);

      if (proxy != None)
	{
	  /* A proxy is set.  Read properties off the proxy.  */
	  ReadProtocolProperties (proxy, &proxy_version, &self);

	  /* Check the proxy to make sure its XdndProxy property
	     points to itself.  If it does not, the proxy property is
	     left over from a crash.  */
	  if (self != proxy)
	    proxy = None;
	  else
	    /* Otherwise, set the version to the value of XdndAware on
	       the proxy window.  */
	    version = proxy_version;
	}
    }

  /* Now, toplevel is the toplevel itself, version is the version of
     the target, and the target is proxy, if set, or toplevel, if
     not.  Send XdndLeave to any previous target.  */
  if (toplevel != drag_state.toplevel)
    {
      SendLeave ();

      drag_state.toplevel = None;
      drag_state.target = None;
      drag_state.version = 0;
      drag_state.action = None;

      /* Clear flags that are specific to each toplevel.  */
      drag_state.flags &= ~WillAcceptDrop;
      drag_state.flags &= ~NeedMouseRect;
      drag_state.flags &= ~PendingPosition;
      drag_state.flags &= ~PendingDrop;
      drag_state.flags &= ~WaitingForStatus;

      /* Report the changed state to the source.  */
      ReportStateToSource ();

      /* Set the toplevel and target accordingly.  */
      if (toplevel)
	{
	  drag_state.toplevel = toplevel;
	  drag_state.target = (proxy != None
			       ? proxy : toplevel);
	  drag_state.version = version;

	  /* Then, send XdndEnter followed by XdndPosition, and wait
	     for an XdndStatus event.  */
	  SendEnter ();
	}
    }

  /* Send the position to any attached toplevel, then wait for
     XdndStatus.  */
  SendPosition (root_x, root_y);
}

void
XLDoDragFinish (Seat *seat)
{
  if (seat == drag_state.seat)
    {
      /* If nothing was dropped, finish the drag now.  */
      if (!finish_source)
	FinishDrag ();
    }
}

static void
StartFinishTimeout (void)
{
  /* Wait for the XdndFinish event to arrive, or a timeout to
     expire.  */
  finish_source = XLSeatGetDragDataSource (drag_state.seat);
  finish_source_key = XLDataSourceAddDestroyCallback (finish_source,
						      HandleDataSourceDestroy,
						      NULL);
  finish_version = drag_state.version;
  finish_action = drag_state.action;

  /* Use a 5 second timeout like we do for all other selection-related
     stuff.  */
  finish_timeout = AddTimer (HandleTimerExpired, NULL, MakeTimespec (5, 0));
}

Bool
XLDoDragDrop (Seat *seat)
{
  if (seat != drag_state.seat)
    return False;

  if (drag_state.version < 3)
    return False;

  if (!(drag_state.flags & WaitingForStatus))
    {      
      /* If no status event is pending, and no action was specified or
	 no type has been specified, return False.  */

      if (!(drag_state.flags & WillAcceptDrop)
	  || drag_state.action == None)
	return False;

      /* Start the finish timeout.  */
      StartFinishTimeout ();

      /* Send the drop now.  */
      SendDrop ();
      return True;
    }
  else
    {
      /* Set PendingDrop.  Then, return True, so the code in seat.c does
	 not clobber the data in drag_state.  */
      drag_state.flags |= PendingDrop;

      /* Start the finish timeout.  */
      StartFinishTimeout ();
      return True;
    }

  return False;
}
