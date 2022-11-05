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

#include "compositor.h"
#include "xdg-shell.h"

#include <X11/extensions/XInput2.h>

#define PopupFromRoleImpl(impl) ((XdgPopup *) (impl))

typedef struct _XdgPopup XdgPopup;
typedef struct _PropMotifWmHints PropMotifWmHints;

enum
  {
    StateIsMapped	 = 1,
    StateIsGrabbed	 = (1 << 1),
    StatePendingGrab	 = (1 << 2),
    StatePendingPosition = (1 << 3),
    StateAckPosition     = (1 << 4),
    StateIsTopmost	 = (1 << 5),
  };

struct _PropMotifWmHints
{
  unsigned long flags;
  unsigned long functions;
  unsigned long decorations;
  long input_mode;
  unsigned long status;
};

struct _XdgPopup
{
  /* The parent role implementation.  */
  XdgRoleImplementation impl;

  /* The role associated with this popup.  */
  Role *role;

  /* The parent xdg_surface object.  */
  Role *parent;

  /* The wl_resource associated with this popup.  */
  struct wl_resource *resource;

  /* The number of references to this popup.  */
  int refcount;

  /* Some state associated with this popup.  */
  int state;

  /* Whether or not we are waiting for a reply to a configure
     event.  */
  Bool conf_reply;

  /* The serial of the last configure event sent, and the last
     position event sent.  */
  uint32_t conf_serial, position_serial;

  /* The associated positioner.  */
  Positioner *positioner;

  /* Any pending seat on which a grab should be asserted.  */
  Seat *pending_grab_seat;

  /* The serial to use for that grab.  */
  uint32_t pending_grab_serial;

  /* The seat that currently holds the grab.  */
  Seat *grab_holder;

  /* The current grab serial.  */
  uint32_t current_grab_serial;

  /* Its destroy callback key.  */
  void *seat_callback_key, *pending_callback_key;

  /* The current position.  */
  int x, y;

  /* The pending coordinates.  */
  int pending_x, pending_y;

  /* The current width and height.  */
  int width, height;

  /* Reconstrain callback associated with the parent.  */
  void *reconstrain_callback_key;
};



/* Forward declarations.  */

static void DoGrab (XdgPopup *, Seat *, uint32_t);
static void Dismiss (XdgPopup *, Bool);

static void
DestroyBacking (XdgPopup *popup)
{
  void *key;

  if (--popup->refcount)
    return;

  key = popup->reconstrain_callback_key;

  if (key)
    XLXdgRoleCancelReconstrainCallback (key);

  /* Release the parent if it exists.  */
  if (popup->parent)
    XLReleaseXdgRole (popup->parent);

  /* Release seat callbacks if they exist.  */
  if (popup->seat_callback_key)
    XLSeatCancelDestroyListener (popup->seat_callback_key);

  if (popup->pending_callback_key)
    XLSeatCancelDestroyListener (popup->pending_callback_key);

  /* Release the positioner and free the popup.  */
  XLReleasePositioner (popup->positioner);
  XLFree (popup);
}

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  XdgPopup *popup;

  popup = wl_resource_get_user_data (resource);
  popup->resource = NULL;

  DestroyBacking (popup);
}

static void
Attach (Role *role, XdgRoleImplementation *impl)
{
  XdgPopup *popup;
  XSetWindowAttributes attrs;
  PropMotifWmHints hints;
  Window window;

  popup = PopupFromRoleImpl (impl);
  popup->refcount++;
  popup->role = role;

  window = XLWindowFromXdgRole (role);

  /* Make the popup override-redirect.  */
  attrs.override_redirect = True;
  XChangeWindowAttributes (compositor.display, window,
			   CWOverrideRedirect, &attrs);

  /* It turns out that Mutter still draws drop shadows for popups, so
     turn them off.  */
  hints.flags = 0;
  hints.decorations = 0;

  /* Add _NET_WM_SYNC_REQUEST to the list of supported protocols.  */
  XSetWMProtocols (compositor.display, XLWindowFromXdgRole (role),
		   &_NET_WM_SYNC_REQUEST, 1);

  XChangeProperty (compositor.display, window,
		   _MOTIF_WM_HINTS, _MOTIF_WM_HINTS, 32,
		   PropModeReplace,
		   (unsigned char *) &hints, 5);
}

static void
Unmap (XdgPopup *popup)
{
  popup->state &= ~StateIsMapped;

  XUnmapWindow (compositor.display,
		XLWindowFromXdgRole (popup->role));
}

static void
RevertGrabTo (XdgPopup *popup, Role *parent_role)
{
  XdgPopup *parent;
  XdgRoleImplementation *impl;

  impl = XLImplementationOfXdgRole (parent_role);

  if (!impl || XLTypeOfXdgRole (parent_role) != TypePopup)
    return;

  parent = PopupFromRoleImpl (impl);

  DoGrab (parent, popup->grab_holder,
	  popup->current_grab_serial);
}

static void
RevertTopmostTo (Role *parent_role)
{
  XdgPopup *parent;
  XdgRoleImplementation *impl;

  impl = XLImplementationOfXdgRole (parent_role);

  if (!impl || XLTypeOfXdgRole (parent_role) != TypePopup)
    return;

  parent = PopupFromRoleImpl (impl);

  /* Now, make the parent the topmost popup again.  This is done
     outside RevertGrabTo because it is valid to destroy an unmapped
     topmost popup.  */
  parent->state |= StateIsTopmost;
}

static void
ClearTopmostOf (Role *parent_role)
{
  XdgPopup *parent;
  XdgRoleImplementation *impl;

  impl = XLImplementationOfXdgRole (parent_role);

  if (!impl || XLTypeOfXdgRole (parent_role) != TypePopup)
    return;

  parent = PopupFromRoleImpl (impl);
  parent->state &= ~StateIsTopmost;
}
  
static void
Detach (Role *role, XdgRoleImplementation *impl)
{
  XdgPopup *popup;
  XSetWindowAttributes attrs;

  popup = PopupFromRoleImpl (impl);

  if (popup->parent)
    {
      if (popup->state & StateIsGrabbed
	  || popup->state & StatePendingGrab)
	RevertTopmostTo (popup->parent);

      /* Detaching the popup means that it will be destroyed soon.
	 Revert the grab to the parent and unmap it.  */

      if (popup->state & StateIsGrabbed)
	RevertGrabTo (popup, popup->parent);
    }

  if (popup->state & StateIsMapped)
    Unmap (popup);

  popup->role = NULL;

  DestroyBacking (popup);

  /* Make the window non-override-redirect.  */
  attrs.override_redirect = False;
  XChangeWindowAttributes (compositor.display,
			   XLWindowFromXdgRole (role),
			   CWOverrideRedirect, &attrs);
}

static void
SendConfigure (XdgPopup *popup, int x, int y, int width, int height)
{
  uint32_t serial;

  serial = wl_display_next_serial (compositor.wl_display);

  if (width != -1 && height != -1)
    {
      xdg_popup_send_configure (popup->resource,
				x, y, width, height);
      popup->state |= StateAckPosition;
    }

  XLXdgRoleSendConfigure (popup->role, serial);

  popup->conf_reply = True;
  popup->conf_serial = serial;
  popup->position_serial = serial;
}

static void
MoveWindow (XdgPopup *popup)
{
  int root_x, root_y, parent_gx, parent_gy;
  int geometry_x, geometry_y, x, y;
  Window window;

  /* No parent was specified or the role is detached.  */
  if (!popup->role || !popup->parent)
    return;

  if (!popup->role->surface || !popup->parent->surface)
    /* No surface being available means we cannot obtain the window
       scale.  */
    return;

  window = XLWindowFromXdgRole (popup->role);

  XLXdgRoleGetCurrentGeometry (popup->parent, &parent_gx,
			       &parent_gy, NULL, NULL);
  XLXdgRoleGetCurrentGeometry (popup->role, &geometry_x,
			       &geometry_y, NULL, NULL);
  XLXdgRoleCurrentRootPosition (popup->parent, &root_x,
				&root_y);

  /* Parent geometry is relative to the parent coordinate system.  */
  TruncateSurfaceToWindow (popup->parent->surface, parent_gx, parent_gy,
			   &parent_gx, &parent_gy);

  /* geometry_x and geometry_y are relative to the local coordinate
     system.  */
  TruncateSurfaceToWindow (popup->role->surface, geometry_x,
			   geometry_y, &geometry_x, &geometry_y);

  /* X and Y are relative to the parent coordinate system.  */
  TruncateSurfaceToWindow (popup->parent->surface, popup->x,
			   popup->y, &x, &y);

  XMoveWindow (compositor.display, window,
	       x + root_x + parent_gx - geometry_x,
	       y + root_y + parent_gy - geometry_y);
}

static void
Map (XdgPopup *popup)
{
  /* We can't guarantee that the toplevel contents will be preserved
     at this point.  */
  SubcompositorGarbage (XLSubcompositorFromXdgRole (popup->role));

  /* Update the state.  */
  popup->state |= StateIsMapped;

  /* Move the window to the correct position.  */
  MoveWindow (popup);

  /* And map the window.  */
  XMapRaised (compositor.display, XLWindowFromXdgRole (popup->role));

  /* Do any pending grab if the seat is still there.  */
  if (popup->state & StatePendingGrab)
    {
      if (popup->pending_grab_seat)
	DoGrab (popup, popup->pending_grab_seat,
		popup->pending_grab_serial);
      else
	Dismiss (popup, False);

      /* Now free the callback belonging to the pending grab seat.  */
      if (popup->pending_callback_key)
	XLSeatCancelDestroyListener (popup->pending_callback_key);

      popup->pending_grab_seat = NULL;
      popup->pending_callback_key = NULL;
      popup->state &= ~StatePendingGrab;
    }
}

static void
Commit (Role *role, Surface *surface, XdgRoleImplementation *impl)
{
  XdgPopup *popup;

  popup = PopupFromRoleImpl (impl);

  if (popup->state & StatePendingPosition)
    MoveWindow (popup);

  popup->state &= ~StatePendingPosition;

  if (!surface->current_state.buffer)
    {
      /* No buffer was attached, unmap the window.  */

      if (popup->state & StateIsMapped)
	Unmap (popup);
    }
  else if (!popup->conf_reply)
    {
      /* Map the window if a reply was received.  */

      if (!(popup->state & StateIsMapped))
	Map (popup);
    }
}

static void
AckConfigure (Role *role, XdgRoleImplementation *impl, uint32_t serial)
{
  XdgPopup *popup;

  popup = PopupFromRoleImpl (impl);

  if (serial == popup->conf_serial)
    {
      popup->conf_reply = False;
      popup->conf_serial = 0;
    }

  if (serial == popup->position_serial
      && popup->state & StateAckPosition)
    {
      /* Now apply the position of the popup.  */
      popup->x = popup->pending_x;
      popup->y = popup->pending_y;

      /* The position has been acked.  Clear that flag.  */
      popup->state &= ~StateAckPosition;

      /* Set a new flag which tells commit to move the popup.  */
      popup->state |= StatePendingPosition;
      popup->position_serial = 0;
    }
}

static void
InternalReposition (XdgPopup *popup)
{
  int x, y, width, height;
  FrameClock *clock;

  /* No parent was specified or the role is detached.  */
  if (!popup->role || !popup->parent)
    return;

  XLPositionerCalculateGeometry (popup->positioner,
				 popup->parent, &x, &y,
				 &width, &height);

  popup->pending_x = x;
  popup->pending_y = y;

  SendConfigure (popup, popup->pending_x, popup->pending_y,
		 width, height);

  /* Now, freeze the frame clock, to avoid flicker when the client
     commits before ack_configure.  */
  clock = XLXdgRoleGetFrameClock (popup->role);
  XLFrameClockFreeze (clock);

  popup->state |= StateAckPosition;
}

static void
HandleGeometryChange (Role *role, XdgRoleImplementation *impl)
{
  XdgPopup *popup;

  popup = PopupFromRoleImpl (impl);

  MoveWindow (popup);
}

static Bool
CheckCanGrab (Role *parent, Seat *seat)
{
  XdgRoleImplementationType type;
  XdgRoleImplementation *parent_impl;
  XdgPopup *popup;

  if (!parent->surface)
    return False;

  parent_impl = XLImplementationOfXdgRole (parent);

  if (!parent_impl)
    return False;

  type = XLTypeOfXdgRole (parent);

  if (type == TypeToplevel)
    return True;

  if (type == TypePopup)
    {
      popup = PopupFromRoleImpl (parent_impl);

      return (popup->state & StateIsGrabbed
	      && popup->grab_holder == seat);
    }

  return False;
}

static void
HandleGrabHolderDestroy (void *data)
{
  XdgPopup *popup;

  popup = data;
  popup->grab_holder = NULL;
  popup->seat_callback_key = NULL;

  Dismiss (popup, False);
}

static void
SaveGrabHolder (XdgPopup *popup, Seat *seat)
{
  if (popup->grab_holder == seat)
    return;

  if (popup->grab_holder)
    {
      XLSeatCancelDestroyListener (popup->seat_callback_key);

      popup->seat_callback_key = NULL;
      popup->grab_holder = NULL;
    }

  if (seat)
    {
      popup->grab_holder = seat;
      popup->seat_callback_key
	= XLSeatRunOnDestroy (seat, HandleGrabHolderDestroy,
			      popup);
    }
}

static void
DoGrab (XdgPopup *popup, Seat *seat, uint32_t serial)
{
  if (popup->resource
      && popup->role && popup->role->surface
      && CheckCanGrab (popup->parent, seat)
      && XLSeatExplicitlyGrabSurface (seat,
				      popup->role->surface,
				      serial))
    {
      popup->current_grab_serial = serial;
      SaveGrabHolder (popup, seat);

      popup->state |= StateIsGrabbed;
    }
  else
    Dismiss (popup, False);
}

static void
Dismiss (XdgPopup *popup, Bool do_parents)
{
  Role *role;
  XdgRoleImplementation *impl;
  XdgPopup *parent;

  if (popup->state & StateIsGrabbed
      && popup->parent)
    RevertGrabTo (popup, popup->parent);

  if (popup->state & StateIsMapped)
    Unmap (popup);

  popup->state &= ~StateIsGrabbed;

  if (popup->resource)
    xdg_popup_send_popup_done (popup->resource);

  if (do_parents && popup->parent)
    {
      role = popup->parent;
      impl = XLImplementationOfXdgRole (role);

      if (impl && XLTypeOfXdgRole (role) == TypePopup)
	{
	  parent = PopupFromRoleImpl (impl);
	  Dismiss (parent, True);
	}
    }
}

static void
HandleSeatDestroy (void *data)
{
  XdgPopup *popup;

  popup = data;
  popup->pending_callback_key = NULL;
  popup->pending_grab_seat = NULL;

  /* The popup will later be dismissed upon mapping.  */
}

static void
RecordGrabPending (XdgPopup *popup, Seat *seat, uint32_t serial)
{
  void *key;

  if (popup->seat_callback_key || popup->pending_callback_key)
    return;

  key = XLSeatRunOnDestroy (seat, HandleSeatDestroy, popup);

  if (!key)
    Dismiss (popup, False);
  else
    {
      popup->pending_callback_key = key;
      popup->pending_grab_seat = seat;
      popup->pending_grab_serial = serial;

      /* This popup is now the topmost popup.  */
      popup->state |= StateIsTopmost;

      /* If the parent is also a popup, then it is no longer the
	 topmost popup.  */
      if (popup->parent)
	ClearTopmostOf (popup->parent);

      popup->state |= StatePendingGrab;
    }
}

static void
Grab (struct wl_client *client, struct wl_resource *resource,
      struct wl_resource *seat_resource, uint32_t serial)
{
  Seat *seat;
  XdgPopup *popup;

  seat = wl_resource_get_user_data (seat_resource);
  popup = wl_resource_get_user_data (resource);

  if (!popup->role || !popup->role->surface)
    return;

  if (popup->state & StateIsGrabbed)
    return;

  if (!(popup->state & StateIsMapped))
    RecordGrabPending (popup, seat, serial);
  else
    wl_resource_post_error (resource, XDG_POPUP_ERROR_INVALID_GRAB,
			    "trying to grab mapped popup");
}

static void
Reposition (struct wl_client *client, struct wl_resource *resource,
	    struct wl_resource *positioner_resource, uint32_t token)
{
  XdgPopup *popup;

  popup = wl_resource_get_user_data (resource);

  XLReleasePositioner (popup->positioner);
  popup->positioner
    = wl_resource_get_user_data (positioner_resource);
  XLRetainPositioner (popup->positioner);

  /* Make sure that the positioner is complete.  */
  XLCheckPositionerComplete (popup->positioner);

  xdg_popup_send_repositioned (resource, token);
  InternalReposition (popup);
}

static Bool
CanDestroyPopup (XdgPopup *popup)
{
  if (popup->state & StateIsTopmost)
    /* This is the topmost popup and can be destroyed.  */
    return True;

  if (!(popup->state & StateIsGrabbed)
      && !(popup->state & StatePendingGrab))
    /* This popup is not grabbed.  */
    return True;

  /* Otherwise, this popup cannot be destroyed; it is grabbed, but not
     the topmost popup.  */
  return False;
}

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  XdgPopup *popup;

  popup = wl_resource_get_user_data (resource);

  if (!CanDestroyPopup (popup))
    wl_resource_post_error (resource,
			    XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
			    "trying to destroy non-topmost popup");

  if (popup->role)
    XLXdgRoleDetachImplementation (popup->role,
				   &popup->impl);

  wl_resource_destroy (resource);
}

static Bool
HandleOneConfigureNotify (XEvent *event)
{
  XdgPopup *popup;
  XdgRoleImplementation *impl;

  impl = XLLookUpXdgPopup (event->xconfigure.window);

  if (!impl)
    return False;

  popup = PopupFromRoleImpl (impl);
  XLXdgRoleNoteConfigure (popup->role, event);

  return False;
}

static void
NoteSize (Role *role, XdgRoleImplementation *impl,
	  int width, int height)
{
  XdgPopup *popup;

  popup = PopupFromRoleImpl (impl);
  popup->width = width;
  popup->height = height;
}

static void
HandleParentConfigure (void *data, XEvent *xevent)
{
  XdgPopup *popup;

  popup = data;

  if (XLPositionerIsReactive (popup->positioner))
    InternalReposition (popup);
}

static void
HandleParentResize (void *data)
{
  XdgPopup *popup;

  popup = data;

  if (XLPositionerIsReactive (popup->positioner))
    InternalReposition (popup);
}

static Bool
IsWindowMapped (Role *role, XdgRoleImplementation *impl)
{
  XdgPopup *popup;

  popup = PopupFromRoleImpl (impl);
  return popup->state & StateIsMapped;
}

static const struct xdg_popup_interface xdg_popup_impl =
  {
    .destroy = Destroy,
    .grab = Grab,
    .reposition = Reposition,
  };

void
XLGetXdgPopup (struct wl_client *client, struct wl_resource *resource,
	       uint32_t id, struct wl_resource *parent_resource,
	       struct wl_resource *positioner)
{
  XdgPopup *popup;
  Role *role, *parent;
  void *key;

  popup = XLSafeMalloc (sizeof *popup);
  role = wl_resource_get_user_data (resource);

  if (!popup)
    {
      wl_client_post_no_memory (client);
      return;
    }

  memset (popup, 0, sizeof *popup);
  popup->resource = wl_resource_create (client, &xdg_popup_interface,
					wl_resource_get_version (resource),
					id);

  if (!popup->resource)
    {
      wl_resource_post_no_memory (resource);
      XLFree (popup);
      return;
    }

  popup->impl.funcs.attach = Attach;
  popup->impl.funcs.commit = Commit;
  popup->impl.funcs.detach = Detach;

  popup->impl.funcs.ack_configure = AckConfigure;
  popup->impl.funcs.note_size = NoteSize;
  popup->impl.funcs.handle_geometry_change = HandleGeometryChange;
  popup->impl.funcs.is_window_mapped = IsWindowMapped;

  if (parent_resource)
    {
      parent = wl_resource_get_user_data (parent_resource);
      key = XLXdgRoleRunOnReconstrain (parent, HandleParentConfigure,
				       HandleParentResize, popup);
      XLRetainXdgRole (parent);

      popup->parent = parent;
      popup->reconstrain_callback_key = key;
    }

  popup->positioner = wl_resource_get_user_data (positioner);
  XLRetainPositioner (popup->positioner);

  /* Make sure that the positioner is complete.  */
  XLCheckPositionerComplete (popup->positioner);

  wl_resource_set_implementation (popup->resource, &xdg_popup_impl,
				  popup, HandleResourceDestroy);
  popup->refcount++;

  XLXdgRoleAttachImplementation (role, &popup->impl);

  /* Send the initial configure event.  */
  InternalReposition (popup);
}

Bool
XLHandleXEventForXdgPopups (XEvent *event)
{
  if (event->type == ConfigureNotify)
    return HandleOneConfigureNotify (event);

  return False;
}

void
XLInitPopups (void)
{
  /* Nothing to do here.  */
}
