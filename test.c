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
#include "12to11-test.h"

#define TestSurfaceFromRole(role) ((TestSurface *) (role))

#define DefaultEventMask					\
  (ExposureMask | StructureNotifyMask | PropertyChangeMask)

enum
  {
    IsSurfaceMapped	 = 1,
    PendingBufferRelease = 1 << 1,
    PendingFrameCallback = 1 << 2,
  };

typedef struct _TestSurface TestSurface;

struct _TestSurface
{
  /* The associated role.  */
  Role role;

  /* The associated subcompositor.  */
  Subcompositor *subcompositor;

  /* The associated buffer release helper.  */
  BufferReleaseHelper *release_helper;

  /* The associated window.  */
  Window window;

  /* The associated rendering target.  */
  RenderTarget target;

  /* The number of references to this test surface, and flags.  */
  int refcount, flags;

  /* The last known width and height.  */
  int bounds_width, bounds_height;
};

/* The locked output scale.  N.B. that a test_scale_lock is not an
   actual resource, and just represents the state of this
   variable.  */
int locked_output_scale;

/* The test surface manager global.  */
static struct wl_global *test_manager_global;

/* Hash table of all surfaces.  */
static XLAssocTable *surfaces;

static void
DestroyBacking (TestSurface *test)
{
  if (--test->refcount)
    return;

  /* Release all allocated resources.  */
  RenderDestroyRenderTarget (test->target);
  XDestroyWindow (compositor.display, test->window);

  /* And the buffer release helper.  */
  FreeBufferReleaseHelper (test->release_helper);

  /* Delete the association.  */
  XLDeleteAssoc (surfaces, test->window);

  /* Free the subcompositor.  */
  SubcompositorFree (test->subcompositor);

  /* And since there are no C level references to the role anymore, it
     can be freed.  */
  XLFree (test);
}

static void
RunFrameCallbacks (TestSurface *test)
{
  struct timespec time;

  clock_gettime (CLOCK_MONOTONIC, &time);
  XLSurfaceRunFrameCallbacks (test->role.surface, time);

  test->flags &= ~PendingFrameCallback;
}

static void
RunFrameCallbacksConditionally (TestSurface *test)
{
  if (!test->role.surface)
    return;

  if (test->flags & PendingBufferRelease)
    /* Wait for all buffers to be released first.  */
    test->flags |= PendingFrameCallback;
  else
    RunFrameCallbacks (test);
}

static void
AllBuffersReleased (void *data)
{
  TestSurface *test;

  test = data;

  if (!test->role.surface)
    return;

  test->flags &= ~PendingBufferRelease;

  /* Run pending frame callbacks.  */
  if (test->flags & PendingFrameCallback)
    RunFrameCallbacks (test);
}

static void
NoteBounds (void *data, int min_x, int min_y, int max_x, int max_y)
{
  TestSurface *test;
  int bounds_width, bounds_height;

  test = data;

  /* Avoid resizing the window should its actual size not have
     changed.  */

  bounds_width = max_x - min_x + 1;
  bounds_height = max_y - min_y + 1;

  if (test->bounds_width != bounds_width
      || test->bounds_height != bounds_height)
    {
      /* Resize the window to fit.  */
      XResizeWindow (compositor.display, test->window,
		     bounds_width, bounds_height);
      /* Sync with the X server.  */
      XSync (compositor.display, False);

      test->bounds_width = bounds_width;
      test->bounds_height = bounds_height;
    }
}

static void
NoteFrame (FrameMode mode, uint64_t id, void *data)
{
  if (mode != ModeComplete && mode != ModePresented)
    return;

  /* Run the frame callbacks.  With the test surface, this also serves
     to mean that painting has completed.  */
  RunFrameCallbacksConditionally (data);
}

static void
MapTestSurface (TestSurface *test)
{
  /* Set the bounds width and height.  */
  test->bounds_width = SubcompositorWidth (test->subcompositor);
  test->bounds_height = SubcompositorHeight (test->subcompositor);

  /* First, resize the window to the current bounds.  */
  XResizeWindow (compositor.display, test->window,
		 test->bounds_width, test->bounds_height);

  /* Next, map the window and raise it.  Wait for a subsequent
     MapNotify before sending the map event.  */
  XMapRaised (compositor.display, test->window);

  /* And say that the window is now mapped.  */
  test->flags |= IsSurfaceMapped;
}

static void
UnmapTestSurface (TestSurface *test)
{
  if (test->flags & IsSurfaceMapped)
    /* Unmap the surface.  */
    XUnmapWindow (compositor.display, test->window);
}

static void
Commit (Surface *surface, Role *role)
{
  TestSurface *test;

  test = TestSurfaceFromRole (role);

  if (surface->current_state.buffer
      && !(test->flags & IsSurfaceMapped))
    /* Map the surface now.  */
    MapTestSurface (test);
  else if (!surface->current_state.buffer)
    {
      /* Unmap the surface now.  */
      UnmapTestSurface (test);

      /* Run frame callbacks if necessary.  */
      RunFrameCallbacksConditionally (test);
    }

  /* Finally, do a subcompositor update if the surface is now
     mapped.  */
  if (test->flags & IsSurfaceMapped)
    SubcompositorUpdate (test->subcompositor);
}

static Bool
Setup (Surface *surface, Role *role)
{
  TestSurface *test;

  test = TestSurfaceFromRole (role);

  /* Set role->surface here, since this is where the refcounting is
     done as well.  */
  role->surface = surface;

  /* Prevent the surface from ever holding another kind of role.  */
  surface->role_type = TestSurfaceType;

  /* Attach the views to the subcompositor.  */
  ViewSetSubcompositor (surface->view, test->subcompositor);
  ViewSetSubcompositor (surface->under, test->subcompositor);

  /* Make sure the under view ends up beneath surface->view.  */
  SubcompositorInsert (test->subcompositor, surface->under);
  SubcompositorInsert (test->subcompositor, surface->view);

  /* Retain the backing data.  */
  test->refcount++;
  return True;
}

static void
Teardown (Surface *surface, Role *role)
{
  TestSurface *test;

  /* Clear role->surface here, since this is where the refcounting is
     done as well.  */
  role->surface = NULL;

  test = TestSurfaceFromRole (role);

  /* Unparent the surface's views as well.  */
  ViewUnparent (surface->view);
  ViewUnparent (surface->under);

  /* Detach the surface's views from the subcompositor.  */
  ViewSetSubcompositor (surface->view, NULL);
  ViewSetSubcompositor (surface->under, NULL);

  /* Release the backing data.  */
  DestroyBacking (test);
}

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  TestSurface *test;

  test = wl_resource_get_user_data (resource);

  /* Now detach the role from its surface, which can be reused in the
     future.  */
  if (test->role.surface)
    XLSurfaceReleaseRole (test->role.surface, &test->role);

  /* And destroy the resource.  */
  wl_resource_destroy (resource);
}

static void
ReleaseBuffer (Surface *surface, Role *role, ExtBuffer *buffer)
{
  TestSurface *test;
  RenderBuffer render_buffer;

  test = TestSurfaceFromRole (role);
  render_buffer = XLRenderBufferFromBuffer (buffer);

  if (RenderIsBufferIdle (render_buffer, test->target))
    /* Release the buffer now -- it is already idle.  */
    XLReleaseBuffer (buffer);
  else
    {
      /* Release the buffer once it becomes idle, or is destroyed.  */
      ReleaseBufferWithHelper (test->release_helper, buffer,
			       test->target);

      /* Mark the surface as pending buffer release, so frame
	 callbacks can be deferred until all buffers are released.  */
      test->flags |= PendingBufferRelease;
    }
}

static void
SubsurfaceUpdate (Surface *surface, Role *role)
{
  TestSurface *test;

  test = TestSurfaceFromRole (role);
  SubcompositorUpdate (test->subcompositor);
}

static Window
GetWindow (Surface *surface, Role *role)
{
  TestSurface *test;

  test = TestSurfaceFromRole (role);
  return test->window;
}

static void
Activate (Surface *surface, Role *role, int deviceid,
	  Timestamp timestamp, Surface *activator_surface)
{
  struct wl_resource *resource;
  TestSurface *test;

  test = TestSurfaceFromRole (role);

  if (test->role.resource)
    {
      /* If the activator surface belongs to the same client as the
	 client who created the test surface, set the resource to the
	 activator surface.  */
      if (wl_resource_get_client (activator_surface->resource)
	  == wl_resource_get_client (test->role.resource))
	resource = activator_surface->resource;
      else
	resource = NULL;

      test_surface_send_activated (test->role.resource,
				   timestamp.months,
				   timestamp.milliseconds,
				   resource);
    }
}

static const struct test_surface_interface test_surface_impl =
  {
    .destroy = Destroy,
  };

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  TestSurface *test;

  test = wl_resource_get_user_data (resource);
  test->role.resource = NULL;

  /* Dereference the backing data.  */
  DestroyBacking (test);
}



static void
DestroyScaleLock (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
SetScale (struct wl_client *client, struct wl_resource *resource,
	  uint32_t scale)
{
  /* If the scale is invalid, reject it.  */
  if (!scale)
    {
      wl_resource_post_error (resource, TEST_MANAGER_ERROR_INVALID_SCALE,
			      "scale of 0 specified");
      return;
    }

  /* Set the scale.  As there can only be one lock at any given
     time, there is no need to check the resource data.  */
  locked_output_scale = scale;
  XLOutputHandleScaleChange (scale);
}

static const struct test_scale_lock_interface scale_lock_impl =
  {
    .destroy = DestroyScaleLock,
    .set_scale = SetScale,
  };

static void
HandleScaleLockResourceDestroy (struct wl_resource *resource)
{
  /* There is no resource data associated with scale locks.  Just
     unlock the scale.  */
  locked_output_scale = 0;
  XLOutputHandleScaleChange (-1);
}

static void
GetTestSurface (struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *surface_resource)
{
  Surface *surface;
  TestSurface *test;
  XSetWindowAttributes attrs;
  unsigned long flags;

  surface = wl_resource_get_user_data (surface_resource);

  if (surface->role_type != AnythingType
      && surface->role_type != TestSurfaceType)
    {
      /* The client is trying to create a test surface for a surface
	 that has or had some other role.  */
      wl_resource_post_error (resource, TEST_MANAGER_ERROR_ROLE_PRESENT,
			      "a role is/was already present on the given surface");
      return;
    }

  test = XLSafeMalloc (sizeof *test);

  if (!test)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (test, 0, sizeof *test);

  /* Now create the associated resource.  */
  test->role.resource
    = wl_resource_create (client, &test_surface_interface,
			  wl_resource_get_version (resource),
			  id);
  if (!test->role.resource)
    {
      wl_resource_post_no_memory (resource);
      XLFree (test);

      return;
    }

  /* Create the window.  */
  attrs.colormap = compositor.colormap;
  attrs.border_pixel = border_pixel;
  attrs.event_mask = DefaultEventMask;
  attrs.cursor = InitDefaultCursor ();
  attrs.override_redirect = True;
  flags = (CWColormap | CWBorderPixel | CWEventMask
	   | CWCursor | CWOverrideRedirect);

  test->window = XCreateWindow (compositor.display,
				DefaultRootWindow (compositor.display),
				0, 0, 20, 20, 0, compositor.n_planes,
				InputOutput, compositor.visual, flags,
				&attrs);

  /* And the subcompositor and rendering target.  */
  test->subcompositor = MakeSubcompositor ();
  test->target = RenderTargetFromWindow (test->window, DefaultEventMask);

  /* Set the client.  */
  RenderSetClient (test->target, client);

  /* And a buffer release helper.  */
  test->release_helper = MakeBufferReleaseHelper (AllBuffersReleased,
						  test);

  /* Set the subcompositor target.  */
  SubcompositorSetTarget (test->subcompositor, &test->target);

  /* Set some callbacks.  The note frame callback is not useful as
     test surfaces have no frame clock.  */
  SubcompositorSetBoundsCallback (test->subcompositor, NoteBounds, test);
  SubcompositorSetNoteFrameCallback (test->subcompositor, NoteFrame,
				     test);

  /* Create the hash table used to look up test surfaces if
     necessary.  */

  if (!surfaces)
    surfaces = XLCreateAssocTable (16);

  /* Associate the window with the role.  */
  XLMakeAssoc (surfaces, test->window, test);

  /* Set the role implementation.  */
  test->role.funcs.commit = Commit;
  test->role.funcs.teardown = Teardown;
  test->role.funcs.setup = Setup;
  test->role.funcs.release_buffer = ReleaseBuffer;
  test->role.funcs.subsurface_update = SubsurfaceUpdate;
  test->role.funcs.get_window = GetWindow;
  test->role.funcs.activate = Activate;

  /* Add the resource implementation.  */
  wl_resource_set_implementation (test->role.resource, &test_surface_impl,
				  test, HandleResourceDestroy);
  test->refcount++;

  /* Attach the role.  */
  if (!XLSurfaceAttachRole (surface, &test->role))
    abort ();
}

static void
GetScaleLock (struct wl_client *client, struct wl_resource *resource,
	      uint32_t id, uint32_t scale)
{
  struct wl_resource *lock_resource;

  if (!scale)
    {
      wl_resource_post_error (resource, TEST_MANAGER_ERROR_INVALID_SCALE,
			      "scale of 0 specified");
      return;
    }

  if (locked_output_scale)
    {
      /* The scale is already locked, so don't create another
	 lock.  */
      wl_resource_post_error (resource, TEST_MANAGER_ERROR_SCALE_LOCK_EXISTS,
			      "a scale lock already exists (another test is"
			      " already running?)");
      return;
    }

  lock_resource = wl_resource_create (client, &test_scale_lock_interface,
				      wl_resource_get_version (resource),
				      id);

  if (!lock_resource)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  /* Now, set the locked scale.  */
  locked_output_scale = scale;

  /* And update the global scale factor if need be.  */
  if (scale != global_scale_factor)
    XLOutputHandleScaleChange (scale);

  /* And resource implementation.  */
  wl_resource_set_implementation (lock_resource, &scale_lock_impl,
				  NULL, HandleScaleLockResourceDestroy);
}

static void
GetTestSeat (struct wl_client *client, struct wl_resource *resource,
	     uint32_t id)
{
  XLGetTestSeat (client, resource, id);
}

static void
GetSerial (struct wl_client *client, struct wl_resource *resource)
{
  uint32_t serial;

  /* Send the display's next serial to the client.  */
  serial = wl_display_next_serial (compositor.wl_display);
  test_manager_send_serial (resource, serial);
}

static void
SetBufferLabel (struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *buffer_resource, const char *label)
{
  ExtBuffer *buffer;

  buffer = wl_resource_get_user_data (buffer_resource);

  XLFree (buffer->label);
  buffer->label = XLStrdup (label);
}

static const struct test_manager_interface test_manager_impl =
  {
    .get_test_surface = GetTestSurface,
    .get_scale_lock = GetScaleLock,
    .get_test_seat = GetTestSeat,
    .get_serial = GetSerial,
    .set_buffer_label = SetBufferLabel,
  };



static void
HandleBind (struct wl_client *client, void *data, uint32_t version,
	    uint32_t id)
{
  struct wl_resource *resource;
  char *name;

  resource = wl_resource_create (client, &test_manager_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &test_manager_impl,
				  NULL, NULL);

  /* Send the display name to the client.  */
  name = DisplayString (compositor.display);
  test_manager_send_display_string (resource, name);
}

void
XLInitTest (void)
{
  test_manager_global
    = wl_global_create (compositor.wl_display, &test_manager_interface,
			1, NULL, HandleBind);
}

static Bool
DispatchMapNotify (XEvent *event)
{
  TestSurface *test;

  /* Try to look up the surface.  */
  test = XLLookUpAssoc (surfaces, event->xmap.window);

  if (!test)
    return False;

  /* The surface is now mapped.  Dispatch the mapped event.  */
  if (test->flags & IsSurfaceMapped && test->role.resource)
    test_surface_send_mapped (test->role.resource, test->window,
			      DisplayString (compositor.display));
  return True;
}

static Bool
DispatchExpose (XEvent *event)
{
  TestSurface *test;

  /* Try to look up the surface.  */
  test = XLLookUpAssoc (surfaces, event->xexpose.window);

  if (!test)
    return False;

  /* Expose the subcompositor.  */
  SubcompositorExpose (test->subcompositor, event);

  return True;
}

Bool
XLHandleOneXEventForTest (XEvent *event)
{
  if (!surfaces)
    return False;

  switch (event->type)
    {
    case MapNotify:
      return DispatchMapNotify (event);

    case Expose:
      return DispatchExpose (event);
    }

  return False;
}

Surface *
XLLookUpTestSurface (Window window, Subcompositor **subcompositor)
{
  TestSurface *test;

  if (!surfaces)
    return NULL;

  test = XLLookUpAssoc (surfaces, window);

  if (!test)
    return NULL;

  *subcompositor = test->subcompositor;
  return test->role.surface;
}
