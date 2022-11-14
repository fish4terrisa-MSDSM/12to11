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

#include <stdio.h>
#include <string.h>

#include "compositor.h"
#include "xdg-activation-v1.h"

typedef struct _XdgActivationToken XdgActivationToken;

struct _XdgActivationToken
{
  /* The wl_resource associated with this activation token.  */
  struct wl_resource *resource;

  /* The seat associated with this activation token.  */
  Seat *seat;

  /* The seat destroy callback.  */
  void *seat_destroy_callback;

  /* The serial associated with this activation token.  */
  uint32_t serial;
};

/* The xdg_activation_v1 global.  */
static struct wl_global *xdg_activation_global;



static void
HandleSeatDestroyed (void *data)
{
  XdgActivationToken *token;

  token = data;
  token->seat = NULL;
  token->serial = 0;
}

/* Compositor activation policy.  The protocol translator,
   notwithstanding the judgement of the window manager, allows any
   client whose token was created with (IOW, had at the time of
   Commit) at least the latest key or pointer serial on its seat to
   activate its toplevels.  The timestamp used to focus the toplevels
   is the activation token.  */

static void
SetSerial (struct wl_client *client, struct wl_resource *resource,
	   uint32_t serial, struct wl_resource *seat_resource)
{
  XdgActivationToken *token;
  Seat *seat;

  token = wl_resource_get_user_data (resource);

  /* If token is NULL, then the token has already been used.  Silently
     ignore this request.  */
  if (!token)
    return;

  /* First, clear the current seat.  */
  if (token->seat_destroy_callback)
    XLSeatCancelDestroyListener (token->seat_destroy_callback);
  token->seat = NULL;
  token->seat_destroy_callback = NULL;

  seat = wl_resource_get_user_data (seat_resource);

  if (XLSeatIsInert (seat))
    /* Just return if the seat is inert.  */
    return;

  /* Otherwise, set the seat and serial.  */
  token->seat = seat;
  token->serial = serial;
  token->seat_destroy_callback
    = XLSeatRunOnDestroy (seat, HandleSeatDestroyed, token);
}

static void
SetAppId (struct wl_client *client, struct wl_resource *resource,
	  const char *app_id)
{
  /* This information is not useful.  */
}

static void
SetSurface (struct wl_client *client, struct wl_resource *resource,
	    struct wl_resource *surface_resource)
{
  /* This information is not useful.  */
}

static void
Commit (struct wl_client *client, struct wl_resource *resource)
{
  XdgActivationToken *token;
  Timestamp last_user_time;
  char buffer[80];

  token = wl_resource_get_user_data (resource);

  if (!token)
    {
      wl_resource_post_error (resource,
			      XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED,
			      "the specified activation token has been passed"
			      " to a previous commit request and is no longer"
			      " valid");
      return;
    }

  wl_resource_set_user_data (resource, NULL);

  if (!token->seat || !XLSeatCheckActivationSerial (token->seat,
						    token->serial))
    {
      /* Send an invalid serial.  */
      xdg_activation_token_v1_send_done (token->resource,
					 "activation_rejected");
      goto finish;
    }

  /* Send the last user time as the activation token.  */
  last_user_time = XLSeatGetLastUserTime (token->seat);
  sprintf (buffer, "%"PRIu32".%"PRIu32".%d",
	   last_user_time.months,
	   last_user_time.milliseconds,
	   XLSeatGetPointerDevice (token->seat));
  xdg_activation_token_v1_send_done (token->resource, buffer);

  /* Free the token.  */
 finish:
  if (token->seat_destroy_callback)
    XLSeatCancelDestroyListener (token->seat_destroy_callback);

  XLFree (token);
}

static void
DestroyActivationToken (struct wl_client *client,
			struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct xdg_activation_token_v1_interface xdg_activation_token_impl =
  {
    .set_serial = SetSerial,
    .set_app_id = SetAppId,
    .set_surface = SetSurface,
    .commit = Commit,
    .destroy = DestroyActivationToken,
  };

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  XdgActivationToken *token;

  token = wl_resource_get_user_data (resource);

  if (!token)
    return;

  if (token->seat_destroy_callback)
    XLSeatCancelDestroyListener (token->seat_destroy_callback);

  XLFree (token);
}



static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
GetActivationToken (struct wl_client *client, struct wl_resource *resource,
		    uint32_t id)
{
  XdgActivationToken *token;

  token = XLSafeMalloc (sizeof *token);

  if (!token)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (token, 0, sizeof *token);
  token->resource
    = wl_resource_create (client, &xdg_activation_token_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!token->resource)
    {
      XLFree (token);
      wl_resource_post_no_memory (resource);
      return;
    }

  wl_resource_set_implementation (token->resource,
				  &xdg_activation_token_impl,
				  token, HandleResourceDestroy);
}

static void
Activate (struct wl_client *client, struct wl_resource *resource,
	  const char *token, struct wl_resource *surface_resource)
{
  Timestamp timestamp;
  Surface *surface;
  int deviceid;

  if (sscanf (token, "%"SCNu32".%"SCNu32".%d", &timestamp.months,
	      &timestamp.milliseconds, &deviceid) != 3)
    /* The activation token is invalid.  */
    return;

  /* Activate the surface with the given token.  */

  surface = wl_resource_get_user_data (surface_resource);

  if (surface->role->funcs.activate)
    surface->role->funcs.activate (surface, surface->role,
				   deviceid, timestamp);
}

static const struct xdg_activation_v1_interface xdg_activation_impl =
  {
    .destroy = Destroy,
    .get_activation_token = GetActivationToken,
    .activate = Activate,
  };

static void
HandleBind (struct wl_client *client, void *data, uint32_t version,
	    uint32_t id)
{
  struct wl_resource *resource;

  /* Create an xdg_activation_v1 resource.  This resource is then used
     to create activation tokens and activate surfaces.  */
  resource = wl_resource_create (client, &xdg_activation_v1_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &xdg_activation_impl,
				  NULL, NULL);
}



void
XLInitXdgActivation (void)
{
  xdg_activation_global
    = wl_global_create (compositor.wl_display,
			&xdg_activation_v1_interface,
			1, NULL, HandleBind);
}
