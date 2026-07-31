/* Wayland compositor running on top of an X server.

Copyright (C) 2022 to various contributors.

This file is part of 12to11.

12to11 is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

12to11 is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with 12to11.  If not, see <https://www.gnu.org/licenses/>.  */

/* ext_image_copy_capture_manager_v1 and ext_image_capture_source_v1
   implementation.

   This allows Wayland clients (such as screenshot tools) to capture
   the contents of an output into a client-supplied shared memory
   buffer.  The pixels are read back from the X server with
   XGetImage.  */

#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "compositor.h"
#include "ext-image-capture-source-v1.h"
#include "ext-image-copy-capture-v1.h"

typedef struct _CaptureSource CaptureSource;
typedef struct _CaptureSession CaptureSession;
typedef struct _CaptureFrame CaptureFrame;

struct _CaptureSource
{
  /* The resource for this source.  */
  struct wl_resource *resource;

  /* The output being captured, or NULL.  */
  struct wl_resource *output_resource;
};

struct _CaptureSession
{
  /* The resource for this session.  */
  struct wl_resource *resource;

  /* The output being captured.  */
  struct wl_resource *output_resource;

  /* The capture rectangle in root window coordinates and the buffer
     dimensions advertised to the client.  */
  int ox, oy, ow, oh;

  /* Options given at creation.  */
  uint32_t options;

  /* Whether this is a cursor capture session (unsupported, always
     stops).  */
  Bool cursor;

  /* The single live frame, or NULL.  */
  CaptureFrame *frame;
};

struct _CaptureFrame
{
  /* The resource for this frame.  */
  struct wl_resource *resource;

  /* The owning session, or NULL if the session was destroyed.  */
  CaptureSession *session;

  /* The buffer attached by the client.  */
  struct wl_resource *buffer;

  /* Whether capture has been requested.  */
  Bool captured;
};

/* The two globals.  */
static struct wl_global *source_manager_global;
static struct wl_global *copy_capture_manager_global;

/* Forward declarations of static functions.  */

static void ComputeRect (struct wl_resource *, int *, int *, int *,
			 int *);
static void SendConstraints (CaptureSession *);
static void FailFrame (CaptureFrame *, uint32_t);
static void SendPresentationTime (struct wl_resource *);



/* Geometry helper.  */

static void
ComputeRect (struct wl_resource *output_resource, int *ox, int *oy,
	     int *ow, int *oh)
{
  if (output_resource
      && XLGetOutputRectFromResource (output_resource, ox, oy, ow, oh))
    return;

  /* Fall back to the whole screen.  */
  *ox = 0;
  *oy = 0;
  *ow = DisplayWidth (compositor.display,
		      DefaultScreen (compositor.display));
  *oh = DisplayHeight (compositor.display,
		       DefaultScreen (compositor.display));
}

static void
SendConstraints (CaptureSession *session)
{
  /* Tell the client the buffer dimensions.  */
  ext_image_copy_capture_session_v1_send_buffer_size (session->resource,
						       session->ow,
						       session->oh);

  /* Advertise the shared memory formats we can produce by direct
     copy.  The X server provides 32-bit XRGB/ARGB data, little
     endian.  */
  ext_image_copy_capture_session_v1_send_shm_format (session->resource,
						      WL_SHM_FORMAT_XRGB8888);
  ext_image_copy_capture_session_v1_send_shm_format (session->resource,
						      WL_SHM_FORMAT_ARGB8888);

  ext_image_copy_capture_session_v1_send_done (session->resource);
}

static void
SendPresentationTime (struct wl_resource *resource)
{
  struct timespec ts;
  uint64_t sec;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  sec = (uint64_t) ts.tv_sec;

  ext_image_copy_capture_frame_v1_send_presentation_time
    (resource,
     (uint32_t) (sec >> 32),
     (uint32_t) (sec & 0xffffffffu),
     (uint32_t) ts.tv_nsec);
}

static void
FailFrame (CaptureFrame *frame, uint32_t reason)
{
  if (!frame->resource)
    return;

  ext_image_copy_capture_frame_v1_send_failed (frame->resource, reason);
}


/* Capture frame requests.  */

static void
FrameDestroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
FrameAttachBuffer (struct wl_client *client, struct wl_resource *resource,
		   struct wl_resource *buffer)
{
  CaptureFrame *frame;

  frame = wl_resource_get_user_data (resource);

  if (frame->captured)
    {
      wl_resource_post_error (resource,
			      EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
			      "attach_buffer sent after capture");
      return;
    }

  frame->buffer = buffer;
}

static void
FrameDamageBuffer (struct wl_client *client, struct wl_resource *resource,
		   int32_t x, int32_t y, int32_t width, int32_t height)
{
  CaptureFrame *frame;

  frame = wl_resource_get_user_data (resource);

  if (frame->captured)
    {
      wl_resource_post_error (resource,
			      EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
			      "damage_buffer sent after capture");
      return;
    }

  /* Validate the damage rectangle.  The protocol requires this to be a
     proper region; the actual damage is ignored here, since the whole
     buffer is always copied.  */
  if (x < 0 || y < 0 || width <= 0 || height <= 0)
    {
      wl_resource_post_error (resource,
			      EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_INVALID_BUFFER_DAMAGE,
			      "invalid buffer damage");
      return;
    }
}

static void
FrameCapture (struct wl_client *client, struct wl_resource *resource)
{
  CaptureFrame *frame;
  CaptureSession *session;
  ExtBuffer *eb;
  int fd, offset, stride, y;
  unsigned int bw, bh;
  size_t pool_size;
  uint32_t format;
  void *data;
  XImage *image;
  uint8_t *dst, *src;

  frame = wl_resource_get_user_data (resource);

  if (frame->captured)
    {
      wl_resource_post_error (resource,
			      EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
			      "capture sent more than once");
      return;
    }

  if (!frame->buffer)
    {
      wl_resource_post_error (resource,
			      EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
			      "capture sent without attach_buffer");
      return;
    }

  frame->captured = True;

  session = frame->session;

  if (!session || session->cursor)
    {
      FailFrame (frame,
		 EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
      return;
    }

  /* Describe the attached shared memory buffer.  */
  eb = wl_resource_get_user_data (frame->buffer);

  if (!XLShmBufferDescribe (eb, &fd, &pool_size, &format, &offset,
			    &stride, &bw, &bh))
    {
      FailFrame (frame,
		 EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
      return;
    }

  /* The buffer must match the advertised dimensions.  */
  if ((int) bw != session->ow || (int) bh != session->oh)
    {
      close (fd);
      FailFrame (frame,
		 EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
      return;
    }

  /* Only 32-bit XRGB/ARGB can be produced by direct copy.  */
  if (format != WL_SHM_FORMAT_XRGB8888
      && format != WL_SHM_FORMAT_ARGB8888)
    {
      close (fd);
      FailFrame (frame,
		 EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
      return;
    }

  /* Map the pool writable.  */
  data = mmap (NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	       fd, 0);
  close (fd);

  if (data == MAP_FAILED)
    {
      FailFrame (frame,
		 EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
      return;
    }

  /* Read back the output rectangle from the root window.  */
  image = XGetImage (compositor.display,
		     DefaultRootWindow (compositor.display),
		     session->ox, session->oy,
		     session->ow, session->oh,
		     AllPlanes, ZPixmap);

  if (!image)
    {
      munmap (data, pool_size);
      FailFrame (frame,
		 EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
      return;
    }

  /* The X server is expected to provide 32 bits per pixel, little
     endian, matching the shared memory formats above.  */
  if (image->bits_per_pixel != 32
      || image->byte_order != LSBFirst)
    {
      XDestroyImage (image);
      munmap (data, pool_size);
      FailFrame (frame,
		 EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
      return;
    }

  /* Copy each scanline into the client buffer, accounting for
     differing strides.  */
  dst = (uint8_t *) data + offset;
  src = (uint8_t *) image->data;

  for (y = 0; y < session->oh; ++y)
    memcpy (dst + y * stride, src + y * image->bytes_per_line,
	    session->ow * 4);

  msync (data, pool_size, MS_SYNC);
  munmap (data, pool_size);
  XDestroyImage (image);

  /* Emit the frame metadata followed by ready.  */
  ext_image_copy_capture_frame_v1_send_transform
    (resource, WL_OUTPUT_TRANSFORM_NORMAL);
  ext_image_copy_capture_frame_v1_send_damage (resource, 0, 0,
					       session->ow, session->oh);
  SendPresentationTime (resource);
  ext_image_copy_capture_frame_v1_send_ready (resource);
}

static const struct ext_image_copy_capture_frame_v1_interface frame_impl =
  {
    .destroy = FrameDestroy,
    .attach_buffer = FrameAttachBuffer,
    .damage_buffer = FrameDamageBuffer,
    .capture = FrameCapture,
  };

static void
HandleFrameResourceDestroy (struct wl_resource *resource)
{
  CaptureFrame *frame;

  frame = wl_resource_get_user_data (resource);

  /* Detach the frame from its session.  */
  if (frame->session)
    {
      XLAssert (frame->session->frame == frame);
      frame->session->frame = NULL;
    }

  frame->resource = NULL;
  XLFree (frame);
}


/* Capture session requests.  */

static void
SessionDestroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
SessionCreateFrame (struct wl_client *client, struct wl_resource *resource,
		    uint32_t id)
{
  CaptureSession *session;
  CaptureFrame *frame;

  session = wl_resource_get_user_data (resource);

  if (session->frame)
    {
      wl_resource_post_error (resource,
			      EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME,
			      "create_frame sent before destroying the"
			      " previous frame");
      return;
    }

  frame = XLSafeMalloc (sizeof *frame);

  if (!frame)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (frame, 0, sizeof *frame);

  frame->resource
    = wl_resource_create (client,
			  &ext_image_copy_capture_frame_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!frame->resource)
    {
      wl_resource_post_no_memory (resource);
      XLFree (frame);
      return;
    }

  frame->session = session;
  session->frame = frame;

  wl_resource_set_implementation (frame->resource, &frame_impl,
				  frame, HandleFrameResourceDestroy);
}

static const struct ext_image_copy_capture_session_v1_interface
  session_impl =
  {
    .create_frame = SessionCreateFrame,
    .destroy = SessionDestroy,
  };

static void
HandleSessionResourceDestroy (struct wl_resource *resource)
{
  CaptureSession *session;

  session = wl_resource_get_user_data (resource);

  /* The session can be destroyed before its frame.  Detach any live
     frame so it cannot reference this session any more.  */
  if (session->frame)
    session->frame->session = NULL;

  session->resource = NULL;
  XLFree (session);
}

static CaptureSession *
MakeSession (struct wl_client *client, struct wl_resource *manager,
	     uint32_t id, struct wl_resource *output_resource,
	     uint32_t options, Bool cursor)
{
  CaptureSession *session;

  session = XLSafeMalloc (sizeof *session);

  if (!session)
    {
      wl_resource_post_no_memory (manager);
      return NULL;
    }

  memset (session, 0, sizeof *session);

  session->resource
    = wl_resource_create (client,
			  &ext_image_copy_capture_session_v1_interface,
			  wl_resource_get_version (manager), id);

  if (!session->resource)
    {
      wl_resource_post_no_memory (manager);
      XLFree (session);
      return NULL;
    }

  session->output_resource = output_resource;
  session->options = options;
  session->cursor = cursor;

  ComputeRect (output_resource, &session->ox, &session->oy,
	       &session->ow, &session->oh);

  wl_resource_set_implementation (session->resource, &session_impl,
				  session, HandleSessionResourceDestroy);

  return session;
}


/* Cursor capture session requests.  */

static void
CursorSessionDestroy (struct wl_client *client,
		      struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
CursorSessionGetCaptureSession (struct wl_client *client,
				struct wl_resource *resource, uint32_t id)
{
  CaptureSession *session;

  /* Cursor capture is not implemented.  Create a session bound to no
     output and immediately stop it, signaling unavailability.  */
  session = MakeSession (client, resource, id, NULL, 0, True);

  if (!session)
    return;

  ext_image_copy_capture_session_v1_send_stopped (session->resource);
}

static const struct ext_image_copy_capture_cursor_session_v1_interface
  cursor_session_impl =
  {
    .destroy = CursorSessionDestroy,
    .get_capture_session = CursorSessionGetCaptureSession,
  };


/* Copy capture manager requests.  */

static void
ManagerDestroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
ManagerCreateSession (struct wl_client *client,
		      struct wl_resource *resource, uint32_t id,
		      struct wl_resource *source_resource, uint32_t options)
{
  CaptureSource *source;
  CaptureSession *session;

  /* Validate options.  Only paint_cursors is defined.  */
  if (options & ~EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS)
    {
      wl_resource_post_error (resource,
			      EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_ERROR_INVALID_OPTION,
			      "invalid option flag");
      return;
    }

  source = wl_resource_get_user_data (source_resource);

  session = MakeSession (client, resource, id,
			 source ? source->output_resource : NULL,
			 options, False);

  if (!session)
    return;

  /* Send the buffer constraints.  */
  SendConstraints (session);
}

static void
ManagerCreatePointerCursorSession (struct wl_client *client,
				   struct wl_resource *resource,
				   uint32_t id,
				   struct wl_resource *source_resource,
				   struct wl_resource *pointer)
{
  struct wl_resource *cursor_resource;

  cursor_resource
    = wl_resource_create (client,
			  &ext_image_copy_capture_cursor_session_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!cursor_resource)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  wl_resource_set_implementation (cursor_resource, &cursor_session_impl,
				  NULL, NULL);
}

static const struct ext_image_copy_capture_manager_v1_interface
  manager_impl =
  {
    .create_session = ManagerCreateSession,
    .create_pointer_cursor_session = ManagerCreatePointerCursorSession,
    .destroy = ManagerDestroy,
  };

static void
HandleManagerBind (struct wl_client *client, void *data,
		   uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client,
				 &ext_image_copy_capture_manager_v1_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &manager_impl, NULL, NULL);
}


/* Capture source manager requests.  */

static void
SourceDestroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct ext_image_capture_source_v1_interface source_impl =
  {
    .destroy = SourceDestroy,
  };

static void
HandleSourceResourceDestroy (struct wl_resource *resource)
{
  CaptureSource *source;

  source = wl_resource_get_user_data (resource);
  source->resource = NULL;
  XLFree (source);
}

static void
SourceManagerCreateSource (struct wl_client *client,
			   struct wl_resource *resource, uint32_t id,
			   struct wl_resource *output)
{
  CaptureSource *source;

  source = XLSafeMalloc (sizeof *source);

  if (!source)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (source, 0, sizeof *source);

  source->resource
    = wl_resource_create (client,
			  &ext_image_capture_source_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!source->resource)
    {
      wl_resource_post_no_memory (resource);
      XLFree (source);
      return;
    }

  source->output_resource = output;

  wl_resource_set_implementation (source->resource, &source_impl,
				  source, HandleSourceResourceDestroy);
}

static void
SourceManagerDestroy (struct wl_client *client,
		      struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct
  ext_output_image_capture_source_manager_v1_interface
  source_manager_impl =
  {
    .create_source = SourceManagerCreateSource,
    .destroy = SourceManagerDestroy,
  };

static void
HandleSourceManagerBind (struct wl_client *client, void *data,
			 uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create
    (client, &ext_output_image_capture_source_manager_v1_interface,
     version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &source_manager_impl,
				  NULL, NULL);
}

void
XLInitImageCopyCapture (void)
{
  source_manager_global
    = wl_global_create (compositor.wl_display,
			&ext_output_image_capture_source_manager_v1_interface,
			1, NULL, HandleSourceManagerBind);

  copy_capture_manager_global
    = wl_global_create (compositor.wl_display,
			&ext_image_copy_capture_manager_v1_interface,
			1, NULL, HandleManagerBind);
}
