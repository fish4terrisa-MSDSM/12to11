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
#include "xdg-shell.h"

/* The xdg_wm_base global.  */
static struct wl_global *global_xdg_wm_base;

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
  XdgWmBase *wm_base;
  XdgRoleList *role;

  /* Ping-pong implementation.  Every time a ping request is received
     from the window manager, it is linked onto the list of all such
     requests on the toplevel.  Then, ping is sent with a serial.
     Once the pong with the latest serial arrives from the client,
     pending requests are sent back to the window manager on all
     windows.  */
  wm_base = wl_resource_get_user_data (resource);

  if (serial == wm_base->last_ping)
    {
      /* Reply to the ping events sent to each surface created with
	 this wm_base.  */
      role = wm_base->list.next;
      while (role != &wm_base->list)
	{
	  XLXdgRoleReplyPing (role->role);
	  role = role->next;
	}
    }
}

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  XdgWmBase *wm_base;

  /* If there are still xdg_surfaces created by this xdg_wm_base
     resource, post an error.  */
  wm_base = wl_resource_get_user_data (resource);

  if (wm_base->list.next != &wm_base->list)
    wl_resource_post_error (resource, XDG_WM_BASE_ERROR_DEFUNCT_SURFACES,
			    "surfaces created by this xdg_wm_base still"
			    " exist, yet it is being destroyed");

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
  XdgWmBase *wm_base;
  XdgRoleList *role, *last;

  wm_base = wl_resource_get_user_data (resource);

  /* Detach each surface.  */
  role = wm_base->list.next;
  while (role != &wm_base->list)
    {
      last = role;
      role = role->next;

      /* Complete all ping events.  */
      XLXdgRoleReplyPing (last->role);

      /* Tell the surface to not bother unlinking itself.  */
      last->next = NULL;
      last->last = NULL;
      last->role = NULL;
    }

  XLFree (wm_base);
}

static void
HandleBind (struct wl_client *client, void *data,
	    uint32_t version, uint32_t id)
{
  XdgWmBase *wm_base;

  wm_base = XLSafeMalloc (sizeof *wm_base);

  if (!wm_base)
    {
      wl_client_post_no_memory (client);
      return;
    }

  memset (wm_base, 0, sizeof *wm_base);
  wm_base->resource
    = wl_resource_create (client, &xdg_wm_base_interface,
			  version, id);

  if (!wm_base->resource)
    {
      XLFree (wm_base);
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (wm_base->resource, &xdg_wm_base_impl,
				  wm_base, HandleResourceDestroy);
  wm_base->list.next = &wm_base->list;
  wm_base->list.last = &wm_base->list;
}

void
XLInitXdgWM (void)
{
  global_xdg_wm_base
    = wl_global_create (compositor.wl_display,
			&xdg_wm_base_interface,
			5, NULL, HandleBind);
}

void
XLXdgWmBaseSendPing (XdgWmBase *wm_base)
{
  xdg_wm_base_send_ping (wm_base->resource,
			 ++wm_base->last_ping);
}
