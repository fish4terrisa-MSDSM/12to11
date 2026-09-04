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
#include "xdg-decoration-unstable-v1.h"

/* The xdg_decoration_manager_v1 global.  */
static struct wl_global *decoration_manager_global;

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
GetToplevelDecoration (struct wl_client *client, struct wl_resource *resource,
		       uint32_t id, struct wl_resource *toplevel_resource)
{
  XdgRoleImplementation *impl;

  impl = wl_resource_get_user_data (toplevel_resource);
  XLXdgToplevelGetDecoration (impl, resource, id);
}

static const struct zxdg_decoration_manager_v1_interface manager_impl =
  {
    .destroy = Destroy,
    .get_toplevel_decoration = GetToplevelDecoration,
  };

static void
HandleBind (struct wl_client *client, void *data, uint32_t version,
	    uint32_t id)
{
  struct wl_resource *resource;

  resource
    = wl_resource_create (client,
			  &zxdg_decoration_manager_v1_interface,
			  version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &manager_impl, NULL, NULL);
}

void
XLInitDecoration (void)
{
  decoration_manager_global
    = wl_global_create (compositor.wl_display,
			&zxdg_decoration_manager_v1_interface,
			1, NULL, HandleBind);
}
