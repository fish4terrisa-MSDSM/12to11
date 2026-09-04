/* Wayland compositor running on top of an X server.

Copyright (C) 2026 fish4terrisa-MSDSM <flyingfish.msdsm@gmail.com>

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
#include "cursor-shape-v1.h"

/* Dummy interface declaration to satisfy the linker since the tablet
   protocol is not implemented or compiled in 12to11. */
const struct wl_interface zwp_tablet_tool_v2_interface = {
  "zwp_tablet_tool_v2", 2, 0, NULL, 0, NULL
};

static void
device_destroy(struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy(resource);
}

static void
device_set_shape(struct wl_client *client, struct wl_resource *resource, uint32_t serial, uint32_t shape)
{
  Pointer *pointer = wl_resource_get_user_data(resource);
  if (!pointer) return;
  
  XLPointerSetCursorShape(pointer, serial, shape);
}

static const struct wp_cursor_shape_device_v1_interface device_impl = {
  .destroy = device_destroy,
  .set_shape = device_set_shape,
};

static void
manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy(resource);
}

static void
manager_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *pointer_resource)
{
  Pointer *pointer = wl_resource_get_user_data(pointer_resource);
  struct wl_resource *device_res = wl_resource_create(client, &wp_cursor_shape_device_v1_interface, wl_resource_get_version(resource), id);
  if (!device_res) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(device_res, &device_impl, pointer, NULL);
}

static void
manager_get_tablet_tool(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *tablet_tool_resource)
{
  /* 12to11 does not currently support tablet tools, create inert resource */
  struct wl_resource *device_res = wl_resource_create(client, &wp_cursor_shape_device_v1_interface, wl_resource_get_version(resource), id);
  if (!device_res) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(device_res, &device_impl, NULL, NULL);
}

static const struct wp_cursor_shape_manager_v1_interface manager_impl = {
  .destroy = manager_destroy,
  .get_pointer = manager_get_pointer,
  .get_tablet_tool_v2 = manager_get_tablet_tool,
};

static void
bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  struct wl_resource *resource = wl_resource_create(client, &wp_cursor_shape_manager_v1_interface, version, id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

void
XLInitCursorShape(void)
{
  wl_global_create(compositor.wl_display, &wp_cursor_shape_manager_v1_interface, 1, NULL, bind_manager);
}
