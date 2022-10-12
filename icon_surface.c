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

/* Generic "icon surface" role.  */

#include <stdlib.h>

#include "compositor.h"

#include <X11/extensions/shape.h>

#define IconSurfaceFromRole(role) ((IconSurface *) role)

enum
  {
    StateLateFrame  = 1,
    StateIsMapped   = (1 << 1),
    StateIsReleased = (1 << 2),
  };

struct _IconSurface
{
  /* The role object itself.  */
  Role role;

  /* The window used by this role.  */
  Window window;

  /* The rendering target associated with this role.  */
  RenderTarget target;

  /* The subcompositor associated with this role.  */
  Subcompositor *subcompositor;

  /* The frame clock associated with this role.  */
  FrameClock *clock;

  /* The number of references to this role.  */
  int refcount;

  /* Some state.  */
  int state;

  /* The position of this icon surface relative to the root
     window.  */
  int x, y;

  /* The last known bounds of this icon surface.  */
  int min_x, min_y, max_x, max_y;
};

/* Hash table of all icon surfaces.  */
static XLAssocTable *surfaces;

static void
WriteRedirectProperty (IconSurface *icon)
{
  unsigned long bypass_compositor;

  bypass_compositor = 2;
  XChangeProperty (compositor.display, icon->window,
		   _NET_WM_BYPASS_COMPOSITOR, XA_CARDINAL,
		   32, PropModeReplace,
		   (unsigned char *) &bypass_compositor, 1);
}

static void
ReleaseBacking (IconSurface *icon)
{
  if (--icon->refcount)
    return;

  /* Release all allocated resources.  */
  RenderDestroyRenderTarget (icon->target);
  XDestroyWindow (compositor.display, icon->window);

  /* And the association.  */
  XLDeleteAssoc (surfaces, icon->window);

  /* There shouldn't be any children of the subcompositor at this
     point.  */
  SubcompositorFree (icon->subcompositor);

  /* The frame clock is no longer useful.  */
  XLFreeFrameClock (icon->clock);

  /* And since there are no C level references to the icon surface
     anymore, it can be freed.  */
  XLFree (icon);
}

static void
Teardown (Surface *surface, Role *role)
{
  IconSurface *icon;

  icon = IconSurfaceFromRole (role);
  role->surface = NULL;

  /* Unparent the surface's views as well.  */
  ViewUnparent (surface->view);
  ViewUnparent (surface->under);

  /* Detach the surface's views from the subcompositor.  */
  ViewSetSubcompositor (surface->view, NULL);
  ViewSetSubcompositor (surface->under, NULL);

  /* Release the backing data.  */
  ReleaseBacking (icon);
}

static Bool
Setup (Surface *surface, Role *role)
{
  IconSurface *icon;

  /* Set role->surface here, since this is where the refcounting is
     done as well.  */
  role->surface = surface;

  icon = IconSurfaceFromRole (role);
  ViewSetSubcompositor (surface->view,
		        icon->subcompositor);
  ViewSetSubcompositor (surface->under,
		        icon->subcompositor);

  /* Make sure the under view ends up beneath surface->view.  */
  SubcompositorInsert (icon->subcompositor,
		       surface->under);
  SubcompositorInsert (icon->subcompositor,
		       surface->view);

  /* Retain the backing data.  */
  icon->refcount++;

  return True;
}

static void
ReleaseBuffer (Surface *surface, Role *role, ExtBuffer *buffer)
{
  IconSurface *icon;

  icon = IconSurfaceFromRole (role);

  /* Icon surfaces are not supposed to change much, so doing an XSync
     (or XIfEvent) here is okay.  */
  RenderWaitForIdle (XLRenderBufferFromBuffer (buffer),
		     icon->target);

  /* Now really release the buffer.  */
  XLReleaseBuffer (buffer);
}

static void
UpdateOutputs (IconSurface *icon)
{
  int x_off, y_off;

  if (!icon->role.surface)
    return;

  x_off = icon->role.surface->current_state.x;
  y_off = icon->role.surface->current_state.y;

  XLUpdateSurfaceOutputs (icon->role.surface,
			  icon->x + icon->min_x + x_off,
			  icon->y + icon->min_y + y_off,
			  icon->max_x - icon->min_x + 1,
			  icon->max_y - icon->min_y + 1);
}

static void
NoteBounds (void *data, int min_x, int min_y, int max_x, int max_y)
{
  IconSurface *icon;
  int x, y;

  icon = data;

  if (min_x != icon->min_x || min_y != icon->min_y
      || max_x != icon->max_x || max_y != icon->max_y)
    {
      x = icon->x + icon->role.surface->current_state.x;
      y = icon->y + icon->role.surface->current_state.y;

      /* If the bounds changed, move the window to the right
	 position.  */
      XMoveResizeWindow (compositor.display, icon->window,
			 x + min_x, y + min_y, max_x - min_x + 1,
			 max_y - min_y + 1);

      /* Update the outputs that this surface is inside.  */
      UpdateOutputs (icon);

      /* Save the new bounds.  */
      icon->min_x = min_x;
      icon->min_y = min_y;
      icon->max_x = max_x;
      icon->max_y = max_y;
    }
}

static void
RunFrameCallbacks (Surface *surface, FrameClock *clock)
{
  struct timespec time;
  uint64_t last_drawn_time;

  /* Surface can be NULL for various reasons, especially events
     arriving after the icon surface is detached.  */
  if (!surface)
    return;

  last_drawn_time = XLFrameClockGetFrameTime (clock);

  if (!last_drawn_time)
    {
      clock_gettime (CLOCK_MONOTONIC, &time);
      XLSurfaceRunFrameCallbacks (surface, time);
    }
  else
    XLSurfaceRunFrameCallbacksMs (surface, last_drawn_time / 1000);
}

static void
AfterFrame (FrameClock *clock, void *data)
{
  IconSurface *icon;

  icon = data;

  if (icon->state & StateLateFrame)
    {
      icon->state &= ~StateLateFrame;

      /* Since we are running late, make the compositor draw the frame
	 now.  */
      XLFrameClockStartFrame (clock, True);
      SubcompositorUpdate (icon->subcompositor);
      XLFrameClockEndFrame (clock);

      return;
    }

  RunFrameCallbacks (icon->role.surface, clock);
}

static void
MaybeMapWindow (IconSurface *icon)
{
  if (icon->state & StateIsMapped)
    return;

  if (icon->state & StateIsReleased)
    return;

  XMapRaised (compositor.display, icon->window);
  icon->state |= StateIsMapped;

  UpdateOutputs (icon);
}

static void
MaybeUnmapWindow (IconSurface *icon)
{
  if (!(icon->state & StateIsMapped))
    return;

  XUnmapWindow (compositor.display, icon->window);
  icon->state &= ~StateIsMapped;

  if (icon->role.surface)
    XLClearOutputs (icon->role.surface);
}

static void
MoveWindowTo (IconSurface *icon, int x, int y)
{
  int x_off, y_off;

  if (icon->x == x && icon->y == y)
    return;

  icon->x = x;
  icon->y = y;
  x_off = icon->role.surface->current_state.x;
  y_off = icon->role.surface->current_state.y;

  XMoveWindow (compositor.display, icon->window,
	       icon->x + icon->min_x + x_off,
	       icon->y + icon->min_y + y_off);
  UpdateOutputs (icon);
}

static void
Commit (Surface *surface, Role *role)
{
  IconSurface *icon;

  icon = IconSurfaceFromRole (role);

  if (XLFrameClockFrameInProgress (icon->clock))
    {
      /* A frame is already in progress; schedule another one for
	 later.  */
      icon->state |= StateLateFrame;
    }
  else
    {
      /* Start a frame and update the icon surface now.  */
      XLFrameClockStartFrame (icon->clock, False);
      SubcompositorUpdate (icon->subcompositor);
      XLFrameClockEndFrame (icon->clock);
    }

  /* Move the window if any offset was specified.  */
  if (surface->pending_state.pending & PendingAttachments)
    MoveWindowTo (icon, icon->x, icon->y);

  /* Map or unmap the window according to whether or not the surface
     has an attached buffer.  */
  if (surface->current_state.buffer)
    MaybeMapWindow (icon);
  else
    MaybeUnmapWindow (icon);
}

static Bool
Subframe (Surface *surface, Role *role)
{
  IconSurface *icon;

  icon = IconSurfaceFromRole (role);

  if (XLFrameClockFrameInProgress (icon->clock))
    {
      /* A frame is already in progress; schedule another one for
	 later.  */
      icon->state |= StateLateFrame;
      return False;
    }

  /* I guess subsurface updates don't count as urgent frames?  */
  XLFrameClockStartFrame (icon->clock, False);
  return True;
}

static void
EndSubframe (Surface *surface, Role *role)
{
  IconSurface *icon;

  icon = IconSurfaceFromRole (role);
  XLFrameClockEndFrame (icon->clock);
}

static Window
GetWindow (Surface *surface, Role *role)
{
  /* XLWindowFromSurface is used to obtain a window for input-related
     purposes.  Icon surfaces cannot be subject to input, so don't
     return the backing window.  */
  return None;
}

IconSurface *
XLGetIconSurface (Surface *surface)
{
  IconSurface *role;
  XSetWindowAttributes attrs;
  unsigned int flags;

  role = XLCalloc (1, sizeof *role);
  role->refcount = 1;

  role->role.funcs.commit = Commit;
  role->role.funcs.teardown = Teardown;
  role->role.funcs.setup = Setup;
  role->role.funcs.release_buffer = ReleaseBuffer;
  role->role.funcs.subframe = Subframe;
  role->role.funcs.end_subframe = EndSubframe;
  role->role.funcs.get_window = GetWindow;

  /* Make an override-redirect window to use as the icon surface.  */
  flags = (CWColormap | CWBorderPixel | CWEventMask
	   | CWOverrideRedirect);
  attrs.colormap = compositor.colormap;
  attrs.border_pixel = border_pixel;
  attrs.event_mask = (ExposureMask | StructureNotifyMask);
  attrs.override_redirect = 1;

  role->window = XCreateWindow (compositor.display,
				DefaultRootWindow (compositor.display),
				0, 0, 1, 1, 0, compositor.n_planes,
				InputOutput, compositor.visual, flags,
				&attrs);

  /* Add _NET_WM_SYNC_REQUEST to the list of supported protocols.  */
  XSetWMProtocols (compositor.display, role->window,
		   &_NET_WM_SYNC_REQUEST, 1);

  /* Set _NET_WM_WINDOW_TYPE to _NET_WM_WINDOW_TYPE_DND.  */
  XChangeProperty (compositor.display, role->window,
		   _NET_WM_WINDOW_TYPE, XA_ATOM, 32,
		   PropModeReplace,
		   (unsigned char *) &_NET_WM_WINDOW_TYPE_DND, 1);

  /* Create a target associated with the window.  */
  role->target = RenderTargetFromWindow (role->window, None);

  /* For simplicity reasons we do not handle idle notifications
     asynchronously.  */
  RenderSetNeedWaitForIdle (role->target);

  /* Create a subcompositor associated with the window.  */
  role->subcompositor = MakeSubcompositor ();
  role->clock = XLMakeFrameClockForWindow (role->window);

  /* Set the subcompositor target and some callbacks.  */
  SubcompositorSetTarget (role->subcompositor, &role->target);
  SubcompositorSetBoundsCallback (role->subcompositor,
				  NoteBounds, role);

  /* Clear the input region of the window.  */
  XShapeCombineRectangles (compositor.display, role->window,
			   ShapeInput, 0, 0, NULL, 0, ShapeSet,
			   Unsorted);

  XLMakeAssoc (surfaces, role->window, role);

  /* Tell the compositing manager to never un-redirect this window.
     If it does, frame synchronization will not work.  */
  WriteRedirectProperty (role);

  /* Initialize frame callbacks.  */
  XLFrameClockAfterFrame (role->clock, AfterFrame, role);

  if (!XLSurfaceAttachRole (surface, &role->role))
    abort ();

  return role;
}

Bool
XLHandleOneXEventForIconSurfaces (XEvent *event)
{
  IconSurface *icon;

  if (event->type == ClientMessage
      && ((event->xclient.message_type == _NET_WM_FRAME_DRAWN
	   || event->xclient.message_type == _NET_WM_FRAME_TIMINGS)
	  || (event->xclient.message_type == WM_PROTOCOLS
	      && event->xclient.data.l[0] == _NET_WM_SYNC_REQUEST)))
    {
      icon = XLLookUpAssoc (surfaces, event->xclient.window);

      if (icon)
	{
	  XLFrameClockHandleFrameEvent (icon->clock, event);
	  return True;
	}

      return False;
    }

  if (event->type == Expose)
    {
      icon = XLLookUpAssoc (surfaces, event->xexpose.window);

      if (icon)
	{
	  SubcompositorExpose (icon->subcompositor, event);
	  return True;
	}

      return False;
    }

  return False;
}

void
XLMoveIconSurface (IconSurface *surface, int root_x, int root_y)
{
  MoveWindowTo (surface, root_x, root_y);
}

void
XLInitIconSurfaces (void)
{
  /* This assoc table is rather small, since the amount of icon
     surfaces alive at any given time is also low.  */
  surfaces = XLCreateAssocTable (25);
}

void
XLReleaseIconSurface (IconSurface *icon)
{
  /* Unmap the surface and mark it as released, meaning it will not be
     mapped again in the future.  */
  MaybeUnmapWindow (icon);
  icon->state |= StateIsReleased;

  /* Release the icon surface.  */
  ReleaseBacking (icon);
}

Bool
XLIsWindowIconSurface (Window window)
{
  return XLLookUpAssoc (surfaces, window) != NULL;
}
