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

typedef struct _DataSource DataSource;
typedef struct _DataDeviceReference DataDeviceReference;
typedef struct _DataOffer DataOffer;
typedef struct _DataDestroyCallback DataDestroyCallback;

enum
  {
    IsDragAndDrop      = 1,
    IsMimeTypeAccepted = (1 << 2),
    IsActionSent       = (1 << 3),
    IsFinished	       = (1 << 4),
  };

enum
  {
    ActionsSet	      = 1,
    ActionsSent	      = (1 << 2),
    TypeAccepted      = (1 << 3),
    Version3Supported = (1 << 4),
  };

struct _DataDestroyCallback
{
  /* The next and last destroy callbacks in this list.  */
  DataDestroyCallback *next, *last;

  /* Function called when the surface is destroyed.  */
  void (*destroy_func) (void *data);

  /* Data for the surface.  */
  void *data;
};

struct _DataOffer
{
  /* The next data offer object in this list.  */
  DataOffer *next, *last;

  /* The DataSource corresponding to this data offer.  */
  DataSource *source;

  /* Some state and flags.  */
  int state;

  /* The last action sent.  -1 if invalid.  */
  int last_action;

  /* The struct wl_resource of this data offer.  */
  struct wl_resource *resource;

  /* The serial of the data device entry in response to which this
     object was created.  */
  uint32_t dnd_serial;
};

struct _DataDeviceReference
{
  /* The next and last data device references.  */
  DataDeviceReference *next, *last;

  /* The associated data device.  */
  DataDevice *device;

  /* The associated struct wl_resource.  */
  struct wl_resource *resource;
};

struct _DataSource
{
  /* List of const char *, which are the MIME types offered by this
     data source.  */
  XLList *mime_types;

  /* List of atoms corresponding to those MIME types, in the same
     order.  */
  XIDList *atom_types;

  /* Number of corresponding MIME types.  */
  int n_mime_types;

  /* The resource associated with this data source.  */
  struct wl_resource *resource;

  /* List of data offers associated with this data source.  */
  DataOffer offers;

  /* Some flags associated with this data source.  */
  int state;

  /* Drag-and-drop actions supported by this data source.  */
  uint32_t actions;

  /* The data device from which this data source is being dragged.  */
  DataDevice *drag_device;

  /* The destroy callback associated with that data device.  */
  DataDestroyCallback *drag_device_callback;

  /* List of destroy callbacks.  */
  DataDestroyCallback destroy_callbacks;
};

struct _DataDevice
{
  /* The associated seat.  */
  Seat *seat;

  /* The number of references to this data device.  */
  int refcount;

  /* Linked list of references to this data device.  */
  DataDeviceReference references;

  /* The drag and drop operation state.  supported_actions is the mask
     consisting of actions supported by the target.  */
  uint32_t supported_actions;

  /* This is the mask containing actions preferred by the target.  */
  uint32_t preferred_action;

  /* This is the "serial" of the last enter event.  */
  uint32_t dnd_serial;

  /* List of destroy callbacks.  */
  DataDestroyCallback destroy_callbacks;
};

/* The data device manager global.  */
static struct wl_global *global_data_device_manager;

/* The current selection.  */
static DataSource *current_selection_data;

/* A sentinel value that means a foreign selection is in use.  */
static DataSource foreign_selection_key;

/* Functions to call to obtain wl_data_offer resources and send
   associated data for foreign selections.  */
static CreateOfferFuncs foreign_selection_functions;

/* Time the foreign selection was changed.  */
static Time foreign_selection_time;

/* When it changed.  */
static uint32_t last_selection_change_serial;

/* Generic destroy callback implementation.  */


static DataDestroyCallback *
AddDestroyCallbackAfter (DataDestroyCallback *start,
			 void (*destroy_func) (void *),
			 void *data)
{
  DataDestroyCallback *callback;

  callback = XLMalloc (sizeof *callback);
  callback->last = start;
  callback->next = start->next;

  start->next->last = callback;
  start->next = callback;

  callback->destroy_func = destroy_func;
  callback->data = data;

  return callback;
}

static void
FreeDestroyCallbacks (DataDestroyCallback *start)
{
  DataDestroyCallback *next, *last;

  next = start->next;

  while (next != start)
    {
      last = next;
      next = last->next;

      last->destroy_func (last->data);
      XLFree (last);
    }
}

static void
CancelDestroyCallback (DataDestroyCallback *start)
{
  start->next->last = start->last;
  start->last->next = start->next;

  XLFree (start);
}

/* Data offer implementation.  */

static void
FreeDataOffer (DataOffer *offer)
{
  /* Mark this offer as invalid by setting the resource's user_data to
     NULL.  */
  if (offer->resource)
    wl_resource_set_user_data (offer->resource, NULL);

  /* Unlink the offer.  */
  offer->last->next = offer->next;
  offer->next->last = offer->last;

  /* Free the offer.  */
  XLFree (offer);
}

static void
Accept (struct wl_client *client, struct wl_resource *resource,
	uint32_t serial, const char *mime_type)
{
  DataOffer *offer;

  offer = wl_resource_get_user_data (resource);

  if (!offer)
    return;

  wl_data_source_send_target (offer->source->resource,
			      mime_type);

  if (mime_type)
    {
      offer->state |= IsMimeTypeAccepted;
      offer->source->state |= TypeAccepted;
    }
  else
    {
      offer->state &= ~IsMimeTypeAccepted;
      offer->source->state &= ~TypeAccepted;
    }
}

static void
Receive (struct wl_client *client, struct wl_resource *resource,
	 const char *mime_type, int fd)
{
  DataOffer *offer;

  offer = wl_resource_get_user_data (resource);

  if (!offer)
    {
#ifdef DEBUG
      fprintf (stderr, "wl_client@%p is trying to receive from an outdated"
	       " wl_data_offer@%u\n", client, wl_resource_get_id (resource));
#endif
      close (fd);
      return;
    }

  if (offer->state & IsFinished)
    {
      wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
			      "trying to receive from finished offer");
      return;
    }

#ifdef DEBUG
  fprintf (stderr, "wl_client@%p is now receiving from wl_data_offer@%u\n",
	   client, wl_resource_get_id (resource));
#endif

  wl_data_source_send_send (offer->source->resource, mime_type, fd);
  close (fd);
}

static void
DestroyDataOffer (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
Finish (struct wl_client *client, struct wl_resource *resource)
{
  DataOffer *offer;

  offer = wl_resource_get_user_data (resource);

  if (!offer)
    /* The data source was destroyed... */
    return;

  if (!(offer->state & IsDragAndDrop))
    {
      wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
			      "trying to finish non-drag-and-drop data offer");
      return;
    }

  /* The serial or drag device is not tested here, since CancelDrag
     detaches the attached drag-and-drop data device before finish is
     called in response to a drop.  */

  if (!(offer->state & IsMimeTypeAccepted))
    {
      wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
			      "trying to finish drag and drop offer with nothing"
			      " accepted");
      return;
    }

  if (!(offer->state & IsActionSent))
    {
      wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
			      "trying to finish drag and drop offer with no action"
			      " sent");
      return;
    }

  if (offer->state & IsFinished)
    {
      wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
			      "trying to finish drag and drop offer which was"
			      " already finished");
      return;
    }

  offer->state |= IsFinished;

  if (wl_resource_get_version (offer->source->resource) < 3)
    return;

  if (offer->source->state & Version3Supported
      && (!(offer->source->state & ActionsSent)
	  || !(offer->source->state & TypeAccepted)))
    {
      /* The drag and drop operation is no longer eligible for
	 successful completion.  Cancel it and return.  */
      wl_data_source_send_cancelled (offer->source->resource);
      return;
    }

  wl_data_source_send_dnd_finished (offer->source->resource);
}

static void
UpdateDeviceActions (DataDevice *device, DataSource *source)
{
  uint32_t action, intersection;
  unsigned int modifiers;
  DataOffer *offer;

  modifiers = XLSeatGetEffectiveModifiers (device->seat);

  /* How actions are resolved.  First, the preferred action is matched
     against the actions supported by the source, and used if it is
     supported.  If it is not supported by the data source, then the
     following actions are tried in order: copy, move, ask, none.  If
     none of those are supported, then no action is used.  The
     preferred action is ignored if the Shift key is held down, in
     which case Move is also preferred to copy.  */

  if (!(modifiers & ShiftMask)
      && device->preferred_action & source->actions)
    action = device->preferred_action;
  else
    {
      intersection = (device->supported_actions
		      & source->actions);

      /* Now, try the following actions in order, preferring move to
	 copy if Shift is held down.  */

      if (modifiers & ShiftMask
	  && intersection & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
	action = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
      else if (intersection & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
        action = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
      else if (intersection & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
        action = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
      else if (intersection & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
        action = WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
      else
        action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    }

  /* Send the action to all attached data offers.  */
  offer = source->offers.next;
  while (offer != &source->offers)
    {
      if (!(offer->state & IsDragAndDrop))
	goto next;

      if (offer->dnd_serial < device->dnd_serial)
	goto next;

      if (offer->last_action != action)
	wl_data_offer_send_action (offer->resource, action);

      offer->last_action = action;
      offer->state |= IsActionSent;

    next:
      offer = offer->next;
    }

  /* Set flags on the source indicating that an action has been set,
     unless action is 0, in which case clear it.  */
  if (action)
    source->state |= ActionsSent;
  else
    source->state &= ~ActionsSent;

  /* Send the new action to the data source.  */
  if (wl_resource_get_version (source->resource) >= 3)
    wl_data_source_send_action (source->resource, action);
}

static void
DataOfferSetActions (struct wl_client *client, struct wl_resource *resource,
		     uint32_t dnd_actions, uint32_t preferred_action)
{
  DataOffer *offer;

  offer = wl_resource_get_user_data (resource);

  if (!offer)
    /* The data source was destroyed... */
    return;

  if (!(offer->state & IsDragAndDrop))
    {
      wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_OFFER,
			      "trying to finish non-drag-and-drop data offer");
      return;
    }

  if (!offer->source->drag_device)
    /* The data device has been destroyed.  */
    return;

  if (offer->dnd_serial < offer->source->drag_device->dnd_serial)
    /* The data offer is out of data and effectively inert.  */
    return;

  if (dnd_actions & ~(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
		      | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
		      | WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK))
    {
      wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_ACTION,
			      "invalid action or action mask among: %u",
			      dnd_actions);
      return;
    }

  if (preferred_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK
      && preferred_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
      && preferred_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
      && preferred_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE)
    {
      wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_ACTION,
			      "invalid action not in enum: %u",
			      preferred_action);
      return;
    }

  offer->source->drag_device->supported_actions = dnd_actions;
  offer->source->drag_device->preferred_action = preferred_action;
  UpdateDeviceActions (offer->source->drag_device, offer->source);
}

static const struct wl_data_offer_interface wl_data_offer_impl =
  {
    .accept = Accept,
    .receive = Receive,
    .destroy = DestroyDataOffer,
    .finish = Finish,
    .set_actions = DataOfferSetActions,
  };

static void
HandleOfferResourceDestroy (struct wl_resource *resource)
{
  DataOffer *offer;

  offer = wl_resource_get_user_data (resource);

  if (!offer)
    /* The offer was made inert.  */
    return;

  offer->resource = NULL;
  FreeDataOffer (offer);
}

static struct wl_resource *
AddDataOffer (struct wl_client *client, DataSource *source)
{
  DataOffer *offer;
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_data_offer_interface,
				 /* 0 means to allocate a new resource
				    ID server-side.  */
				 3, 0);

  if (!resource)
    return NULL;

  offer = XLCalloc (1, sizeof *offer);
  offer->next = source->offers.next;
  offer->last = &source->offers;

  source->offers.next->last = offer;
  source->offers.next = offer;

  offer->resource = resource;
  offer->source = source;

  wl_resource_set_implementation (resource, &wl_data_offer_impl,
				  offer, HandleOfferResourceDestroy);

  return resource;
}


/* Data device and source implementations.  */

/* Forward declaration.  */

static void SendDataOffers (void);

static void
HandleDragDeviceDestroyed (void *data)
{
  DataSource *data_source;

  data_source = data;
  data_source->drag_device = NULL;
  data_source->drag_device_callback = NULL;

  if (wl_resource_get_version (data_source->resource) >= 3)
    wl_data_source_send_cancelled (data_source->resource);
}

static void
HandleSourceResourceDestroy (struct wl_resource *resource)
{
  DataSource *data_source;
  DataOffer *offer, *last;

  data_source = wl_resource_get_user_data (resource);

  /* If data_source is currently the selection, remove it.  */
  if (data_source == current_selection_data)
    {
      current_selection_data = NULL;

      /* Send the updated data to clients.  */
      SendDataOffers ();
    }

  /* Tell the X selection code that this data source has been
     destroyed.  */
  XLNoteSourceDestroyed (data_source);

  XLListFree (data_source->mime_types, XLFree);
  XIDListFree (data_source->atom_types, NULL);

  /* Make inert and release all data offers on this data source.  */

  offer = data_source->offers.next;

  while (offer != &data_source->offers)
    {
      last = offer;
      offer = offer->next;

      FreeDataOffer (last);
    }

  /* Free the destroy callback for the data device.  */
  if (data_source->drag_device_callback)
    CancelDestroyCallback (data_source->drag_device_callback);

  /* Run all destroy callbacks for this data source.  */
  FreeDestroyCallbacks (&data_source->destroy_callbacks);

  XLFree (data_source);
}

static const char *
FindAtom (DataSource *source, Atom atom)
{
  XIDList *tem;
  XLList *tem1;

  /* Find the MIME type corresponding to an atom in source.
     Return NULL if the atom is not there.

     source->mime_types must be the same length as
     source->atom_types.  */

  tem = source->atom_types;
  tem1 = source->mime_types;

  for (; tem; tem = tem->next, tem1 = tem1->next)
    {
      if (tem->data == atom)
	return tem1->data;
    }

  return NULL;
}

static void
Offer (struct wl_client *client, struct wl_resource *resource,
       const char *mime_type)
{
  Atom atom;
  DataSource *data_source;
  DataOffer *offer;

  data_source = wl_resource_get_user_data (resource);

  /* It is more efficient to record both atoms and strings in the data
     source, since its contents will be offered to X and Wayland
     clients.  */
  atom = InternAtom (mime_type);

  /* If the type was already offered, simply return.  */
  if (FindAtom (data_source, atom))
    return;

  /* Otherwise, link the atom and the mime type onto the list
     simultaneously.  */
#ifdef DEBUG
  fprintf (stderr, "Offering: %s (X atom: %lu) from wl_data_source@%u\n",
	   mime_type, atom, wl_resource_get_id (resource));
#endif
  data_source->atom_types = XIDListPrepend (data_source->atom_types, atom);
  data_source->mime_types = XLListPrepend (data_source->mime_types,
					   XLStrdup (mime_type));
  data_source->n_mime_types++;

  /* Send the new MIME type to any attached offers.  */

  offer = data_source->offers.next;

  while (offer != &data_source->offers)
    {
      wl_data_offer_send_offer (offer->resource, mime_type);
      offer = offer->next;
    }
}

static void
SetActions (struct wl_client *client, struct wl_resource *resource,
	    uint32_t actions)
{
  DataSource *source;

  source = wl_resource_get_user_data (resource);

  if (source->state & ActionsSet)
    {
      wl_resource_post_error (resource, WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
			      "actions already set on this offer or it has"
			      " already been used.");
      return;
    }

  if (actions & ~(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
		  | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
		  | WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK))
    {
      wl_resource_post_error (resource, WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
			      "unknown actions specified (mask value %u)",
			      actions);
      return;
    }

  source->state |= ActionsSet;
  source->actions = actions;
}

static void
DestroySource (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_data_source_interface wl_data_source_impl =
  {
    .offer = Offer,
    .set_actions = SetActions,
    .destroy = DestroySource,
  };

static void
CreateDataSource (struct wl_client *client, struct wl_resource *resource,
		  uint32_t id)
{
  DataSource *source;

  source = XLSafeMalloc (sizeof *source);

  if (!source)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (source, 0, sizeof *source);
  source->resource = wl_resource_create (client, &wl_data_source_interface,
					 wl_resource_get_version (resource),
					 id);

  if (!source->resource)
    {
      wl_resource_post_no_memory (resource);
      XLFree (source);
      return;
    }

  source->offers.next = &source->offers;
  source->offers.last = &source->offers;
  source->destroy_callbacks.next = &source->destroy_callbacks;
  source->destroy_callbacks.last = &source->destroy_callbacks;

  wl_resource_set_implementation (source->resource, &wl_data_source_impl,
				  source, HandleSourceResourceDestroy);
}

static void
UpdateSingleReferenceWithForeignOffer (struct wl_client *client,
				       DataDeviceReference *reference)
{
  struct wl_resource *resource;
  Time time;

  time = foreign_selection_time;
  resource = foreign_selection_functions.create_offer (client, time);

  if (!resource)
    return;

  /* Make the data offer known to the client.  */
  wl_data_device_send_data_offer (reference->resource, resource);

  /* Tell the foreign selection provider to send supported resources
     to the client.  */
  foreign_selection_functions.send_offers (resource, time);

  /* Finally, tell the client that the offer is a selection.  */
  wl_data_device_send_selection (reference->resource, resource);
}

static void
UpdateForSingleReference (DataDeviceReference *device)
{
  struct wl_resource *resource;
  struct wl_client *client;
  XLList *type;

  if (!current_selection_data)
    {
      wl_data_device_send_selection (device->resource,
				     NULL);
      return;
    }

  client = wl_resource_get_client (device->resource);

  if (current_selection_data == &foreign_selection_key)
    {
      /* This means a foreign selection is in use.  */
      UpdateSingleReferenceWithForeignOffer (client, device);

      return;
    }

  resource = AddDataOffer (client, current_selection_data);

  if (!resource)
    return;

  /* First, introduce the data offer to the client.  */
  wl_data_device_send_data_offer (device->resource, resource);

  /* Send all the offered MIME types.  */

  type = current_selection_data->mime_types;

  for (; type; type = type->next)
    wl_data_offer_send_offer (resource, type->data);

  /* Finally, tell the client it is a selection.  */
  wl_data_device_send_selection (device->resource, resource);
}

static void
SendDataOffersForDevice (DataDevice *device)
{
  DataDeviceReference *reference;
  struct wl_client *client;

  reference = device->references.next;

  while (reference != &device->references)
    {
      client = wl_resource_get_client (reference->resource);

      if (XLSeatIsClientFocused (device->seat, client))
	UpdateForSingleReference (reference);

      reference = reference->next;
    }
}

static void
SendDataOffers (void)
{
  XLList *seat;
  DataDevice *device;

  for (seat = live_seats; seat; seat = seat->next)
    {
      device = XLSeatGetDataDevice (seat->data);

      if (device)
	SendDataOffersForDevice (device);
    }
}

static void
DestroyReference (DataDeviceReference *reference)
{
  reference->next->last = reference->last;
  reference->last->next = reference->next;

  XLFree (reference);
}

static void
ReleaseReferences (DataDevice *device)
{
  DataDeviceReference *reference;

  reference = device->references.next;

  while (reference != &device->references)
    {
      reference->device = NULL;
      reference = reference->next;
    }
}

static void
DestroyBacking (DataDevice *device)
{
  if (--device->refcount)
    return;

  ReleaseReferences (device);
  FreeDestroyCallbacks (&device->destroy_callbacks);
  XLFree (device);
}

static DataDevice *
GetDataDeviceInternal (Seat *seat)
{
  DataDevice *device;

  device = XLSeatGetDataDevice (seat);

  if (!device)
    {
      device = XLCalloc (1, sizeof *device);
      device->seat = seat;
      device->references.next = &device->references;
      device->references.last = &device->references;
      device->destroy_callbacks.next = &device->destroy_callbacks;
      device->destroy_callbacks.last = &device->destroy_callbacks;

      XLSeatSetDataDevice (seat, device);
    }

  return device;
}

static DataDeviceReference *
AddReferenceTo (DataDevice *device, struct wl_resource *resource)
{
  DataDeviceReference *reference;

  reference = XLCalloc (1, sizeof *reference);
  reference->next = device->references.next;
  reference->last = &device->references;
  reference->resource = resource;

  device->references.next->last = reference;
  device->references.next = reference;

  reference->device = device;

  return reference;
}

static void
StartDrag (struct wl_client *client, struct wl_resource *resource,
	   struct wl_resource *source_resource,
	   struct wl_resource *origin_resource,
	   struct wl_resource *icon_resource, uint32_t serial)
{
  DataDeviceReference *device;
  Surface *icon, *origin;
  DataSource *source;

  device = wl_resource_get_user_data (resource);

  if (!device->device || !device->device->seat)
    /* This device is inert, since the seat has been deleted.  */
    return;

  if (icon_resource)
    icon = wl_resource_get_user_data (icon_resource);
  else
    icon = NULL;

  origin = wl_resource_get_user_data (origin_resource);
  source = wl_resource_get_user_data (source_resource);

  if (source == current_selection_data)
    {
      /* The specification doesn't say anything about this!  */

      wl_resource_post_error (source_resource,
			      WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
			      "trying to drag the selection");
      return;
    }

  if (source->drag_device)
    {
      /* The specification doesn't say anything about this!  */

      wl_resource_post_error (source_resource,
			      WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
			      "trying to drag a data source that is"
			      " already being dragged");
      return;
    }

  /* If the icon surface isn't the right type, throw an error.  */
  if (icon && icon->role_type != AnythingType
      && icon->role_type != DndIconType)
    {
      wl_resource_post_error (resource, WL_DATA_DEVICE_ERROR_ROLE,
			      "the given surface already has/had"
			      " another role");
      return;
    }

  /* Now make it impossible to set this source as the selection.  */
  source->state |= ActionsSet;

  XLSeatBeginDrag (device->device->seat, source, origin,
		   icon, serial);
}

static void
SetSelection (struct wl_client *client, struct wl_resource *resource,
	      struct wl_resource *source_resource, uint32_t serial)
{
  DataSource *source;
  DataDeviceReference *device;

#ifdef DEBUG
  if (source_resource)
    fprintf (stderr, "wl_client@%p is setting the selection to "
	     "wl_data_source@%u, at %u\n",
	     client, wl_resource_get_id (source_resource), serial);
#endif

  if (serial < last_selection_change_serial)
    {
#ifdef DEBUG
      fprintf (stderr, "wl_client@%p could not set the selection, "
	       "because the selection changed.  (%u < %u)\n",
	       client, serial, last_selection_change_serial);
#endif
      return;
    }

  device = wl_resource_get_user_data (resource);

  if (!device->device || !device->device->seat)
    /* This device is inert, since the seat has been deleted.  */
    return;

  /* Set the last selection change serial.  This enables us to avoid
     race conditions caused by multiple clients simultaneously setting
     the clipboard according to different events, by keeping track of
     which event is newer.  */
  last_selection_change_serial = serial;

  if (source_resource)
    source = wl_resource_get_user_data (source_resource);
  else
    source = NULL;

  /* If the data source is destined for drag and drop, report an
     error.  */
  if (source && source->state & ActionsSet)
    {
      wl_resource_post_error (resource, WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
			      "trying to set dnd source as the selection");
      return;
    }

  /* Try to own the X selection.  If it fails, refrain from changing
     the current selection data.  */
  if (!XLNoteLocalSelection (device->device->seat, source))
    return;

  if (current_selection_data && current_selection_data != source)
    {
      /* If the current selection data is already set, cancel it.  */
      if (current_selection_data != &foreign_selection_key)
	wl_data_source_send_cancelled (current_selection_data->resource);
    }

  if (current_selection_data != source)
    {
      current_selection_data = source;

      /* Create data offer objects for the new selection data and send
	 it to clients.  */
      SendDataOffers ();
    }
}

static void
Release (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_data_device_interface wl_data_device_impl =
  {
    .start_drag = StartDrag,
    .set_selection = SetSelection,
    .release = Release,
  };

static void
HandleDeviceResourceDestroy (struct wl_resource *resource)
{
  DataDeviceReference *reference;

  reference = wl_resource_get_user_data (resource);
  DestroyReference (reference);
}

static void
GetDataDevice (struct wl_client *client, struct wl_resource *resource,
	       uint32_t id, struct wl_resource *seat_resource)
{
  struct wl_resource *device_resource;
  Seat *seat;
  DataDevice *device;
  DataDeviceReference *reference;

  device_resource = wl_resource_create (client, &wl_data_device_interface,
					wl_resource_get_version (resource),
					id);

  if (!device_resource)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  seat = wl_resource_get_user_data (seat_resource);
  device = GetDataDeviceInternal (seat);
  reference = AddReferenceTo (device, device_resource);

  wl_resource_set_implementation (device_resource, &wl_data_device_impl,
				  reference, HandleDeviceResourceDestroy);
}

static struct wl_data_device_manager_interface wl_data_device_manager_impl =
  {
    .create_data_source = CreateDataSource,
    .get_data_device = GetDataDevice,
  };

static void
HandleBind (struct wl_client *client, void *data,
	    uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource
    = wl_resource_create (client, &wl_data_device_manager_interface,
			  version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource,
				  &wl_data_device_manager_impl,
				  NULL, NULL);
}

void
XLInitDataDevice (void)
{
  global_data_device_manager
    = wl_global_create (compositor.wl_display,
			&wl_data_device_manager_interface,
			3, NULL, HandleBind);
}

void
XLRetainDataDevice (DataDevice *device)
{
  device->refcount++;
}

void
XLReleaseDataDevice (DataDevice *device)
{
  DestroyBacking (device);
}

void
XLDataDeviceClearSeat (DataDevice *device)
{
  device->seat = NULL;
}

void
XLDataDeviceHandleFocusChange (DataDevice *device)
{
  SendDataOffersForDevice (device);
}

void
XLSetForeignSelection (Time time, CreateOfferFuncs functions)
{
  uint32_t serial;

  if (time < foreign_selection_time)
    return;

  serial = wl_display_next_serial (compositor.wl_display);

  /* Use this serial to prevent clients from changing the selection
     again until the next event is sent.  */
  last_selection_change_serial = serial;

  foreign_selection_time = time;
  foreign_selection_functions = functions;

  if (current_selection_data
      && current_selection_data != &foreign_selection_key)
    wl_data_source_send_cancelled (current_selection_data->resource);

  /* Use a special value of current_selection_data to mean that
     foreign selections are in use.  */
  current_selection_data = &foreign_selection_key;

  /* Send new data offers to current clients.  */
  SendDataOffers ();
}

void
XLClearForeignSelection (Time time)
{
  if (time < foreign_selection_time)
    return;

  if (current_selection_data == &foreign_selection_key)
    {
      current_selection_data = NULL;

      SendDataOffers ();
    }

  foreign_selection_time = time;
}

int
XLDataSourceTargetCount (DataSource *source)
{
  return source->n_mime_types;
}

void
XLDataSourceGetTargets (DataSource *source, Atom *targets)
{
  int i;
  XIDList *list;

  list = source->atom_types;

  for (i = 0; i < source->n_mime_types; ++i)
    {
      targets[i] = list->data;
      list = list->next;
    }
}

struct wl_resource *
XLResourceFromDataSource (DataSource *source)
{
  return source->resource;
}

Bool
XLDataSourceHasAtomTarget (DataSource *source, Atom target)
{
  XIDList *list;

  for (list = source->atom_types; list; list = list->next)
    {
      if (list->data == target)
	return True;
    }

  return False;
}

Bool
XLDataSourceHasTarget (DataSource *source, const char *mime_type)
{
  XLList *list;

  for (list = source->mime_types; list; list = list->next)
    {
      if (!strcmp (list->data, mime_type))
	return True;
    }

  return False;
}

void
XLDataDeviceMakeOffers (Seat *seat, DndOfferFuncs funcs, Surface *surface,
			int x, int y)
{
  DataDevice *device;
  DataDeviceReference *reference;
  struct wl_resource *resource;
  struct wl_client *client;
  int version;
  uint32_t serial;

  device = XLSeatGetDataDevice (seat);
  client = wl_resource_get_client (surface->resource);
  reference = device->references.next;
  serial = wl_display_next_serial (compositor.wl_display);

  while (reference != &device->references)
    {
      /* If this reference to the data device belongs to the client,
	 continue.  */
      if (wl_resource_get_client (reference->resource)
	  == wl_resource_get_client (surface->resource))
	{
	  version = wl_resource_get_version (reference->resource);

	  /* Create the offer.  */
	  resource = funcs.create (client, version);

	  if (resource)
	    {
	      /* Actually send the data offer to the client.  */
	      wl_data_device_send_data_offer (reference->resource,
					      resource);

	      /* And data offers.  */
	      funcs.send_offers (resource);

	      /* And send the entry event.  */
	      wl_data_device_send_enter (reference->resource,
					 serial, surface->resource,
					 wl_fixed_from_double (x),
					 wl_fixed_from_double (y),
					 resource);
	    }
	}

      reference = reference->next;
    }
}

void
XLDataDeviceSendEnter (Seat *seat, Surface *surface, double x, double y,
		       DataSource *source)
{
  DataDevice *device;
  DataDeviceReference *reference;
  uint32_t serial;
  struct wl_resource *resource;
  struct wl_client *client;
  DataOffer *offer;
  XLList *type;

  device = XLSeatGetDataDevice (seat);

  if (!device)
    /* No data device has been created for this seat yet.  */
    return;

  reference = device->references.next;
  serial = wl_display_next_serial (compositor.wl_display);
  client = wl_resource_get_client (surface->resource);
  device->dnd_serial = serial;

  /* Clear the selected actions.  */
  device->supported_actions = 0;
  device->preferred_action = 0;

  /* And some flags.  */
  if (source)
    source->state = 0;

  while (reference != &device->references)
    {
      /* If this reference to the data device belongs to the client,
	 continue.  */
      if (wl_resource_get_client (reference->resource) == client)
	{
	  if (source)
	    {
	      /* First, create a data offer corresponding to the data
		 source if it exists.  */
	      resource = AddDataOffer (client, source);

	      if (!resource)
		/* Allocation of the resource failed.  */
		goto next;

	      offer = wl_resource_get_user_data (resource);
	      offer->dnd_serial = serial;
	      offer->last_action = -1;

	      /* Mark the offer as a drag-and-drop offer.  */
	      offer->state |= IsDragAndDrop;

	      /* Introduce the data offer to the client.  */
	      wl_data_device_send_data_offer (reference->resource, resource);

	      /* Send all the offered data types to the client.  */
	      type = source->mime_types;

	      for (; type; type = type->next)
		wl_data_offer_send_offer (resource, type->data);

	      /* Send the source actions.  */
	      wl_data_offer_send_source_actions (resource, source->actions);

	      /* If the data device supports version 3 or later, set
		 the flag.  */
	      if (wl_resource_get_version (resource) >= 3)
	        source->state |= Version3Supported;
	    }

	  wl_data_device_send_enter (reference->resource,
				     serial, surface->resource,
				     wl_fixed_from_double (x),
				     wl_fixed_from_double (y),
				     source ? resource : NULL);

	}

    next:
      reference = reference->next;
    }
}

void
XLDataDeviceSendMotion (Seat *seat, Surface *surface,
		        double x, double y, Time time)
{
  DataDevice *device;
  DataDeviceReference *reference;

  device = XLSeatGetDataDevice (seat);

  if (!device)
    /* No data device has been created for this seat yet.  */
    return;

  reference = device->references.next;

  while (reference != &device->references)
    {
      /* If this reference to the data device belongs to the client,
	 continue.  */
      if (wl_resource_get_client (reference->resource)
	  == wl_resource_get_client (surface->resource))
	wl_data_device_send_motion (reference->resource, time,
				    wl_fixed_from_double (x),
				    wl_fixed_from_double (y));

      reference = reference->next;
    }
}

void
XLDataDeviceSendLeave (Seat *seat, Surface *surface, DataSource *source)
{
  DataDevice *device;
  DataDeviceReference *reference;

  device = XLSeatGetDataDevice (seat);

  if (!device)
    /* No data device has been created for this seat yet.  */
    return;

  reference = device->references.next;

  /* This serial doesn't actually mean anything.  It's only used to
     invalidate previous data offers.  */
  device->dnd_serial = wl_display_next_serial (compositor.wl_display);

  while (reference != &device->references)
    {
      /* If this reference to the data device belongs to the client,
	 continue.  */
      if (wl_resource_get_client (reference->resource)
	  == wl_resource_get_client (surface->resource))
	wl_data_device_send_leave (reference->resource);

      reference = reference->next;
    }

  if (source)
    {
      /* Clear the selected actions.  */
      device->supported_actions = 0;
      device->preferred_action = 0;

      /* Since the pointer has left the source, clear several
	 flags.  */
      source->state = 0;

      /* Send the new effective action to the source.  */
      if (wl_resource_get_version (source->resource) >= 3)
	wl_data_source_send_action (source->resource,
				    WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);

      /* Also tell the source that there is no longer any
	 target accepted.  */
      wl_data_source_send_target (source->resource, NULL);
    }
}

void
XLDataDeviceSendDrop (Seat *seat, Surface *surface)
{
  DataDevice *device;
  DataDeviceReference *reference;

  device = XLSeatGetDataDevice (seat);

  if (!device)
    /* No data device has been created for this seat yet.  */
    return;

  reference = device->references.next;

  while (reference != &device->references)
    {
      /* If this reference to the data device belongs to the client,
	 continue.  */
      if (wl_resource_get_client (reference->resource)
	  == wl_resource_get_client (surface->resource))
	wl_data_device_send_drop (reference->resource);

      reference = reference->next;
    }
}



void
XLDataSourceAttachDragDevice (DataSource *source, DataDevice *device)
{
  if (source->drag_device)
    {
      CancelDestroyCallback (source->drag_device_callback);
      source->drag_device_callback = NULL;
    }

  source->drag_device = device;

  if (device)
    source->drag_device_callback
      = AddDestroyCallbackAfter (&device->destroy_callbacks,
				 HandleDragDeviceDestroyed,
				 source);
}

void *
XLDataSourceAddDestroyCallback (DataSource *source,
				void (*destroy_func) (void *),
				void *data)
{
  return AddDestroyCallbackAfter (&source->destroy_callbacks,
				  destroy_func, data);
}

void
XLDataSourceCancelDestroyCallback (void *key)
{
  CancelDestroyCallback (key);
}

void
XLDataSourceSendDropPerformed (DataSource *source)
{
  if (wl_resource_get_version (source->resource) >= 3)
    wl_data_source_send_dnd_drop_performed (source->resource);
}

void
XLDataSourceSendDropCancelled (DataSource *source)
{
  if (wl_resource_get_version (source->resource) >= 3)
    wl_data_source_send_cancelled (source->resource);
}

Bool
XLDataSourceCanDrop (DataSource *source)
{
  /* If version 3 is supported, require that an action has been sent
     and a data type has been accepted.  Otherwise, always do the
     drop.  */
  if (source->state & Version3Supported)
    return (source->state & ActionsSent
	    && source->state & TypeAccepted);

  return True;
}

uint32_t
XLDataSourceGetSupportedActions (DataSource *source)
{
  return source->actions;
}

XLList *
XLDataSourceGetMimeTypeList (DataSource *source)
{
  return source->mime_types;
}

void
XLDataSourceUpdateDeviceActions (DataSource *drag_source)
{
  UpdateDeviceActions (drag_source->drag_device, drag_source);
}
