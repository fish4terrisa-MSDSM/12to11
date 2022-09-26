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

#include <alloca.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/fcntl.h>
#include <drm_fourcc.h>

#include "compositor.h"

#include <xcb/dri3.h>

#include <X11/Xmd.h>
#include <X11/extensions/dri3proto.h>

typedef struct _DrmFormatInfo DrmFormatInfo;
typedef struct _DmaBufRecord DmaBufRecord;

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

struct _DmaBufRecord
{
  /* The XID of the pixmap.  */
  Pixmap pixmap;

  /* The success callback.  */
  DmaBufSuccessFunc success_func;

  /* The failure callback.  */
  DmaBufFailureFunc failure_func;

  /* The callback data.  */
  void *data;

  /* The picture format that will be used.  */
  XRenderPictFormat *format;

  /* The next and last pending buffers in this list.  */
  DmaBufRecord *next, *last;
};

/* The identity transform.  */

static XTransform identity_transform;

/* The default SHM formats.  */

static ShmFormat default_formats[] =
  {
    { WL_SHM_FORMAT_ARGB8888 },
    { WL_SHM_FORMAT_XRGB8888 },
  };

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

/* DRM formats reported to the caller.  */
static DrmFormat *drm_formats;

/* Number of formats available.  */
static int n_drm_formats;

/* List of buffers that are still pending asynchronous creation.  */
static DmaBufRecord pending_success;

/* The id of the next round trip event.  */
static uint64_t next_roundtrip_id;

/* A window used to receive round trip events.  */
static Window round_trip_window;

/* The opcode of the DRI3 extension.  */
static int dri3_opcode;

/* List of pixmap format values supported by the X server.  */
static XPixmapFormatValues *x_formats;

/* Number of those formats.  */
static int num_x_formats;

/* XRender and DRI3-based renderer.  A RenderTarget is just a
   Picture.  */

static Visual *
PickVisual (int *depth)
{
  int n_visuals;
  XVisualInfo vinfo, *visuals;
  Visual *selection;

  vinfo.screen = DefaultScreen (compositor.display);
  vinfo.class = TrueColor;
  vinfo.depth = 32;

  visuals = XGetVisualInfo (compositor.display, (VisualScreenMask
						 | VisualClassMask
						 | VisualDepthMask),
			    &vinfo, &n_visuals);

  if (n_visuals)
    {
      selection = visuals[0].visual;
      *depth = visuals[0].depth;
      XFree (visuals);

      return selection;
    }

  return NULL;
}

static Bool
InitRenderFuncs (void)
{
  /* Set up the default visual.  */
  compositor.visual = PickVisual (&compositor.n_planes);

  /* Return success if the visual was found.  */
  return compositor.visual != NULL;
}

static RenderTarget
TargetFromDrawable (Drawable drawable)
{
  XRenderPictureAttributes picture_attrs;

  /* This is just to pacify GCC; picture_attrs is not used as mask is
     0.  */
  memset (&picture_attrs, 0, sizeof picture_attrs);

  return (RenderTarget) XRenderCreatePicture (compositor.display,
					      drawable,
					      compositor.argb_format,
					      0, &picture_attrs);
}

static RenderTarget
TargetFromPixmap (Pixmap pixmap)
{
  return TargetFromDrawable (pixmap);
}

static RenderTarget
TargetFromWindow (Window window)
{
  return TargetFromDrawable (window);
}

static Picture
PictureFromTarget (RenderTarget target)
{
  return target.xid;
}

static void
FreePictureFromTarget (Picture picture)
{
  /* There is no need to free these pictures.  */
}

static void
DestroyRenderTarget (RenderTarget target)
{
  XRenderFreePicture (compositor.display, target.xid);
}

static void
FillBoxesWithTransparency (RenderTarget target, pixman_box32_t *boxes,
			   int nboxes, int min_x, int min_y)
{
  XRectangle *rects;
  static XRenderColor color;
  int i;

  if (nboxes < 256)
    rects = alloca (sizeof *rects * nboxes);
  else
    rects = XLMalloc (sizeof *rects * nboxes);

  for (i = 0; i < nboxes; ++i)
    {
      rects[i].x = BoxStartX (boxes[i]) - min_x;
      rects[i].y = BoxStartY (boxes[i]) - min_y;
      rects[i].width = BoxWidth (boxes[i]);
      rects[i].height = BoxHeight (boxes[i]);
    }

  XRenderFillRectangles (compositor.display, PictOpClear,
			 target.xid, &color, rects, nboxes);

  if (nboxes >= 256)
    XLFree (rects);
}

static void
ClearRectangle (RenderTarget target, int x, int y, int width, int height)
{
  static XRenderColor color;

  XRenderFillRectangle (compositor.display, PictOpClear,
			target.xid, &color, x, y, width, height);
}

static void
ApplyTransform (RenderBuffer buffer, double divisor)
{
  XTransform transform;

  memset (&transform, 0, sizeof transform);

  transform.matrix[0][0] = XDoubleToFixed (divisor);
  transform.matrix[1][1] = XDoubleToFixed (divisor);
  transform.matrix[2][2] = XDoubleToFixed (1);

  XRenderSetPictureTransform (compositor.display, buffer.xid,
			      &transform);
}

static int
ConvertOperation (Operation op)
{
  switch (op)
    {
    case OperationOver:
      return PictOpOver;

    case OperationSource:
      return PictOpSrc;
    }

  abort ();
}

static void
Composite (RenderBuffer buffer, RenderTarget target,
	   Operation op, int src_x, int src_y, int x, int y,
	   int width, int height)
{
  XRenderComposite (compositor.display, ConvertOperation (op),
		    buffer.xid, None, target.xid,
		    /* src-x, src-y, mask-x, mask-y.  */
		    src_x, src_y, 0, 0,
		    /* dst-x, dst-y, width, height.  */
		    x, y, width, height);
}

static void
ResetTransform (RenderBuffer buffer)
{
  XRenderSetPictureTransform (compositor.display, buffer.xid,
			      &identity_transform);
}

static int
TargetAge (RenderTarget target)
{
  return 0;
}

/* At first glance, it seems like this should be easy to support using
   DRI3 and synchronization extension fences.  Unfortunately, the
   "fences" used by DRI3 are userspace fences implemented by the
   xshmfence library, and not Android dma-fences, so there is no
   straightforward implementation.  */

static RenderFence
ImportFdFence (int fd, Bool *error)
{
  *error = True;
  return (RenderFence) (XID) None;
}

static void
WaitFence (RenderFence fence)
{
  /* Unsupported.  */
}

static void
DeleteFence (RenderFence fence)
{
  /* Unsupported.  */
}

static int
GetFinishFence (Bool *error)
{
  *error = True;
  return -1;
}

static RenderFuncs picture_render_funcs =
  {
    .init_render_funcs = InitRenderFuncs,
    .target_from_window = TargetFromWindow,
    .target_from_pixmap = TargetFromPixmap,
    .picture_from_target = PictureFromTarget,
    .free_picture_from_target = FreePictureFromTarget,
    .destroy_render_target = DestroyRenderTarget,
    .fill_boxes_with_transparency = FillBoxesWithTransparency,
    .clear_rectangle = ClearRectangle,
    .apply_transform = ApplyTransform,
    .composite = Composite,
    .reset_transform = ResetTransform,
    .target_age = TargetAge,
    .import_fd_fence = ImportFdFence,
    .wait_fence = WaitFence,
    .delete_fence = DeleteFence,
    .get_finish_fence = GetFinishFence,
    .flags = NeverAges,
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

static Bool
HavePixmapFormat (int depth, int bpp)
{
  int i;

  for (i = 0; i < num_x_formats; ++i)
    {
      if (x_formats[i].depth == depth
	  && x_formats[i].bits_per_pixel == bpp)
	return True;
    }

  return False;
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

      /* See if the info's depth and bpp are supported.  */
      if (info
	  && !HavePixmapFormat (info->depth,
				info->bits_per_pixel))
	continue;

      if (info && !info->format)
	info->format = format;

      if (info)
	supported = True;
    }
  while (format);

  return supported;
}

static Window
MakeCheckWindow (void)
{
  XSetWindowAttributes attrs;
  unsigned long flags;

  /* Make an override-redirect window to use as the icon surface.  */
  flags = (CWColormap | CWBorderPixel | CWEventMask
	   | CWOverrideRedirect);
  attrs.colormap = compositor.colormap;
  attrs.border_pixel = border_pixel;
  attrs.event_mask = (ExposureMask | StructureNotifyMask);
  attrs.override_redirect = 1;

  return XCreateWindow (compositor.display,
			DefaultRootWindow (compositor.display),
			0, 0, 1, 1, 0, compositor.n_planes,
			InputOutput, compositor.visual, flags,
			&attrs);
}

static void
FindSupportedModifiers (int *pair_count_return)
{
  Window check_window;
  xcb_dri3_get_supported_modifiers_cookie_t *cookies;
  xcb_dri3_get_supported_modifiers_reply_t *reply;
  int i, length, pair_count;
  uint64_t *mods;

  cookies = alloca (sizeof *cookies * ArrayElements (all_formats));

  /* Create a temporary window similar to ones surfaces will use to
     determine which modifiers are supported.  */
  check_window = MakeCheckWindow ();
  pair_count = 0;

  for (i = 0; i < ArrayElements (all_formats); ++i)
    {
      if (all_formats[i].format)
	{
	  cookies[i]
	    = xcb_dri3_get_supported_modifiers (compositor.conn,
						check_window, all_formats[i].depth,
						all_formats[i].bits_per_pixel);

	  /* pair_count is the number of format-modifier pairs that
	     will be returned.  First, add one for each implicit
	     modifier.  */
	  pair_count += 1;
	}
    }

  /* Delete the temporary window used to query for modifiers.  */
  XDestroyWindow (compositor.display, check_window);

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

      /* Then, add length for each explicit modifier.  */
      pair_count += length;

      memcpy (all_formats[i].supported_modifiers, mods,
	      sizeof *mods * length);
      free (reply);
    }

  *pair_count_return = pair_count;
}

static void
InitDrmFormats (void)
{
  int pair_count, i, j, n;

  /* First, look up which formats are supported.  */
  if (!FindSupportedFormats ())
    return;

  /* Then, look up modifiers.  */
  FindSupportedModifiers (&pair_count);

  /* Allocate the amount of memory we need to store the DRM format
     list.  */
  drm_formats = XLCalloc (pair_count, sizeof *drm_formats);
  n = 0;

  /* Populate the format list.  */
  for (i = 0; i < ArrayElements (all_formats); ++i)
    {
      if (!all_formats[i].format)
	continue;

      /* Check n < pair_count.  */
      XLAssert (n < pair_count);

      /* Add the implicit modifier.  */
      drm_formats[n].drm_format = all_formats[i].format_code;
      drm_formats[n].drm_modifier = DRM_FORMAT_MOD_INVALID;
      n++;

      /* Now add every supported explicit modifier.  */
      for (j = 0; j < all_formats[i].n_supported_modifiers; ++i)
	{
	  /* Check n < pair_count.  */
	  XLAssert (n < pair_count);

	  /* Add the implicit modifier.  */
	  drm_formats[n].drm_format = all_formats[i].format_code;
	  drm_formats[n].drm_modifier
	    = all_formats[i].supported_modifiers[j];
	  n++;
	}
    }

  /* Set the number of supported formats to the pair count.  */
  n_drm_formats = pair_count;

  /* Return.  */
  return;
}

static DrmFormat *
GetDrmFormats (int *num_formats)
{
  *num_formats = n_drm_formats;
  return drm_formats;
}

static dev_t
GetRenderDevice (Bool *error)
{
  xcb_dri3_open_cookie_t cookie;
  xcb_dri3_open_reply_t *reply;
  int *fds, fd;
  struct stat dev_stat;

  /* TODO: if this ever calls exec, set FD_CLOEXEC, and implement
     multiple providers.  */
  cookie = xcb_dri3_open (compositor.conn,
			  DefaultRootWindow (compositor.display),
			  None);
  reply = xcb_dri3_open_reply (compositor.conn, cookie, NULL);

  if (!reply)
    {
      *error = True;
      return (dev_t) 0;
    }

  fds = xcb_dri3_open_reply_fds (compositor.conn, reply);

  if (!fds)
    {
      free (reply);
      *error = True;

      return (dev_t) 0;
    }

  fd = fds[0];

  if (fstat (fd, &dev_stat) != 0)
    {
      close (fd);
      free (reply);
      *error = True;

      return (dev_t) 0;
    }

  if (!dev_stat.st_rdev)
    {
      close (fd);
      free (reply);
      *error = True;

      return (dev_t) 0;
    }

  close (fd);
  free (reply);

  return dev_stat.st_rdev;
}

static ShmFormat *
GetShmFormats (int *num_formats)
{
  /* Return the two mandatory shm formats.  */
  *num_formats = ArrayElements (default_formats);
  return default_formats;
}

static int
DepthForDmabufFormat (uint32_t drm_format, int *bits_per_pixel)
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

static XRenderPictFormat *
PictFormatForDmabufFormat (uint32_t drm_format)
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
CloseFileDescriptors (DmaBufAttributes *attributes)
{
  int i;

  for (i = 0; i < attributes->n_planes; ++i)
    close (attributes->fds[i]);
}

static RenderBuffer
BufferFromDmaBuf (DmaBufAttributes *attributes, Bool *error)
{
  int depth, bpp;
  Pixmap pixmap;
  Picture picture;
  xcb_void_cookie_t check_cookie;
  xcb_generic_error_t *xerror;
  XRenderPictFormat *format;
  XRenderPictureAttributes picture_attrs;

  /* Find the depth and bpp corresponding to the format.  */
  depth = DepthForDmabufFormat (attributes->drm_format, &bpp);

  /* Flags are not supported.  */
  if (attributes->flags || depth == -1)
    goto error;

  /* Create the pixmap.  */
  pixmap = xcb_generate_id (compositor.conn);
  check_cookie
    = xcb_dri3_pixmap_from_buffers (compositor.conn, pixmap,
				    DefaultRootWindow (compositor.display),
				    attributes->n_planes,
				    attributes->width,
				    attributes->height,
				    attributes->offsets[0],
				    attributes->strides[0],
				    attributes->offsets[1],
				    attributes->strides[1],
				    attributes->offsets[2],
				    attributes->strides[2],
				    attributes->offsets[3],
				    attributes->strides[3],
				    depth, bpp,
				    attributes->modifier,
				    attributes->fds);
  xerror = xcb_request_check (compositor.conn, check_cookie);

  /* A platform specific error occured creating this buffer.  Signal
     failure.  */
  if (xerror)
    {
      free (xerror);
      goto error;
    }

  /* Otherwise, create the picture and free the pixmap.  */
  format = PictFormatForDmabufFormat (attributes->drm_format);
  XLAssert (format != NULL);

  picture = XRenderCreatePicture (compositor.display, pixmap,
				  format, 0, &picture_attrs);
  XFreePixmap (compositor.display, pixmap);

  return (RenderBuffer) picture;

 error:
  CloseFileDescriptors (attributes);
  *error = True;
  return (RenderBuffer) (XID) None;
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

static void
FinishDmaBufRecord (DmaBufRecord *pending, Bool success)
{
  Picture picture;
  XRenderPictureAttributes picture_attrs;

  if (success)
    {
      /* This is just to pacify GCC.  */
      memset (&picture_attrs, 0, sizeof picture_attrs);

      picture = XRenderCreatePicture (compositor.display,
				      pending->pixmap,
				      pending->format, 0,
				      &picture_attrs);
      XFreePixmap (compositor.display, pending->pixmap);

      /* Call the creation success function with the new picture.  */
      pending->success_func ((RenderBuffer) picture,
			     pending->data);
    }
  else
    /* Call the failure function with the data.  */
    pending->failure_func (pending->data);

  /* Unlink the creation record.  */
  pending->last->next = pending->next;
  pending->next->last = pending->last;

  XLFree (pending);
}

static void
FinishBufferCreation (void)
{
  DmaBufRecord *next, *last;

  /* It is now known that all records in pending_success have been
     created.  Create pictures and call the success function for
     each.  */
  next = pending_success.next;
  while (next != &pending_success)
    {
      last = next;
      next = next->next;

      FinishDmaBufRecord (last, True);
    }
}

/* N.B. that the caller is supposed to keep callback_data around until
   one of success_func or failure_func is called.  */

static void
BufferFromDmaBufAsync (DmaBufAttributes *attributes,
		       DmaBufSuccessFunc success_func,
		       DmaBufFailureFunc failure_func,
		       void *callback_data)
{
  DmaBufRecord *record;
  int depth, bpp;
  Pixmap pixmap;

  /* Find the depth and bpp corresponding to the format.  */
  depth = DepthForDmabufFormat (attributes->drm_format, &bpp);

  /* Flags are not supported.  */
  if (attributes->flags || depth == -1)
    goto error;

  /* Create the pixmap.  */
  pixmap = xcb_generate_id (compositor.conn);
  xcb_dri3_pixmap_from_buffers (compositor.conn, pixmap,
				DefaultRootWindow (compositor.display),
				attributes->n_planes, attributes->width,
				attributes->height,
				attributes->offsets[0],
				attributes->strides[0],
				attributes->offsets[1],
				attributes->strides[1],
				attributes->offsets[2],
				attributes->strides[2],
				attributes->offsets[3],
				attributes->strides[3],
				depth, bpp,
				attributes->modifier, attributes->fds);

  /* Then, link the resulting pixmap and callbacks onto the list of
     pending buffers.  Right now, we do not know if the creation will
     be rejected by the X server, so we first arrange to catch all
     errors from DRI3PixmapFromBuffers, and send the create event the
     next time we know that a roundtrip has happened without any
     errors being raised.  */

  record = XLMalloc (sizeof *record);
  record->success_func = success_func;
  record->failure_func = failure_func;
  record->data = callback_data;
  record->format
    = PictFormatForDmabufFormat (attributes->drm_format);
  record->pixmap = pixmap;
  XLAssert (record->format != NULL);

  record->next = pending_success.next;
  record->last = &pending_success;
  pending_success.next->last = record;
  pending_success.next = record;

  ForceRoundTrip ();

  return;

 error:
  /* Just call the failure func to signal failure this early on.  */
  failure_func (callback_data);

  /* Then, close the file descriptors.  */
  CloseFileDescriptors (attributes);
}

static int
DepthForFormat (uint32_t format, int *bpp)
{
  switch (format)
    {
    case WL_SHM_FORMAT_ARGB8888:
      *bpp = 32;
      return 32;

    case WL_SHM_FORMAT_XRGB8888:
      *bpp = 32;
      return 24;

    default:
      return 0;
    }
}

static XRenderPictFormat *
PictFormatForFormat (uint32_t format)
{
  switch (format)
    {
    case WL_SHM_FORMAT_ARGB8888:
      return compositor.argb_format;

    case WL_SHM_FORMAT_XRGB8888:
      return compositor.xrgb_format;

    default:
      return 0;
    }
}

static RenderBuffer
BufferFromShm (SharedMemoryAttributes *attributes, Bool *error)
{
  XRenderPictureAttributes picture_attrs;
  xcb_shm_seg_t seg;
  Pixmap pixmap;
  Picture picture;
  int fd, depth, format, bpp;

  depth = DepthForFormat (attributes->format, &bpp);
  format = attributes->format;

  /* Duplicate the fd, since XCB closes file descriptors after sending
     them.  */
  fd = fcntl (attributes->fd, F_DUPFD_CLOEXEC, 0);

  if (fd < 0)
    {
      *error = True;
      return (RenderBuffer) (XID) None;
    }

  /* Now, allocate the XIDs for the shm segment and pixmap.  */
  seg = xcb_generate_id (compositor.conn);
  pixmap = xcb_generate_id (compositor.conn);

  /* Create the segment and attach the pixmap to it.  */
  xcb_shm_attach_fd (compositor.conn, seg, fd, false);
  xcb_shm_create_pixmap (compositor.conn, pixmap,
			 DefaultRootWindow (compositor.display),
			 attributes->width, attributes->height,
			 depth, seg, attributes->offset);
  xcb_shm_detach (compositor.conn, seg);

  /* Create the picture for the pixmap, and free the pixmap.  */
  picture = XRenderCreatePicture (compositor.display, pixmap,
				  PictFormatForFormat (format),
				  0, &picture_attrs);
  XFreePixmap (compositor.display, pixmap);

  /* Return the picture.  */
  return (RenderBuffer) picture;
}

static int
GetScanlinePad (int depth)
{
  int i;

  for (i = 0; i < num_x_formats; ++i)
    {
      if (x_formats[i].depth == depth)
	return x_formats[i].scanline_pad;
    }

  /* This should never happen in practice.  */
  return -1;
}

/* Roundup rounds up NBYTES to PAD.  PAD is a value that can appear as
   the scanline pad.  Macro borrowed from Xlib, as usual for everyone
   working with such images.  */
#define Roundup(nbytes, pad) ((((nbytes) + ((pad) - 1)) / (pad)) * ((pad) >> 3))

static Bool
ValidateShmParams (uint32_t format, uint32_t width, uint32_t height,
		   int32_t offset, int32_t stride, size_t pool_size)
{
  int bpp, depth;
  long wanted_stride;
  size_t total_size;

  /* Obtain the depth and bpp.  */
  depth = DepthForFormat (format, &bpp);
  XLAssert (depth != -1);

  /* If any signed values are negative, return.  */
  if (offset < 0 || stride < 0)
    return False;

  /* Obtain width * bpp padded to the scanline pad.  Xlib or the X
     server do not try to handle overflow here... */
  wanted_stride = Roundup (width * (long) bpp,
			   GetScanlinePad (depth));

  /* Assume that size_t can hold int32_t.  */
  total_size = offset;

  /* Calculate the total data size.  */
  if (IntMultiplyWrapv (stride, height, &total_size))
    return False;

  if (IntAddWrapv (offset, total_size, &total_size))
    return False;

  /* Verify that the stride is correct and the image fits.  */
  if (stride != wanted_stride || total_size > pool_size)
    return False;

  return True;
}

static void
FreeShmBuffer (RenderBuffer buffer)
{
  XRenderFreePicture (compositor.display, buffer.xid);
}

static void
FreeDmabufBuffer (RenderBuffer buffer)
{
  /* N.B. that the picture is the only reference to the pixmap
     here.  */
  XRenderFreePicture (compositor.display, buffer.xid);
}

static void
SetupMitShm (void)
{
  xcb_shm_query_version_reply_t *reply;
  xcb_shm_query_version_cookie_t cookie;

  /* This shouldn't be freed.  */
  const xcb_query_extension_reply_t *ext;

  ext = xcb_get_extension_data (compositor.conn, &xcb_shm_id);

  if (!ext || !ext->present)
    {
      fprintf (stderr, "The MIT-SHM extension is not supported by this X server.\n");
      exit (1);
    }

  cookie = xcb_shm_query_version (compositor.conn);
  reply = xcb_shm_query_version_reply (compositor.conn,
				       cookie, NULL);

  if (!reply)
    {
      fprintf (stderr, "The MIT-SHM extension on this X server is too old.\n");
      exit (1);
    }
  else if (reply->major_version < 1
	   || (reply->major_version == 1
	       && reply->minor_version < 2))
    {
      fprintf (stderr, "The MIT-SHM extension on this X server is too old"
	       " to support POSIX shared memory.\n");
      exit (1);
    }

  free (reply);

  /* Now check that the mandatory image formats are supported.  */

  if (!HavePixmapFormat (24, 32))
    {
      fprintf (stderr, "X server does not support pixmap format"
	       " of depth 24 with 32 bits per pixel\n");
      exit (1);
    }

  if (!HavePixmapFormat (32, 32))
    {
      fprintf (stderr, "X server does not support pixmap format"
	       " of depth 32 with 32 bits per pixel\n");
      exit (1);
    }
}

static void
InitBufferFuncs (void)
{
  xcb_dri3_query_version_cookie_t cookie;
  xcb_dri3_query_version_reply_t *reply;
  const xcb_query_extension_reply_t *ext;

  /* Obtain the list of supported pixmap formats from the X
     server.  */
  x_formats = XListPixmapFormats (compositor.display, &num_x_formats);

  if (!x_formats)
    {
      fprintf (stderr, "No pixmap formats could be retrieved from"
	       " the X server\n");
      return;
    }

  /* Set up the MIT shared memory extension.  It is required to
     work.  */
  SetupMitShm ();

  /* XRender should already have been set up; it is used for things
     other than rendering as well.  */

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

      /* Initialize DRM formats.  */
      InitDrmFormats ();
    }
  else
  error:
    fprintf (stderr, "Warning: the X server does not support a new enough version of"
	     " the DRI3 extension.\nHardware acceleration will not be available.\n");

  if (reply)
    free (reply);
}

static Bool
CanReleaseNow (RenderBuffer buffer)
{
  return False;
}

static BufferFuncs picture_buffer_funcs =
  {
    .get_drm_formats = GetDrmFormats,
    .get_render_device = GetRenderDevice,
    .get_shm_formats = GetShmFormats,
    .buffer_from_dma_buf = BufferFromDmaBuf,
    .buffer_from_dma_buf_async = BufferFromDmaBufAsync,
    .buffer_from_shm = BufferFromShm,
    .validate_shm_params = ValidateShmParams,
    .free_shm_buffer = FreeShmBuffer,
    .free_dmabuf_buffer = FreeDmabufBuffer,
    .can_release_now = CanReleaseNow,
    .init_buffer_funcs = InitBufferFuncs,
  };

Bool
HandleErrorForPictureRenderer (XErrorEvent *error)
{
  DmaBufRecord *record, *next;

  if (error->request_code == dri3_opcode
      && error->minor_code == xDRI3BuffersFromPixmap)
    {
      /* Something chouldn't be created.  Find what failed and unlink
	 it.  */

      next = pending_success.next;

      while (next != &pending_success)
	{
	  record = next;
	  next = next->next;

	  if (record->pixmap == error->resourceid)
	    {
	      /* Call record's failure callback and unlink it.  */
	      FinishDmaBufRecord (record, False);
	      break;
	    }
	}

      return True;
    }

  return False;
}

Bool
HandleOneXEventForPictureRenderer (XEvent *event)
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
	/* Otherwise, it means buffer creation was successful.
	   Complete all pending buffer creation.  */
	FinishBufferCreation ();

      return True;
    }

  return False;
}

void
InitPictureRenderer (void)
{
  XSetWindowAttributes attrs;

  identity_transform.matrix[0][0] = 1;
  identity_transform.matrix[1][1] = 1;
  identity_transform.matrix[2][2] = 1;

  pending_success.next = &pending_success;
  pending_success.last = &pending_success;

  /* Create an unmapped, InputOnly window, that is used to receive
     roundtrip events.  */
  attrs.override_redirect = True;
  round_trip_window = XCreateWindow (compositor.display,
				     DefaultRootWindow (compositor.display),
				     -1, -1, 1, 1, 0, CopyFromParent, InputOnly,
				     CopyFromParent, CWOverrideRedirect,
				     &attrs);

  /* Initialize the renderer with our functions.  */
  RegisterStaticRenderer ("picture", &picture_render_funcs,
			  &picture_buffer_funcs);
}
