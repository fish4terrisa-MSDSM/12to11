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
  callback = surface->commit_callbacks.next;

  while (callback != &surface->commit_callbacks)
    {
      callback->commit (surface, callback->data);
      callback = callback->next;
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
ApplyScale (Surface *surface)
{
  int scale, effective;

  scale = surface->current_state.buffer_scale;
  effective = GetEffectiveScale (scale);

  ViewSetScale (surface->view, effective);
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
		     global_scale_factor, global_scale_factor);
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
		     global_scale_factor, global_scale_factor);
      ViewSetInput (surface->view, &temp);
      pixman_region32_fini (&temp);
    }
}

static void
HandleScaleChanged (void *data, int new_scale)
{
  Surface *surface;
  Subcompositor *subcompositor;

  surface = data;

  /* First, reapply various regions that depend on the surface
     scale.  */
  ApplyInputRegion (surface);
  ApplyOpaqueRegion (surface);
  ApplyScale (surface);

  /* Next, call any role-specific hooks.  */
  if (surface->role && surface->role->funcs.rescale)
    surface->role->funcs.rescale (surface, surface->role);

  /* Then, redisplay the view if a subcompositor is already
     attached.  */
  subcompositor = ViewGetSubcompositor (surface->view);

  if (subcompositor)
    {
      /* When updating stuff out-of-band, a subframe must be started
	 around the update.  */

      if (surface->role && surface->role->funcs.subframe
	  && surface->role->funcs.subframe (surface, surface->role))
	{
	  SubcompositorUpdate (subcompositor);

	  if (surface->role && surface->role->funcs.end_subframe)
	    surface->role->funcs.end_subframe (surface, surface->role);
	}
    }
}

static void
ApplyDamage (Surface *surface)
{
  pixman_region32_t temp;
  int scale;

  scale = GetEffectiveScale (surface->current_state.buffer_scale);

  /* N.B. that this must come after the scale is applied.  */

  if (scale)
    {
      pixman_region32_init (&temp);

      if (scale > 0)
	XLScaleRegion (&temp, &surface->current_state.damage,
		       1.0 / (scale + 1), 1.0 / (scale + 1));
      else
	XLScaleRegion (&temp, &surface->current_state.damage,
		       abs (scale) + 1, abs (scale) + 1);

      ViewDamage (surface->view, &temp);

      pixman_region32_fini (&temp);
    }
  else
    ViewDamage (surface->view,
		&surface->current_state.damage);
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
		     global_scale_factor, global_scale_factor);
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
InternalCommit (Surface *surface, State *pending)
{
  FrameCallback *start, *end;

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
	}
      else
	{
	  ClearBuffer (&surface->current_state);
	  ApplyBuffer (surface);
	  ClearBuffer (pending);
	}
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

  if (pending->pending & PendingBufferScale)
    {
      surface->current_state.buffer_scale = pending->buffer_scale;
      ApplyScale (surface);
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

static void
SetBufferTransform (struct wl_client *client, struct wl_resource *resource,
		    int32_t transform)
{
  if (transform != WL_OUTPUT_TRANSFORM_NORMAL)
    wl_resource_post_error (resource, WL_DISPLAY_ERROR_IMPLEMENTATION,
			    "this compositor does not support buffer transforms");
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

  /* Initialize the sentinel node.  */
  state->frame_callbacks.next = &state->frame_callbacks;
  state->frame_callbacks.last = &state->frame_callbacks;
  state->frame_callbacks.resource = NULL;
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

  if (surface->role)
    XLSubsurfaceParentDestroyed (surface->role);
}

static void
HandleSurfaceDestroy (struct wl_resource *resource)
{
  Surface *surface;
  int i;

  surface = wl_resource_get_user_data (resource);

  if (surface->role)
    XLSurfaceReleaseRole (surface, surface->role);

  /* Keep surface->resource around until the role is released; some
     code (such as dnd.c) assumes that surface->resource will always
     be available in unmap callbacks.  */
  surface->resource = NULL;

  /* First, free all subsurfaces.  */
  XLListFree (surface->subsurfaces,
	      NotifySubsurfaceDestroyed);

  /* Then release all client data.  */
  for (i = 0; i < MaxClientData; ++i)
    {
      if (surface->client_data[i])
	surface->free_client_data[i] (surface->client_data[i]);
      XLFree (surface->client_data[i]);

      surface->client_data[i] = NULL;
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

  /* Now the default input has been initialized, so apply it to the
     view.  */
  ApplyInputRegion (surface);

  /* Likewise for the scale.  */
  ApplyScale (surface);

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

void
XLStateAttachBuffer (State *state, ExtBuffer *buffer)
{
  AttachBuffer (state, buffer);
}

void
XLStateDetachBuffer (State *state)
{
  ClearBuffer (state);
}


/* Various other functions exported for roles.  */

void
XLSurfaceRunFrameCallbacks (Surface *surface, struct timespec time)
{
  uint32_t ms_time;
  XLList *list;

  /* I don't know what else is reasonable in case of overflow.  */

  if (IntMultiplyWrapv (time.tv_sec, 1000, &ms_time))
    ms_time = UINT32_MAX;
  else if (IntAddWrapv (ms_time, time.tv_nsec / 1000000,
			&ms_time))
    ms_time = UINT32_MAX;

  RunFrameCallbacks (&surface->current_state.frame_callbacks,
		     ms_time);

  /* Run frame callbacks for each attached subsurface as well.  */
  for (list = surface->subsurfaces; list; list = list->next)
    XLSurfaceRunFrameCallbacks (list->data, time);
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
  if (surface->client_data[type])
    return surface->client_data[type];

  surface->client_data[type] = XLCalloc (1, size);
  surface->free_client_data[type] = free_func;

  return surface->client_data[type];
}

Window
XLWindowFromSurface (Surface *surface)
{
  if (!surface->role
      || !surface->role->funcs.get_window (surface,
					   surface->role))
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
