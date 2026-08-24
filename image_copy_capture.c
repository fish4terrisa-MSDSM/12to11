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

  /* Destroy listener for the attached buffer.  */
  struct wl_listener buffer_destroy_listener;

  /* Whether capture has been requested.  */
  Bool captured;

  /* The timer used to delay capture.  */
  Timer *timer;
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
  int scale = global_scale_factor;

  /* Tell the client the buffer dimensions.  */
  ext_image_copy_capture_session_v1_send_buffer_size (session->resource,
						       session->ow / scale,
						       session->oh / scale);

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
HandleBufferDestroy (struct wl_listener *listener, void *data)
{
  CaptureFrame *frame = wl_container_of (listener, frame, buffer_destroy_listener);
  frame->buffer = NULL;
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

  if (frame->buffer)
    wl_list_remove (&frame->buffer_destroy_listener.link);

  frame->buffer = buffer;

  if (buffer)
    {
      frame->buffer_destroy_listener.notify = HandleBufferDestroy;
      wl_resource_add_destroy_listener (buffer, &frame->buffer_destroy_listener);
    }
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
CaptureTimerCallback (Timer *timer, void *data, struct timespec time)
{
  CaptureFrame *frame = data;
  CaptureSession *session;
  ExtBuffer *eb;
  int fd, offset, stride, y, x;
  unsigned int bw, bh;
  size_t pool_size;
  uint32_t format;
  void *ptr;
  XImage *image;
  uint8_t *dst, *src;

  /* Remove the repeating timer so it only executes once.  */
  RemoveTimer (timer);
  frame->timer = NULL;

  session = frame->session;

  if (!session || session->cursor || !frame->buffer || !frame->resource)
    {
      FailFrame (frame,
		 EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
      return;
    }

  /* Describe the attached shared memory buffer.  */
  eb = wl_resource_get_user_data (frame->buffer);

  if (!eb || !XLShmBufferDescribe (eb, &fd, &pool_size, &format, &offset,
				   &stride, &bw, &bh))
    {
      FailFrame (frame,
		 EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
      return;
    }

  int scale = global_scale_factor;
  if (scale < 1)
    scale = 1;
  int logical_w = session->ow / scale;
  int logical_h = session->oh / scale;

  /* The buffer must match the advertised dimensions.  */
  if ((int) bw != logical_w || (int) bh != logical_h)
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
  ptr = mmap (NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	       fd, 0);
  close (fd);

  if (ptr == MAP_FAILED)
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
      munmap (ptr, pool_size);
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
      munmap (ptr, pool_size);
      FailFrame (frame,
		 EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
      return;
    }

  /* Copy each scanline into the client buffer, accounting for
     differing strides and scaling.  */
  dst = (uint8_t *) ptr + offset;
  src = (uint8_t *) image->data;

  for (y = 0; y < logical_h; ++y)
    {
      uint32_t *d = (uint32_t *)(dst + y * stride);
      for (x = 0; x < logical_w; ++x)
        {
          uint32_t *s = (uint32_t *)(src + (y * scale) * image->bytes_per_line + (x * scale) * 4);
          d[x] = *s;
        }
    }

  msync (ptr, pool_size, MS_SYNC);
  munmap (ptr, pool_size);
  XDestroyImage (image);

  /* Emit the frame metadata followed by ready.  */
  uint32_t transform = XLGetOutputTransformFromResource (session->output_resource);
  ext_image_copy_capture_frame_v1_send_transform
    (frame->resource, transform);
  ext_image_copy_capture_frame_v1_send_damage (frame->resource, 0, 0,
					       logical_w, logical_h);
  SendPresentationTime (frame->resource);
  ext_image_copy_capture_frame_v1_send_ready (frame->resource);
}

static void
FrameCapture (struct wl_client *client, struct wl_resource *resource)
{
  CaptureFrame *frame;
  CaptureSession *session;

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

  /* Sync X server to make sure any unmaps are processed */
  XSync(compositor.display, False);

  /* Delay capture to allow X11 compositor to render the screen without the dim layer */
  frame->timer = AddTimer(CaptureTimerCallback, frame, MakeTimespec(0, 50000000));
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

  if (frame->timer)
    {
      RemoveTimer (frame->timer);
      frame->timer = NULL;
    }

  if (frame->buffer)
    {
      wl_list_remove (&frame->buffer_destroy_listener.link);
      frame->buffer = NULL;
    }

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
  wl_list_init (&frame->buffer_destroy_listener.link);

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
