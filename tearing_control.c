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

#include "compositor.h"
#include "tearing-control-v1.h"

typedef struct _TearingControl TearingControl;

struct _TearingControl
{
  /* The associated surface.  NULL when detached.  */
  Surface *surface;

  /* The associated resource.  */
  struct wl_resource *resource;
};

/* The tearing control manager.  */
static struct wl_global *tearing_control_manager_global;



static void
DestroyTearingControl (struct wl_client *client, struct wl_resource *resource)
{
  TearingControl *control;

  control = wl_resource_get_user_data (resource);

  if (control->surface)
    {
      /* Reset the presentation hint.  */
      control->surface->pending_state.presentation_hint
	= PresentationHintVsync;
      control->surface->pending_state.pending
	|= PendingPresentationHint;
    }

  wl_resource_destroy (resource);
}

static void
SetPresentationHint (struct wl_client *client, struct wl_resource *resource,
		     uint32_t hint)
{
  TearingControl *control;

  control = wl_resource_get_user_data (resource);

  if (control->surface)
    {
      switch (hint)
	{
	case WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC:
	  control->surface->pending_state.presentation_hint
	    = PresentationHintAsync;
	  break;

	default:
	  control->surface->pending_state.presentation_hint
	    = PresentationHintVsync;
	  break;
	}

      control->surface->pending_state.pending |= PendingPresentationHint;
    }
}

static const struct wp_tearing_control_v1_interface control_impl =
  {
    .destroy = DestroyTearingControl,
    .set_presentation_hint = SetPresentationHint,
  };

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  TearingControl *control, **reference;

  control = wl_resource_get_user_data (resource);

  /* If the surface is still attached to the tearing control, remove
     it from the surface.  */

  if (control->surface)
    {
      reference
	= XLSurfaceFindClientData (control->surface,
				   TearingControlData);
      XLAssert (reference != NULL);

      *reference = NULL;
    }

  XLFree (control);
}



static void
FreeTearingControlData (void *data)
{
  TearingControl **control;

  control = data;

  if (!*control)
    return;

  /* Detach the surface from the tearing control.  */
  (*control)->surface = NULL;
}

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
GetTearingControl (struct wl_client *client, struct wl_resource *resource,
		   uint32_t id, struct wl_resource *surface_resource)
{
  Surface *surface;
  TearingControl **control;

  surface = wl_resource_get_user_data (surface_resource);
  control = XLSurfaceGetClientData (surface, TearingControlData,
				    sizeof *control,
				    FreeTearingControlData);

#define ControlExists						\
  WP_TEARING_CONTROL_MANAGER_V1_ERROR_TEARING_CONTROL_EXISTS

  if (*control)
    {
      /* A tearing control resource already exists for this
	 surface.  */
      wl_resource_post_error (resource, ControlExists,
			      "a wp_tearing_control_v1 resource already exists"
			      " for the specified surface");
      return;
    }

#undef ControlExists

  (*control) = XLCalloc (1, sizeof **control);
  (*control)->resource
    = wl_resource_create (client,
			  &wp_tearing_control_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!(*control)->resource)
    {
      XLFree (*control);
      (*control) = NULL;

      wl_resource_post_no_memory (resource);
      return;
    }

  (*control)->surface = surface;
  wl_resource_set_implementation ((*control)->resource, &control_impl,
				  (*control), HandleResourceDestroy);
}

static const struct wp_tearing_control_manager_v1_interface manager_impl =
  {
    .destroy = Destroy,
    .get_tearing_control = GetTearingControl,
  };



static void
HandleBind (struct wl_client *client, void *data,
	    uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client,
				 &wp_tearing_control_manager_v1_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &manager_impl,
				  NULL, NULL);
}

void
XLInitTearingControl (void)
{
  tearing_control_manager_global
    = wl_global_create (compositor.wl_display,
			&wp_tearing_control_manager_v1_interface,
			1, NULL, HandleBind);
}
