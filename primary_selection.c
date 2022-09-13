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

#include "primary-selection-unstable-v1.h"

typedef struct _PDataDevice PDataDevice;
typedef struct _PDataSource PDataSource;
typedef struct _PDataOffer PDataOffer;

enum
  {
    WasSentOffer = 1,
  };

struct _PDataDevice
{
  /* The seat associated with this data device.  */
  Seat *seat;

  /* The destroy callback associated with that seat.  */
  void *seat_destroy_key;

  /* Some flags.  */
  int flags;

  /* The wl_resource associated with this data device.  */
  struct wl_resource *resource;

  /* The next and last data devices.  */
  PDataDevice *next, *last;
};

struct _PDataOffer
{
  /* The data source associated with this offer.  */
  PDataSource *source;

  /* The resource associated with this offer.  */
  struct wl_resource *resource;

  /* The next and last data offers associated with the data
     source.  */
  PDataOffer *next, *last;
};

struct _PDataSource
{
  /* The wl_resource associated with this data source.  */
  struct wl_resource *resource;

  /* List of all data offers attached to this source.  */
  PDataOffer offers;

  /* List of all MIME types provided by this source.  */
  XLList *mime_types;

  /* Number of MIME types provided by this source.  */
  int n_mime_types;
};

/* The global primary selection manager.  */
static struct wl_global *global_primary_selection_device_manager;

/* The data source currently providing the primary selection.  */
static PDataSource *primary_selection;

/* The last time the primary selection changed.  */
static uint32_t last_change_serial;

/* All data devices.  */
static PDataDevice all_devices;

/* Forward declaration.  */

static void NoticeChanged (void);

/* Data offer implementation.  */


static void
Receive (struct wl_client *client, struct wl_resource *resource,
	 const char *mime_type, int32_t fd)
{
  PDataOffer *offer;

  offer = wl_resource_get_user_data (resource);

  if (!offer)
    {
      /* This data offer is inert.  */
      close (fd);
      return;
    }

  zwp_primary_selection_source_v1_send_send (offer->source->resource,
					     mime_type, fd);
  close (fd);
}

static void
DestroyOffer (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct zwp_primary_selection_offer_v1_interface offer_impl =
  {
    .receive = Receive,
    .destroy = DestroyOffer,
  };

static void
FreeDataOffer (PDataOffer *offer)
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
HandleOfferResourceDestroy (struct wl_resource *resource)
{
  PDataOffer *offer;

  offer = wl_resource_get_user_data (resource);

  if (!offer)
    /* The offer was made inert.  */
    return;

  offer->resource = NULL;
  FreeDataOffer (offer);
}

static struct wl_resource *
AddDataOffer (struct wl_client *client, PDataSource *source)
{
  PDataOffer *offer;
  struct wl_resource *resource;

  resource = wl_resource_create (client,
				 &zwp_primary_selection_offer_v1_interface,
				 /* 0 means to allocate a new resource
				    ID server-side.  */
				 1, 0);

  if (!resource)
    return NULL;

  offer = XLCalloc (1, sizeof *offer);
  offer->next = source->offers.next;
  offer->last = &source->offers;

  source->offers.next->last = offer;
  source->offers.next = offer;

  offer->resource = resource;
  offer->source = source;

  wl_resource_set_implementation (resource, &offer_impl,
				  offer, HandleOfferResourceDestroy);

  return resource;
}

/* Data source implementation.  */


static Bool
FindType (PDataSource *source, const char *mime_type)
{
  XLList *tem;

  for (tem = source->mime_types; tem; tem = tem->next)
    {
      if (!strcmp (tem->data, mime_type))
	return True;
    }

  return False;
}

static void
Offer (struct wl_client *client, struct wl_resource *resource,
       const char *mime_type)
{
  PDataSource *source;

  source = wl_resource_get_user_data (resource);

  /* If the type was already offered, simply return.  */
  if (FindType (source, mime_type))
    return;

  /* Otherwise, add the MIME type to the list of types provided by
     this source.  */
  source->mime_types = XLListPrepend (source->mime_types,
				      XLStrdup (mime_type));
  source->n_mime_types++;
}

static void
DestroySource (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct zwp_primary_selection_source_v1_interface source_impl =
  {
    .offer = Offer,
    .destroy = DestroySource,
  };

static void
HandleSourceResourceDestroy (struct wl_resource *resource)
{
  PDataSource *source;
  PDataOffer *offer, *last;

  source = wl_resource_get_user_data (resource);

  /* Now free every data offer.  */
  offer = source->offers.next;
  while (offer != &source->offers)
    {
      last = offer;
      offer = offer->next;

      FreeDataOffer (last);
    }

  /* And free the MIME types offered by the source.  */
  XLListFree (source->mime_types, XLFree);

  /* If source is the primary selection, clear it and send the change
     to all clients.  */
  if (source == primary_selection)
    {
      primary_selection = NULL;
      NoticeChanged ();
    }

  /* And the source itself.  */
  XLFree (source);
}


/* Device implementation.  */


static void
UpdateForSingleReference (PDataDevice *reference)
{
  struct wl_resource *scratch, *offer;
  struct wl_client *client;
  XLList *type;

  if (!reference->seat)
    /* This data device is inert... */
    return;

  client = wl_resource_get_client (reference->resource);

  if (!XLSeatIsClientFocused (reference->seat, client))
    /* The client is not focused.  */
    return;

  scratch = reference->resource;

  if (!primary_selection)
    {
      /* There is no primary selection.  Send a NULL selection.  */
      zwp_primary_selection_device_v1_send_selection (scratch, NULL);
      return;
    }

  /* Otherwise, create a data offer for that client.  */
  offer = AddDataOffer (client, primary_selection);

  if (!offer)
    /* Allocation of the offer failed.  */
    return;

  /* Introduce the offer to the client.  */
  zwp_primary_selection_device_v1_send_data_offer (scratch, offer);

  /* Send all the offered MIME types.  */

  type = primary_selection->mime_types;

  for (; type; type = type->next)
    zwp_primary_selection_offer_v1_send_offer (offer, type->data);

  /* And tell the client it is the primary selection.  */
  zwp_primary_selection_device_v1_send_selection (scratch, offer);

  /* Set or clear WasSentOffer based on whether or not an offer was
     sent.  */
  if (offer)
    reference->flags |= WasSentOffer;
  else
    reference->flags &= ~WasSentOffer;
}

void
XLPrimarySelectionHandleFocusChange (Seat *seat)
{
  PDataDevice *device;
  struct wl_client *client;
  struct wl_resource *scratch;

  /* The focus changed.  Send NULL data offers to any non-focused data
     device that was previously sent a valid data offer, and send data
     offers to any focused data device that was not.  */

  device = all_devices.next;
  while (device != &all_devices)
    {
      if (!device->seat)
	/* Inert device.  */
	goto next;

      scratch = device->resource;
      client = wl_resource_get_client (scratch);

      if (device->flags & WasSentOffer
	  && !XLSeatIsClientFocused (device->seat, client))
	{
	  /* The device was previously sent a data offer, but is no
	     longer focused.  Send NULL and clear WasSentOffer.  */
	  zwp_primary_selection_device_v1_send_selection (scratch,
							  NULL);
	  device->flags &= ~WasSentOffer;
	}
      else if (!(device->flags & WasSentOffer)
	       && primary_selection
	       && XLSeatIsClientFocused (device->seat, client))
	/* The device is now focused, there is a primary selection,
	   and the device was not sent a data offer.  Send the missing
	   data offer now.  */
	UpdateForSingleReference (device);

    next:
      device = device->next;
    }
}

static void
NoticeChanged (void)
{
  PDataDevice *device;

  /* Send data offers to each data device.  */
  device = all_devices.next;

  while (device != &all_devices)
    {
      UpdateForSingleReference (device);
      device = device->next;
    }
}

static void
SetSelection (struct wl_client *client, struct wl_resource *resource,
	      struct wl_resource *source_resource, uint32_t serial)
{
  struct wl_resource *scratch;

  if (serial < last_change_serial)
    /* This change is out of date.  Do nothing.  */
    return;

  /* Otherwise, set the primary selection.  */
  last_change_serial = serial;

  if (primary_selection)
    {
      /* The primary selection already exists.  Send the cancelled
	 event and clear the primary selection variable.  */
      scratch = primary_selection->resource;
      primary_selection = NULL;

      zwp_primary_selection_source_v1_send_cancelled (scratch);
    }

  if (source_resource)
    /* Make source_resource the new primary selection.  */
    primary_selection = wl_resource_get_user_data (source_resource);

  /* Now, send the updated primary selection to clients.  */
  NoticeChanged ();
}

static void
DestroyDevice (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct zwp_primary_selection_device_v1_interface device_impl =
  {
    .set_selection = SetSelection,
    .destroy = DestroyDevice,
  };

static void
HandleSeatDestroy (void *data)
{
  PDataDevice *device;

  device = data;

  /* The seat destroy listener must be cancelled manually.  */
  XLSeatCancelDestroyListener (device->seat_destroy_key);
  device->seat_destroy_key = NULL;
  device->seat = NULL;
}

static void
HandleDeviceResourceDestroy (struct wl_resource *resource)
{
  PDataDevice *device;

  device = wl_resource_get_user_data (resource);

  /* Cancel the seat destroy listener should it exist.  */

  if (device->seat)
    XLSeatCancelDestroyListener (device->seat_destroy_key);

  /* Unlink the device.  */

  device->last->next = device->next;
  device->next->last = device->last;

  XLFree (device);
}

/* Device manager implementation.  */


static void
CreateSource (struct wl_client *client, struct wl_resource *resource,
	      uint32_t id)
{
  PDataSource *source;

  source = XLSafeMalloc (sizeof *source);

  if (!source)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (source, 0, sizeof *source);
  source->resource
    = wl_resource_create (client, &zwp_primary_selection_source_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!source->resource)
    {
      XLFree (source);
      wl_resource_post_no_memory (resource);
      return;
    }

  source->offers.next = &source->offers;
  source->offers.last = &source->offers;

  wl_resource_set_implementation (source->resource, &source_impl,
				  source, HandleSourceResourceDestroy);
}

static void
GetDevice (struct wl_client *client, struct wl_resource *resource,
	   uint32_t id, struct wl_resource *seat)
{
  PDataDevice *device;

  device = XLSafeMalloc (sizeof *device);

  if (!device)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (device, 0, sizeof *device);
  device->resource
    = wl_resource_create (client,
			  &zwp_primary_selection_device_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!device->resource)
    {
      XLFree (device);
      wl_resource_post_no_memory (resource);
      return;
    }

  device->seat = wl_resource_get_user_data (seat);

  if (XLSeatIsInert (device->seat))
    device->seat = NULL;
  else
    device->seat_destroy_key
      = XLSeatRunOnDestroy (device->seat, HandleSeatDestroy,
			    device);

  /* Link the device into the chain of all devices.  */
  device->next = all_devices.next;
  device->last = &all_devices;
  all_devices.next->last = device;
  all_devices.next = device;

  wl_resource_set_implementation (device->resource, &device_impl,
				  device, HandleDeviceResourceDestroy);

  /* If the primary selection is set and the client is focused, send
     the data offer now.  */
  if (primary_selection && device->seat
      && XLSeatIsClientFocused (device->seat, client))
    UpdateForSingleReference (device);
}

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct zwp_primary_selection_device_manager_v1_interface manager_impl =
  {
    .create_source = CreateSource,
    .get_device = GetDevice,
    .destroy = Destroy,
  };

static void
HandleBind (struct wl_client *client, void *data, uint32_t version,
	    uint32_t id)
{
  struct wl_resource *resource;

  resource
    = wl_resource_create (client,
			  &zwp_primary_selection_device_manager_v1_interface,
			  version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &manager_impl, NULL, NULL);
}

void
XLInitPrimarySelection (void)
{
  global_primary_selection_device_manager
    = wl_global_create (compositor.wl_display,
			&zwp_primary_selection_device_manager_v1_interface,
			1, NULL, HandleBind);
  all_devices.next = &all_devices;
  all_devices.last = &all_devices;
}
