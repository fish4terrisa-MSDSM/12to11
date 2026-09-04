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
#include "keyboard-shortcuts-inhibit-unstable-v1.h"

typedef struct _ShortcutInhibitDataRecord ShortcutInhibitDataRecord;
typedef struct _KeyboardShortcutInhibitor KeyboardShortcutInhibitor;

enum
  {
    IsGrabbed = 1,
  };

struct _KeyboardShortcutInhibitor
{
  /* The surface to which the inhibitor applies.  */
  Surface *surface;

  /* The associated struct wl_resource.  */
  struct wl_resource *resource;

  /* The next and last shortcut inhibitors in this list.  Not valid if
     surface is NULL.  */
  KeyboardShortcutInhibitor *next, *last;

  /* The seat.  */
  Seat *seat;

  /* The seat destruction key.  */
  void *seat_key;

  /* Some flags.  */
  int flags;
};

struct _ShortcutInhibitDataRecord
{
  /* List of all keyboard shortcut inhibitors.  */
  KeyboardShortcutInhibitor inhibitors;
};

/* The zwp_keyboard_shortcuts_inhibit_manager_v1 global.  */
struct wl_global *inhibit_manager_global;

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
FreeShortcutInhibitData (void *data)
{
  ShortcutInhibitDataRecord *record;
  KeyboardShortcutInhibitor *inhibitor;

  record = data;

  /* Clear the surface of every attached keyboard shortcut
     inhibitor.  */
  inhibitor = record->inhibitors.next;
  XLAssert (inhibitor != NULL);

  while (inhibitor != &record->inhibitors)
    {
      inhibitor->surface = NULL;

      /* Move to the next inhibitor.  */
      inhibitor = inhibitor->next;
    }
}

static void
InitShortcutInhibitData (ShortcutInhibitDataRecord *data)
{
  /* If data is already initialized, do nothing.  */
  if (data->inhibitors.next)
    return;

  /* Otherwise, initialize the list of inhibitors.  */
  data->inhibitors.next = &data->inhibitors;
  data->inhibitors.last = &data->inhibitors;
}

static KeyboardShortcutInhibitor *
FindKeyboardShortcutInhibitor (Surface *surface, Seat *seat)
{
  ShortcutInhibitDataRecord *data;
  KeyboardShortcutInhibitor *inhibitor;

  data = XLSurfaceFindClientData (surface, ShortcutInhibitData);

  if (!data)
    return NULL;

  inhibitor = data->inhibitors.next;
  while (inhibitor != &data->inhibitors)
    {
      if (inhibitor->seat == seat)
	return inhibitor;

      inhibitor = data->inhibitors.next;
    }

  /* There is no inhibitor for this seat on the given surface.  */
  return NULL;
}

static void
DestroyKeyboardShortcutsInhibitor (struct wl_client *client,
				   struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct zwp_keyboard_shortcuts_inhibitor_v1_interface inhibitor_impl =
  {
    .destroy = DestroyKeyboardShortcutsInhibitor,
  };

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  KeyboardShortcutInhibitor *inhibitor;

  inhibitor = wl_resource_get_user_data (resource);

  if (inhibitor->surface)
    {
      /* Unlink the inhibitor from its surroundings.  */
      inhibitor->next->last = inhibitor->last;
      inhibitor->last->next = inhibitor->next;
    }

  if (inhibitor->seat)
    {
      /* Cancel the seat destruction callback.  */
      XLSeatCancelDestroyListener (inhibitor->seat_key);

      /* Ungrab the keyboard if it is grabbed.  */
      if (inhibitor->flags & IsGrabbed)
        XLSeatCancelExternalGrab (inhibitor->seat);
    }

  /* Free the inhibitor.  */
  XLFree (inhibitor);
}

static void
HandleSeatDestroy (void *data)
{
  KeyboardShortcutInhibitor *inhibitor;

  inhibitor = data;

  /* The seat was destroyed.  Unlink the inhibitor, then remove the
     seat.  */
  if (inhibitor->surface)
    {
      /* Unlink the inhibitor from its surroundings.  */
      inhibitor->next->last = inhibitor->last;
      inhibitor->last->next = inhibitor->next;
    }

  /* Clear the seat.  */
  inhibitor->seat = NULL;
  inhibitor->seat_key = NULL;
}



static void
InhibitShortcuts (struct wl_client *client, struct wl_resource *resource,
		  uint32_t id, struct wl_resource *surface_resource,
		  struct wl_resource *seat_resource)
{
  ShortcutInhibitDataRecord *record;
  Surface *surface;
  Seat *seat;
  KeyboardShortcutInhibitor *inhibitor;
  struct wl_resource *dummy_resource;

  surface = wl_resource_get_user_data (surface_resource);
  seat = wl_resource_get_user_data (seat_resource);

  /* If the seat is inert, return an empty inhibitor.  */
  if (XLSeatIsInert (seat))
    {
      dummy_resource
	= wl_resource_create (client,
			      &zwp_keyboard_shortcuts_inhibitor_v1_interface,
			      wl_resource_get_version (resource), id);

      if (!dummy_resource)
	wl_resource_post_no_memory (resource);
      else
	wl_resource_set_implementation (dummy_resource, &inhibitor_impl,
					NULL, NULL);

      return;
    }

  /* Check that there is no keyboard shortcut inhibitor already
     present.  */

#define AlreadyInhibited				\
  ZWP_KEYBOARD_SHORTCUTS_INHIBIT_MANAGER_V1_ERROR_ALREADY_INHIBITED

  if (FindKeyboardShortcutInhibitor (surface, seat))
    {
      wl_resource_post_error (resource, AlreadyInhibited,
			      "inhibitor already attached to surface and seat");
      return;
    }

#undef AlreadyInhibited

  record = XLSurfaceGetClientData (surface, ShortcutInhibitData,
				   sizeof *record,
				   FreeShortcutInhibitData);
  InitShortcutInhibitData (record);

  /* Allocate a new keyboard shortcut inhibitor.  */
  inhibitor = XLSafeMalloc (sizeof *inhibitor);

  if (!inhibitor)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (inhibitor, 0, sizeof *inhibitor);
  inhibitor->resource
    = wl_resource_create (client,
			  &zwp_keyboard_shortcuts_inhibitor_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!inhibitor->resource)
    {
      wl_resource_post_no_memory (resource);
      XLFree (inhibitor);
      return;
    }

  /* Link the inhibitor onto the list.  */
  inhibitor->next = record->inhibitors.next;
  inhibitor->last = &record->inhibitors;
  record->inhibitors.next->last = inhibitor;
  record->inhibitors.next = inhibitor;

  /* Attach the surface.  */
  inhibitor->surface = surface;

  /* And the seat.  */
  inhibitor->seat = seat;
  inhibitor->seat_key
    = XLSeatRunOnDestroy (seat, HandleSeatDestroy, inhibitor);

  /* Attach the resource implementation.  */
  wl_resource_set_implementation (inhibitor->resource, &inhibitor_impl,
				  inhibitor, HandleResourceDestroy);

  /* If the given surface is the seat's focus, try to apply the grab
     now.  */
  if (surface == XLSeatGetFocus (seat)
      && XLSeatApplyExternalGrab (seat, surface))
    {
      /* The external grab is now active, so send the active
	 signal.  */
      zwp_keyboard_shortcuts_inhibitor_v1_send_active (inhibitor->resource);

      /* Mark the inhibitor as active.  */
      inhibitor->flags |= IsGrabbed;
    }
}

static struct zwp_keyboard_shortcuts_inhibit_manager_v1_interface manager_impl =
  {
    .inhibit_shortcuts = InhibitShortcuts,
    .destroy = Destroy,
  };

static void
HandleBind (struct wl_client *client, void *data, uint32_t version,
	    uint32_t id)
{
  struct wl_resource *resource;

  resource
    = wl_resource_create (client,
			  &zwp_keyboard_shortcuts_inhibit_manager_v1_interface,
			  version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &manager_impl, NULL, NULL);
}

void
XLInitKeyboardShortcutsInhibit (void)
{
  inhibit_manager_global
    = wl_global_create (compositor.wl_display,
			&zwp_keyboard_shortcuts_inhibit_manager_v1_interface,
			1, NULL, HandleBind);
}

void
XLCheckShortcutInhibition (Seat *seat, Surface *surface)
{
  KeyboardShortcutInhibitor *inhibitor;

  /* If SURFACE has a shortcut inhibitor, inhibit shortcuts and send
     the active signal.  */

  inhibitor = FindKeyboardShortcutInhibitor (surface, seat);

  if (!inhibitor)
    return;

  /* Try to apply an external grab.  */
  if (XLSeatApplyExternalGrab (seat, surface))
    {
      /* The external grab is now active, so send the active
	 signal.  */
      zwp_keyboard_shortcuts_inhibitor_v1_send_active (inhibitor->resource);

      /* Mark the inhibitor as active.  */
      inhibitor->flags |= IsGrabbed;
    }
  else if (inhibitor->flags & IsGrabbed)
    {
      /* The grab failed, and inhibitor was already grabbed (can that
	 even happen?) */
      inhibitor->flags &= ~IsGrabbed;
      zwp_keyboard_shortcuts_inhibitor_v1_send_inactive (inhibitor->resource);
    }
}

void
XLReleaseShortcutInhibition (Seat *seat, Surface *surface)
{
  KeyboardShortcutInhibitor *inhibitor;

  inhibitor = FindKeyboardShortcutInhibitor (surface, seat);

  if (!inhibitor || !(inhibitor->flags & IsGrabbed))
    return;

  /* Cancel the grab.  */
  XLSeatCancelExternalGrab (seat);

  /* Mark the inhibitor as no longer holding a grab.  */
  inhibitor->flags &= IsGrabbed;
  zwp_keyboard_shortcuts_inhibitor_v1_send_inactive (inhibitor->resource);
}
