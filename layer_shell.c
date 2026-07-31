/* Wayland compositor running on top of an X server.

Copyright (C) 2022 to various contributors.

This file is part of 12to11.

12to11 is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

12to11 is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with 12to11.  If not, see <https://www.gnu.org/licenses/>.  */

#include <string.h>

#include <X11/extensions/XInput2.h>

#include "compositor.h"
#include "wlr-layer-shell-unstable-v1.h"

#define LayerSurfaceFromRole(role) ((LayerSurface *) (role))

#define DefaultEventMask					\
  (ExposureMask | StructureNotifyMask | PropertyChangeMask)

/* Anchor bits.  */
enum
  {
    AnchorTop		= 1,
    AnchorBottom	= 2,
    AnchorLeft		= 4,
    AnchorRight		= 8,
  };

/* Layer values.  */
enum
  {
    LayerBackground	= 0,
    LayerBottom		= 1,
    LayerTop		= 2,
    LayerOverlay	= 3,
  };

/* Keyboard interactivity.  */
enum
  {
    KeyboardNone	= 0,
    KeyboardExclusive	= 1,
    KeyboardOnDemand	= 2,
  };

enum
  {
    IsMapped		= 1,
    IsConfigured	= 1 << 1,
    HasAck		= 1 << 2,
    KeyboardGrabbed	= 1 << 3,
    PendingBufferRelease = 1 << 4,
    PendingFrameCallback = 1 << 5,
  };

typedef struct _LayerSurface LayerSurface;

struct _LayerSurface
{
  /* The associated role.  */
  Role role;

  /* The subcompositor used to composite the surface.  */
  Subcompositor *subcompositor;

  /* The buffer release helper.  */
  BufferReleaseHelper *release_helper;

  /* The window and rendering target.  */
  Window window;
  RenderTarget target;

  /* Reference count and flags.  */
  int refcount, flags;

  /* The output this surface is bound to, or NULL.  */
  struct wl_resource *output_resource;

  /* Double-buffered state.  Pending is applied at commit time.  */
  uint32_t pending_anchor, current_anchor;
  uint32_t pending_layer, current_layer;
  uint32_t pending_keyboard, current_keyboard;
  int pending_zone, current_zone;
  int pending_margin_top, pending_margin_right;
  int pending_margin_bottom, pending_margin_left;
  int current_margin_top, current_margin_right;
  int current_margin_bottom, current_margin_left;
  int pending_width, pending_height;
  int current_width, current_height;
  uint32_t pending_edge, current_edge;

  /* The size last sent in a configure event.  */
  int configured_width, configured_height;

  /* The geometry last applied to the window.  */
  int win_x, win_y, win_w, win_h;

  /* The layer whose EWMH hints were last applied.  */
  uint32_t hinted_layer;

  /* The pending configure serial.  */
  uint32_t pending_serial;
};

/* The layer shell global.  */
static struct wl_global *layer_shell_global;

/* Association table of windows to layer surfaces, for X event
   dispatch.  */
static XLAssocTable *layer_surfaces;

/* Forward declarations of static functions.  */

static void ApplyState (LayerSurface *);
static Bool GetOutputRect (LayerSurface *, int *, int *, int *, int *);
static void ComputeGeometry (LayerSurface *, int, int, int, int,
			     int *, int *, int *, int *);
static void SendConfigure (LayerSurface *, int, int);
static void SetEwmhHints (LayerSurface *);
static void ApplyKeyboardInteractivity (LayerSurface *);
static void DestroyBacking (LayerSurface *);
static void RunFrameCallbacksConditionally (LayerSurface *);



/* Geometry and state computation.  */

static void
ApplyState (LayerSurface *ls)
{
  ls->current_anchor = ls->pending_anchor;
  ls->current_layer = ls->pending_layer;
  ls->current_keyboard = ls->pending_keyboard;
  ls->current_zone = ls->pending_zone;
  ls->current_margin_top = ls->pending_margin_top;
  ls->current_margin_right = ls->pending_margin_right;
  ls->current_margin_bottom = ls->pending_margin_bottom;
  ls->current_margin_left = ls->pending_margin_left;
  ls->current_width = ls->pending_width;
  ls->current_height = ls->pending_height;
  ls->current_edge = ls->pending_edge;
}

static Bool
GetOutputRect (LayerSurface *ls, int *ox, int *oy, int *ow, int *oh)
{
  if (XLGetOutputRectFromResource (ls->output_resource, ox, oy, ow, oh))
    return True;

  /* No specific output was given; fall back to the whole screen.  */
  *ox = 0;
  *oy = 0;
  *ow = DisplayWidth (compositor.display,
		      DefaultScreen (compositor.display));
  *oh = DisplayHeight (compositor.display,
		       DefaultScreen (compositor.display));
  return True;
}

static void
ComputeGeometry (LayerSurface *ls, int ox, int oy, int ow, int oh,
		 int *x_out, int *y_out, int *w_out, int *h_out)
{
  Bool left, right, top, bottom;
  int x, y, w, h;

  left = ls->current_anchor & AnchorLeft;
  right = ls->current_anchor & AnchorRight;
  top = ls->current_anchor & AnchorTop;
  bottom = ls->current_anchor & AnchorBottom;

  /* Horizontal axis.  */
  if (left && right)
    {
      w = ow - ls->current_margin_left - ls->current_margin_right;
      x = ox + ls->current_margin_left;
    }
  else
    {
      w = ls->current_width ? ls->current_width : ow;

      if (w < 1)
	w = 1;

      if (left)
	x = ox + ls->current_margin_left;
      else if (right)
	x = ox + ow - w - ls->current_margin_right;
      else
	x = ox + (ow - w) / 2;
    }

  /* Vertical axis.  */
  if (top && bottom)
    {
      h = oh - ls->current_margin_top - ls->current_margin_bottom;
      y = oy + ls->current_margin_top;
    }
  else
    {
      h = ls->current_height ? ls->current_height : oh;

      if (h < 1)
	h = 1;

      if (top)
	y = oy + ls->current_margin_top;
      else if (bottom)
	y = oy + oh - h - ls->current_margin_bottom;
      else
	y = oy + (oh - h) / 2;
    }

  *x_out = x;
  *y_out = y;
  *w_out = w;
  *h_out = h;
}

static void
SendConfigure (LayerSurface *ls, int width, int height)
{
  ls->pending_serial = wl_display_next_serial (compositor.wl_display);

  zwlr_layer_surface_v1_send_configure (ls->role.resource,
					ls->pending_serial,
					width, height);

  ls->configured_width = width;
  ls->configured_height = height;
  ls->flags |= IsConfigured;
}

static void
SetWindowType (LayerSurface *ls, Atom type_atom)
{
  XChangeProperty (compositor.display, ls->window,
		   _NET_WM_WINDOW_TYPE, XA_ATOM, 32, PropModeReplace,
		   (unsigned char *) &type_atom, 1);
}

static void
SetWindowState (LayerSurface *ls, Atom *states, int nstates)
{
  XChangeProperty (compositor.display, ls->window,
		   _NET_WM_STATE, XA_ATOM, 32, PropModeReplace,
		   (unsigned char *) states, nstates);
}

static void
SetEwmhHints (LayerSurface *ls)
{
  Atom states[4];
  Atom type_atom;
  int n;

  switch (ls->current_layer)
    {
    case LayerBackground:
      type_atom = _NET_WM_WINDOW_TYPE_DESKTOP;
      states[0] = _NET_WM_STATE_BELOW;
      break;

    case LayerBottom:
      type_atom = _NET_WM_WINDOW_TYPE_DOCK;
      states[0] = _NET_WM_STATE_BELOW;
      break;

    case LayerTop:
      type_atom = _NET_WM_WINDOW_TYPE_DOCK;
      states[0] = _NET_WM_STATE_ABOVE;
      break;

    case LayerOverlay:
    default:
      type_atom = _NET_WM_WINDOW_TYPE_NOTIFICATION;
      states[0] = _NET_WM_STATE_ABOVE;
      break;
    }

  states[1] = _NET_WM_STATE_STICKY;
  states[2] = _NET_WM_STATE_SKIP_TASKBAR;
  states[3] = _NET_WM_STATE_SKIP_PAGER;
  n = ArrayElements (states);

  SetWindowType (ls, type_atom);
  SetWindowState (ls, states, n);

  ls->hinted_layer = ls->current_layer;
}

static void
AcquireKeyboardGrab (LayerSurface *ls)
{
  XLList *tem;
  Time time;
  unsigned char mask_buf[XIMaskLen (XI_LASTEVENT)];
  XIEventMask mask;

  /* Use a freshly round-tripped server time, so the grab cannot fail
     because of a stale timestamp.  */
  time = XLGetServerTimeRoundtrip ();

  mask.mask = mask_buf;
  mask.mask_len = sizeof (mask_buf);
  mask.deviceid = XIAllMasterDevices;

  memset (mask_buf, 0, sizeof (mask_buf));
  XISetMask (mask_buf, XI_FocusIn);
  XISetMask (mask_buf, XI_FocusOut);
  XISetMask (mask_buf, XI_KeyPress);
  XISetMask (mask_buf, XI_KeyRelease);

  for (tem = live_seats; tem; tem = tem->next)
    {
      Seat *seat;
      int kbd;

      seat = (Seat *) tem->data;
      kbd = XLSeatGetKeyboardDevice (seat);

      /* Grab the keyboard to the window, holding focus on it.  */
      XIGrabDevice (compositor.display, kbd, ls->window,
		    time, None, XIGrabModeAsync, XIGrabModeAsync,
		    False, &mask);
    }
}

static void
ReleaseKeyboardGrab (LayerSurface *ls)
{
  XLList *tem;
  Time time;

  time = XLGetServerTimeRoundtrip ();

  for (tem = live_seats; tem; tem = tem->next)
    {
      Seat *seat;

      seat = (Seat *) tem->data;

      XIUngrabDevice (compositor.display,
		      XLSeatGetKeyboardDevice (seat), time);
    }
}

static void
ApplyKeyboardInteractivity (LayerSurface *ls)
{
  Bool desired;

  if (!ls->role.surface)
    return;

  /* Override-redirect windows do not participate in window-manager
     focus, so any keyboard interactivity requires a keyboard grab.
     The pointer is left alone.  */
  desired = ((ls->flags & IsMapped)
	     && ls->current_keyboard != KeyboardNone);

  if (desired && !(ls->flags & KeyboardGrabbed))
    {
      /* Make sure the window is actually viewable on the X server
	 before grabbing.  */
      XSync (compositor.display, False);

      AcquireKeyboardGrab (ls);
      ls->flags |= KeyboardGrabbed;
    }
  else if (!desired && (ls->flags & KeyboardGrabbed))
    {
      ReleaseKeyboardGrab (ls);
      ls->flags &= ~KeyboardGrabbed;
    }
}


/* Role callbacks.  */

static void
RunFrameCallbacks (LayerSurface *ls)
{
  struct timespec time;

  if (!ls->role.surface)
    return;

  clock_gettime (CLOCK_MONOTONIC, &time);
  XLSurfaceRunFrameCallbacks (ls->role.surface, time);

  ls->flags &= ~PendingFrameCallback;
}

static void
RunFrameCallbacksConditionally (LayerSurface *ls)
{
  if (!ls->role.surface)
    return;

  if (ls->flags & PendingBufferRelease)
    /* Wait for all buffers to be released first.  */
    ls->flags |= PendingFrameCallback;
  else
    RunFrameCallbacks (ls);
}

static void
NoteFrame (FrameMode mode, uint64_t id, void *data,
	   uint64_t msc, uint64_t ust)
{
  if (mode != ModeComplete && mode != ModePresented)
    return;

  /* Run frame callbacks, since a frame was just presented.  */
  RunFrameCallbacksConditionally ((LayerSurface *) data);
}

static void
AllBuffersReleased (void *data)
{
  LayerSurface *ls;

  ls = data;

  if (!ls->role.surface)
    return;

  ls->flags &= ~PendingBufferRelease;

  /* Run pending frame callbacks.  */
  if (ls->flags & PendingFrameCallback)
    RunFrameCallbacks (ls);
}

static Window
GetWindow (Surface *surface, Role *role)
{
  LayerSurface *ls;

  ls = LayerSurfaceFromRole (role);
  return ls->window;
}

static Bool
Setup (Surface *surface, Role *role)
{
  LayerSurface *ls;

  ls = LayerSurfaceFromRole (role);

  role->surface = surface;

  /* Prevent the surface from holding any other role.  */
  surface->role_type = LayerType;

  /* Attach the views to the subcompositor.  */
  ViewSetSubcompositor (surface->view, ls->subcompositor);
  ViewSetSubcompositor (surface->under, ls->subcompositor);

  SubcompositorInsert (ls->subcompositor, surface->under);
  SubcompositorInsert (ls->subcompositor, surface->view);

  ls->refcount++;
  return True;
}

static void
Teardown (Surface *surface, Role *role)
{
  LayerSurface *ls;

  role->surface = NULL;
  ls = LayerSurfaceFromRole (role);

  /* Release any keyboard grab held by this surface.  */
  if (ls->flags & KeyboardGrabbed)
    {
      ReleaseKeyboardGrab (ls);
      ls->flags &= ~KeyboardGrabbed;
    }

  ViewUnparent (surface->view);
  ViewUnparent (surface->under);

  ViewSetSubcompositor (surface->view, NULL);
  ViewSetSubcompositor (surface->under, NULL);

  DestroyBacking (ls);
}

static void
Commit (Surface *surface, Role *role)
{
  LayerSurface *ls;
  int ox, oy, ow, oh, x, y, w, h;
  struct timespec time;

  ls = LayerSurfaceFromRole (role);

  if (!ls->role.resource)
    return;

  /* Apply double-buffered state.  */
  ApplyState (ls);

  /* Update EWMH hints if the layer changed.  */
  if (ls->hinted_layer != ls->current_layer)
    SetEwmhHints (ls);

  /* Compute the desired geometry.  */
  GetOutputRect (ls, &ox, &oy, &ow, &oh);
  ComputeGeometry (ls, ox, oy, ow, oh, &x, &y, &w, &h);

  /* Send a configure event if the size has changed or no configure
     has yet been sent.  */
  if (!(ls->flags & IsConfigured)
      || w != ls->configured_width
      || h != ls->configured_height)
    SendConfigure (ls, w, h);

  /* Map or unmap the surface depending on whether a buffer has been
     attached and a configure has been acknowledged.  */
  if (surface->current_state.buffer && (ls->flags & HasAck))
    {
      /* Apply the new geometry if it changed.  */
      if (x != ls->win_x || y != ls->win_y
	  || w != ls->win_w || h != ls->win_h)
	{
	  XMoveResizeWindow (compositor.display, ls->window, x, y, w, h);
	  ls->win_x = x;
	  ls->win_y = y;
	  ls->win_w = w;
	  ls->win_h = h;
	}

      if (!(ls->flags & IsMapped))
	{
	  XMapRaised (compositor.display, ls->window);
	  ls->flags |= IsMapped;
	}

      SubcompositorUpdate (ls->subcompositor);
    }
  else
    {
      if (ls->flags & IsMapped)
	{
	  XUnmapWindow (compositor.display, ls->window);
	  ls->flags &= ~IsMapped;
	}

      /* Run frame callbacks even when unmapping.  */
      clock_gettime (CLOCK_MONOTONIC, &time);
      XLSurfaceRunFrameCallbacks (surface, time);
    }

  /* Apply or release the keyboard grab as appropriate.  */
  ApplyKeyboardInteractivity (ls);
}

static void
ReleaseBuffer (Surface *surface, Role *role, ExtBuffer *buffer)
{
  LayerSurface *ls;
  RenderBuffer render_buffer;

  ls = LayerSurfaceFromRole (role);
  render_buffer = XLRenderBufferFromBuffer (buffer);

  if (RenderIsBufferIdle (render_buffer, ls->target))
    XLReleaseBuffer (buffer);
  else
    {
      /* Release the buffer once it becomes idle, or is destroyed.  */
      ReleaseBufferWithHelper (ls->release_helper, buffer, ls->target);

      /* Defer frame callbacks until the buffer is released.  */
      ls->flags |= PendingBufferRelease;
    }
}

static void
SubsurfaceUpdate (Surface *surface, Role *role)
{
  LayerSurface *ls;

  ls = LayerSurfaceFromRole (role);
  SubcompositorUpdate (ls->subcompositor);
}

static void
DestroyBacking (LayerSurface *ls)
{
  if (--ls->refcount)
    return;

  RenderDestroyRenderTarget (ls->target);
  XDestroyWindow (compositor.display, ls->window);
  FreeBufferReleaseHelper (ls->release_helper);

  if (layer_surfaces)
    XLDeleteAssoc (layer_surfaces, ls->window);

  SubcompositorFree (ls->subcompositor);
  XLFree (ls);
}


/* Layer surface resource requests.  */

static void
DestroyLayerSurface (struct wl_client *client, struct wl_resource *resource)
{
  LayerSurface *ls;

  ls = wl_resource_get_user_data (resource);

  if (ls->role.surface)
    XLSurfaceReleaseRole (ls->role.surface, &ls->role);

  wl_resource_destroy (resource);
}

static void
SetSize (struct wl_client *client, struct wl_resource *resource,
	 uint32_t width, uint32_t height)
{
  LayerSurface *ls;

  ls = wl_resource_get_user_data (resource);
  ls->pending_width = width;
  ls->pending_height = height;
}

static void
SetAnchor (struct wl_client *client, struct wl_resource *resource,
	   uint32_t anchor)
{
  LayerSurface *ls;

  if (anchor > (AnchorTop | AnchorBottom | AnchorLeft | AnchorRight))
    {
      wl_resource_post_error (resource,
			      ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR,
			      "invalid anchor bitfield");
      return;
    }

  ls = wl_resource_get_user_data (resource);
  ls->pending_anchor = anchor;
}

static void
SetExclusiveZone (struct wl_client *client, struct wl_resource *resource,
		  int32_t zone)
{
  LayerSurface *ls;

  ls = wl_resource_get_user_data (resource);
  ls->pending_zone = zone;
}

static void
SetMargin (struct wl_client *client, struct wl_resource *resource,
	   int32_t top, int32_t right, int32_t bottom, int32_t left)
{
  LayerSurface *ls;

  ls = wl_resource_get_user_data (resource);
  ls->pending_margin_top = top;
  ls->pending_margin_right = right;
  ls->pending_margin_bottom = bottom;
  ls->pending_margin_left = left;
}

static void
SetKeyboardInteractivity (struct wl_client *client,
			  struct wl_resource *resource,
			  uint32_t keyboard_interactivity)
{
  LayerSurface *ls;

  if (keyboard_interactivity > KeyboardOnDemand)
    {
      wl_resource_post_error (resource,
			      ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY,
			      "invalid keyboard interactivity");
      return;
    }

  ls = wl_resource_get_user_data (resource);
  ls->pending_keyboard = keyboard_interactivity;
}

static void
GetPopup (struct wl_client *client, struct wl_resource *resource,
	  struct wl_resource *popup_resource)
{
  /* Popup positioning relative to a layer surface is not implemented;
     layer surfaces are generally full-output overlays.  */
}

static void
AckConfigure (struct wl_client *client, struct wl_resource *resource,
	      uint32_t serial)
{
  LayerSurface *ls;

  ls = wl_resource_get_user_data (resource);

  /* Allow mapping once the client has acknowledged a configure.  */
  ls->flags |= HasAck;
}

static void
SetLayer (struct wl_client *client, struct wl_resource *resource,
	  uint32_t layer)
{
  LayerSurface *ls;

  if (layer > LayerOverlay)
    {
      wl_resource_post_error (resource,
			      ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
			      "invalid layer");
      return;
    }

  ls = wl_resource_get_user_data (resource);
  ls->pending_layer = layer;
}

static void
SetExclusiveEdge (struct wl_client *client, struct wl_resource *resource,
		  uint32_t edge)
{
  LayerSurface *ls;

  if (edge > (AnchorTop | AnchorBottom | AnchorLeft | AnchorRight))
    {
      wl_resource_post_error (resource,
			      ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_EXCLUSIVE_EDGE,
			      "invalid exclusive edge");
      return;
    }

  ls = wl_resource_get_user_data (resource);
  ls->pending_edge = edge;
}

static const struct zwlr_layer_surface_v1_interface layer_surface_impl =
  {
    .set_size = SetSize,
    .set_anchor = SetAnchor,
    .set_exclusive_zone = SetExclusiveZone,
    .set_margin = SetMargin,
    .set_keyboard_interactivity = SetKeyboardInteractivity,
    .get_popup = GetPopup,
    .ack_configure = AckConfigure,
    .destroy = DestroyLayerSurface,
    .set_layer = SetLayer,
    .set_exclusive_edge = SetExclusiveEdge,
  };

static void
HandleLayerSurfaceResourceDestroy (struct wl_resource *resource)
{
  LayerSurface *ls;

  ls = wl_resource_get_user_data (resource);
  ls->role.resource = NULL;
  DestroyBacking (ls);
}

static void
GetLayerSurface (struct wl_client *client, struct wl_resource *resource,
		 uint32_t id, struct wl_resource *surface_resource,
		 struct wl_resource *output_resource, uint32_t layer,
		 const char *name)
{
  Surface *surface;
  LayerSurface *ls;
  XSetWindowAttributes attrs;
  unsigned long flags;

  surface = wl_resource_get_user_data (surface_resource);

  if (layer > LayerOverlay)
    {
      wl_resource_post_error (resource,
			      ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
			      "invalid layer value");
      return;
    }

  if (surface->role_type != AnythingType
      && surface->role_type != LayerType)
    {
      wl_resource_post_error (resource, ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
			      "a role is/was already present on the"
			      " given surface");
      return;
    }

  if (surface->current_state.buffer)
    {
      wl_resource_post_error (resource,
			      ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED,
			      "surface already has a buffer attached or"
			      " committed");
      return;
    }

  ls = XLSafeMalloc (sizeof *ls);

  if (!ls)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (ls, 0, sizeof *ls);

  ls->role.resource
    = wl_resource_create (client, &zwlr_layer_surface_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!ls->role.resource)
    {
      wl_resource_post_no_memory (resource);
      XLFree (ls);
      return;
    }

  ls->pending_layer = layer;
  ls->current_layer = layer;
  ls->output_resource = output_resource;

  /* Create the override-redirect window.  */
  attrs.colormap = compositor.colormap;
  attrs.border_pixel = border_pixel;
  attrs.event_mask = DefaultEventMask;
  attrs.cursor = InitDefaultCursor ();
  attrs.override_redirect = True;
  flags = (CWColormap | CWBorderPixel | CWEventMask
	   | CWCursor | CWOverrideRedirect);

  ls->window = XCreateWindow (compositor.display,
			      DefaultRootWindow (compositor.display),
			      0, 0, 20, 20, 0, compositor.n_planes,
			      InputOutput, compositor.visual, flags,
			      &attrs);

  /* Select input events so that pointer and keyboard events are
     delivered to this window and routed to the surface.  */
  XLSelectStandardEvents (ls->window);

  ls->subcompositor = MakeSubcompositor ();
  ls->target = RenderTargetFromWindow (ls->window, DefaultEventMask);
  RenderSetClient (ls->target, client);

  ls->release_helper = MakeBufferReleaseHelper (AllBuffersReleased, ls);

  SubcompositorSetTarget (ls->subcompositor, &ls->target);

  /* Run frame callbacks whenever a frame is presented, so that the
     client can keep updating its contents (e.g. a region-selection
     rectangle).  */
  SubcompositorSetNoteFrameCallback (ls->subcompositor, NoteFrame, ls);

  /* Apply the initial EWMH hints for this layer.  */
  SetEwmhHints (ls);

  /* Create the association table if necessary.  */
  if (!layer_surfaces)
    layer_surfaces = XLCreateAssocTable (16);

  XLMakeAssoc (layer_surfaces, ls->window, ls);

  /* Fill in the role function table.  */
  ls->role.funcs.setup = Setup;
  ls->role.funcs.teardown = Teardown;
  ls->role.funcs.commit = Commit;
  ls->role.funcs.release_buffer = ReleaseBuffer;
  ls->role.funcs.subsurface_update = SubsurfaceUpdate;
  ls->role.funcs.get_window = GetWindow;

  wl_resource_set_implementation (ls->role.resource,
				  &layer_surface_impl, ls,
				  HandleLayerSurfaceResourceDestroy);
  ls->refcount++;

  if (!XLSurfaceAttachRole (surface, &ls->role))
    abort ();
}

static void
DestroyShell (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct zwlr_layer_shell_v1_interface layer_shell_impl =
  {
    .get_layer_surface = GetLayerSurface,
    .destroy = DestroyShell,
  };

static void
HandleBind (struct wl_client *client, void *data,
	    uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &zwlr_layer_shell_v1_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &layer_shell_impl,
				  NULL, NULL);
}

void
XLInitLayerShell (void)
{
  layer_shell_global
    = wl_global_create (compositor.wl_display,
			&zwlr_layer_shell_v1_interface,
			4, NULL, HandleBind);
}


/* X event dispatch.  */

Bool
XLHandleOneXEventForLayerShell (XEvent *event)
{
  LayerSurface *ls;
  Window window;

  if (!layer_surfaces)
    return False;

  window = None;

  if (event->type == Expose)
    window = event->xexpose.window;
  else if (event->type == GenericEvent
	   && event->xgeneric.extension == xi2_opcode)
    /* Determine the window this input event is directed at.  */
    window = XLGetGEWindowForSeats (event);

  if (window == None)
    return False;

  ls = XLLookUpAssoc (layer_surfaces, window);

  if (!ls)
    return False;

  if (event->type == Expose)
    {
      SubcompositorExpose (ls->subcompositor, event);
      return True;
    }

  if (event->type == GenericEvent)
    {
      /* Route pointer and keyboard events to the surface owning this
	 window, just as is done for xdg surfaces.  */
      if (ls->role.surface)
	XLDispatchGEForSeats (event, ls->role.surface,
			      ls->subcompositor);

      return True;
    }

  return False;
}
