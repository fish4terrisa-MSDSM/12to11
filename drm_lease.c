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
#include <string.h>
#include <stdio.h>

#include <sys/fcntl.h>

#include <xcb/randr.h>
#include <xcb/dri3.h>

#include <xf86drm.h>

#include "compositor.h"
#include "drm-lease-v1.h"

#if defined DEBUG
#define DebugPrint(format, args...)				\
  fprintf (stderr, "%s: " format "\n", __FUNCTION__, ## args)
#else
#define DebugPrint(fmt, ...) ((void) 0)
#endif

/* DRM Leasing.

   Modern applications can demand direct access to the kernel
   modesetting resources underlying an output.  Wayland exposes this
   functionality via the wp_drm_lease_device_v1 protocol.

   There is some mismatch between the X server terminology and the
   kernel speak used in Wayland and in the KMS/DRM APIs themselves.
   Under X, DRM nodes are named "providers", and DRM connectors are
   named "outputs".  This file uses a mix of both the the X
   terminology and the KMS/DRM terminology.  */

/* TODO: dynamic output updating.  */

typedef struct _DrmLeaseDevice DrmLeaseDevice;
typedef struct _DrmLeaseDeviceRef DrmLeaseDeviceRef;
typedef struct _DrmLeaseConnector DrmLeaseConnector;
typedef struct _DrmLeaseConnectorRef DrmLeaseConnectorRef;
typedef struct _DrmLeaseConnectorList DrmLeaseConnectorList;
typedef struct _DrmLeaseRequest DrmLeaseRequest;
typedef struct _DrmLease DrmLease;

typedef struct _ProviderOutputTree ProviderOutputTree;

enum
  {
    /* These flags are only used by outputs.  */
    InvalidConnectorID = 1,
    IsDisconnected     = (1 << 2),

    /* These flags are shared by both providers and outputs.  */
    IsMarked           = (1 << 3),
    IsRemoved	       = (1 << 4),

    /* These flags apply to both outputs and output references.  */
    IsWithdrawn        = (1 << 5),
  };

struct _DrmLeaseConnectorRef
{
  /* The next and last references to this connector.  */
  DrmLeaseConnectorRef *next, *last;

  /* The next and last global references to this connector.  */
  DrmLeaseConnectorRef *gcnext, *gclast;

  /* The associated connector.  */
  DrmLeaseConnector *connector;

  /* The resource associated with this connector.  */
  struct wl_resource *resource;

  /* Flags.  */
  int flags;
};

struct _DrmLeaseConnector
{
  /* The output associated with this connector.  */
  RROutput output;

  /* The CRTC associated with this connector.  */
  RRCrtc crtc;

  /* The connector ID and some flags.  */
  int connector_id, flags;

  /* The next and last outputs associated with this device.  */
  DrmLeaseConnector *next, *last;

  /* References to this connector.  */
  DrmLeaseConnectorRef references;

  /* The associated device.  */
  DrmLeaseDevice *device;

  /* The human readable name of this output.  */
  char *name;
};

struct _DrmLeaseDeviceRef
{
  /* The next and last references to this provider.  */
  DrmLeaseDeviceRef *next, *last;

  /* The next and last global references to this provider.  */
  DrmLeaseDeviceRef *gcnext, *gclast;

  /* The referenced device.  */
  DrmLeaseDevice *device;

  /* The wl_resource associated with this reference.  */
  struct wl_resource *resource;
};

struct _DrmLeaseDevice
{
  /* The struct wl_global associated with this provider.  */
  struct wl_global *global;

  /* Any references to this provider.  */
  DrmLeaseDeviceRef references;

  /* The provider associated with this provider.  */
  RRProvider provider;

  /* The next and last devices in this list.  */
  DrmLeaseDevice *next, *last;

  /* The file descriptor of this provider, and some flags.  */
  int fd, flags;

  /* The outputs attached to this provider.  */
  DrmLeaseConnector outputs;
};

struct _DrmLeaseConnectorList
{
  /* The next and last connectors in this list.  */
  DrmLeaseConnectorList *next, *last;

  /* The output.  */
  DrmLeaseConnector *connector;
};

struct _DrmLeaseRequest
{
  /* List of requested outputs.  */
  DrmLeaseConnectorList outputs;

  /* The next and last lease requests.  */
  DrmLeaseRequest *gcnext, *gclast;

  /* The request device.  */
  DrmLeaseDevice *device;

  /* The struct wl_resource associated with this lease request.  */
  struct wl_resource *resource;

  /* The number of outputs requested.  */
  int noutputs;
};

struct _DrmLease
{
  /* The XID of the lease.  */
  xcb_randr_lease_t lease;

  /* The resource of the lease.  */
  struct wl_resource *resource;
};

struct _ProviderOutputTree
{
  /* List of provider IDs.  */
  xcb_randr_provider_t *providers;

  /* List of outputs associated with each of the providers.  */
  xcb_randr_output_t *outputs;

  /* List of output info associated with each of the providers.  */
  xcb_randr_get_output_info_reply_t **output_info;

  /* Number of outputs and crtcs associated with each provider.  */
  int *nconnectors;

  /* Number of providers.  */
  int nproviders;

  /* When the tree data was found.  */
  Time timestamp;
};

/* List of all providers.  */
static DrmLeaseDevice all_devices;

/* List of all provider references.  */
static DrmLeaseDeviceRef all_device_references;

/* List of all connector references.  */
static DrmLeaseConnectorRef all_connector_references;

/* List of all lease requests.  */
static DrmLeaseRequest all_lease_requests;

/* The last time the provider info was updated.  */
static Time last_change_time;

static void
DeleteConnector (DrmLeaseConnector *connector)
{
  /* There should be no more references at this point.  */
  XLAssert (connector->references.next == &connector->references);

  /* Free the name.  */
  XLFree (connector->name);

  DebugPrint ("destroying connector %p (crtc %lu output %lu)",
	      connector, connector->crtc, connector->output);

  /* Unlink the connector.  */
  connector->next->last = connector->last;
  connector->last->next = connector->next;

  /* Free the connector.  */
  XLFree (connector);
  return;
}

static void
DeleteDevice (DrmLeaseDevice *device)
{
  /* There should be no more connectors at this point.  */
  XLAssert (device->outputs.next == &device->outputs);

  /* device->global must be gone as well.  */
  XLAssert (device->global == NULL);

  DebugPrint ("destroying device %p (%lu) w/ fd %d",
	      device, device->provider, device->fd);

  /* Close the fd.  */
  close (device->fd);
}

/* Connector and device "garbage collection".

   Managing the complicated reference cycles between connectors
   resources, outputs, device resources and providers is a tricky
   business.  Every time a resource is destroyed, we mark each
   provider and output referenced from Wayland resources, and if there
   are no more references to a dead provider or output, destroy
   them.  */

static void
CollectDeadResources (void)
{
  DrmLeaseDeviceRef *device_ref;
  DrmLeaseConnectorRef *connector_ref;
  DrmLeaseRequest *request;
  DrmLeaseConnectorList *item;
  DrmLeaseDevice *device, *last_device;
  DrmLeaseConnector *connector, *last_connector;

  DebugPrint ("collecting dead resources");

  /* Mark all provider references.  */
  device_ref = all_device_references.gcnext;
  while (device_ref != &all_device_references)
    {
      /* Mark the device referenced.  */
      device_ref->device->flags |= IsMarked;

      /* Move to the next device.  */
      device_ref = device_ref->gcnext;
    }

  /* Mark all connector references.  */
  connector_ref = all_connector_references.gcnext;
  while (connector_ref != &all_connector_references)
    {
      DebugPrint ("marked via connector: connector %p, device %p (%lu)",
		  connector_ref->connector,
		  connector_ref->connector->device,
		  connector_ref->connector->device->provider);

      /* Mark the connector and device referenced.  */
      connector_ref->connector->flags |= IsMarked;
      connector_ref->connector->device->flags |= IsMarked;

      /* Move to the next connector reference.  */
      connector_ref = connector_ref->gcnext;
    }

  /* Mark all lease requests.  */
  request = all_lease_requests.gcnext;
  while (request != &all_lease_requests)
    {
      /* Mark each referenced connector.  */
      item = request->outputs.next;
      while (item != &request->outputs)
	{
	  DebugPrint ("marked via req: connector %p, device %p (%lu)",
		      item->connector, item->connector->device,
		      item->connector->device->provider);

	  item->connector->flags |= IsMarked;
	  item->connector->device->flags |= IsMarked;

	  item = item->next;
	}

      /* Move to the next lease request.  */
      request = request->gcnext;
    }

  /* Now, judge each device's connectors and then the device
     itself.  */
  device = all_devices.next;
  while (device != &all_devices)
    {
      DebugPrint ("judging device %p", device);

      /* Do the connectors first.  If the device is not marked, then
	 there should be no marked connectors at all, but if it is,
	 then any dead connectors must be removed.  */
      connector = device->outputs.next;
      while (connector != &device->outputs)
	{
	  DebugPrint ("judging connector %p of device %p", connector,
		      connector->device);

	  XLAssert (connector->device == device);

	  if (!(device->flags & IsMarked))
	    XLAssert (!(connector->flags & IsMarked));

	  last_connector = connector;
	  connector = connector->next;

	  /* If the connector is no longer marked and also dead,
	     remove it.  */
	  if (!(last_connector->flags & IsMarked))
	    {
	      DebugPrint ("connector %lu %lu is no longer marked",
			  last_connector->output, last_connector->crtc);

	      if (last_connector->flags & IsRemoved)
		/* Delete the connector.  */
		DeleteConnector (last_connector);
	      else
		/* The connector is still alive.  */
		DebugPrint ("not removing live connector");
	    }
	  else
	    /* Clear the marked flag.  */
	    last_connector->flags &= ~IsMarked;
	}

      last_device = device;
      device = device->next;

      /* Now, consider the device.  */
      if (!(device->flags & IsMarked))
	{
	  DebugPrint ("device %p (%lu) is no longer marked",
		      device, device->provider);

	  if (device->flags & IsRemoved)
	    DeleteDevice (device);
	  else
	    DebugPrint ("not removing live device");
	}
      else
	last_device->flags &= ~IsMarked;
    }
}



static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct wp_drm_lease_connector_v1_interface drm_lease_connector_impl =
  {
    .destroy = Destroy,
  };

static void
HandleConnectorResourceDestroy (struct wl_resource *resource)
{
  DrmLeaseConnectorRef *ref;

  ref = wl_resource_get_user_data (resource);
  ref->last->next = ref->next;
  ref->next->last = ref->last;
  ref->gcnext->gclast = ref->gclast;
  ref->gclast->gcnext = ref->gcnext;

  XLFree (ref);
  CollectDeadResources ();
}



static void
RequestConnector (struct wl_client *client, struct wl_resource *resource,
		  struct wl_resource *connector_resource)
{
  DrmLeaseRequest *request;
  DrmLeaseConnectorRef *ref;
  DrmLeaseConnector *connector;
  DrmLeaseConnectorList *list;

  request = wl_resource_get_user_data (resource);
  ref = wl_resource_get_user_data (connector_resource);
  connector = ref->connector;

  if (connector->device != request->device)
    {
      wl_resource_post_error (resource,
			      WP_DRM_LEASE_REQUEST_V1_ERROR_WRONG_DEVICE,
			      "the specified connector is on a different device");
      return;
    }

#define DuplicateConnector					\
  WP_DRM_LEASE_REQUEST_V1_ERROR_DUPLICATE_CONNECTOR

  /* See if the connector has already been added.  */
  list = request->outputs.next;
  while (list != &request->outputs)
    {
      if (connector == list->connector)
	{
	  wl_resource_post_error (resource, DuplicateConnector,
				  "the same connector got attached twice");
	  return;
	}

      list = list->next;
    }

#undef DuplicateConnector

  DebugPrint ("requesting connector %p", connector);

  /* Insert the connector into the list.  */
  list = XLCalloc (1, sizeof *list);
  list->next = request->outputs.next;
  list->last = &request->outputs;
  list->connector = connector;
  request->outputs.next->last = list;
  request->outputs.next = list;
  request->noutputs++;
}



static void
DestroyLease (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wp_drm_lease_v1_interface drm_lease_impl =
  {
    .destroy = DestroyLease,
  };

static void
HandleLeaseResourceDestroy (struct wl_resource *resource)
{
  DrmLease *lease;
  xcb_void_cookie_t cookie;
  xcb_generic_error_t *error;

  lease = wl_resource_get_user_data (resource);

  /* Cancel the lease.  */
  if (lease->lease)
    {
      cookie = xcb_randr_free_lease_checked (compositor.conn,
					     lease->lease, 1);
      error = xcb_request_check (compositor.conn, cookie);

      if (error)
	{
	  DebugPrint ("rid: %"PRIu32", minor: %"PRIu16", major: %"PRIu8", "
		      "error: %"PRIu8, error->resource_id, error->minor_code,
		      error->major_code, error->error_code);
	  free (error);
	}
    }

  /* Free the rec.  */
  XLFree (lease);
  CollectDeadResources ();
}

static void
Submit (struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
  DrmLeaseRequest *request;
  xcb_randr_crtc_t *crtcs;
  xcb_randr_output_t *outputs;
  xcb_randr_lease_t lease_id;
  DrmLease *lease;
  DrmLeaseConnectorList *item;
  int i, *fds;
  xcb_randr_create_lease_cookie_t cookie;
  xcb_randr_create_lease_reply_t *reply;
  xcb_generic_error_t *error;

  request = wl_resource_get_user_data (resource);

  /* If the lease request is empty, post that error.  */
  if (request->outputs.next == &request->outputs)
    {
      wl_resource_post_error (resource,
			      WP_DRM_LEASE_REQUEST_V1_ERROR_EMPTY_LEASE,
			      "trying to lease without specifying connectors");
      return;
    }

  lease = XLSafeMalloc (sizeof *lease);
  error = NULL;

  if (!lease)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  /* Create the lease resource.  */
  memset (lease, 0, sizeof *lease);
  lease->resource = wl_resource_create (client, &wp_drm_lease_v1_interface,
					wl_resource_get_version (resource),
					id);

  if (!lease->resource)
    {
      XLFree (lease);
      wl_resource_post_no_memory (resource);
      return;
    }

  /* Populate crtcs and outputs.  */
  crtcs = alloca (sizeof *crtcs * request->noutputs);
  outputs = alloca (sizeof *outputs * request->noutputs);
  item = request->outputs.next;
  i = 0;
  reply = NULL;

  while (item != &request->outputs)
    {
      if (item->connector->flags & IsRemoved)
	{
	  DebugPrint ("removed connector was used in lease request");

	  /* The connector was removed.  This means it can no longer
	     be leased.  */
	  wl_resource_set_implementation (lease->resource, &drm_lease_impl,
					  lease, HandleLeaseResourceDestroy);

	  /* Send failure and return.  */
	  wp_drm_lease_v1_send_finished (lease->resource);
	  return;
	}

      crtcs[i++] = item->connector->crtc;
      outputs[i - 1] = item->connector->output;

      DebugPrint ("adding output: %u crtc: %u",
		  outputs[i - 1], crtcs[i - 1]);

      item = item->next;
    }

  /* Do the lease.  Generate the resource ID for the lease.  */
  lease_id = xcb_generate_id (compositor.conn);

  /* Now, try to create the lease.  */
  cookie = xcb_randr_create_lease (compositor.conn,
				   DefaultRootWindow (compositor.display),
				   lease_id, request->noutputs,
				   request->noutputs, crtcs, outputs);
  reply = xcb_randr_create_lease_reply (compositor.conn, cookie, &error);

  /* Set the resource implementation now.  */
  wl_resource_set_implementation (lease->resource, &drm_lease_impl,
				  lease, HandleLeaseResourceDestroy);

  if (!reply)
    {
      DebugPrint ("lease failure");

      if (error)
	DebugPrint ("rid: %"PRIu32", minor: %"PRIu16", major: %"PRIu8", "
		    "error: %"PRIu8, error->resource_id, error->minor_code,
		    error->major_code, error->error_code);

      /* Send failure.  */
      wp_drm_lease_v1_send_finished (lease->resource);

      if (error)
	free (error);
    }
  else
    {
      fds = xcb_randr_create_lease_reply_fds (compositor.conn, reply);

      if (!fds)
	/* Obtaining the reply fds failed.  */
	wp_drm_lease_v1_send_finished (lease->resource);
      else
	{
	  /* Send the lease file descriptor.  */
	  wp_drm_lease_v1_send_lease_fd (lease->resource, fds[0]);
	  close (fds[0]);
	}

      /* Set the lease resource.  */
      lease->lease = lease_id;

      /* Free the reply.  */
      free (reply);
    }
}

static struct wp_drm_lease_request_v1_interface drm_lease_request_impl =
  {
    .request_connector = RequestConnector,
    .submit = Submit,
  };

static void
HandleRequestResourceDestroy (struct wl_resource *resource)
{
  DrmLeaseRequest *request;
  DrmLeaseConnectorList *item, *last;

  request = wl_resource_get_user_data (resource);

  /* Free each element of the connector list.  */
  item = request->outputs.next;
  while (item != &request->outputs)
    {
      last = item;
      item = item->next;

      XLFree (last);
    }

  /* Remove the request from the live request list.  */
  request->gclast->gcnext = request->gcnext;
  request->gcnext->gclast = request->gclast;

  /* Free the request itself.  */
  XLFree (request);
}



static void
CreateLeaseRequest (struct wl_client *client, struct wl_resource *resource,
		    uint32_t id)
{
  DrmLeaseRequest *request;
  DrmLeaseDeviceRef *ref;
  DrmLeaseDevice *device;

  request = XLSafeMalloc (sizeof *request);

  if (!request)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (request, 0, sizeof *request);
  request->resource
    = wl_resource_create (client, &wp_drm_lease_request_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!request->resource)
    {
      XLFree (request);
      return;
    }

  ref = wl_resource_get_user_data (resource);
  device = ref->device;

  /* Initialize the list of DRM connectors.  */
  request->outputs.next = &request->outputs;
  request->outputs.last = &request->outputs;

  /* Set the device.  */
  request->device = device;

  /* Link the device onto the list of references.  */
  request->gcnext = all_lease_requests.gcnext;
  request->gclast = &all_lease_requests;
  all_lease_requests.gcnext->gclast = request;
  all_lease_requests.gcnext = request;

  /* Set the implementation.  */
  wl_resource_set_implementation (request->resource, &drm_lease_request_impl,
				  request, HandleRequestResourceDestroy);
}

static void
Release (struct wl_client *client, struct wl_resource *resource)
{
  /* Release the resource, but not before sending a `released'
     event.  */
  wp_drm_lease_device_v1_send_released (resource);
  wl_resource_destroy (resource);
}

static const struct wp_drm_lease_device_v1_interface drm_lease_device_impl =
  {
    .release = Release,
    .create_lease_request = CreateLeaseRequest,
  };

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  DrmLeaseDeviceRef *ref;

  ref = wl_resource_get_user_data (resource);
  ref->last->next = ref->next;
  ref->next->last = ref->last;
  ref->gcnext->gclast = ref->gclast;
  ref->gclast->gcnext = ref->gcnext;

  XLFree (ref);
  CollectDeadResources ();
}

static DrmLeaseConnectorRef *
AddConnectorRef (DrmLeaseConnector *connector,
		 DrmLeaseDeviceRef *ref)
{
  DrmLeaseConnectorRef *connector_ref;
  struct wl_client *client;

  client = wl_resource_get_client (ref->resource);
  connector_ref = XLCalloc (1, sizeof *connector_ref);
  connector_ref->resource
    = wl_resource_create (client,
			  &wp_drm_lease_connector_v1_interface,
			  wl_resource_get_version (ref->resource),
			  0);

  if (!connector_ref->resource)
    {
      fprintf (stderr, "failed to allocate output ref\n");
      abort ();
    }

  connector_ref->connector = connector;
  connector_ref->next = connector->references.next;
  connector_ref->last = &connector->references;
  connector_ref->gcnext = all_connector_references.gcnext;
  connector_ref->gclast = &all_connector_references;
  all_connector_references.gcnext->gclast = connector_ref;
  all_connector_references.gcnext = connector_ref;
  connector->references.next->last = connector_ref;
  connector->references.next = connector_ref;
  wl_resource_set_implementation (connector_ref->resource,
				  &drm_lease_connector_impl,
				  connector_ref,
				  HandleConnectorResourceDestroy);

  return connector_ref;
}

static void
SendOutputs (DrmLeaseDevice *device, DrmLeaseDeviceRef *ref)
{
  DrmLeaseConnector *connector;
  DrmLeaseConnectorRef *connector_ref;
  char buf[sizeof "xxxxxxxxxx" + 1];

  connector = device->outputs.next;
  while (connector != &device->outputs)
    {
      if (!(connector->flags & IsDisconnected)
	  && !(connector->flags & InvalidConnectorID)
	  && !(connector->flags & IsRemoved))
	{
	  connector_ref = AddConnectorRef (connector, ref);
	  wp_drm_lease_device_v1_send_connector (ref->resource,
						 connector_ref->resource);

	  DebugPrint ("sending connector %lu:%lu to %p",
		      connector->output, connector->crtc, ref);

	  /* Send the connector id.  */
	  wp_drm_lease_connector_v1_send_connector_id (connector_ref->resource,
						       connector->connector_id);

	  /* Send the unique connector name.  */
	  sprintf (buf, "%d", connector->connector_id);
	  wp_drm_lease_connector_v1_send_name (connector_ref->resource, buf);

	  /* Send the connector description.  */
	  wp_drm_lease_connector_v1_send_description (connector_ref->resource,
						      connector->name);

	  /* Send done.  */
	  wp_drm_lease_connector_v1_send_done (connector_ref->resource);
	}

      connector = connector->next;
    }
}

static void
HandleBind (struct wl_client *client, void *data, uint32_t version,
	    uint32_t id)
{
  DrmLeaseDeviceRef *ref;
  DrmLeaseDevice *device;

  ref = XLSafeMalloc (sizeof *ref);
  device = data;

  if (!ref)
    {
      wl_client_post_no_memory (client);
      return;
    }

  memset (ref, 0, sizeof *ref);
  ref->resource = wl_resource_create (client,
				      &wp_drm_lease_device_v1_interface,
				      version, id);

  if (!ref->resource)
    {
      XLFree (ref);
      wl_client_post_no_memory (client);
      return;
    }

  ref->next = device->references.next;
  ref->last = &device->references;
  ref->device = device;
  device->references.next->last = ref;
  device->references.next = ref;
  ref->gcnext = all_device_references.gcnext;
  ref->gclast = &all_device_references;
  all_device_references.gcnext->gclast = ref;
  all_device_references.gcnext = ref;

  wl_resource_set_implementation (ref->resource, &drm_lease_device_impl,
				  ref, HandleResourceDestroy);

  DebugPrint ("sending fd %d to %p", device->fd, ref);

  /* Send the drm_fd to the client.  */
  wp_drm_lease_device_v1_send_drm_fd (ref->resource, device->fd);

  /* Send each output.  */
  SendOutputs (device, ref);

  /* Send done.  */
  wp_drm_lease_device_v1_send_done (ref->resource);
}

static DrmLeaseDevice *
AddProvider (RRProvider provider)
{
  DrmLeaseDevice *device;
  xcb_dri3_open_cookie_t cookie;
  xcb_dri3_open_reply_t *reply;
  int *fds, fd, new;
  xcb_generic_error_t *error;
  char *name;

  error = NULL;

  device = XLCalloc (1, sizeof *device);
  device->references.next = &device->references;
  device->references.last = &device->references;

  /* Add the given provider.  Obtain the file descriptor it is
     associated with.  */
  cookie = xcb_dri3_open (compositor.conn,
			  DefaultRootWindow (compositor.display),
			  provider);
  reply = xcb_dri3_open_reply (compositor.conn, cookie, &error);

  if (!reply)
    goto error;

  fds = xcb_dri3_open_reply_fds (compositor.conn, reply);

  if (!fds)
    {
      free (reply);
      goto error;
    }

  fd = fds[0];

  if (drmGetNodeTypeFromFd (fd) != DRM_NODE_RENDER)
    {
      name = drmGetDeviceNameFromFd2 (fd);

      if (name)
	{
	  DebugPrint ("device name is %s", name);
	  new = open (name, O_RDWR);

	  if (new >= 0)
	    {
	      if (drmIsMaster (fd))
		drmDropMaster (fd);

	      /* Close the old file descriptor.  */
	      close (fd);
	      fd = new;
	    }
	  else
	    DebugPrint ("failed to open device");

	  free (name);
	}
    }

  device->fd = fd;

  DebugPrint ("obtained provider %lu's fd %d", provider, fd);

  free (reply);

  /* Set the provider.  */
  device->provider = provider;

  /* Create the global.  */
  device->global = wl_global_create (compositor.wl_display,
				     &wp_drm_lease_device_v1_interface,
				     1, device, HandleBind);

  /* Chain the provider onto the list of all devices.  */
  device->next = all_devices.next;
  device->last = &all_devices;
  all_devices.next->last = device;
  all_devices.next = device;

  /* Initialize the device's connector list.  */
  device->outputs.next = &device->outputs;
  device->outputs.last = &device->outputs;

  return device;

 error:
  if (error)
    free (error);

  XLFree (device);
  return NULL;
}

static DrmLeaseConnector *
AddOutput (DrmLeaseDevice *device, RROutput output, RRCrtc crtc,
	   XRROutputInfo *info)
{
  DrmLeaseConnector *connector;
  int rc, actual_format;
  Atom actual_type;
  unsigned long nitems, bytes_after;
  unsigned char *data;

  connector = XLCalloc (1, sizeof *connector);
  connector->output = output;
  connector->crtc = crtc;
  connector->name = XLStrdup (info->name);

  /* Try to determine the connector ID.  */
  data = NULL;

  CatchXErrors ();
  rc = XRRGetOutputProperty (compositor.display, output,
			     CONNECTOR_ID, 0, 1, False,
			     False, XA_INTEGER, &actual_type,
			     &actual_format, &nitems, &bytes_after,
			     &data);
  UncatchXErrors (NULL);

  if (rc != Success || !data || actual_format != 32
      || nitems < 1 || actual_type != XA_INTEGER)
    {
      if (data)
	XFree (data);

      /* Mark this connector as invalid.  */
      connector->flags |= InvalidConnectorID;
      DebugPrint ("invalid connector id");
    }
  else
    {
      /* Set the connector ID.  */
      connector->connector_id = *(unsigned long *) data;
      DebugPrint ("connector ID is %d", connector->connector_id);
    }

  if (info->connection == RR_Disconnected)
    connector->flags |= IsDisconnected;

  connector->references.next = &connector->references;
  connector->references.last = &connector->references;

  /* Link the output onto the device's output list.  */
  connector->next = device->outputs.next;
  connector->last = &device->outputs;
  connector->device = device;
  device->outputs.next->last = connector;
  device->outputs.next = connector;

  if (data)
    XFree (data);

  return connector;
}

static void
InitializeProviderOutputs (void)
{
  XRRProviderInfo *info;
  XRRScreenResources *screen_resources;
  Window root;
  DrmLeaseDevice *device;
  int i;
  XRROutputInfo *output;

  root = DefaultRootWindow (compositor.display);
  screen_resources = XRRGetScreenResources (compositor.display,
					    root);

  device = all_devices.next;
  while (device != &all_devices)
    {
      CatchXErrors ();
      info = XRRGetProviderInfo (compositor.display,
				 screen_resources,
				 device->provider);
      UncatchXErrors (NULL);

      DebugPrint ("provider info: %p", info);

      if (!info)
	goto next;

      DebugPrint ("obtained provider info %lu; cap: %u"
		  " ncrtcs: %d noutputs %d",
		  device->provider, info->capabilities,
		  info->ncrtcs, info->noutputs);

      /* Now loop through each output.  */
      for (i = 0; i < info->noutputs; ++i)
	{
	  /* Try to obtain the output info.  */
	  CatchXErrors ();
	  output = XRRGetOutputInfo (compositor.display,
				     screen_resources,
				     info->outputs[i]);
	  UncatchXErrors (NULL);

	  DebugPrint ("obtained output %i %lu %p", i,
		      info->outputs[i], output);

	  if (!output)
	    continue;

	  DebugPrint ("output %s crtc is %lu", output->name,
		      output->crtc);
	  AddOutput (device, info->outputs[i], output->crtc,
		     output);
	  XRRFreeOutputInfo (output);
	}

      XRRFreeProviderInfo (info);
    next:
      device = device->next;
    }

  XRRFreeScreenResources (screen_resources);
}

static void
InitializeProviderList (void)
{
  Window root;
  XRRProviderResources *resources;
  int i;

  root = DefaultRootWindow (compositor.display);
  resources = XRRGetProviderResources (compositor.display, root);

  DebugPrint ("providers: %d", resources->nproviders);

  for (i = 0; i < resources->nproviders; ++i)
    AddProvider (resources->providers[i]);

  XRRFreeProviderResources (resources);

  DebugPrint ("initializing outputs");
  InitializeProviderOutputs ();
}

static ProviderOutputTree *
BuildProviderTree (void)
{
  ProviderOutputTree *tree;
  xcb_randr_get_providers_cookie_t cookie;
  xcb_randr_get_providers_reply_t *reply;
  xcb_randr_get_provider_info_cookie_t *cookies;
  xcb_randr_get_provider_info_reply_t **replies;
  xcb_randr_get_output_info_cookie_t *output_cookies;
  xcb_randr_get_output_info_reply_t **output_replies;
  int i, noutputs, j, num_outputs, k;
  xcb_timestamp_t reply_timestamp;
  Window root;
  xcb_randr_output_t *output_ptr, *outputs;
  xcb_randr_get_output_info_reply_t **output_info_ptr;
  xcb_generic_error_t *error;

  tree = XLCalloc (1, sizeof *tree);
  root = DefaultRootWindow (compositor.display);

  /* Now, query for all providers.  */
  cookie = xcb_randr_get_providers (compositor.conn, root);
  reply = xcb_randr_get_providers_reply (compositor.conn, cookie,
					 NULL);

  if (!reply)
    abort ();

  /* Obtain the providers.  */
  tree->nproviders = xcb_randr_get_providers_providers_length (reply);
  tree->providers = XLCalloc (tree->nproviders, sizeof *tree->providers);
  memcpy (tree->providers, xcb_randr_get_providers_providers (reply),
	  sizeof *tree->providers * tree->nproviders);

  /* Record the timestamp.  */
  reply_timestamp = reply->timestamp;
  tree->timestamp = reply_timestamp;

  /* Free the reply.  */
  free (reply);

  /* Now that we know how many providers there are, look at all the
     outputs for each provider.  */
  cookies = alloca (sizeof *cookies * tree->nproviders);
  replies = alloca (sizeof *replies * tree->nproviders);

  /* Satisfy -Wanalyzer-use-of-uninitialized-value 13 lines below.
     Where is the uninitialized value?  */
  memset (cookies, 0, sizeof *cookies * tree->nproviders);
  memset (replies, 0, sizeof *replies * tree->nproviders);

  for (i = 0; i < tree->nproviders; i++)
    cookies[i] = xcb_randr_get_provider_info (compositor.conn,
					      tree->providers[i],
					      reply_timestamp);
  noutputs = 0;

  for (i = 0; i < tree->nproviders; i++)
    {
      error = NULL;
      replies[i] = xcb_randr_get_provider_info_reply (compositor.conn,
						      cookies[i], &error);
      if (error)
	free (error);

      if (replies[i])
	/* Set the number of outputs.  */
	noutputs += xcb_randr_get_provider_info_outputs_length (replies[i]);
    }

  /* Retrieve the output info for each provider.  It is too hard to
     reason about doing this asychronously across providers, so we
     sync at the end of each processing outputs for each provider
     despite there being no hard data dependency there.  */
  tree->outputs = XLCalloc (noutputs, sizeof *tree->outputs);
  tree->output_info = XLCalloc (noutputs, sizeof *tree->output_info);
  tree->nconnectors = XLCalloc (tree->nproviders,
				sizeof *tree->nconnectors);
  output_ptr = tree->outputs;
  output_info_ptr = tree->output_info;

  for (i = 0, j = 0; i < tree->nproviders; ++i)
    {
      if (!replies[i])
	continue;

      num_outputs = xcb_randr_get_provider_info_outputs_length (replies[i]);

      DebugPrint ("num_outputs: %d", num_outputs);

      outputs = xcb_randr_get_provider_info_outputs (replies[i]);
      output_cookies = alloca (num_outputs * sizeof *output_cookies);
      output_replies = alloca (num_outputs * sizeof *output_replies);

      for (k = 0; k < num_outputs; ++k)
	output_cookies[k] = xcb_randr_get_output_info (compositor.conn,
						       outputs[k],
						       reply_timestamp);

      for (k = 0; k < num_outputs; ++k)
	{
	  error = NULL;
	  output_replies[k]
	    = xcb_randr_get_output_info_reply (compositor.conn,
					       output_cookies[k],
					       &error);

	  if (error)
	    free (error);

	  if (!output_replies[k])
	    continue;

	  tree->nconnectors[j] += 1;
	  DebugPrint ("nconnectors[%d] became: %d",
		      j, tree->nconnectors[j]);

	  /* Record the output and output info.  */
	  XLAssert (output_ptr < tree->outputs + noutputs);

	  *output_ptr++ = outputs[k];
	  *output_info_ptr++ = output_replies[k];
	}

      /* Free the provider info.  */
      free (replies[i]);

      j++;
    }

  /* Return the resulting tree.  */
  return tree;
}

static void
FreeProviderTree (ProviderOutputTree *tree)
{
  int i, j, k;

  XLFree (tree->providers);
  XLFree (tree->outputs);

  /* Free all output info.  */
  for (i = 0, j = 0; i < tree->nproviders; ++i)
    {
      for (k = 0; k < tree->nconnectors[i]; ++k)
	free (tree->output_info[j + k]);
      j += k;
    }

  XLFree (tree->output_info);
  XLFree (tree->nconnectors);
  XLFree (tree);
}

static DrmLeaseDevice *
FindProvider (RRProvider id)
{
  DrmLeaseDevice *device;

  device = all_devices.next;
  while (device != &all_devices)
    {
      if (!(device->flags & IsRemoved)
	  && device->provider == id)
	return device;
    }

  return NULL;
}

static void
RemoveDevice (DrmLeaseDevice *device)
{
  /* Mark the device as invalid and free its fd and global.  The
     device itself will be destroyed once no more references to it
     exist from clients.  */
  device->flags |= IsRemoved;
  close (device->fd);
  wl_global_destroy (device->global);
}

static void
RemoveConnector (DrmLeaseConnector *connector)
{
  DrmLeaseConnectorRef *ref;

  /* Mark the output as removed.  */
  connector->flags |= IsRemoved | IsWithdrawn;

  /* Withdraw each of the references.  */
  ref = connector->references.next;
  while (ref != &connector->references)
    {
      if (!(ref->flags & IsWithdrawn))
	wp_drm_lease_connector_v1_send_withdrawn (ref->resource);

      ref->flags |= IsWithdrawn;
      ref = ref->next;
    }
}

static void
WithdrawConnector (DrmLeaseConnector *connector)
{
  DrmLeaseConnectorRef *ref;

  if (connector->flags & IsWithdrawn)
    return;

  connector->flags |= IsWithdrawn;

  /* Withdraw each of the references.  */
  ref = connector->references.next;
  while (ref != &connector->references)
    {
      if (!(ref->flags & IsWithdrawn))
	wp_drm_lease_connector_v1_send_withdrawn (ref->resource);

      ref->flags |= IsWithdrawn;
      ref = ref->next;
    }
}

static void
SendConnectorToClients (DrmLeaseConnector *connector)
{
  DrmLeaseConnectorRef *ref;
  DrmLeaseDeviceRef *device_ref;
  char buf[sizeof "xxxxxxxxxx" + 1];

  XLAssert (!(connector->flags & IsRemoved));

  connector->flags &= ~IsWithdrawn;

  device_ref = connector->device->references.next;
  while (device_ref != &connector->device->references)
    {
      ref = AddConnectorRef (connector, device_ref);

      wp_drm_lease_device_v1_send_connector (device_ref->resource,
					     ref->resource);

      /* Send the connector id.  */
      wp_drm_lease_connector_v1_send_connector_id (ref->resource,
						   connector->connector_id);

      /* Send the unique connector name.  */
      sprintf (buf, "%d", connector->connector_id);
      wp_drm_lease_connector_v1_send_name (ref->resource, buf);

      /* Send the connector description.  */
      wp_drm_lease_connector_v1_send_description (ref->resource,
						  connector->name);

      /* Send done.  */
      wp_drm_lease_connector_v1_send_done (ref->resource);

      device_ref = device_ref->next;
    }
}

static DrmLeaseConnector *
FindOutput (DrmLeaseDevice *device, RROutput id)
{
  DrmLeaseConnector *connector;

  connector = device->outputs.next;
  while (connector != &device->outputs)
    {
      if (connector->output == id)
	return connector;

      connector = connector->next;
    }

  return NULL;
}

static void
HandleSingleProvider (ProviderOutputTree *tree, int index,
		      int connector_offset)
{
  RRProvider provider;
  xcb_randr_output_t *outputs;
  xcb_randr_get_output_info_reply_t **info;
  DrmLeaseDevice *device;
  int i, name_length;
  XRROutputInfo outputinfo;
  DrmLeaseConnector *connector;
  DrmLeaseDeviceRef *ref;

  provider = tree->providers[index];
  outputs = tree->outputs + connector_offset;
  info = tree->output_info + connector_offset;

  /* Try to find an existing provider.  */
  device = FindProvider (provider);

  /* If there is no existing provider, then add the new device.  */
  if (!device)
    {
      DebugPrint ("adding provider for provider %lu", provider);

      device = AddProvider (provider);

      /* Add all the outputs.  */
      for (i = 0; i < tree->nconnectors[index]; ++i)
	{
	  name_length = xcb_randr_get_output_info_name_length (*info);

	  outputinfo.connection = (*info)->connection;
	  outputinfo.name = XLMalloc (name_length + 1);
	  memcpy (outputinfo.name,
		  xcb_randr_get_output_info_name (*info),
		  name_length);
	  outputinfo.name[name_length] = '\0';

	  DebugPrint ("adding output named %s", outputinfo.name);

	  /* It seems a little wrong to use a fake output info
	     structure.  */
	  AddOutput (device, *outputs, (*info)->crtc, &outputinfo);

	  XLFree (outputinfo.name);
	  outputs++;
	  info++;
	}
    }
  else
    {
      DebugPrint ("provider %p found", device);

      /* Otherwise, compare the outputs of the provider with what is
	 currently present.  First, remove each output that is not
	 still present.  */

      connector = device->outputs.next;
      while (connector != &device->outputs)
	{
	  if (connector->flags & IsRemoved)
	    continue;

	  for (i = 0; i < tree->nconnectors[index]; ++i)
	    {
	      DebugPrint ("consideration: %p %"PRIu32" %lu", connector,
			  outputs[i], connector->output);

	      if (outputs[i] == connector->output)
		goto next;
	    }

	  DebugPrint ("removing connector %p", connector);

	  /* Remove the connector.  */
	  RemoveConnector (connector);

	next:
	  connector = connector->next;
	}

      /* Next, look through each output.  */
      for (i = 0; i < tree->nconnectors[index]; ++i)
	{
	  connector = FindOutput (device, outputs[i]);

	  if (!connector)
	    {
	      /* If the connector does not exist, add it.  */
	      name_length = xcb_randr_get_output_info_name_length (info[i]);

	      outputinfo.connection = info[i]->connection;
	      outputinfo.name = XLMalloc (name_length + 1);
	      memcpy (outputinfo.name,
		      xcb_randr_get_output_info_name (info[i]),
		      name_length);
	      outputinfo.name[name_length] = '\0';

	      /* It seems a little wrong to use a fake output info
		 structure.  */
	      connector = AddOutput (device, outputs[i], info[i]->crtc,
				     &outputinfo);
	      SendConnectorToClients (connector);
	      DebugPrint ("added output named %s", outputinfo.name);

	      XLFree (outputinfo.name);
	      continue;
	    }

	  DebugPrint ("updating existing connector %p", connector);

	  /* Otherwise, the connector already exists.  Compare it with
	     the new connector info and see what changed.  */
	  if (connector->flags & IsDisconnected
	      && info[i]->connection != XCB_RANDR_CONNECTION_DISCONNECTED)
	    {
	      /* The connector was previously disconnected, but not
		 anymore.  Send the connector to clients.  */
	      SendConnectorToClients (connector);

	      /* Update the flag.  */
	      connector->flags &= ~IsDisconnected;

	      DebugPrint ("output named %s was connected", connector->name);
	    }
	  else if (!(connector->flags & IsDisconnected)
		   && info[i]->connection == XCB_RANDR_CONNECTION_DISCONNECTED)
	    {
	      /* The connector was just disconnected.  Withdraw the
		 connector.  */
	      WithdrawConnector (connector);

	      /* Update the flag.  */
	      connector->flags |= IsDisconnected;

	      DebugPrint ("output named %s disconnected", connector->name);
	    }

	  /* Set the crtc.  */
	  connector->crtc = info[i]->crtc;
	}
    }

  /* Now send done to each device.  */
  ref = device->references.next;
  while (ref != &device->references)
    {
      wp_drm_lease_device_v1_send_done (ref->resource);
      ref = ref->next;
    }
}

static void
HandleOutputOrResourceChange (Time timestamp)
{
  ProviderOutputTree *tree;
  int i, connectors_read;
  DrmLeaseDevice *device;

  DebugPrint ("timestamp: %lu, last-change-time: %lu", timestamp,
	      last_change_time);

  if (timestamp != CurrentTime
      && (timestamp - last_change_time) <= 0
      /* If timestamp is 500 ms later, assume that the time
	 overflowed.  */
      && (timestamp - last_change_time) > -500)
    {
      DebugPrint ("rejecting outdated event");
      return;
    }

  /* Outputs or resources changed.  First, build a "provider-output
     tree" structure.  */
  tree = BuildProviderTree ();

  DebugPrint ("provider tree obtained with %d providers",
	      tree->nproviders);

  /* Afterwards, mark every provider that is no longer present as
     removed.  */
  device = all_devices.next;
  while (device != &all_devices)
    {
      if (device->flags & IsRemoved)
	goto next_device;

      for (i = 0; i < tree->nproviders; ++i)
	{
	  if (tree->providers[i] == device->provider)
	    goto next_device;
	}

      DebugPrint ("device %p was not found in tree",
		  device);

      /* Remove the device.  */
      RemoveDevice (device);

    next_device:
      device = device->next;
    }

  /* Next, compare each provider in the tree with the currently
     attached devices.  */

  connectors_read = 0;
  for (i = 0; i < tree->nproviders; ++i)
    {
      HandleSingleProvider (tree, i, connectors_read);
      connectors_read += tree->nconnectors[i];
    }

  /* Set the last change time.  */
  last_change_time = MAX (tree->timestamp, timestamp);

  /* Finally, free the provider tree.  */
  FreeProviderTree (tree);

  /* And collect dead resources.  */
  CollectDeadResources ();
}

void
XLInitDrmLease (void)
{
  xcb_randr_query_version_reply_t *reply;
  xcb_randr_query_version_cookie_t cookie;

  /* This shouldn't be freed.  */
  const xcb_query_extension_reply_t *ext;

  /* Initialize XRandR with XCB as well.  Version 1.6 of the extension
     must be available.  */
  ext = xcb_get_extension_data (compositor.conn, &xcb_randr_id);

  if (!ext || !ext->present)
    /* DRM leasing will not be supported.  */
    return;


  cookie = xcb_randr_query_version (compositor.conn, 1, 6);
  reply = xcb_randr_query_version_reply (compositor.conn,
					 cookie, NULL);

  if (!reply)
    return;

  if (reply->major_version < 1
      || (reply->major_version == 1
	  && reply->minor_version < 6))
    {
      free (reply);
      return;
    }

  /* Free the reply.  */
  free (reply);

  all_devices.next = &all_devices;
  all_devices.last = &all_devices;
  all_device_references.gclast = &all_device_references;
  all_device_references.gcnext = &all_device_references;
  all_connector_references.gcnext = &all_connector_references;
  all_connector_references.gclast = &all_connector_references;
  all_lease_requests.gcnext = &all_lease_requests;
  all_lease_requests.gclast = &all_lease_requests;

  /* Initialize the provider list.  */
  InitializeProviderList ();

  /* Add a hook that runs upon notification.  */
  XLOutputSetChangeFunction (HandleOutputOrResourceChange);
}
