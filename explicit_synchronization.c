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

#include "linux-explicit-synchronization-unstable-v1.h"

struct _Synchronization
{
  /* The surface destroy listener.  */
  DestroyCallback *destroy_listener;

  /* The surface.  */
  Surface *surface;

  /* The file descriptor of any pending acquire fence.  */
  int acquire_fence;

  /* Any associated release object.  */
  SyncRelease *release;

  /* The associated resource.  */
  struct wl_resource *resource;
};

struct _SyncRelease
{
  /* The associated surface.  */
  Surface *surface;

  /* The associated synchronization.  */
  Synchronization *synchronization;

  /* The associated resource.  */
  struct wl_resource *resource;
};

/* The global zwp_linux_explicit_synchronization_v1 object.  */
static struct wl_global *explicit_sync_global;

static void
HandleReleaseDestroy (struct wl_resource *resource)
{
  SyncRelease *release;

  release = wl_resource_get_user_data (resource);

  /* If release is attached to a surface, remove it from the
     surface.  */

  if (release->surface)
    release->surface->release = NULL;

  /* Do the same for the synchronization object.  */

  if (release->synchronization)
    release->synchronization->release = NULL;

  /* Free the release object.  */
  XLFree (release);
}



static void
DestroySynchronization (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
SetAcquireFence (struct wl_client *client, struct wl_resource *resource,
		 int32_t fd)
{
  Synchronization *synchronization;

  synchronization = wl_resource_get_user_data (resource);

#define NoSurface ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_NO_SURFACE

  /* If the surface was destroyed, raise such an error.  */
  if (!synchronization->surface)
    {
      wl_resource_post_error (resource, NoSurface,
			      "the surface associated with this"
			      " resource was destroyed");
      goto error;
    }

#undef NoSurface

#define DuplicateFence						\
  ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_DUPLICATE_FENCE

  /* If a fence was already attached, raise that as an errore.  */
  if (synchronization->acquire_fence != -1)
    {
      wl_resource_post_error (resource, DuplicateFence,
			      "another fence has already been attached"
			      " during this commit cycle");
      goto error;
    }

#undef DuplicateFence

  /* Set the file descriptor and return.  */
  synchronization->acquire_fence = fd;
  return;

 error:
  close (fd);
}

static void
GetRelease (struct wl_client *client, struct wl_resource *resource,
	    uint32_t id)
{
  Synchronization *synchronization;
  SyncRelease *release;

  /* The release lifecycle is somewhat like this:

     First, it starts out as the `release' field of a Synchronization
     resource.  When the synchronization is committed, it is moved to
     the release field of the surface, and it is detatched from both
     once release events are sent.  */

  synchronization = wl_resource_get_user_data (resource);

#define DuplicateRelease						\
  ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_DUPLICATE_RELEASE

  if (synchronization->release)
    {
      /* Uncommitted release already exists.  Post an error.  */
      wl_resource_post_error (resource, DuplicateRelease,
			      "another release has already been acquired"
			      " during this commit cycle");
      return;
    }

#undef DuplicateRelease

  release = XLSafeMalloc (sizeof *release);

  if (!release)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (release, 0, sizeof *release);
  release->resource
    = wl_resource_create (client,
			  &zwp_linux_buffer_release_v1_interface,
			  wl_resource_get_version (resource),
			  id);

  if (!release->resource)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  /* Attach the release.  */
  release->synchronization = synchronization;
  synchronization->release = release;

  /* Set the resource implementation.  */
  wl_resource_set_implementation (release->resource,
				  NULL, release,
				  HandleReleaseDestroy);
}

static void
HandleSurfaceDestroy (void *data)
{
  Synchronization *synchronization;

  synchronization = data;

  /* The surface was destroyed.  Mark the object as invalid.  */
  synchronization->surface = NULL;
  synchronization->destroy_listener = NULL;
}

static void
HandleSurfaceCommit (Synchronization *synchronization, Surface *surface)
{
#define NoBuffer ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_NO_BUFFER

  if (synchronization->acquire_fence != -1)
    {
      if (!(surface->pending_state.pending & PendingBuffer
	    && surface->pending_state.buffer))
	{
	  /* If no buffer was attached, report an error.  */
	  wl_resource_post_error (synchronization->resource,
				  NoBuffer, "no buffer attached"
				  " but acquire fence provided");
	  close (synchronization->acquire_fence);
	  synchronization->acquire_fence = -1;

	  return;
	}

      if (surface->acquire_fence != -1)
	close (surface->acquire_fence);

      surface->acquire_fence = synchronization->acquire_fence;
      synchronization->acquire_fence = -1;
    }

  /* Move the release callback to the surface.  Assume one does
     not already exist.  */
  XLAssert (surface->release == NULL);

  /* surface->release can still end up NULL if no release was
     attached.  */
  surface->release = synchronization->release;

  if (surface->release)
    {
      /* Clear these fields, to detach the release from the
	 synchronization.  */
      surface->release->synchronization = NULL;
      synchronization->release = NULL;

      /* And set this field to attach the release to the
	 surface.  */
      surface->release->surface = surface;

      if (surface->release
	  && !(surface->pending_state.pending & PendingBuffer
	       && surface->pending_state.buffer))
	wl_resource_post_error (synchronization->resource,
				NoBuffer, "no buffer attached"
				" but release provided");
    }
#undef NoBuffer
}

static void
HandleSynchronizationDestroy (struct wl_resource *resource)
{
  Synchronization *synchronization;

  /* Free the synchronization as it has been destroyed.  */
  synchronization = wl_resource_get_user_data (resource);

  if (synchronization->destroy_listener)
    XLSurfaceCancelRunOnFree (synchronization->destroy_listener);

  if (synchronization->surface)
    /* Also detach it from the surface.  */
    synchronization->surface->synchronization = NULL;

  /* Detach any attached release callback.  */
  if (synchronization->release)
    wl_resource_destroy (synchronization->release->resource);

  /* If a fence happens to still be attached, close it.  */
  if (synchronization->acquire_fence != -1)
    close (synchronization->acquire_fence);

  XLFree (synchronization);
}

static struct zwp_linux_surface_synchronization_v1_interface synchronization_impl =
  {
    .destroy = DestroySynchronization,
    .set_acquire_fence = SetAcquireFence,
    .get_release = GetRelease,
  };



static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
GetSynchronization (struct wl_client *client, struct wl_resource *resource,
		    uint32_t id, struct wl_resource *surface_resource)
{
  Synchronization *synchronization;
  Surface *surface;

  surface = wl_resource_get_user_data (surface_resource);

#define SynchronizationAlreadyExists					\
  ZWP_LINUX_EXPLICIT_SYNCHRONIZATION_V1_ERROR_SYNCHRONIZATION_EXISTS

  if (surface->synchronization)
    {
      /* Don't let clients create more synchronization objects if one
	 already exists.  */
      wl_resource_post_error (resource, SynchronizationAlreadyExists,
			      "synchronization object already exists");
      return;
    }

#undef SynchronizationAlreadyExists

  /* Allocate the synchronization object.  */
  synchronization = XLSafeMalloc (sizeof *synchronization);

  if (!synchronization)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (synchronization, 0, sizeof *synchronization);
  synchronization->resource
    = wl_resource_create (client,
			  &zwp_linux_surface_synchronization_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!synchronization->resource)
    {
      XLFree (synchronization);
      wl_resource_post_no_memory (resource);
      return;
    }

  /* Now, attach the synchronization object to the surface.  */
  surface->synchronization = synchronization;

  /* And attach a destroy listener.  */
  synchronization->destroy_listener
    = XLSurfaceRunOnFree (surface, HandleSurfaceDestroy,
			  synchronization);

  /* And attach the surface to the synchronization.  */
  synchronization->surface = surface;

  /* Set the implementation.  */
  wl_resource_set_implementation (synchronization->resource,
				  &synchronization_impl,
				  synchronization,
				  HandleSynchronizationDestroy);

  /* Clear initial values.  */
  synchronization->acquire_fence = -1;
}

static struct zwp_linux_explicit_synchronization_v1_interface explicit_sync_impl =
  {
    .destroy = Destroy,
    .get_synchronization = GetSynchronization,
  };

static void
HandleBind (struct wl_client *client, void *data, uint32_t version,
	    uint32_t id)
{
  struct wl_resource *resource;

  resource
    = wl_resource_create (client,
			  &zwp_linux_explicit_synchronization_v1_interface,
			  version, id);
  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &explicit_sync_impl,
				  NULL, NULL);
}

void
XLDestroyRelease (SyncRelease *release)
{
  /* Destroying the resoruce will cause the release to be freed.  */
  wl_resource_destroy (release->resource);
}

void
XLSyncRelease (SyncRelease *release)
{
  Bool error;
  int fd;

  /* Optimization ideas.  Every time an shm buffer is attached with a
     release object, create new finish_fence for the buffer, that is
     signalled whenever the contents are uploaded.  Then, just use
     that fence here instead.  */

  error = False;
  fd = RenderGetFinishFence (&error);

  if (error)
#define InvalidFence ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_INVALID_FENCE
    wl_resource_post_error (release->resource, InvalidFence,
			    "server failed to create finish fence");
#undef InvalidFence
  else
    {
      /* Post a fenced release with the fence.  */
      zwp_linux_buffer_release_v1_send_fenced_release (release->resource,
						       fd);
      /* Close the file descriptor.  */
      close (fd);
    }

  /* Destroy the sync object.  */
  wl_resource_destroy (release->resource);
}

void
XLSyncCommit (Synchronization *synchronization)
{
  HandleSurfaceCommit (synchronization, synchronization->surface);
}

void
XLWaitFence (Surface *surface)
{
  RenderFence fence;
  Bool error;

  if (surface->acquire_fence == -1)
    return;

  error = False;

  /* This is expected to close the file descriptor.  */
  fence = RenderImportFdFence (surface->acquire_fence, &error);

  if (error)
    {
#define InvalidFence ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_INVALID_FENCE
      /* Importing the fence failed; signal an error to the
	 server.  */
      wl_resource_post_error (surface->resource,
			      InvalidFence, "the specified sync"
			      " fence could not be imported");
#undef InvalidFence
      /* Try closing the file descriptor as well.  */
      close (surface->acquire_fence);
      surface->acquire_fence = -1;

      return;
    }

  surface->acquire_fence = -1;

  /* Wait for the fence to be triggered.  */
  RenderWaitFence (fence);

  /* Delete the fence.  */
  RenderDeleteFence (fence);
}

void
XLInitExplicitSynchronization (void)
{
  /* If the renderer does not support explicit synchronization,
     return.  */
  if (!(renderer_flags & SupportsExplicitSync))
    return;

  explicit_sync_global
    = wl_global_create (compositor.wl_display,
			&zwp_linux_explicit_synchronization_v1_interface,
		        2, NULL, HandleBind);
}
