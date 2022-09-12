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
#include "xdg-shell.h"

/* The xdg_wm_base global.  */
static struct wl_global *global_xdg_wm_base;

/* All xdg_wm_base resources.  */
static XLList *all_xdg_wm_bases;

static void
CreatePositioner (struct wl_client *client, struct wl_resource *resource,
		  uint32_t id)
{
  XLCreateXdgPositioner (client, resource, id);
}

static void
GetXdgSurface (struct wl_client *client, struct wl_resource *resource,
	       uint32_t id, struct wl_resource *surface_resource)
{
  XLGetXdgSurface (client, resource, id, surface_resource);
}

static void
Pong (struct wl_client *client, struct wl_resource *resource,
      uint32_t serial)
{
  /* TODO... */
}

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct xdg_wm_base_interface xdg_wm_base_impl =
  {
    .destroy = Destroy,
    .create_positioner = CreatePositioner,
    .get_xdg_surface = GetXdgSurface,
    .pong = Pong,
  };

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  all_xdg_wm_bases = XLListRemove (all_xdg_wm_bases, resource);
}

static void
HandleBind (struct wl_client *client, void *data,
	    uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &xdg_wm_base_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &xdg_wm_base_impl,
				  NULL, HandleResourceDestroy);
  all_xdg_wm_bases = XLListPrepend (all_xdg_wm_bases, resource);
}

void
XLInitXdgWM (void)
{
  global_xdg_wm_base
    = wl_global_create (compositor.wl_display,
			&xdg_wm_base_interface,
			5, NULL, HandleBind);
}
