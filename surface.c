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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <inttypes.h>
#include <float.h>

#include "compositor.h"

/* List of all currently existing surfaces.  */
Surface all_surfaces;

static DestroyCallback *
AddDestroyCallbackAfter (DestroyCallback *after)
{
  DestroyCallback *callback;

  callback = XLCalloc (1, sizeof *callback);

  callback->next = after->next;
  callback->last = after;

  after->next->last = callback;
  after->next = callback;

  return callback;
}

static void
UnlinkDestroyCallback (DestroyCallback *callback)
{
  callback->last->next = callback->next;
  callback->next->last = callback->last;

  callback->last = callback;
  callback->next = callback;
}

static UnmapCallback *
AddUnmapCallbackAfter (UnmapCallback *after)
{
  UnmapCallback *callback;

  callback = XLCalloc (1, sizeof *callback);

  callback->next = after->next;
  callback->last = after;

  after->next->last = callback;
  after->next = callback;

  return callback;
}

static void
UnlinkUnmapCallback (UnmapCallback *callback)
{
  callback->last->next = callback->next;
  callback->next->last = callback->last;

  callback->last = callback;
  callback->next = callback;
}

static CommitCallback *
AddCommitCallbackAfter (CommitCallback *after)
{
  CommitCallback *callback;

  callback = XLSafeMalloc (sizeof *callback);

  if (!callback)
    return callback;

  callback->next = after->next;
  callback->last = after;

  after->next->last = callback;
  after->next = callback;

  return callback;
}

static void
UnlinkCommitCallback (CommitCallback *callback)
{
  callback->last->next = callback->next;
  callback->next->last = callback->last;

  callback->last = callback;
  callback->next = callback;
}

static void
RunCommitCallbacks (Surface *surface)
{
  CommitCallback *callback;

  /* first is a sentinel node.  */
  callback = surface->commit_callbacks.last;

  /* Run commit callbacks in the order that they were created in.  The
     subsurface code relies on this for subsurfaces to be confirmed in
     the right order.  */
  while (callback != &surface->commit_callbacks)
    {
      callback->commit (surface, callback->data);
      callback = callback->last;
    }
}

static void
RunUnmapCallbacks (Surface *surface)
{
  UnmapCallback *callback, *last;

  /* first is a sentinel node.  */
  callback = surface->unmap_callbacks.next;

  while (callback != &surface->unmap_callbacks)
    {
      last = callback;
      callback = callback->next;

      last->unmap (last->data);
    }
}

static void
FreeCommitCallbacks (CommitCallback *first)
{
  CommitCallback *callback, *last;

  /* first is a sentinel node.  */
  callback = first->next;

  while (callback != first)
    {
      last = callback;
      callback = callback->next;

      XLFree (last);
    }
}

static void
FreeUnmapCallbacks (UnmapCallback *first)
{
  UnmapCallback *callback, *last;

  /* first is a sentinel node.  */
  callback = first->next;

  while (callback != first)
    {
      last = callback;
      callback = callback->next;

      XLFree (last);
    }
}

static void
FreeDestroyCallbacks (DestroyCallback *first)
{
  DestroyCallback *callback, *last;

  callback = first->next;

  while (callback != first)
    {
      last = callback;
      callback = callback->next;

      last->destroy_func (last->data);
      XLFree (last);
    }
}

static FrameCallback *
AddCallbackAfter (FrameCallback *after)
{
  FrameCallback *callback;

  callback = XLSafeMalloc (sizeof *callback);

  if (!callback)
    return callback;

  callback->next = after->next;
  callback->last = after;

  after->next->last = callback;
  after->next = callback;

  return callback;
}

static void
UnlinkCallbacks (FrameCallback *start, FrameCallback *end)
{
  /* First, make the list skip past END.  */
  start->last->next = end->next;
  end->next->last = start->last;

  /* Then, unlink the list.  */
  start->last = end;
  end->next = start;
}

static void
RelinkCallbacksAfter (FrameCallback *start, FrameCallback *end,
		      FrameCallback *dest)
{
  end->next = dest->next;
  start->last = dest;

  dest->next->last = end;
  dest->next = start;
}

static void
HandleCallbackResourceDestroy (struct wl_resource *resource)
{
  FrameCallback *callback;

  callback = wl_resource_get_user_data (resource);
  UnlinkCallbacks (callback, callback);
  XLFree (callback);
}

static void
FreeFrameCallbacks (FrameCallback *start)
{
  FrameCallback *callback, *last;

  callback = start->next;

  while (callback != start)
    {
      last = callback;
      callback = callback->next;

      /* This will unlink last from its surroundings and free it.  */
      wl_resource_destroy (last->resource);
    }
}

static void
RunFrameCallbacks (FrameCallback *start, uint32_t time)
{
  FrameCallback *callback, *last;

  callback = start->next;

  while (callback != start)
    {
      last = callback;
      callback = callback->next;

      wl_callback_send_done (last->resource, time);
      /* This will unlink last from its surroundings and free it.  */
      wl_resource_destroy (last->resource);
    }
}

static void
AttachBuffer (State *state, ExtBuffer *buffer)
{
  if (state->buffer)
    XLDereferenceBuffer (state->buffer);

  state->buffer = buffer;
  XLRetainBuffer (buffer);
}

static void
ClearBuffer (State *state)
{
  if (!state->buffer)
    return;

  XLDereferenceBuffer (state->buffer);
  state->buffer = NULL;
}

static void
DoRelease (Surface *surface, ExtBuffer *buffer)
{
  /* Release the buffer now.  */
  if (surface->role && !(renderer_flags & ImmediateRelease))
    surface->role->funcs.release_buffer (surface, surface->role, buffer);
  else
    XLReleaseBuffer (buffer);
}

static void
DestroySurface (struct wl_client *client,
		struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
Attach (struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *buffer_resource, int32_t x, int32_t y)
{
  Surface *surface;
  ExtBuffer *buffer;

  if (x != 0 && y != 0
      && wl_resource_get_version (resource) >= 5)
    {
      wl_resource_post_error (resource, WL_SURFACE_ERROR_INVALID_OFFSET,
			      "invalid offsets given to wl_surface_attach");
      return;
    }

  surface = wl_resource_get_user_data (resource);

  if (buffer_resource)
    {
      buffer = wl_resource_get_user_data (buffer_resource);
      AttachBuffer (&surface->pending_state, buffer);
    }
  else
    ClearBuffer (&surface->pending_state);

  surface->pending_state.x = x;
  surface->pending_state.y = y;

  surface->pending_state.pending |= PendingBuffer;
  surface->pending_state.pending |= PendingAttachments;
}

static void
Offset (struct wl_client *client, struct wl_resource *resource,
	int32_t x, int32_t y)
{
  Surface *surface;

  surface = wl_resource_get_user_data (resource);

  surface->pending_state.x = x;
  surface->pending_state.y = y;

  surface->pending_state.pending |= PendingAttachments;
}

static void
Damage (struct wl_client *client, struct wl_resource *resource,
	int32_t x, int32_t y, int32_t width, int32_t height)
{
  Surface *surface;

  surface = wl_resource_get_user_data (resource);

  /* Prevent integer overflow during later processing, since some
     clients really set the damage region to INT_MAX.  */

  pixman_region32_union_rect (&surface->pending_state.surface,
			      &surface->pending_state.surface,
			      x, y, MIN (65535, width),
			      MIN (65535, height));

  surface->pending_state.pending |= PendingSurfaceDamage;
}

static void
Frame (struct wl_client *client, struct wl_resource *resource,
       uint32_t callback_id)
{
  struct wl_resource *callback_resource;
  FrameCallback *callback;
  Surface *surface;

  surface = wl_resource_get_user_data (resource);
  callback = AddCallbackAfter (&surface->pending_state.frame_callbacks);

  if (!callback)
    {
      wl_client_post_no_memory (client);

      return;
    }

  callback_resource = wl_resource_create (client, &wl_callback_interface,
					  1, callback_id);

  if (!callback_resource)
    {
      wl_client_post_no_memory (client);
      UnlinkCallbacks (callback, callback);
      XLFree (callback);

      return;
    }

  wl_resource_set_implementation (callback_resource, NULL,
				  callback, HandleCallbackResourceDestroy);

  callback->resource = callback_resource;
  surface->pending_state.pending |= PendingFrameCallbacks;
}

static void
SetOpaqueRegion (struct wl_client *client, struct wl_resource *resource,
		 struct wl_resource *region_resource)
{
  Surface *surface;
  pixman_region32_t *region;

  surface = wl_resource_get_user_data (resource);

  if (region_resource)
    {
      region = wl_resource_get_user_data (region_resource);

      /* Some ugly clients give the region ridiculous dimensions like
	 0, 0, INT_MAX, INT_MAX, which causes overflows later on.  So
	 intersect it with the largest possible dimensions of a
	 view.  */
      pixman_region32_intersect_rect (&surface->pending_state.opaque,
				      region, 0, 0, 65535, 65535);
    }
  else
    pixman_region32_clear (&surface->pending_state.opaque);

  surface->pending_state.pending |= PendingOpaqueRegion;
}

static void
SetInputRegion (struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *region_resource)
{
  Surface *surface;
  pixman_region32_t *region;

  surface = wl_resource_get_user_data (resource);

  if (region_resource)
    {
      region = wl_resource_get_user_data (region_resource);

      /* Some ugly clients give the region ridiculous dimensions like
	 0, 0, INT_MAX, INT_MAX, which causes overflows later on.  So
	 intersect it with the largest possible dimensions of a
	 view.  */
      pixman_region32_intersect_rect (&surface->pending_state.input,
				      region, 0, 0, 65535, 65535);
    }
  else
    {
      pixman_region32_clear (&surface->pending_state.input);
      pixman_region32_union_rect (&surface->pending_state.input,
				  &surface->pending_state.input,
				  0, 0, 65535, 65535);
    }

  surface->pending_state.pending |= PendingInputRegion;
}

void
XLDefaultCommit (Surface *surface)
{
  /* Nothing has to be done here yet.  */
}

static void
ApplyBuffer (Surface *surface)
{
  if (surface->current_state.buffer)
    ViewAttachBuffer (surface->view, surface->current_state.buffer);
  else
    ViewDetach (surface->view);
}

/* Return the effective scale.

   The effective scale is a value by which to scale down the contents
   of a surface on display.  */

static int
GetEffectiveScale (int scale)
{
  /* A "scale" is how many times to scale _down_ a surface, not up.
     Negative values mean to scale the surface up instead of down.  */
  scale = scale - global_scale_factor;

  return scale;
}

static void
ApplyBufferTransform (Surface *surface)
{
  ViewSetTransform (surface->view,
		    surface->current_state.transform);
}

static void
ApplyScale (Surface *surface)
{
  int scale, effective;
  double b, g, e, d;
  XLList *subsurface;
  Surface *subsurface_surface;
  Role *role;

  scale = surface->current_state.buffer_scale;
  effective = GetEffectiveScale (scale);

  ViewSetScale (surface->view, effective);

  /* Now calculate the surface factor, a factor used to scale surface
     coordinates to view (X window) coordinates.

     The scale we want is the width of the view (area on the X screen)
     divided by the surface width, which is the width of the buffer
     after it has been shrunk B - 1 times, B being the buffer scale.

     However, the size of the view is not available during computation.
     So, computing the scale looks something like this:

	 A = width of buffer <-------------+-- we must reduce these out
	 B = buffer scale                  |
	 C = width of view <---------------+
	 L = surface width <---------------+
	 G = global scale
	 E = scale after accounting for difference between the global
	     and buffer scales
	 D = desired scale, otherwise C / surface width

	 A = 2004
	 B = 3
	 G = 2

	 L = A / B
	 E = G - B

     if E is not less than 0

         E = E + 1

     else

         E = 1 / abs (E - 1)

     finally

	C = A * E
	D = C / L

	D = (A * E) / (A / B)
	D = B * E.  */

  b = scale;
  g = global_scale_factor;
  e = g - b;

  if (e >= 0.0)
    e = e + 1;
  else
    e = 1.0 / fabs (e - 1);

  d = b * e;

  if (surface->factor != d)
    {
      /* The scale factor changed.  */
      surface->factor = d;

      /* Notify all subsurfaces to move themselves to a more correct
	 location.  */
      subsurface = surface->subsurfaces;
      for (; subsurface; subsurface = subsurface->next)
	{
	  /* Get the subsurface role.  */
	  subsurface_surface = subsurface->data;
	  role = subsurface_surface->role;

	  /* Make sure it still has a surface, since it should not be
	     in surface->subsurfaces otherwise.  */
	  XLAssert (role->surface != NULL);

	  /* Call the parent rescale hook.  */
	  if (role->funcs.rescale)
	    role->funcs.rescale (role->surface, role);
	}
    }
}

static void
ApplyOpaqueRegion (Surface *surface)
{
  pixman_region32_t temp;

  /* These regions, along with the global damage, must be multipled by
     the global scale factor.  */
  if (global_scale_factor == 1)
    ViewSetOpaque (surface->view,
		   &surface->current_state.opaque);
  else
    {
      pixman_region32_init (&temp);
      XLScaleRegion (&temp, &surface->current_state.opaque,
		     surface->factor, surface->factor);
      ViewSetOpaque (surface->view, &temp);
      pixman_region32_fini (&temp);
    }
}

static void
ApplyInputRegion (Surface *surface)
{
  pixman_region32_t temp;

  /* These regions, along with the global damage, must be multipled by
     the global scale factor.  */
  if (global_scale_factor == 1)
    ViewSetInput (surface->view,
		  &surface->current_state.input);
  else
    {
      pixman_region32_init (&temp);
      XLScaleRegion (&temp, &surface->current_state.input,
		     surface->factor, surface->factor);
      ViewSetInput (surface->view, &temp);
      pixman_region32_fini (&temp);
    }

  /* The input region has changed, so pointer confinement must be
     redone.  */
  XLPointerConstraintsReconfineSurface (surface);
}

static void
ApplyViewport (Surface *surface)
{
  State *state;
  double dest_width, dest_height;
  double crop_width, crop_height, src_x, src_y;
  double max_width, max_height;

  state = &surface->current_state;

  /* If no values are specified, return and clear the viewport.  */
  if (state->src_x == -1 && state->dest_width == -1)
    {
      ViewClearViewport (surface->view);
      return;
    }

  /* Calculate the viewport.  crop_width and crop_height describe the
     amount by which to crop the surface contents, after conversion to
     window geometry.  dest_width and dest_height then describe how
     large the surface should be.  src_x and src_y describe the
     origin at which to start sampling from the buffer.  */

  if (state->buffer)
    {
      max_width = (RotatesDimensions (state->transform)
		   ? XLBufferHeight (state->buffer)
		   : XLBufferWidth (state->buffer));
      max_height = (RotatesDimensions (state->transform)
		    ? XLBufferWidth (state->buffer)
		    : XLBufferHeight (state->buffer));
    }
  else
    {
      /* If state->buffer is not set then the source rectangle does
	 not have to be validated now.  It will be validated later
	 once the buffer is attached.  */
      max_width = DBL_MAX;
      max_height = DBL_MAX;
    }

  if (state->src_x != -1.0)
    {
      /* This means a source rectangle has been specified.  Set src_x
	 and src_y.  */
      src_x = state->src_x;
      src_y = state->src_y;

      /* Also set crop_width and crop_height.  */
      crop_width = state->src_width;
      crop_height = state->src_height;
    }
  else
    {
      /* Set crop_width and crop_height to the default values, which
	 are the width and height of the buffer divided by the buffer
	 scale.  */
      src_x = 0;
      src_y = 0;

      crop_width = -1;
      crop_height = -1;
    }

  /* Now, either dest_width/dest_height are specified, or dest_width
     and dest_height should be crop_width and crop_height.  If the
     latter, then crop_width and crop_height must be integer
     values.  */

  if (state->dest_width != -1)
    {
      /* This means dest_width and dest_height have been explicitly
	 specified.  */
      dest_width = state->dest_width;
      dest_height = state->dest_height;
    }
  else
    {
      if ((rint (crop_width) != crop_width
	   || rint (crop_height) != crop_height)
	  /* If the src_width and src_height were not specified
	     manually but were computed from the buffer scale, don't
	     complain that they are not integer values.  The
	     underlying viewport code satisfactorily handles
	     fractional width and height anyway.  */
	  && state->src_x != 1.0)
	goto bad_size;

      dest_width = state->src_width;
      dest_height = state->src_height;
    }

  /* Now all of the fields above must be set.  Verify that none of
     them lie outside the buffer.  */
  if (state->src_x != -1
      && (src_x + crop_width - 1 >= max_width / state->buffer_scale
	  || src_y + crop_height - 1 >= max_height / state->buffer_scale))
    goto out_of_buffer;

  /* Finally, set the viewport.  Convert the values to window
     coordinates.  */
  src_x *= surface->factor;
  src_y *= surface->factor;

  if (crop_width != -1)
    {
      crop_width *= surface->factor;
      crop_height *= surface->factor;
    }

  dest_width *= surface->factor;
  dest_height *= surface->factor;

  /* And really set the viewport.  */
  ViewSetViewport (surface->view, src_x, src_y, crop_width,
		   crop_height, dest_width, dest_height);

  return;

 bad_size:
  /* By this point, surface->viewport should be non-NULL; however, if
     a synchronous subsurface applies invalid viewporter state,
     commits it, destroys the wp_viewport resource, and the parent
     commits, then the cached state applied due to the parent commit
     will be invalid, but the viewport resource will no longer be
     associated with the surface.  I don't know what to do in that
     case, so leave the behavior undefined.  */
  if (surface->viewport)
    XLWpViewportReportBadSize (surface->viewport);
  return;

 out_of_buffer:
  if (surface->viewport)
    XLWpViewportReportOutOfBuffer (surface->viewport);
}

static void
CheckViewportValues (Surface *surface)
{
  State *state;
  int width, height;

  state = &surface->current_state;

  if (!surface->viewport || state->src_x == -1.0
      || !state->buffer)
    return;

  /* A buffer is attached and a viewport source rectangle is set;
     check that it remains in bounds.  */

  if (RotatesDimensions (state->transform))
    {
      width = XLBufferHeight (state->buffer);
      height = XLBufferWidth (state->buffer);
    }
  else
    {
      width = XLBufferWidth (state->buffer);
      height = XLBufferHeight (state->buffer);
    }

  if (state->src_x + state->src_width - 1 >= width / state->buffer_scale
      || state->src_y + state->src_height - 1 >= height / state->buffer_scale)
    XLWpViewportReportBadSize (surface->viewport);
}

static void
HandleScaleChanged (void *data, int new_scale)
{
  Surface *surface;
  Subcompositor *subcompositor;

  surface = data;

  /* First, reapply various regions that depend on the surface
     scale.  */
  ApplyScale (surface);
  ApplyInputRegion (surface);
  ApplyOpaqueRegion (surface);
  ApplyViewport (surface);

  /* Next, call any role-specific hooks.  */
  if (surface->role && surface->role->funcs.rescale)
    surface->role->funcs.rescale (surface, surface->role);

  /* Then, redisplay the view if a subcompositor is already
     attached.  */
  subcompositor = ViewGetSubcompositor (surface->view);

  if (subcompositor
      && surface->role
      && surface->role->funcs.subsurface_update)
    surface->role->funcs.subsurface_update (surface, surface->role);

  /* The scale has changed, so pointer confinement must be redone.  */
  XLPointerConstraintsReconfineSurface (surface);
}

static void
ApplyDamage (Surface *surface)
{
  /* N.B. that this must come after the scale and viewport is
     applied.  */
  ViewDamageBuffer (surface->view, &surface->current_state.damage);
}

static void
ApplySurfaceDamage (Surface *surface)
{
  pixman_region32_t temp;

  /* These regions, along with the global damage, must be multipled by
     the global scale factor.  */

  if (global_scale_factor == 1)
    ViewDamage (surface->view,
		&surface->current_state.surface);
  else
    {
      pixman_region32_init (&temp);
      XLScaleRegion (&temp, &surface->current_state.surface,
		     surface->factor, surface->factor);
      ViewDamage (surface->view, &temp);
      pixman_region32_fini (&temp);
    }
}

static void
SavePendingState (Surface *surface)
{
  FrameCallback *start, *end;

  /* Save pending state to cached state.  Release any buffer
     previously in the cached state.  */

  if (surface->pending_state.pending & PendingBuffer)
    {
      if (surface->cached_state.buffer
	  && (surface->pending_state.buffer
	      != surface->cached_state.buffer)
	  /* If the cached buffer has already been applied, releasing
	     it is a mistake!  */
	  && (surface->cached_state.buffer
	      != surface->current_state.buffer))
        DoRelease (surface, surface->cached_state.buffer);

      if (surface->pending_state.buffer)
	{
	  AttachBuffer (&surface->cached_state,
			surface->pending_state.buffer);
	  ClearBuffer (&surface->pending_state);
	}
      else
	ClearBuffer (&surface->cached_state);
    }

  if (surface->pending_state.pending & PendingInputRegion)
    pixman_region32_copy (&surface->cached_state.input,
			  &surface->pending_state.input);

  if (surface->pending_state.pending & PendingOpaqueRegion)
    pixman_region32_copy (&surface->cached_state.opaque,
			  &surface->pending_state.opaque);

  if (surface->pending_state.pending & PendingBufferScale)
    surface->cached_state.buffer_scale
      = surface->pending_state.buffer_scale;

  if (surface->pending_state.pending & PendingBufferTransform)
    surface->cached_state.transform = surface->pending_state.transform;

  if (surface->pending_state.pending & PendingViewportDest)
    {
      surface->cached_state.dest_width
	= surface->pending_state.dest_width;
      surface->cached_state.dest_height
	= surface->pending_state.dest_height;
    }

  if (surface->pending_state.pending & PendingViewportSrc)
    {
      surface->cached_state.src_x = surface->pending_state.src_x;
      surface->cached_state.src_y = surface->pending_state.src_y;

      surface->cached_state.src_width
	= surface->pending_state.src_width;
      surface->cached_state.src_height
	= surface->pending_state.src_height;
    }

  if (surface->pending_state.pending & PendingAttachments)
    {
      surface->cached_state.x = surface->pending_state.x;
      surface->cached_state.y = surface->pending_state.y;
    }

  if (surface->pending_state.pending & PendingDamage)
    {
      pixman_region32_union (&surface->cached_state.damage,
			     &surface->cached_state.damage,
			     &surface->pending_state.damage);
      pixman_region32_clear (&surface->pending_state.damage);
    }

  if (surface->pending_state.pending & PendingSurfaceDamage)
    {
      pixman_region32_union (&surface->cached_state.surface,
			     &surface->cached_state.surface,
			     &surface->pending_state.surface);
      pixman_region32_clear (&surface->pending_state.surface);
    }

  if (surface->pending_state.pending & PendingFrameCallbacks
      && (surface->pending_state.frame_callbacks.next
	  != &surface->pending_state.frame_callbacks))
    {
      start = surface->pending_state.frame_callbacks.next;
      end = surface->pending_state.frame_callbacks.last;

      UnlinkCallbacks (start, end);
      RelinkCallbacksAfter (start, end,
			    &surface->cached_state.frame_callbacks);
    }

  surface->cached_state.pending |= surface->pending_state.pending;
  surface->pending_state.pending = PendingNone;
}

static void
TryEarlyRelease (Surface *surface)
{
  ExtBuffer *buffer;
  RenderBuffer render_buffer;

  /* The rendering backend may have copied the contents of, i.e., a
     shared memory buffer to a backing texture.  In that case buffers
     can be released immediately after commit.  Programs such as GTK
     also rely on the compositor performing such an optimization, or
     else they will constantly create new buffers to back their back
     buffer contents.  */

  buffer = surface->current_state.buffer;

  if (!buffer)
    return;

  /* Get the render buffer.  */
  render_buffer = XLRenderBufferFromBuffer (buffer);

  /* Don't release immediately if not okay.  */
  if (!RenderCanReleaseNow (render_buffer))
    return;

  DoRelease (surface, buffer);

  /* Set the flag saying that the buffer has been released.  */
  surface->current_state.pending |= BufferAlreadyReleased;
}

static void
InternalCommit1 (Surface *surface, State *pending)
{
  FrameCallback *start, *end;

  /* Merge the state in pending into the surface's current state.  */

  if (pending->pending & PendingBuffer)
    {
      /* The buffer may already released if its contents were
	 copied, i.e. uploaded to a texture, during updates.  */
      if (!(surface->current_state.pending & BufferAlreadyReleased)
	  && surface->current_state.buffer
	  && surface->current_state.buffer != pending->buffer)
	DoRelease (surface, surface->current_state.buffer);

      /* Clear this flag now, since the attached buffer has
	 changed.  */
      surface->current_state.pending &= ~BufferAlreadyReleased;

      if (pending->buffer)
	{
	  AttachBuffer (&surface->current_state,
			pending->buffer);
	  ApplyBuffer (surface);
	  ClearBuffer (pending);

	  /* Check that any applied viewport source rectangles remain
	     valid.  */
	  if (!(pending->pending & PendingViewportSrc))
	    CheckViewportValues (surface);
	}
      else
	{
	  ClearBuffer (&surface->current_state);
	  ApplyBuffer (surface);
	  ClearBuffer (pending);
	}
    }

  if (pending->pending & PendingBufferScale)
    {
      surface->current_state.buffer_scale = pending->buffer_scale;
      ApplyScale (surface);
    }

  if (pending->pending & PendingBufferTransform)
    {
      surface->current_state.transform = pending->transform;
      ApplyBufferTransform (surface);
    }

  if (pending->pending & PendingInputRegion)
    {
      pixman_region32_copy (&surface->current_state.input,
			    &pending->input);
      ApplyInputRegion (surface);
    }

  if (pending->pending & PendingOpaqueRegion)
    {
      pixman_region32_copy (&surface->current_state.opaque,
			    &pending->opaque);
      ApplyOpaqueRegion (surface);
    }

  if (pending->pending & PendingViewportSrc
      || pending->pending & PendingViewportDest)
    {
      /* Copy the viewport data over to the current state.  */

      if (pending->pending & PendingViewportDest)
	{
	  surface->current_state.dest_width = pending->dest_width;
	  surface->current_state.dest_height = pending->dest_height;
	}

      if (pending->pending & PendingViewportSrc)
	{
	  surface->current_state.src_x = pending->src_x;
	  surface->current_state.src_y = pending->src_y;
	  surface->current_state.src_width = pending->src_width;
	  surface->current_state.src_height = pending->src_height;
	}

      /* And apply the viewport now.  */
      ApplyViewport (surface);
    }

  if (pending->pending & PendingAttachments)
    {
      surface->current_state.x = pending->x;
      surface->current_state.y = pending->y;
    }

  if (pending->pending & PendingDamage)
    {
      pixman_region32_copy (&surface->current_state.damage,
			    &pending->damage);
      pixman_region32_clear (&pending->damage);

      ApplyDamage (surface);
    }

  if (pending->pending & PendingSurfaceDamage)
    {
      pixman_region32_copy (&surface->current_state.surface,
			    &pending->surface);
      pixman_region32_clear (&pending->surface);

      ApplySurfaceDamage (surface);
    }

  if (pending->pending & PendingFrameCallbacks)
    {
      /* Insert the pending frame callbacks in front of the current
	 ones.  */

      if (pending->frame_callbacks.next != &pending->frame_callbacks)
	{
	  start = pending->frame_callbacks.next;
	  end = pending->frame_callbacks.last;

	  UnlinkCallbacks (start, end);
	  RelinkCallbacksAfter (start, end,
				&surface->current_state.frame_callbacks);
	}
    }
}

static void
InternalCommit (Surface *surface, State *pending)
{
  InternalCommit1 (surface, pending);

  /* Run commit callbacks.  This tells synchronous subsurfaces to
     update, and tells explicit synchronization to wait for any sync
     fence.  */
  RunCommitCallbacks (surface);

  if (surface->subsurfaces)
    /* Pending surface stacking actions are stored on the parent so
       they run in the right order.  */
    XLSubsurfaceHandleParentCommit (surface);

  /* Wait for any sync fence to be triggered before proceeding.  */
  XLWaitFence (surface);

  if (!surface->role)
    {
      XLDefaultCommit (surface);
      pending->pending = PendingNone;

      return;
    }

  surface->role->funcs.commit (surface, surface->role);
  pending->pending = PendingNone;

  /* Release the attached buffer if possible.  The role may have
     called SubcompositorUpdate, leading to the buffer contents being
     copied.  */
  TryEarlyRelease (surface);
}

static void
Commit (struct wl_client *client, struct wl_resource *resource)
{
  Surface *surface;

  surface = wl_resource_get_user_data (resource);

  /* First, clear the acquire fence if it is set.  If a
     synchronization object is attached, the following call will then
     attach any new fence specified.  */

  if (surface->acquire_fence != -1)
    close (surface->acquire_fence);

  /* Release any attached explicit synchronization release callback.
     XXX: this is not right with synchronous subsurfaces?  */

  if (surface->release)
    XLSyncRelease (surface->release);

  if (surface->synchronization)
    /* This is done here so early commit hooks can be run for
       i.e. synchronous subsurfaces.  */
    XLSyncCommit (surface->synchronization);

  if (surface->role && surface->role->funcs.early_commit
      /* The role chose to postpone the commit for a later time.  */
      && !surface->role->funcs.early_commit (surface, surface->role))
    {
      /* So save the state for the role to commit later.  */
      SavePendingState (surface);
      return;
    }

  InternalCommit (surface, &surface->pending_state);
}

static Bool
GetBufferTransform (int32_t wayland_transform,
		    BufferTransform *transform)
{
  switch (wayland_transform)
    {
    case WL_OUTPUT_TRANSFORM_NORMAL:
      *transform = Normal;
      return True;

    case WL_OUTPUT_TRANSFORM_90:
      *transform = CounterClockwise90;
      return True;

    case WL_OUTPUT_TRANSFORM_180:
      *transform = CounterClockwise180;
      return True;

    case WL_OUTPUT_TRANSFORM_270:
      *transform = CounterClockwise270;
      return True;

    case WL_OUTPUT_TRANSFORM_FLIPPED:
      *transform = Flipped;
      return True;

    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
      *transform = Flipped90;
      return True;

    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
      *transform = Flipped180;
      return True;

    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
      *transform = Flipped270;
      return True;
    }

  return False;
}

static void
SetBufferTransform (struct wl_client *client, struct wl_resource *resource,
		    int32_t transform)
{
  Surface *surface;

  surface = wl_resource_get_user_data (resource);

  if (!GetBufferTransform (transform, &surface->pending_state.transform))
    wl_resource_post_error (resource, WL_SURFACE_ERROR_INVALID_TRANSFORM,
			    "invalid transform specified");
  else
    surface->pending_state.pending |= PendingBufferTransform;
}

static void
SetBufferScale (struct wl_client *client, struct wl_resource *resource,
		int32_t scale)
{
  Surface *surface;

  if (scale <= 0)
    {
      wl_resource_post_error (resource, WL_SURFACE_ERROR_INVALID_SCALE,
			      "invalid scale: %d", scale);
      return;
    }

  surface = wl_resource_get_user_data (resource);

  surface->pending_state.buffer_scale = scale;
  surface->pending_state.pending |= PendingBufferScale;
}

static void
DamageBuffer (struct wl_client *client, struct wl_resource *resource,
	      int32_t x, int32_t y, int32_t width, int32_t height)
{
  Surface *surface;

  surface = wl_resource_get_user_data (resource);

  /* Prevent integer overflow during later processing, since some
     clients really set the damage region to INT_MAX.  */

  pixman_region32_union_rect (&surface->pending_state.damage,
			      &surface->pending_state.damage,
			      x, y, MIN (65535, width),
			      MIN (65535, height));

  surface->pending_state.pending |= PendingDamage;
}

static const struct wl_surface_interface wl_surface_impl =
  {
    .destroy = DestroySurface,
    .attach = Attach,
    .damage = Damage,
    .frame = Frame,
    .set_opaque_region = SetOpaqueRegion,
    .set_input_region = SetInputRegion,
    .commit = Commit,
    .set_buffer_transform = SetBufferTransform,
    .set_buffer_scale = SetBufferScale,
    .damage_buffer = DamageBuffer,
    .offset = Offset,
  };

static void
InitState (State *state)
{
  pixman_region32_init (&state->damage);
  pixman_region32_init (&state->opaque);
  pixman_region32_init (&state->surface);

  /* The initial state of the input region is always infinite.  */
  pixman_region32_init_rect (&state->input, 0, 0,
			     65535, 65535);

  state->pending = PendingNone;
  state->buffer = NULL;
  state->buffer_scale = 1;
  state->transform = Normal;

  /* Initialize the sentinel node.  */
  state->frame_callbacks.next = &state->frame_callbacks;
  state->frame_callbacks.last = &state->frame_callbacks;
  state->frame_callbacks.resource = NULL;

  /* Initialize the viewport to the default undefined values.  */
  state->dest_width = -1;
  state->dest_height = -1;
  state->src_x = -1.0;
  state->src_y = -1.0;
  state->src_width = -1.0;
  state->src_height = -1.0;
}

static void
FinalizeState (State *state)
{
  pixman_region32_fini (&state->damage);
  pixman_region32_fini (&state->opaque);
  pixman_region32_fini (&state->surface);
  pixman_region32_fini (&state->input);

  if (state->buffer)
    XLDereferenceBuffer (state->buffer);
  state->buffer = NULL;

  /* Destroy any callbacks that might be remaining.  */
  FreeFrameCallbacks (&state->frame_callbacks);
}

static void
NotifySubsurfaceDestroyed (void *data)
{
  Surface *surface;

  surface = data;

  /* If a surface is in the subsurfaces list, it must have a role.  */
  XLAssert (surface->role != NULL);
  XLSubsurfaceParentDestroyed (surface->role);
}

static void
HandleSurfaceDestroy (struct wl_resource *resource)
{
  Surface *surface;
  ClientData *data, *last;

  surface = wl_resource_get_user_data (resource);

  if (surface->role)
    XLSurfaceReleaseRole (surface, surface->role);

  /* Detach all subsurfaces from the parent.  This *must* be done
     after the role is torn down, because that is where the toplevel
     subcompositor is detached from the roles.  */
  XLListFree (surface->subsurfaces,
	      NotifySubsurfaceDestroyed);
  surface->subsurfaces = NULL;

  /* Keep surface->resource around until the role is released; some
     code (such as dnd.c) assumes that surface->resource will always
     be available in unmap callbacks.  */
  surface->resource = NULL;

  /* Then release all client data.  */
  data = surface->client_data;

  while (data)
    {
      if (data->free_function)
	/* Free the client data.  */
	data->free_function (data->data);

      XLFree (data->data);

      /* And its record.  */
      last = data;
      data = data->next;
      XLFree (last);
    }

  /* Release the output region.  */
  pixman_region32_fini (&surface->output_region);

  /* Next, free the views.  */
  ViewFree (surface->view);
  ViewFree (surface->under);

  /* Then, unlink the surface from the list of all surfaces.  */
  surface->next->last = surface->last;
  surface->last->next = surface->next;

  /* Free outputs.  */
  XLFree (surface->outputs);

  /* Free the window scaling factor callback.  */
  XLRemoveScaleChangeCallback (surface->scale_callback_key);

  /* If a release is attached, destroy it and its resource.  */
  if (surface->release)
    XLDestroyRelease (surface->release);

  /* Likewise if a fence is attached.  */
  if (surface->acquire_fence != -1)
    close (surface->acquire_fence);

  FinalizeState (&surface->pending_state);
  FinalizeState (&surface->current_state);
  FinalizeState (&surface->cached_state);
  FreeCommitCallbacks (&surface->commit_callbacks);
  FreeUnmapCallbacks (&surface->unmap_callbacks);
  FreeDestroyCallbacks (&surface->destroy_callbacks);
  XLFree (surface);
}

static void
MaybeResized (View *view)
{
  Surface *surface;

  surface = ViewGetData (view);

  /* The view may have been resized; recompute pointer confinement
     area if necessary.  */
  XLPointerConstraintsReconfineSurface (surface);
}

void
XLCreateSurface (struct wl_client *client,
		 struct wl_resource *resource,
		 uint32_t id)
{
  Surface *surface;

  surface = XLSafeMalloc (sizeof *surface);

  if (!surface)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (surface, 0, sizeof *surface);
  surface->resource
    = wl_resource_create (client, &wl_surface_interface,
			  wl_resource_get_version (resource),
			  id);

  if (!surface->resource)
    {
      wl_resource_post_no_memory (resource);
      XLFree (surface);
      return;
    }

  wl_resource_set_implementation (surface->resource, &wl_surface_impl,
				  surface, HandleSurfaceDestroy);

  surface->role = NULL;
  surface->view = MakeView ();
  surface->under = MakeView ();
  surface->subsurfaces = NULL;

  /* Make it so that seat.c can associate the surface with the
     view.  */
  ViewSetData (surface->view, surface);

  /* Make it so pointer confinement stuff can run after resize.  */
  ViewSetMaybeResizedFunction (surface->view, MaybeResized);

  /* Initialize the sentinel node for the commit callback list.  */
  surface->commit_callbacks.last = &surface->commit_callbacks;
  surface->commit_callbacks.next = &surface->commit_callbacks;
  surface->commit_callbacks.commit = NULL;
  surface->commit_callbacks.data = NULL;

  /* And the sentinel node for the unmap callback list.  */
  surface->unmap_callbacks.last = &surface->unmap_callbacks;
  surface->unmap_callbacks.next = &surface->unmap_callbacks;
  surface->unmap_callbacks.unmap = NULL;
  surface->unmap_callbacks.data = NULL;

  /* And the sentinel node for the destroy callback list.  */
  surface->destroy_callbacks.last = &surface->destroy_callbacks;
  surface->destroy_callbacks.next = &surface->destroy_callbacks;
  surface->destroy_callbacks.destroy_func = NULL;
  surface->destroy_callbacks.data = NULL;

  InitState (&surface->pending_state);
  InitState (&surface->current_state);
  InitState (&surface->cached_state);

  /* Apply the scale to initialize the default.  */
  ApplyScale (surface);

  /* Now the default input has been initialized, so apply it to the
     view.  */
  ApplyInputRegion (surface);

  /* Initially, allow surfaces to accept any kind of role.  */
  surface->role_type = AnythingType;

  /* Initialize the output region.  */
  pixman_region32_init (&surface->output_region);

  /* Link the surface onto the list of all surfaces.  */
  surface->next = all_surfaces.next;
  surface->last = &all_surfaces;
  all_surfaces.next->last = surface;
  all_surfaces.next = surface;

  /* Also add the scale change callback.  */
  surface->scale_callback_key
    = XLAddScaleChangeCallback (surface, HandleScaleChanged);

  /* Clear surface output coordinates.  */
  surface->output_x = INT_MIN;
  surface->output_y = INT_MIN;

  /* Set the acquire fence fd to -1.  */
  surface->acquire_fence = -1;
}

void
XLInitSurfaces (void)
{
  all_surfaces.next = &all_surfaces;
  all_surfaces.last = &all_surfaces;
}


/* Role management: XDG shells, wl_shells, et cetera.  */

Bool
XLSurfaceAttachRole (Surface *surface, Role *role)
{
  if (surface->role)
    return False;

  if (!role->funcs.setup (surface, role))
    return False;

  surface->role = role;

  return True;
}

void
XLSurfaceReleaseRole (Surface *surface, Role *role)
{
  role->funcs.teardown (surface, role);

  if (surface->resource)
    /* Now that the surface is unmapped, leave every output it
       previously entered.  */
    XLClearOutputs (surface);

  surface->role = NULL;
  surface->output_x = INT_MIN;
  surface->output_y = INT_MIN;
  RunUnmapCallbacks (surface);
}


/* Various other functions exported for roles.  */

void
XLSurfaceRunFrameCallbacks (Surface *surface, struct timespec time)
{
  uint64_t ms_time;
  XLList *list;

  /* If ms_time is too large to fit in uint32_t, take the lower 32
     bits.  */

  if (IntMultiplyWrapv (time.tv_sec, 1000, &ms_time))
    ms_time = UINT64_MAX;
  else if (IntAddWrapv (ms_time, time.tv_nsec / 1000000,
			&ms_time))
    ms_time = UINT64_MAX;

  RunFrameCallbacks (&surface->current_state.frame_callbacks,
		     ms_time);

  /* Run frame callbacks for each attached subsurface as well.  */
  for (list = surface->subsurfaces; list; list = list->next)
    XLSurfaceRunFrameCallbacks (list->data, time);
}

void
XLSurfaceRunFrameCallbacksMs (Surface *surface, uint32_t ms_time)
{
  XLList *list;

  RunFrameCallbacks (&surface->current_state.frame_callbacks,
		     ms_time);

  /* Run frame callbacks for each attached subsurface as well.  */
  for (list = surface->subsurfaces; list; list = list->next)
    XLSurfaceRunFrameCallbacksMs (list->data, ms_time);
}

CommitCallback *
XLSurfaceRunAtCommit (Surface *surface,
		      void (*commit_func) (Surface *, void *),
		      void *data)
{
  CommitCallback *callback;

  callback = AddCommitCallbackAfter (&surface->commit_callbacks);
  callback->commit = commit_func;
  callback->data = data;

  return callback;
}

void
XLSurfaceCancelCommitCallback (CommitCallback *callback)
{
  UnlinkCommitCallback (callback);

  XLFree (callback);
}

UnmapCallback *
XLSurfaceRunAtUnmap (Surface *surface,
		     void (*unmap_func) (void *),
		     void *data)
{
  UnmapCallback *callback;

  callback = AddUnmapCallbackAfter (&surface->unmap_callbacks);
  callback->unmap = unmap_func;
  callback->data = data;

  return callback;
}

void
XLSurfaceCancelUnmapCallback (UnmapCallback *callback)
{
  UnlinkUnmapCallback (callback);

  XLFree (callback);
}

void
XLCommitSurface (Surface *surface, Bool use_pending)
{
  InternalCommit (surface, (use_pending
			    ? &surface->pending_state
			    : &surface->cached_state));
}

DestroyCallback *
XLSurfaceRunOnFree (Surface *surface, void (*destroy_func) (void *),
		    void *data)
{
  DestroyCallback *callback;

  callback = AddDestroyCallbackAfter (&surface->destroy_callbacks);
  callback->destroy_func = destroy_func;
  callback->data = data;

  return callback;
}

void
XLSurfaceCancelRunOnFree (DestroyCallback *callback)
{
  UnlinkDestroyCallback (callback);

  XLFree (callback);
}

void *
XLSurfaceGetClientData (Surface *surface, ClientDataType type,
			size_t size, void (*free_func) (void *))
{
  ClientData *data;

  /* First, look for existing client data.  */
  for (data = surface->client_data; data; data = data->next)
    {
      if (data->type == type)
	return data->data;
    }

  /* Next, allocate some new client data.  */
  data = XLMalloc (sizeof *data);
  data->next = surface->client_data;
  surface->client_data = data;
  data->data = XLCalloc (1, size);
  data->free_function = free_func;
  data->type = type;

  return data->data;
}

void *
XLSurfaceFindClientData (Surface *surface, ClientDataType type)
{
  ClientData *data;

  for (data = surface->client_data; data; data = data->next)
    {
      if (data->type == type)
	return data->data;
    }

  return NULL;
}

Window
XLWindowFromSurface (Surface *surface)
{
  if (!surface->role
      || !surface->role->funcs.get_window)
    return None;

  return surface->role->funcs.get_window (surface,
					  surface->role);
}

Bool
XLSurfaceGetResizeDimensions (Surface *surface, int *width, int *height)
{
  if (!surface->role
      || !surface->role->funcs.get_resize_dimensions)
    return False;

  surface->role->funcs.get_resize_dimensions (surface, surface->role,
					      width, height);
  return True;
}

void
XLSurfacePostResize (Surface *surface, int west_motion, int north_motion,
		     int new_width, int new_height)
{
  if (!surface->role
      || !surface->role->funcs.post_resize)
    return;

  surface->role->funcs.post_resize (surface, surface->role,
				    west_motion, north_motion,
				    new_width, new_height);
  return;
}

void
XLSurfaceMoveBy (Surface *surface, int west, int north)
{
  if (!surface->role
      || !surface->role->funcs.move_by)
    return;

  surface->role->funcs.move_by (surface, surface->role,
				west, north);
}

void
XLSurfaceSelectExtraEvents (Surface *surface, unsigned long event_mask)
{
  if (!surface->role
      || !surface->role->funcs.select_extra_events)
    return;

  /* Note that this need only be implemented for surfaces that can get
     the input focus.  */
  surface->role->funcs.select_extra_events (surface, surface->role,
					    event_mask);
}

/* This function doesn't provide the seat that has now been focused
   in.  It is assumed that the role will perform some kind of
   reference counting in order to determine how many seats currently
   have it focused.  */

void
XLSurfaceNoteFocus (Surface *surface, FocusMode focus)
{
  if (!surface->role || !surface->role->funcs.note_focus)
    return;

  switch (focus)
    {
    case SurfaceFocusIn:
      surface->num_focused_seats++;

      /* Check for idle inhibition.  */
      XLIdleInhibitNoticeSurfaceFocused (surface);
      break;

    case SurfaceFocusOut:
      surface->num_focused_seats
	= MAX (0, surface->num_focused_seats - 1);

      if (!surface->num_focused_seats)
	/* Check if any idle inhibitors are still active.  */
	XLDetectSurfaceIdleInhibit ();
      break;
    }

  surface->role->funcs.note_focus (surface, surface->role,
				   focus);
}

/* Merge the cached state in surface into its current state in
   preparation for commit.  */

void
XLSurfaceMergeCachedState (Surface *surface)
{
  InternalCommit1 (surface, &surface->cached_state);
}



/* The following functions convert from window to surface
   coordinates and vice versa:

   SurfaceToWindow - take given surface coordinate, and return a
                     window relative coordinate.
   ScaleToWindow   - take given surface dimension, and return a
                     window relative dimension.
   WindowToSurface - take given window coordinate, and return a
                     surface relative coordinate as a double.
   ScaleToSurface  - take given window dimension, and return a
                     surface relative dimension.

   Functions prefixed by "truncate" return and accept integer values
   instead of floating point ones; truncation is performed on
   fractional values.  */

void
SurfaceToWindow (Surface *surface, double x, double y,
		   double *x_out, double *y_out)
{
  *x_out = x * surface->factor + surface->input_delta_x;
  *y_out = y * surface->factor + surface->input_delta_y;
}

void
ScaleToWindow (Surface *surface, double width, double height,
	       double *width_out, double *height_out)
{
  *width_out = width * surface->factor;
  *height_out = height * surface->factor;
}

void
WindowToSurface (Surface *surface, double x, double y,
		 double *x_out, double *y_out)
{
  *x_out = x / surface->factor - surface->input_delta_x;
  *y_out = y / surface->factor - surface->input_delta_y;
}

void
ScaleToSurface (Surface *surface, double width, double height,
		double *width_out, double *height_out)
{
  *width_out = width / surface->factor;
  *height_out = height / surface->factor;
}

void
TruncateSurfaceToWindow (Surface *surface, int x, int y,
			   int *x_out, int *y_out)
{
  *x_out = x * surface->factor + surface->input_delta_x;
  *y_out = y * surface->factor + surface->input_delta_y;
}

void
TruncateScaleToWindow (Surface *surface, int width, int height,
		       int *width_out, int *height_out)
{
  *width_out = width * surface->factor;
  *height_out = height * surface->factor;
}

void
TruncateWindowToSurface (Surface *surface, int x, int y,
			 int *x_out, int *y_out)
{
  *x_out = x / surface->factor - surface->input_delta_x;
  *y_out = y / surface->factor - surface->input_delta_y;
}

void
TruncateScaleToSurface (Surface *surface, int width, int height,
			int *width_out, int *height_out)
{
  *width_out = width / surface->factor;
  *height_out = height / surface->factor;
}
