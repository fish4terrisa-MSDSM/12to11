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

#include "compositor.h"

/* The compositor global.  */
static struct wl_global *global_compositor;

static void
CreateSurface (struct wl_client *client,
	       struct wl_resource *resource,
	       uint32_t id)
{
  XLCreateSurface (client, resource, id);
}

static void
CreateRegion (struct wl_client *client,
	      struct wl_resource *resource,
	      uint32_t id)
{
  XLCreateRegion (client, resource, id);
}

static const struct wl_compositor_interface wl_compositor_impl =
  {
    .create_surface = CreateSurface,
    .create_region = CreateRegion,
  };

static void
HandleBind (struct wl_client *client, void *data,
	    uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_compositor_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &wl_compositor_impl,
				  NULL, NULL);
}

void
XLInitCompositor (void)
{
  global_compositor
    = wl_global_create (compositor.wl_display,
			&wl_compositor_interface,
			5, NULL, HandleBind);
}
