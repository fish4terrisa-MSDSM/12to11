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
#include "relative-pointer-unstable-v1.h"

/* The zwp_relative_pointer_manager_v1 global.  */
static struct wl_global *relative_pointer_manager_global;

static void
DestroyRelativePointer (struct wl_client *client,
			struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct zwp_relative_pointer_v1_interface relative_pointer_impl =
  {
    .destroy = DestroyRelativePointer,
  };

static void
HandleRelativePointerResourceDestroy (struct wl_resource *resource)
{
  RelativePointer *relative_pointer;

  relative_pointer = wl_resource_get_user_data (resource);
  XLSeatDestroyRelativePointer (relative_pointer);
}



static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
GetRelativePointer (struct wl_client *client, struct wl_resource *resource,
		    uint32_t id, struct wl_resource *pointer_resource)
{
  struct wl_resource *relative_pointer_resource;
  Pointer *pointer;
  RelativePointer *relative_pointer;
  Seat *seat;

  pointer = wl_resource_get_user_data (pointer_resource);
  seat = XLPointerGetSeat (pointer);
  relative_pointer_resource
    = wl_resource_create (client, &zwp_relative_pointer_v1_interface,
			  wl_resource_get_version (pointer_resource),
			  id);

  if (!relative_pointer_resource)
    {
      wl_resource_post_no_memory (pointer_resource);
      return;
    }

  relative_pointer = XLSeatGetRelativePointer (seat,
					       relative_pointer_resource);
  wl_resource_set_implementation (relative_pointer_resource,
				  &relative_pointer_impl, relative_pointer,
				  HandleRelativePointerResourceDestroy);
}

static const struct zwp_relative_pointer_manager_v1_interface manager_impl =
  {
    .destroy = Destroy,
    .get_relative_pointer = GetRelativePointer,
  };

static void
HandleBind (struct wl_client *client, void *data,
	    uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client,
				 &zwp_relative_pointer_manager_v1_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &manager_impl, NULL, NULL);
}

void
XLInitRelativePointer (void)
{
  relative_pointer_manager_global
    = wl_global_create (compositor.wl_display,
			&zwp_relative_pointer_manager_v1_interface,
			1, NULL, HandleBind);
}

void
XLRelativePointerSendRelativeMotion (struct wl_resource *resource,
				     uint64_t microsecond_time, double dx,
				     double dy)
{
  uint32_t time_hi, time_lo;

  time_hi = microsecond_time >> 32;
  time_lo = microsecond_time >> 32;

  zwp_relative_pointer_v1_send_relative_motion (resource, time_hi, time_lo,
						wl_fixed_from_double (dx),
						wl_fixed_from_double (dy),
						wl_fixed_from_double (dx),
						wl_fixed_from_double (dy));
}
