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
#include "viewporter.h"

typedef struct _ViewportExt ViewportExt;

struct _ViewportExt
{
  /* The attached surface.  */
  Surface *surface;

  /* The attached callback.  */
  DestroyCallback *destroy_callback;

  /* The associated wl_resource.  */
  struct wl_resource *resource;
};

/* The wp_viewporter global.  */
static struct wl_global *viewporter_global;

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  ViewportExt *ext;

  ext = wl_resource_get_user_data (resource);

  /* Free the user data and detach the surface destroy callback.  */

  if (ext->surface)
    {
      XLSurfaceCancelRunOnFree (ext->destroy_callback);
      ext->surface->viewport = NULL;

      /* Clear viewport data.  */
      ext->surface->pending_state.pending |= PendingViewportSrc;
      ext->surface->pending_state.pending |= PendingViewportDest;
      ext->surface->pending_state.src_x = -1.0;
      ext->surface->pending_state.src_y = -1.0;
      ext->surface->pending_state.src_width = -1.0;
      ext->surface->pending_state.src_height = -1.0;
      ext->surface->pending_state.dest_width = -1;
      ext->surface->pending_state.dest_height = -1;
    }

  XLFree (ext);
}

static void
HandleSurfaceDestroy (void *data)
{
  ViewportExt *ext;

  ext = data;

  /* Clear callbacks and surface, as they are now gone.  */
  ext->surface = NULL;
  ext->destroy_callback = NULL;
}

static void
DestroyViewport (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
SetSource (struct wl_client *client, struct wl_resource *resource,
	   wl_fixed_t x, wl_fixed_t y, wl_fixed_t width, wl_fixed_t height)
{
  ViewportExt *ext;
  double src_x, src_y, src_width, src_height;

  ext = wl_resource_get_user_data (resource);

  /* Check that the surface is still attached.  */
  if (!ext->surface)
    {
      wl_resource_post_error (resource, WP_VIEWPORT_ERROR_NO_SURFACE,
			      "the surface has been detached");
      return;
    }

  src_x = wl_fixed_to_double (x);
  src_y = wl_fixed_to_double (y);
  src_width = wl_fixed_to_double (width);
  src_height = wl_fixed_to_double (height);

  /* Now, verify that the values are correct.  They can either all be
     -1, or the origin must be positive, and width and height must be
     more than 0.  */
  if (!(src_x == -1.0 && src_y == -1.0 && src_width == -1.0
	&& src_height == -1.0)
      && (src_x < 0 || src_y < 0
	  || src_width < 1.0 || src_height < 1.0))
    wl_resource_post_error (resource, WP_VIEWPORT_ERROR_BAD_VALUE,
			    "invalid source rectangle specified");

  if (ext->surface->current_state.src_x == src_x
      && ext->surface->current_state.src_y == src_y
      && ext->surface->current_state.src_width == src_width
      && ext->surface->current_state.src_height == src_height)
    /* No change happened.  */
    return;

  ext->surface->pending_state.pending |= PendingViewportSrc;
  ext->surface->pending_state.src_x = src_x;
  ext->surface->pending_state.src_y = src_y;
  ext->surface->pending_state.src_width = src_width;
  ext->surface->pending_state.src_height = src_height;
}

static void
SetDestination (struct wl_client *client, struct wl_resource *resource,
		int32_t width, int32_t height)
{
  ViewportExt *ext;

  ext = wl_resource_get_user_data (resource);

  /* Check that the surface is still attached.  */
  if (!ext->surface)
    {
      wl_resource_post_error (resource, WP_VIEWPORT_ERROR_NO_SURFACE,
			      "the surface has been detached");
      return;
    }

  if ((width <= 0 || height <= 0)
      && !(width == -1 && height == -1))
    {
      wl_resource_post_error (resource, WP_VIEWPORT_ERROR_BAD_VALUE,
			      "invalid destination size specified");
      return;
    }

  if (ext->surface->current_state.dest_width == width
      && ext->surface->current_state.dest_height == height)
    /* No change happened.  */
    return;

  ext->surface->pending_state.pending |= PendingViewportDest;
  ext->surface->pending_state.dest_width = width;
  ext->surface->pending_state.dest_height = height;
}

static const struct wp_viewport_interface wp_viewport_impl =
  {
    .destroy = DestroyViewport,
    .set_source = SetSource,
    .set_destination = SetDestination,
  };



static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
GetViewport (struct wl_client *client, struct wl_resource *resource,
	     uint32_t id, struct wl_resource *surface_resource)
{
  ViewportExt *ext;
  Surface *surface;

  surface = wl_resource_get_user_data (surface_resource);

  /* If the surface already has a viewport resource attached, post an
     error.  */
  if (surface->viewport)
    {
      wl_resource_post_error (resource, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
			      "viewport already exists");
      return;
    }

  ext = XLSafeMalloc (sizeof *ext);

  if (!ext)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (ext, 0, sizeof *ext);
  ext->resource
    = wl_resource_create (client, &wp_viewport_interface,
			  wl_resource_get_version (resource), id);

  if (!ext->resource)
    {
      XLFree (ext);
      wl_resource_post_no_memory (resource);
      return;
    }

  /* Attach the surface.  */
  ext->surface = wl_resource_get_user_data (surface_resource);
  ext->destroy_callback
    = XLSurfaceRunOnFree (ext->surface, HandleSurfaceDestroy,
			  ext);
  surface->viewport = ext;

  wl_resource_set_implementation (ext->resource, &wp_viewport_impl,
				  ext, HandleResourceDestroy);
}

static const struct wp_viewporter_interface wp_viewporter_impl =
  {
    .destroy = Destroy,
    .get_viewport = GetViewport,
  };



static void
HandleBind (struct wl_client *client, void *data, uint32_t version,
	    uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wp_viewporter_interface,
				 version, id);
  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &wp_viewporter_impl, NULL,
				  NULL);
}

void
XLInitWpViewporter (void)
{
  viewporter_global = wl_global_create (compositor.wl_display,
					&wp_viewporter_interface,
					1, NULL, HandleBind);
}

void
XLWpViewportReportBadSize (ViewportExt *ext)
{
  wl_resource_post_error (ext->resource, WP_VIEWPORT_ERROR_BAD_SIZE,
			  "invalid non-integer size specified");
}

void
XLWpViewportReportOutOfBuffer (ViewportExt *ext)
{
  wl_resource_post_error (ext->resource, WP_VIEWPORT_ERROR_OUT_OF_BUFFER,
			  "viewport source rectangle out of buffer");
}
