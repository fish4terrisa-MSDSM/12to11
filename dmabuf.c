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

#include <sys/types.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drm_fourcc.h>

#include "compositor.h"

#include "linux-dmabuf-unstable-v1.h"

#include <xcb/dri3.h>

#include <X11/Xmd.h>
#include <X11/extensions/dri3proto.h>

typedef struct _DrmFormatInfo DrmFormatInfo;
typedef struct _BufferParams BufferParams;
typedef struct _Buffer Buffer;
typedef struct _TemporarySetEntry TemporarySetEntry;
typedef struct _FormatModifierPair FormatModifierPair;

enum
  {
    IsUsed	   = 1,
    IsCallbackData = (1 << 2),
  };

struct _TemporarySetEntry
{
  /* These fields mean the same as they do in the args to
     zwp_linux_buffer_params_v1_add.  */

  int fd;
  unsigned int plane_idx, offset, stride;
  uint32_t modifier_hi, modifier_lo;
};

struct _BufferParams
{
  /* Entries for each plane.  DRI3 only supports up to 4 planes.  */
  TemporarySetEntry entries[4];

  /* The struct wl_resource associated with this object.  */
  struct wl_resource *resource;

  /* Some flags.  */
  int flags;

  /* The width and height of the buffer that will be created.  */
  int width, height;
};

struct _Buffer
{
  /* The ExtBuffer associated with this buffer.  */
  ExtBuffer buffer;

  /* The RenderBuffer associated with this buffer.  */
  RenderBuffer render_buffer;

  /* The wl_resource corresponding to this buffer.  */
  struct wl_resource *resource;

  /* List of "destroy listeners" connected to this buffer.  */
  XLList *destroy_listeners;

  /* The width and height of this buffer.  */
  unsigned int width, height;

  /* The number of references to this buffer.  */
  int refcount;
};

struct _FormatModifierPair
{
  /* See the documentation of
     zwp_linux_dmabuf_feedback_v1::format_table for more details. */
  uint32_t format;
  uint32_t padding;
  uint64_t modifier;
};

/* The wl_global associated with linux-dmabuf-unstable-v1.  */
static struct wl_global *global_dmabuf;

/* File descriptor for the format table.  */
static int format_table_fd;

/* Size of the format table.  */
static ssize_t format_table_size;

/* Device node of the DRM device.  TODO: make this
   output-specific.  */
static dev_t drm_device_node;

/* DRM formats supported by the renderer.  */
static DrmFormat *supported_formats;

/* Number of formats.  */
static int n_drm_formats;

static void
CloseFdsEarly (BufferParams *params)
{
  int i;

  for (i = 0; i < ArrayElements (params->entries); ++i)
    {
      if (params->entries[i].fd != -1)
	close (params->entries[i].fd);
    }
}

static void
ReleaseBufferParams (BufferParams *params)
{
  /* Also close any fds that were set if this object was not yet
     used.  */
  if (!(params->flags & IsUsed))
    CloseFdsEarly (params);

  /* params should not be destroyed if it is being used as callback
     data.  */
  XLAssert (!(params->flags & IsCallbackData));

  XLFree (params);
}

static void
HandleParamsResourceDestroy (struct wl_resource *resource)
{
  BufferParams *params;

  params = wl_resource_get_user_data (resource);

  /* First, clear params->resource.  */
  params->resource = NULL;

  if (params->flags & IsCallbackData)
    /* If params is callback data, simply clear params->resource, and
       wait for a callback to be called.  */
    return;

  /* Next, destroy the params now, unless we are waiting for a buffer
     to be created, in which case it is necessary to free the buffer
     should the creation succeed.  */

  ReleaseBufferParams (params);
}

static void
DestroyBufferParams (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static int
ExistingModifier (BufferParams *params, uint32_t *current_hi,
		  uint32_t *current_lo)
{
  int i, count;

  /* Pacify -Wmaybe-uninitialized under -O2.  count is only non-zero
     if both return arguments are initialized, but GCC thinks
     otherwise.  */
  *current_hi = 0;
  *current_lo = 0;

  for (i = 0, count = 0; i < ArrayElements (params->entries); ++i)
    {
      if (params->entries[i].fd != -1)
	{
	  *current_hi = params->entries[i].modifier_hi;
	  *current_lo = params->entries[i].modifier_lo;

	  count++;
	}
    }

  return count;
}

static void
Add (struct wl_client *client, struct wl_resource *resource, int32_t fd,
     uint32_t plane_idx, uint32_t offset, uint32_t stride, uint32_t modifier_hi,
     uint32_t modifier_lo)
{
  BufferParams *params;
  uint32_t current_hi, current_lo;

  params = wl_resource_get_user_data (resource);

  if (params->flags & IsUsed)
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
			      "the given params resource has already been used");
      close (fd);

      return;
    }

  if (plane_idx >= ArrayElements (params->entries))
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
			      "maximum number of planes exceeded");
      close (fd);

      return;
    }

  if (params->entries[plane_idx].fd != -1)
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
			      "the plane has already been added in the temporary set");
      close (fd);

      return;
    }

  if (ExistingModifier (params, &current_hi, &current_lo)
      && (current_hi != modifier_hi || current_lo != modifier_lo))
    {
      wl_resource_post_error (resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
			      "modifier does not match other planes in the temporary set");
      close (fd);

      return;
    }

  params->entries[plane_idx].fd = fd;
  params->entries[plane_idx].plane_idx = plane_idx;
  params->entries[plane_idx].offset = offset;
  params->entries[plane_idx].stride = stride;
  params->entries[plane_idx].modifier_hi = modifier_hi;
  params->entries[plane_idx].modifier_lo = modifier_lo;
}

static void
DestroyBacking (Buffer *buffer)
{
  if (--buffer->refcount)
    return;

  /* Free the renderer-specific dmabuf buffer.  */
  RenderFreeDmabufBuffer (buffer->render_buffer);

  ExtBufferDestroy (&buffer->buffer);
  XLFree (buffer);
}

static void
DestroyBuffer (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct wl_buffer_interface zwp_linux_dmabuf_v1_buffer_impl =
  {
    .destroy = DestroyBuffer,
  };

static void
HandleBufferResourceDestroy (struct wl_resource *resource)
{
  Buffer *buffer;

  buffer = wl_resource_get_user_data (resource);
  buffer->resource = NULL;

  DestroyBacking (buffer);
}

static void
RetainBufferFunc (ExtBuffer *buffer)
{
  Buffer *dmabuf_buffer;

  dmabuf_buffer = (Buffer *) buffer;
  dmabuf_buffer->refcount++;
}

static void
DereferenceBufferFunc (ExtBuffer *buffer)
{
  Buffer *dmabuf_buffer;

  dmabuf_buffer = (Buffer *) buffer;
  DestroyBacking (dmabuf_buffer);
}

static unsigned int
WidthFunc (ExtBuffer *buffer)
{
  Buffer *dmabuf_buffer;

  dmabuf_buffer = (Buffer *) buffer;
  return dmabuf_buffer->width;
}

static unsigned int
HeightFunc (ExtBuffer *buffer)
{
  Buffer *dmabuf_buffer;

  dmabuf_buffer = (Buffer *) buffer;
  return dmabuf_buffer->height;
}

static void
ReleaseBufferFunc (ExtBuffer *buffer)
{
  if (((Buffer *) buffer)->resource)
    wl_buffer_send_release (((Buffer *) buffer)->resource);
}

static RenderBuffer
GetBufferFunc (ExtBuffer *buffer)
{
  Buffer *dmabuf_buffer;

  dmabuf_buffer = (Buffer *) buffer;
  return dmabuf_buffer->render_buffer;
}

static Buffer *
CreateBufferFor (BufferParams *params, RenderBuffer render_buffer,
		 uint32_t id)
{
  Buffer *buffer;
  struct wl_client *client;

  buffer = XLSafeMalloc (sizeof *buffer);
  client = wl_resource_get_client (params->resource);

  if (!buffer)
    {
      RenderFreeDmabufBuffer (render_buffer);
      zwp_linux_buffer_params_v1_send_failed (params->resource);

      return NULL;
    }

  memset (buffer, 0, sizeof *buffer);
  buffer->resource
    = wl_resource_create (client, &wl_buffer_interface, 1, id);

  if (!buffer->resource)
    {
      RenderFreeDmabufBuffer (render_buffer);
      XLFree (buffer);
      zwp_linux_buffer_params_v1_send_failed (params->resource);

      return NULL;
    }

  buffer->render_buffer = render_buffer;
  buffer->width = params->width;
  buffer->height = params->height;
  buffer->destroy_listeners = NULL;

  /* Initialize function pointers.  */
  buffer->buffer.funcs.retain = RetainBufferFunc;
  buffer->buffer.funcs.dereference = DereferenceBufferFunc;
  buffer->buffer.funcs.get_buffer = GetBufferFunc;
  buffer->buffer.funcs.width = WidthFunc;
  buffer->buffer.funcs.height = HeightFunc;
  buffer->buffer.funcs.release = ReleaseBufferFunc;

  buffer->refcount++;

  wl_resource_set_implementation (buffer->resource,
				  &zwp_linux_dmabuf_v1_buffer_impl,
				  buffer, HandleBufferResourceDestroy);

  return buffer;
}

#define ModifierHigh(mod)	((uint64_t) (mod) >> 31 >> 1)
#define ModifierLow(mod)	((uint64_t) (mod) & 0xffffffff)

static Bool
IsFormatSupported (uint32_t format, uint32_t mod_hi, uint32_t mod_low)
{
  int i;

  for (i = 0; i < n_drm_formats; ++i)
    {
      if (format == supported_formats[i].drm_format
	  /* Also check that the modifiers have been announced as
	     supported.  */
	  && ModifierHigh (supported_formats[i].drm_modifier) == mod_hi
	  && ModifierLow (supported_formats[i].drm_modifier) == mod_low)
	/* A match was found, so this format is supported.  */
	return True;
    }

  /* No match was found, so complain that the format is invalid.  This
     does not catch non-obvious errors, such as unsupported flags,
     which may cause buffer creation to fail after on.  */
  return False;
}

static void
CreateSucceeded (RenderBuffer render_buffer, void *data)
{
  BufferParams *params;
  Buffer *buffer;

  /* Buffer creation was successful.  If the resource was already
     destroyed, then simply destroy buffer and free params.
     Otherwise, send the created buffer to the client.  */

  params = data;

  if (!params->resource)
    {
      RenderFreeDmabufBuffer (render_buffer);

      /* Now, release the buffer params.  Since the callback has been
	 run, it is no longer callback data.  */
      params->flags &= ~IsCallbackData;
      ReleaseBufferParams (params);

      return;
    }

  /* Mark the params object as no longer being callback data.  */
  params->flags &= ~IsCallbackData;

  /* Create the buffer.  */
  buffer = CreateBufferFor (params, render_buffer, 0);

  /* If buffer is NULL, then the failure message will already have
     been sent.  */
  if (!buffer)
    return;

  /* Send the buffer to the client.  */
  zwp_linux_buffer_params_v1_send_created (params->resource,
					   buffer->resource);
}

static void
CreateFailed (void *data)
{
  BufferParams *params;

  /* Creation failed.  If no resource is attached, simply free params.
     Otherwise, send failed and wait for the client to destroy it.  */
  params = data;

  params->flags &= ~IsCallbackData;

  if (!params->resource)
    ReleaseBufferParams (params);
  else
    zwp_linux_buffer_params_v1_send_failed (params->resource);
}

static void
Create (struct wl_client *client, struct wl_resource *resource, int32_t width,
	int32_t height, uint32_t format, uint32_t flags)
{
  BufferParams *params;
  DmaBufAttributes attributes;
  int num_buffers, i;
  uint32_t mod_high, mod_low;
  uint32_t all_flags;

  params = wl_resource_get_user_data (resource);

  if (params->flags & IsUsed)
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
			      "the given params resource has already been used.");
      return;
    }

  /* Now mark the params resource as inert.  */
  params->flags |= IsUsed;

  /* And find out how many buffers are attached to the temporary set,
     along with which modifiers are set.  */
  num_buffers = ExistingModifier (params, &mod_high, &mod_low);

  if (!num_buffers)
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			      "no fds were attached to this resource's temporary set");
      goto inert_error;
    }

  if (params->entries[0].fd == -1)
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			      "no fd attached for plane 0 in the temporary set");
      goto inert_error;
    }

  if ((params->entries[3].fd >= 0 || params->entries[2].fd >= 0)
      && (params->entries[2].fd == -1 || params->entries[1].fd == -1))
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			      "gap in planes attached to temporary set");
      goto inert_error;
    }

  if (width < 0 || height < 0 || width > 65535 || height > 65535)
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
			      "size out of bounds for X server");
      goto inert_error;
    }

  /* Check that the client did not define any invalid flags.  */

  all_flags = (ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT
	       | ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_INTERLACED
	       | ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_BOTTOM_FIRST);

  if (flags & ~all_flags)
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
			      "invalid dmabuf flags: %u", flags);
      goto inert_error;
    }

  /* Now, see if the format and modifier pair specified is supported.
     If it is not, post an error for version 4 resources, and fail
     creation for earlier ones.  */
  if (!IsFormatSupported (format, mod_high, mod_low))
    {
      if (wl_resource_get_version (resource) >= 4)
	wl_resource_post_error (resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
				"invalid format/modifiers specified for version 4"
				" resource");
      else
	zwp_linux_buffer_params_v1_send_failed (resource);

      goto inert_error;
    }

  /* Clear the buffer attributes structure.  It has to be initialized
     even if not completely used.  */

  memset (&attributes, 0, sizeof attributes);

  /* Copy the file descriptors and other plane-specific information
     into the buffer attributes structure.  They are not closed here;
     the renderer should do that itself upon both success and
     failure.  */

  for (i = 0; i < num_buffers; ++i)
    {
      attributes.fds[i] = params->entries[i].fd;
      attributes.strides[i] = params->entries[i].stride;
      attributes.offsets[i] = params->entries[i].offset;
    }

  /* Provide the specified modifier in the buffer attributes
     structure.  */
  attributes.modifier = ((uint64_t) mod_high << 32) | mod_low;

  /* Set the number of planes specified.  */
  attributes.n_planes = num_buffers;

  /* And the dimensions of the buffer.  */
  attributes.width = width;
  attributes.height = height;

  /* The format.  */
  attributes.drm_format = format;

  /* The flags.  */
  attributes.flags = flags;

  /* Set params->width and params->height.  They are used by
     CreateBufferFor.  */
  params->width = width;
  params->height = height;

  /* Mark params as callback and post asynchronous creation.  This is
     so that the parameters will not be destroyed until one of the
     callback functions are called.  */
  params->flags |= IsCallbackData;

  /* Post asynchronous creation and return.  */
  RenderBufferFromDmaBufAsync (&attributes, CreateSucceeded,
			       CreateFailed, params);
  return;

 inert_error:
  /* We must also close every file descriptor attached here, as XCB
     has not done that for us yet.  */
  CloseFdsEarly (params);
}

static void
CreateImmed (struct wl_client *client, struct wl_resource *resource, uint32_t id,
	     int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
  BufferParams *params;
  DmaBufAttributes attributes;
  Bool error;
  int num_buffers, i;
  uint32_t mod_high, mod_low;
  uint32_t all_flags;
  RenderBuffer buffer;

  params = wl_resource_get_user_data (resource);

  if (params->flags & IsUsed)
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
			      "the given params resource has already been used.");
      return;
    }

  /* Now mark the params resource as inert.  */
  params->flags |= IsUsed;

  /* And find out how many buffers are attached to the temporary set,
     along with which modifiers are set.  */
  num_buffers = ExistingModifier (params, &mod_high, &mod_low);

  if (!num_buffers)
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			      "no fds were attached to this resource's temporary set");
      goto inert_error;
    }

  if (params->entries[0].fd == -1)
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			      "no fd attached for plane 0 in the temporary set");
      goto inert_error;
    }

  if ((params->entries[3].fd >= 0 || params->entries[2].fd >= 0)
      && (params->entries[2].fd == -1 || params->entries[1].fd == -1))
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			      "gap in planes attached to temporary set");
      goto inert_error;
    }

  if (width < 0 || height < 0 || width > 65535 || height > 65535)
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
			      "size out of bounds for X server");
      goto inert_error;
    }

  /* Check that the client did not define any invalid flags.  */

  all_flags = (ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT
	       | ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_INTERLACED
	       | ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_BOTTOM_FIRST);

  if (flags & ~all_flags)
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
			      "invalid dmabuf flags: %u", flags);
      goto inert_error;
    }

  /* Now, see if the format and modifier pair specified is supported.
     If it is not, post an error for version 4 resources, and fail
     creation for earlier ones.  */
  if (!IsFormatSupported (format, mod_high, mod_low))
    {
      if (wl_resource_get_version (resource) >= 4)
	wl_resource_post_error (resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
				"invalid format/modifiers specified for version 4"
				" resource");
      else
	zwp_linux_buffer_params_v1_send_failed (resource);

      goto inert_error;
    }

  /* Clear the buffer attributes structure.  It has to be initialized
     even if not completely used.  */

  memset (&attributes, 0, sizeof attributes);

  /* Copy the file descriptors and other plane-specific information
     into the buffer attributes structure.  They are not closed here;
     the renderer should do that itself upon both success and
     failure.  */

  for (i = 0; i < num_buffers; ++i)
    {
      attributes.fds[i] = params->entries[i].fd;
      attributes.strides[i] = params->entries[i].stride;
      attributes.offsets[i] = params->entries[i].offset;
    }

  /* Provide the specified modifier in the buffer attributes
     structure.  */
  attributes.modifier = ((uint64_t) mod_high << 32) | mod_low;

  /* Set the number of planes specified.  */
  attributes.n_planes = num_buffers;

  /* And the dimensions of the buffer.  */
  attributes.width = width;
  attributes.height = height;

  /* The format.  */
  attributes.drm_format = format;

  /* The flags.  */
  attributes.flags = flags;

  /* Set params->width and params->height.  They are used by
     CreateBufferFor.  */
  params->width = width;
  params->height = height;

  /* Now, try to create the buffer.  Send failed should it actually
     fail.  */
  error = False;
  buffer = RenderBufferFromDmaBuf (&attributes, &error);

  if (error)
    {
      /* The fds should have been closed by the renderer.  */
      zwp_linux_buffer_params_v1_send_failed (resource);
    }
  else
    /* Otherwise, buffer creation was successful.  Create the buffer
       for the id.  */
    CreateBufferFor (params, buffer, id);

  return;

 inert_error:
  /* We must also close every file descriptor attached here, as XCB
     has not done that for us yet.  */
  CloseFdsEarly (params);
}

static struct zwp_linux_buffer_params_v1_interface zwp_linux_buffer_params_v1_impl =
  {
    .destroy = DestroyBufferParams,
    .add = Add,
    .create = Create,
    .create_immed = CreateImmed,
  };

static void
CreateParams (struct wl_client *client, struct wl_resource *resource,
	      uint32_t id)
{
  BufferParams *params;
  int i;

  params = XLSafeMalloc (sizeof *params);

  if (!params)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (params, 0, sizeof *params);
  params->resource
    = wl_resource_create (client, &zwp_linux_buffer_params_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!params->resource)
    {
      XLFree (params);
      wl_resource_post_no_memory (resource);
      return;
    }

  wl_resource_set_implementation (params->resource,
				  &zwp_linux_buffer_params_v1_impl,
				  params, HandleParamsResourceDestroy);

  /* Initialize all fds to -1.  */
  for (i = 0; i < ArrayElements (params->entries); ++i)
    params->entries[i].fd = -1;
}

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static struct zwp_linux_dmabuf_feedback_v1_interface zld_feedback_v1_impl =
  {
    .destroy = Destroy,
  };

/* TODO: dynamically switch tranche for surface feedbacks based on the
   provider of the crtc the surface is in.  */

static void
MakeFeedback (struct wl_client *client, struct wl_resource *resource,
	      uint32_t id)
{
  struct wl_resource *feedback_resource;
  struct wl_array main_device_array, format_array;
  int i;
  ptrdiff_t format_array_size;
  uint16_t *format_array_data;

  feedback_resource = wl_resource_create (client,
					  &zwp_linux_dmabuf_feedback_v1_interface,
					  wl_resource_get_version (resource), id);

  if (!resource)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  wl_resource_set_implementation (feedback_resource,
				  &zld_feedback_v1_impl,
				  NULL, NULL);

  /* Now, send the relevant information.  This should eventually be
     dynamically updated, but we don't support that yet.  */

  /* First, send the format table.  */

  zwp_linux_dmabuf_feedback_v1_send_format_table (feedback_resource,
						  format_table_fd,
						  format_table_size);

  /* Next, send the main device.  */

  main_device_array.size = sizeof drm_device_node;
  main_device_array.data = &drm_device_node;

  main_device_array.alloc = main_device_array.size;
  zwp_linux_dmabuf_feedback_v1_send_main_device (feedback_resource,
						 &main_device_array);

  /* Then, send the first tranche.  Right now, the only tranche
     contains the formats supported by the default provider.  */

  zwp_linux_dmabuf_feedback_v1_send_tranche_target_device (feedback_resource,
							   &main_device_array);

  /* Populate the formats array with the contents of the format
     table, and send it to the client.  */

  format_array_size = format_table_size / sizeof (FormatModifierPair);
  format_array.size = format_array_size * sizeof (uint16_t);
  format_array.data = format_array_data = alloca (format_array.size);

  /* This must be reset too.  */
  format_array.alloc = format_array.size;

  /* Simply announce every format to the client.  */
  for (i = 0; i < format_array_size; ++i)
    format_array_data[i] = i;

  zwp_linux_dmabuf_feedback_v1_send_tranche_formats (feedback_resource,
						     &format_array);

  /* Send flags.  We don't currently support direct scanout, so send
     nothing.  */

  zwp_linux_dmabuf_feedback_v1_send_tranche_flags (feedback_resource, 0);

  /* Mark the end of the tranche.  */

  zwp_linux_dmabuf_feedback_v1_send_tranche_done (feedback_resource);
}

static void
GetDefaultFeedback (struct wl_client *client, struct wl_resource *resource,
		    uint32_t id)
{
  MakeFeedback (client, resource, id);
}

static void
GetSurfaceFeedback (struct wl_client *client, struct wl_resource *resource,
		    uint32_t id, struct wl_resource *surface_resource)
{
  MakeFeedback (client, resource, id);
}

static struct zwp_linux_dmabuf_v1_interface zwp_linux_dmabuf_v1_impl =
  {
    .create_params = CreateParams,
    .destroy = Destroy,
    .get_default_feedback = GetDefaultFeedback,
    .get_surface_feedback = GetSurfaceFeedback,
  };

static void
SendSupportedFormats (struct wl_resource *resource)
{
  int i;
  uint64_t modifier;

  for (i = 0; i < n_drm_formats; ++i)
    {
      if (wl_resource_get_version (resource) < 3)
	{
	  /* Send a legacy format message, but only as long as the
	     format uses the default (invalid) modifier.  */

	  if (supported_formats[i].drm_modifier == DRM_FORMAT_MOD_INVALID)
	    zwp_linux_dmabuf_v1_send_format (resource,
					     supported_formats[i].drm_format);
	}
      else
	{
	  /* This client supports modifiers, so send everything.  */

	  modifier = supported_formats[i].drm_modifier;

	  zwp_linux_dmabuf_v1_send_modifier (resource,
					     supported_formats[i].drm_format,
					     ModifierHigh (modifier),
					     ModifierLow (modifier));
	}
    }
}

static void
HandleBind (struct wl_client *client, void *data,
	    uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &zwp_linux_dmabuf_v1_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &zwp_linux_dmabuf_v1_impl,
				  NULL, NULL);

  if (version < 4)
    /* Versions later than 4 use the format table.  */
    SendSupportedFormats (resource);
}

static Bool
InitDrmDevice (void)
{
  Bool error;

  error = False;

  /* This can either be a master node or a render node.  */
  drm_device_node = RenderGetRenderDevice (&error);
  return !error;
}

static ssize_t
WriteFormatTable (void)
{
  int fd, i;
  ssize_t written;
  FormatModifierPair pair;

  /* Before writing the format table, make sure the DRM device node
     can be obtained.  */
  if (!InitDrmDevice ())
    {
      fprintf (stderr, "Failed to get direct rendering device node. "
	       "Hardware acceleration will probably be unavailable.\n");
      return -1;
    }

  fd = XLOpenShm ();

  if (fd < 0)
    {
      fprintf (stderr, "Failed to allocate format table fd. "
	       "Hardware acceleration will probably be unavailable.\n");
      return -1;
    }

  written = 0;

  /* Write each format-modifier pair.  */
  for (i = 0; i < n_drm_formats; ++i)
    {
      pair.format = supported_formats[i].drm_format;
      pair.padding = 0;
      pair.modifier = supported_formats[i].drm_modifier;

      if (write (fd, &pair, sizeof pair) != sizeof pair)
	/* Writing the modifier pair failed.  Punt.  */
	goto cancel;

      /* Now tell the caller how much was written.  */
      written += sizeof pair;
    }

  format_table_fd = fd;
  return written;

 cancel:
  close (fd);
  return -1;
}

static Bool
ReadSupportedFormats (void)
{
  /* Read supported formats from the renderer.  If none are supported,
     don't initialize dmabuf.  */
  supported_formats = RenderGetDrmFormats (&n_drm_formats);

  return n_drm_formats > 0;
}

void
XLInitDmabuf (void)
{
  ssize_t size;

  /* First, initialize supported formats.  */
  if (!ReadSupportedFormats ())
    return;

  /* And try to create the format table.  */
  size = WriteFormatTable ();

  global_dmabuf = wl_global_create (compositor.wl_display,
				    &zwp_linux_dmabuf_v1_interface,
				    /* If writing the format table
				       failed, don't announce support
				       for version 4.  */
				    size >= 0 ? 4 : 3, NULL, HandleBind);

  /* If the format table was successfully created, set its size.  */
  format_table_size = size;
}
