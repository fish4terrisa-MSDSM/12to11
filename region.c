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

#include <pixman.h>

#include "compositor.h"

static void
DestroyRegion (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
SubtractRegion (struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height)
{
  pixman_region32_t *region, operand;

  region = wl_resource_get_user_data (resource);
  pixman_region32_init_rect (&operand, x, y, width, height);
  pixman_region32_subtract (region, region, &operand);
}

static void
AddRegion (struct wl_client *client, struct wl_resource *resource,
	   int32_t x, int32_t y, int32_t width, int32_t height)
{
  pixman_region32_t *region, operand;

  region = wl_resource_get_user_data (resource);
  pixman_region32_init_rect (&operand, x, y, width, height);
  pixman_region32_union (region, region, &operand);
}

static const struct wl_region_interface wl_region_impl =
  {
    .destroy = DestroyRegion,
    .subtract = SubtractRegion,
    .add = AddRegion
  };

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  pixman_region32_t *region;

  region = wl_resource_get_user_data (resource);
  pixman_region32_fini (region);
  XLFree (region);
}

void
XLCreateRegion (struct wl_client *client,
		struct wl_resource *resource,
		uint32_t id)
{
  pixman_region32_t *region;
  struct wl_resource *region_resource;

  region = XLSafeMalloc (sizeof *region);

  if (!region)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  region_resource
    = wl_resource_create (client, &wl_region_interface,
			  wl_resource_get_version (resource),
			  id);

  if (!resource)
    {
      wl_resource_post_no_memory (resource);
      XLFree (region);

      return;
    }

  pixman_region32_init (region);
  wl_resource_set_implementation (region_resource,
				  &wl_region_impl,
				  region,
				  HandleResourceDestroy);
}
