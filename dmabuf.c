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
    IsUsed = 1,
  };

struct _DrmFormatInfo
{
  /* The DRM format code.  */
  uint32_t format_code;

  /* The X Windows depth.  */
  int depth;

  /* The X Windows green, red, blue, and alpha masks.  */
  int red, green, blue, alpha;

  /* The number of bits per pixel.  */
  int bits_per_pixel;

  /* PictFormat associated with this format, or NULL if none were
     found.  */
  XRenderPictFormat *format;

  /* List of supported screen modifiers.  */
  uint64_t *supported_modifiers;

  /* Number of supported screen modifiers.  */
  int n_supported_modifiers;
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

  /* Some flags.  */
  int flags;

  /* The struct wl_resource associated with this object.  */
  struct wl_resource *resource;

  /* Possible link to a global list of buffer params pending
     release.  */
  BufferParams *next, *last;

  /* The XID of the buffer that will be created.  */
  Pixmap pixmap;

  /* The width and height of the buffer that will be created.  */
  int width, height;

  /* The buffer ID that will be used to create the buffer.  */
  uint32_t buffer_id;

  /* The DRM format.  */
  uint32_t drm_format;
};

struct _Buffer
{
  /* The ExtBuffer associated with this buffer.  */
  ExtBuffer buffer;

  /* The pixmap associated with this buffer.  */
  Pixmap pixmap;

  /* The picture associated with this buffer.  */
  Picture picture;

  /* The width and height of this buffer.  */
  unsigned int width, height;

  /* The wl_resource corresponding to this buffer.  */
  struct wl_resource *resource;

  /* List of "destroy listeners" connected to this buffer.  */
  XLList *destroy_listeners;

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

/* List of BufferParams pending success.  */
static BufferParams pending_success;

/* The id of the next round trip event.  */
static uint64_t next_roundtrip_id;

/* A window used to receive round trip events.  */
static Window round_trip_window;

/* List of all supported DRM formats.  */
static DrmFormatInfo all_formats[] =
  {
    {
      .format_code = DRM_FORMAT_ARGB8888,
      .depth = 32,
      .red = 0xff0000,
      .green = 0xff00,
      .blue = 0xff,
      .alpha = 0xff000000,
      .bits_per_pixel = 32,
    },
    {
      .format_code = DRM_FORMAT_XRGB8888,
      .depth = 24,
      .red = 0xff0000,
      .green = 0xff00,
      .blue = 0xff,
      .alpha = 0,
      .bits_per_pixel = 32,
    },
    {
      .format_code = DRM_FORMAT_XBGR8888,
      .depth = 24,
      .blue = 0xff0000,
      .green = 0xff00,
      .red = 0xff,
      .alpha = 0,
      .bits_per_pixel = 32,
    },
    {
      .format_code = DRM_FORMAT_ABGR8888,
      .depth = 32,
      .blue = 0xff0000,
      .green = 0xff00,
      .red = 0xff,
      .alpha = 0xff000000,
      .bits_per_pixel = 32,
    },
    {
      .format_code = DRM_FORMAT_BGRA8888,
      .depth = 32,
      .blue = 0xff000000,
      .green = 0xff0000,
      .red = 0xff00,
      .alpha = 0xff,
      .bits_per_pixel = 32,
    },
  };

/* The opcode of the DRI3 extension.  */
static int dri3_opcode;

/* File descriptor for the format table.  */
static int format_table_fd;

/* Size of the format table.  */
static ssize_t format_table_size;

/* Device node of the DRM device.  TODO: make this
   output-specific.  */
static dev_t drm_device_node;

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

  /* If params is linked into the list of buffers pending success,
     remove it.  */

  if (params->last)
    {
      params->next->last = params->last;
      params->last->next = params->next;
    }

  XLFree (params);
}

static void
HandleParamsResourceDestroy (struct wl_resource *resource)
{
  BufferParams *params;

  params = wl_resource_get_user_data (resource);

  /* First, clear params->resource.  */
  params->resource = NULL;

  if (params->next)
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
ForceRoundTrip (void)
{
  uint64_t id;
  XEvent event;

  /* Send an event with a monotonically increasing identifier to
     ourselves.

     Once the last event is received, create the actual buffers for
     each buffer resource for which error handlers have not run.  */

  id = next_roundtrip_id++;

  memset (&event, 0, sizeof event);

  event.xclient.type = ClientMessage;
  event.xclient.window = round_trip_window;
  event.xclient.message_type = _XL_DMA_BUF_CREATED;
  event.xclient.format = 32;

  event.xclient.data.l[0] = id >> 31 >> 1;
  event.xclient.data.l[1] = id & 0xffffffff;

  XSendEvent (compositor.display, round_trip_window,
	      False, NoEventMask, &event);
}

static int
DepthForFormat (uint32_t drm_format, int *bits_per_pixel)
{
  int i;

  for (i = 0; i < ArrayElements (all_formats); ++i)
    {
      if (all_formats[i].format_code == drm_format
	  && all_formats[i].format)
	{
	  *bits_per_pixel = all_formats[i].bits_per_pixel;

	  return all_formats[i].depth;
	}
    }

  return -1;
}

Bool
XLHandleErrorForDmabuf (XErrorEvent *error)
{
  BufferParams *params, *next;

  if (error->request_code == dri3_opcode
      && error->minor_code == xDRI3BuffersFromPixmap)
    {
      /* Something chouldn't be created.  Find what failed and unlink
	 it.  */

      next = pending_success.next;

      while (next != &pending_success)
	{
	  params = next;
	  next = next->next;

	  if (params->pixmap == error->resourceid)
	    {
	      /* Unlink params.  */
	      params->next->last = params->last;
	      params->last->next = params->next;

	      params->next = NULL;
	      params->last = NULL;

	      /* Tell the client that buffer creation failed.  It will
		 then delete the object.  */
	      zwp_linux_buffer_params_v1_send_failed (params->resource);

	      break;
	    }
	}

      return True;
    }

  return False;
}

static XRenderPictFormat *
PictFormatForFormat (uint32_t drm_format)
{
  int i;

  for (i = 0; i < ArrayElements (all_formats); ++i)
    {
      if (all_formats[i].format_code == drm_format
	  && all_formats[i].format)
	return all_formats[i].format;
    }

  /* This shouldn't happen, since the format was already verified in
     Create.  */
  abort ();
}

static void
DestroyBacking (Buffer *buffer)
{
  if (--buffer->refcount)
    return;

  XRenderFreePicture (compositor.display, buffer->picture);
  XFreePixmap (compositor.display, buffer->pixmap);

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

static Picture
GetPictureFunc (ExtBuffer *buffer)
{
  Buffer *dmabuf_buffer;

  dmabuf_buffer = (Buffer *) buffer;
  return dmabuf_buffer->picture;
}

static Pixmap
GetPixmapFunc (ExtBuffer *buffer)
{
  Buffer *dmabuf_buffer;

  dmabuf_buffer = (Buffer *) buffer;
  return dmabuf_buffer->pixmap;
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

static Buffer *
CreateBufferFor (BufferParams *params, uint32_t id)
{
  Buffer *buffer;
  Picture picture;
  struct wl_client *client;
  XRenderPictureAttributes picture_attrs;

  buffer = XLSafeMalloc (sizeof *buffer);
  client = wl_resource_get_client (params->resource);

  if (!buffer)
    {
      XFreePixmap (compositor.display, params->pixmap);
      zwp_linux_buffer_params_v1_send_failed (params->resource);

      return NULL;
    }

  memset (buffer, 0, sizeof *buffer);
  buffer->resource
    = wl_resource_create (client, &wl_buffer_interface, 1, id);

  if (!buffer->resource)
    {
      XFreePixmap (compositor.display, params->pixmap);
      XLFree (buffer);
      zwp_linux_buffer_params_v1_send_failed (params->resource);

      return NULL;
    }

  picture = XRenderCreatePicture (compositor.display, params->pixmap,
				  PictFormatForFormat (params->drm_format),
				  0, &picture_attrs);
  buffer->pixmap = params->pixmap;
  buffer->picture = picture;
  buffer->width = params->width;
  buffer->height = params->height;
  buffer->destroy_listeners = NULL;

  /* Initialize function pointers.  */
  buffer->buffer.funcs.retain = RetainBufferFunc;
  buffer->buffer.funcs.dereference = DereferenceBufferFunc;
  buffer->buffer.funcs.get_picture = GetPictureFunc;
  buffer->buffer.funcs.get_pixmap = GetPixmapFunc;
  buffer->buffer.funcs.width = WidthFunc;
  buffer->buffer.funcs.height = HeightFunc;
  buffer->buffer.funcs.release = ReleaseBufferFunc;

  buffer->refcount++;

  wl_resource_set_implementation (buffer->resource,
				  &zwp_linux_dmabuf_v1_buffer_impl,
				  buffer, HandleBufferResourceDestroy);

  return buffer;
}

static void
FinishBufferCreation (void)
{
  BufferParams *params, *next;
  Buffer *buffer;

  next = pending_success.next;

  while (next != &pending_success)
    {
      params = next;
      next = next->next;

      if (params->resource)
	{
	  buffer = CreateBufferFor (params, 0);

	  /* Send the buffer to the client, unless creation
	     failed.  */
	  if (buffer)
	    zwp_linux_buffer_params_v1_send_created (params->resource,
						     buffer->resource);

	  /* Unlink params, since it's no longer pending creation.  */
	  params->next->last = params->last;
	  params->last->next = params->next;

	  params->next = NULL;
	  params->last = NULL;
	}
      else
	{
	  /* The resource is no longer present, so just delete it.  */
	  XFreePixmap (compositor.display, params->pixmap);
	  ReleaseBufferParams (params);
	}
    }
}

Bool
XLHandleOneXEventForDmabuf (XEvent *event)
{
  uint64_t id, low, high;

  if (event->type == ClientMessage
      && event->xclient.message_type == _XL_DMA_BUF_CREATED)
    {
      /* Values are masked against 0xffffffff, as Xlib sign-extends
	 those longs.  */
      high = event->xclient.data.l[0] & 0xffffffff;
      low = event->xclient.data.l[1] & 0xffffffff;
      id = low | (high << 32);

      /* Ignore the message if the id is too old.  */
      if (id < next_roundtrip_id)
	{
	  /* Otherwise, it means buffer creation was successful.
	     Complete all pending buffer creation.  */

	  FinishBufferCreation ();
	}

      return True;
    }

  return False;
}

#define CreateHeader									\
  BufferParams *params;									\
  int num_buffers, i, depth, bpp;							\
  uint32_t mod_high, mod_low, all_flags;						\
  int32_t *allfds;									\
											\
  params = wl_resource_get_user_data (resource);					\
											\
  if (params->flags & IsUsed)								\
    {											\
      wl_resource_post_error (resource,							\
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,		\
			      "the given params resource has already been used");	\
      return;										\
    }											\
											\
  /* Find the depth and bpp corresponding to the format.  */				\
  depth = DepthForFormat (format, &bpp);						\
											\
  /* Now mark the params resource as inert.  */						\
  params->flags |= IsUsed;								\
											\
  /* Retrieve how many buffers are attached to the temporary set, and			\
     any set modifiers.  */								\
  num_buffers = ExistingModifier (params, &mod_high, &mod_low);				\
											\
  if (!num_buffers)									\
    {											\
      wl_resource_post_error (resource,							\
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,		\
			      "no fds were attached to this resource's temporary set");	\
      goto inert_error;									\
    }											\
											\
  if (params->entries[0].fd == -1)							\
    {											\
      wl_resource_post_error (resource,							\
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,		\
			      "no fd attached for plane 0 in the temporary set");	\
      goto inert_error;									\
    }											\
											\
  if ((params->entries[3].fd >= 0 || params->entries[2].fd >= 0)			\
      && (params->entries[2].fd == -1 || params->entries[1].fd == -1))			\
    {											\
      wl_resource_post_error (resource,							\
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,		\
			      "gap in planes attached to temporary set");		\
      goto inert_error;									\
    }											\
											\
  if (width < 0 || height < 0 || width > 65535 || height > 65535)			\
    {											\
      wl_resource_post_error (resource,							\
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,		\
			      "size out of bounds for X server");			\
      goto inert_error;									\
    }											\
											\
  if (depth == -1)									\
    {											\
      if (wl_resource_get_version (resource) >= 4)					\
	wl_resource_post_error (resource,						\
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,	\
				"invalid format specified for version 4 resource");	\
      else										\
	zwp_linux_buffer_params_v1_send_failed (resource);				\
											\
      goto inert_error;									\
    }											\
											\
  all_flags = (ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT				\
	       | ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_INTERLACED				\
	       | ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_BOTTOM_FIRST);			\
											\
  if (flags & ~all_flags)								\
    {											\
      wl_resource_post_error (resource,							\
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT, 		\
			      "invalid dmabuf flags: %u", flags);			\
      goto inert_error;									\
    }											\
											\
  if (flags)										\
    {											\
      /* Flags are not supported by wlroots, so I guess we don't have			\
	 to either.  */									\
      zwp_linux_buffer_params_v1_send_failed (resource);				\
      goto inert_error;									\
    }											\
											\
  /* Copy the file descriptors into a buffer.  At this point, we know			\
     there are no gaps in params->entries.  */						\
  allfds = alloca (sizeof *allfds * num_buffers);					\
											\
  for (i = 0; i < num_buffers; ++i)							\
    allfds[i] = params->entries[i].fd;							\
											\
  /* Make the request, and then link the buffer params object onto the			\
     list of pending buffers.  We don't know if the creation will be			\
     rejected by the X server, so we first arrange to catch all errors			\
     from DRI3PixmapFromBuffer(s), and send the create event the next			\
     time we know that a roundtrip has happened without any errors			\
     being raised.  */									\
											\
  params->width = width;								\
  params->height = height;								\
  params->drm_format = format;								\
											\
  params->pixmap = xcb_generate_id (compositor.conn)


#define CreateFooter							\
   inert_error:								\
  /* We also have to close each added fd here, since XCB hasn't done	\
     that for us.  */							\
									\
  CloseFdsEarly (params)

static void
Create (struct wl_client *client, struct wl_resource *resource, int32_t width,
	int32_t height, uint32_t format, uint32_t flags)
{
  CreateHeader;

  params->next = pending_success.next;
  params->last = &pending_success;
  pending_success.next->last = params;
  pending_success.next = params;

  /* Now, create the buffers.  XCB will close the file descriptor for
     us once the output buffer is flushed.  */
  xcb_dri3_pixmap_from_buffers (compositor.conn, params->pixmap,
				DefaultRootWindow (compositor.display),
				num_buffers, params->width, params->height,
				params->entries[0].offset,
				params->entries[0].stride,
				params->entries[1].offset,
				params->entries[1].stride,
				params->entries[2].offset,
				params->entries[2].stride,
				params->entries[3].offset,
				params->entries[3].stride, depth, bpp,
				(uint64_t) mod_high << 31 | mod_low, allfds);

  /* And force a roundtrip event.  */
  ForceRoundTrip ();

  return;

  CreateFooter;
}

static void
CreateImmed (struct wl_client *client, struct wl_resource *resource, uint32_t id,
	     int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
  xcb_void_cookie_t check_cookie;
  xcb_generic_error_t *error;

  CreateHeader;

  /* Instead of creating buffers asynchronously, check for failures
     immediately.  */

  check_cookie
    = xcb_dri3_pixmap_from_buffers_checked (compositor.conn, params->pixmap,
					    DefaultRootWindow (compositor.display),
					    num_buffers, params->width, params->height,
					    params->entries[0].offset,
					    params->entries[0].stride,
					    params->entries[1].offset,
					    params->entries[1].stride,
					    params->entries[2].offset,
					    params->entries[2].stride,
					    params->entries[3].offset,
					    params->entries[3].stride, depth, bpp,
					    (uint64_t) mod_high << 31 | mod_low, allfds);
  error = xcb_request_check (compositor.conn, check_cookie);

  /* A platform-specific error occured creating this buffer.
     It is easiest to implement this by sending INVALID_WL_BUFFER.  */
  if (error)
    {
      wl_resource_post_error (resource,
			      ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
			      "platform specific error: response code %u, error code %u,"
			      " serial %u, xid %u, minor %u, major %u", error->response_type,
			      error->error_code, error->sequence, error->resource_id,
			      error->minor_code, error->major_code);
      free (error);
    }
  else
    /* Otherwise, the fds were successfully imported into the X
       server.  */
    CreateBufferFor (params, id);

  return;

  CreateFooter;
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
   crtc of the provider the surface is in.  */

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

static DrmFormatInfo *
FindFormatMatching (XRenderPictFormat *format)
{
  unsigned long alpha, red, green, blue;
  int i;

  if (format->type != PictTypeDirect)
    /* No DRM formats are colormapped.  */
    return NULL;

  alpha = format->direct.alphaMask << format->direct.alpha;
  red = format->direct.redMask << format->direct.red;
  green = format->direct.greenMask << format->direct.green;
  blue = format->direct.blueMask << format->direct.blue;

  for (i = 0; i < ArrayElements (all_formats); ++i)
    {
      if (all_formats[i].depth == format->depth
	  && all_formats[i].red == red
	  && all_formats[i].green == green
	  && all_formats[i].blue == blue
	  && all_formats[i].alpha == alpha)
	return &all_formats[i];
    }

  return NULL;
}

static void
FindSupportedModifiers (void)
{
  Window root_window;
  xcb_dri3_get_supported_modifiers_cookie_t *cookies;
  xcb_dri3_get_supported_modifiers_reply_t *reply;
  int i, length;
  uint64_t *mods;

  cookies = alloca (sizeof *cookies * ArrayElements (all_formats));
  root_window = DefaultRootWindow (compositor.display);

  for (i = 0; i < ArrayElements (all_formats); ++i)
    {
      if (all_formats[i].format)
	cookies[i]
	  = xcb_dri3_get_supported_modifiers (compositor.conn,
					      root_window, all_formats[i].depth,
					      all_formats[i].bits_per_pixel);
    }

  for (i = 0; i < ArrayElements (all_formats); ++i)
    {
      if (!all_formats[i].format)
	continue;

      reply = xcb_dri3_get_supported_modifiers_reply (compositor.conn,
						      cookies[i], NULL);

      if (!reply)
	continue;

      mods
	= xcb_dri3_get_supported_modifiers_screen_modifiers (reply);
      length
	= xcb_dri3_get_supported_modifiers_screen_modifiers_length (reply);

      all_formats[i].supported_modifiers = XLMalloc (sizeof *mods * length);
      all_formats[i].n_supported_modifiers = length;

      memcpy (all_formats[i].supported_modifiers, mods,
	      sizeof *mods * length);
      free (reply);
    }
}

static Bool
FindSupportedFormats (void)
{
  int count;
  XRenderPictFormat *format;
  DrmFormatInfo *info;
  Bool supported;

  count = 0;
  supported = False;

  do
    {
      format = XRenderFindFormat (compositor.display, 0,
				  NULL, count);
      count++;

      if (!format)
	break;

      info = FindFormatMatching (format);

      if (info && !info->format)
	info->format = format;

      if (info)
	supported = True;
    }
  while (format);

  return supported;
}

#define ModifierHigh(mod)	((uint64_t) (mod) >> 31 >> 1)
#define ModifierLow(mod)	((uint64_t) (mod) & 0xffffffff)
#define Mod(format, i)		((format)->supported_modifiers[i])

static void
SendModifiers (struct wl_resource *resource, DrmFormatInfo *format)
{
  int i;

  for (i = 0; i < format->n_supported_modifiers; ++i)
    zwp_linux_dmabuf_v1_send_modifier (resource, format->format_code,
				       ModifierHigh (Mod (format, i)),
				       ModifierLow (Mod (format, i)));
}

static void
SendSupportedFormats (struct wl_resource *resource)
{
  int i;
  uint64_t invalid;

  invalid = DRM_FORMAT_MOD_INVALID;

  for (i = 0; i < ArrayElements (all_formats); ++i)
    {
      /* We consider all formats for which picture formats exist as
	 supported.  Unfortunately, it seems that the formats DRI3 is
	 willing to support slightly differs, so trying to create
	 buffers for some of these formats may fail later.  */
      if (all_formats[i].format)
	{
	  if (wl_resource_get_version (resource) < 3)
	    /* Send a legacy format message.  */
	    zwp_linux_dmabuf_v1_send_format (resource,
					     all_formats[i].format_code);
	  else
	    {
	      zwp_linux_dmabuf_v1_send_modifier (resource,
						 all_formats[i].format_code,
						 ModifierHigh (invalid),
						 ModifierLow (invalid));

	      SendModifiers (resource, &all_formats[i]);
	    }
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

  SendSupportedFormats (resource);
}

static Bool
InitDrmDevice (void)
{
  xcb_dri3_open_cookie_t cookie;
  xcb_dri3_open_reply_t *reply;
  int *fds, fd;
  struct stat dev_stat;

  /* TODO: if this ever calls exec, set FD_CLOEXEC.  TODO TODO
     implement multiple providers.  */
  cookie = xcb_dri3_open (compositor.conn,
			  DefaultRootWindow (compositor.display),
			  None);
  reply = xcb_dri3_open_reply (compositor.conn, cookie, NULL);

  if (!reply)
    return False;

  fds = xcb_dri3_open_reply_fds (compositor.conn, reply);

  if (!fds)
    {
      free (reply);
      return False;
    }

  fd = fds[0];

  if (fstat (fd, &dev_stat) != 0)
    {
      close (fd);
      free (reply);

      return False;
    }

  if (dev_stat.st_rdev)
    drm_device_node = dev_stat.st_rdev;
  else
    {
      close (fd);
      free (reply);

      return False;
    }

  close (fd);
  free (reply);

  return True;
}

static ssize_t
WriteFormatTable (void)
{
  int fd, i, m;
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
      return 1;
    }

  written = 0;

  /* Write each format-modifier pair.  */
  for (i = 0; i < ArrayElements (all_formats); ++i)
    {
      if (!all_formats[i].format)
	continue;

      /* First, send the default implicit modifier pair.  */
      pair.format = all_formats[i].format_code;
      pair.padding = 0;
      pair.modifier = DRM_FORMAT_MOD_INVALID;

      if (write (fd, &pair, sizeof pair) != sizeof pair)
	/* Writing the modifier pair failed.  Punt.  */
	goto cancel;

      /* Now tell the caller how much was written.  */
      written += sizeof pair;

      /* Next, write all the modifier pairs.  */
      for (m = 0; m < all_formats[i].n_supported_modifiers; ++m)
	{
	  pair.modifier = all_formats[i].supported_modifiers[m];

	  if (write (fd, &pair, sizeof pair) != sizeof pair)
	    /* Writing this pair failed, punt.  */
	    goto cancel;

	  written += sizeof pair;
	}
    }

  format_table_fd = fd;
  return written;

 cancel:
  close (fd);
  return -1;
}

static void
ReallyInitDmabuf (void)
{
  XSetWindowAttributes attrs;
  size_t size;

  if (!FindSupportedFormats ())
    {
      fprintf (stderr, "No supported picture formats were found."
	       " Hardware acceleration will not be available.\n");
      return;
    }

  /* Now look up which modifiers are supported for what formats.  */
  FindSupportedModifiers ();

  /* And try to create the format table.  */
  size = WriteFormatTable ();

  /* Create an unmapped, InputOnly window, that is used to receive
     roundtrip events.  */
  attrs.override_redirect = True;
  round_trip_window = XCreateWindow (compositor.display,
				     DefaultRootWindow (compositor.display),
				     -1, -1, 1, 1, 0, CopyFromParent, InputOnly,
				     CopyFromParent, CWOverrideRedirect, &attrs);

  global_dmabuf = wl_global_create (compositor.wl_display,
				    &zwp_linux_dmabuf_v1_interface,
				    /* If writing the format table
				       failed, don't announce support
				       for version 4.  */
				    size >= 0 ? 4 : 3, NULL, HandleBind);

  /* If the format table was successfully created, set its size.  */
  format_table_size = size;

  /* Initialize the sentinel node for buffer creation.  */
  pending_success.next = &pending_success;
  pending_success.last = &pending_success;
}

void
XLInitDmabuf (void)
{
  xcb_dri3_query_version_cookie_t cookie;
  xcb_dri3_query_version_reply_t *reply;
  const xcb_query_extension_reply_t *ext;

  ext = xcb_get_extension_data (compositor.conn, &xcb_dri3_id);
  reply = NULL;

  if (ext && ext->present)
    {
      cookie = xcb_dri3_query_version (compositor.conn, 1, 2);
      reply = xcb_dri3_query_version_reply (compositor.conn, cookie,
					    NULL);

      if (!reply)
	goto error;

      if (reply->major_version < 1
	  || (reply->major_version == 1
	      && reply->minor_version < 2))
	goto error;

      dri3_opcode = ext->major_opcode;
      ReallyInitDmabuf ();
    }
  else
  error:
    fprintf (stderr, "Warning: the X server does not support a new enough version of"
	     " the DRI3 extension.\nHardware acceleration will not be available.\n");

  if (reply)
    free (reply);
}
