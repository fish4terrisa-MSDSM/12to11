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
#include "pointer-gestures-unstable-v1.h"

/* The struct zwp_pointer_gestures_v1 global.  */
static struct wl_global *pointer_gestures_global;

static void
DestroySwipeGesture (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct zwp_pointer_gesture_swipe_v1_interface gesture_swipe_impl =
  {
    .destroy = DestroySwipeGesture,
  };

static void
HandleSwipeGestureResourceDestroy (struct wl_resource *resource)
{
  SwipeGesture *gesture;

  gesture = wl_resource_get_user_data (resource);
  XLSeatDestroySwipeGesture (gesture);
}



static void
DestroyPinchGesture (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct zwp_pointer_gesture_pinch_v1_interface gesture_pinch_impl =
  {
    .destroy = DestroyPinchGesture,
  };

static void
HandlePinchGestureResourceDestroy (struct wl_resource *resource)
{
  PinchGesture *gesture;

  gesture = wl_resource_get_user_data (resource);
  XLSeatDestroyPinchGesture (gesture);
}



static void
DestroyHoldGesture (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct zwp_pointer_gesture_hold_v1_interface gesture_hold_impl =
  {
    .destroy = DestroyHoldGesture,
  };



static void
GetSwipeGesture (struct wl_client *client, struct wl_resource *resource,
		 uint32_t id, struct wl_resource *pointer_resource)
{
  struct wl_resource *gesture_resource;
  Pointer *pointer;
  Seat *seat;
  SwipeGesture *gesture_swipe;

  pointer = wl_resource_get_user_data (pointer_resource);
  seat = XLPointerGetSeat (pointer);
  gesture_resource
    = wl_resource_create (client,
			  &zwp_pointer_gesture_swipe_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!gesture_resource)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  gesture_swipe = XLSeatGetSwipeGesture (seat, gesture_resource);
  wl_resource_set_implementation (gesture_resource, &gesture_swipe_impl,
				  gesture_swipe,
				  HandleSwipeGestureResourceDestroy);
}

static void
GetPinchGesture (struct wl_client *client, struct wl_resource *resource,
		 uint32_t id, struct wl_resource *pointer_resource)
{
  struct wl_resource *gesture_resource;
  Pointer *pointer;
  Seat *seat;
  PinchGesture *gesture_pinch;

  pointer = wl_resource_get_user_data (pointer_resource);
  seat = XLPointerGetSeat (pointer);
  gesture_resource
    = wl_resource_create (client,
			  &zwp_pointer_gesture_pinch_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!gesture_resource)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  gesture_pinch = XLSeatGetPinchGesture (seat, gesture_resource);
  wl_resource_set_implementation (gesture_resource, &gesture_pinch_impl,
				  gesture_pinch,
				  HandlePinchGestureResourceDestroy);
}

static void
Release (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
GetHoldGesture (struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *pointer_resource)
{
  struct wl_resource *gesture_resource;

  /* Hold gestures are not supported by the X Input extension, so a
     dummy resource is created that just does nothing.  */
  gesture_resource
    = wl_resource_create (client, &zwp_pointer_gesture_hold_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!gesture_resource)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  wl_resource_set_implementation (gesture_resource, &gesture_hold_impl,
				  NULL, NULL);
}

static struct zwp_pointer_gestures_v1_interface pointer_gestures_impl =
  {
    .get_swipe_gesture = GetSwipeGesture,
    .get_pinch_gesture = GetPinchGesture,
    .release = Release,
    .get_hold_gesture = GetHoldGesture,
  };

static void
HandleBind (struct wl_client *client, void *data, uint32_t version,
	    uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &zwp_pointer_gestures_v1_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &pointer_gestures_impl,
				  NULL, NULL);
}

void
XLInitPointerGestures (void)
{
  if (xi2_major > 2 || xi2_minor >= 4)
    /* Create the pointer gestures global.  */
    pointer_gestures_global
      = wl_global_create (compositor.wl_display,
			  &zwp_pointer_gestures_v1_interface,
			  3, NULL, HandleBind);

  /* Pointer gestures are not supported without XI 2.4 or later.  */
}
