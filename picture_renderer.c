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
#include "drm_modifiers.h"

#include <xcb/dri3.h>

#include <X11/Xmd.h>
#include <X11/extensions/dri3proto.h>
#include <X11/extensions/Xpresent.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

typedef struct _DrmFormatInfo DrmFormatInfo;
typedef struct _DmaBufRecord DmaBufRecord;
typedef struct _DrmModifierName DrmModifierName;

typedef struct _BackBuffer BackBuffer;

typedef struct _PictureBuffer PictureBuffer;
typedef struct _PictureTarget PictureTarget;
typedef struct _PresentRecord PresentRecord;

typedef struct _BufferActivityRecord BufferActivityRecord;
typedef struct _IdleCallback IdleCallback;
typedef struct _PresentCompletionCallback PresentCompletionCallback;

struct _DrmModifierName
{
  /* The modifier name.  */
  const char *name;

  /* The modifier code.  */
  uint64_t modifier;
};

/* Structure describing an expected PresentIdleNotify from the X
   server.  */

struct _PresentRecord
{
  /* The next and last fields on the buffer.  */
  PresentRecord *buffer_next, *buffer_last;

  /* The next and last fields on the target.  */
  PresentRecord *target_next, *target_last;

  /* The buffer.  */
  PictureBuffer *buffer;

  /* The target.  */
  PictureTarget *target;

  /* The expected serial.  */
  uint32_t serial;
};

/* Structure describing buffer activity.  It is linked onto 3 (!!!)
   lists.  */

struct _BufferActivityRecord
{
  /* The buffer.  */
  PictureBuffer *buffer;

  /* The target.  */
  PictureTarget *target;

  /* The counter ID.  */
  uint64_t id;

  /* The forward links to the three lists.  */
  BufferActivityRecord *buffer_next, *target_next, *global_next;

  /* The backlinks to the three lists.  */
  BufferActivityRecord *buffer_last, *target_last, *global_last;
};

struct _IdleCallback
{
  /* The next and last callbacks in this list, attached to the
     buffer.  */
  IdleCallback *buffer_next, *buffer_last;

  /* The next and last callbacks in this list, attached to the
     target.  */
  IdleCallback *target_next, *target_last;

  /* The associated target.  */
  PictureTarget *target;

  /* The callback data.  */
  void *data;

  /* The callback function.  */
  BufferIdleFunc function;
};

enum
  {
    CanPresent = 1,
    IsOpaque   = (1 << 1),
  };

struct _PictureBuffer
{
  /* The XID of the picture.  */
  Picture picture;

  /* The picture's backing pixmap.  */
  Pixmap pixmap;

  /* The depth of the picture's backing pixmap.  */
  int depth;

  /* Flags.  */
  int flags;

  /* The width and height of the buffer.  */
  short width, height;

  /* The last draw params associated with the picture.  */
  DrawParams params;

  /* List of release records.  */
  PresentRecord pending;

  /* List of idle callbacks.  */
  IdleCallback idle_callbacks;

  /* Ongoing buffer activity.  */
  BufferActivityRecord activity;
};

enum
  {
    JustPresented  = 1,
    NoPresentation = 2,
  };

/* Structure describing presentation callback.  The callback is run
   upon a given presentation being completed.  */

struct _PresentCompletionCallback
{
  /* The next and last presentation callbacks.  */
  PresentCompletionCallback *next, *last;

  /* Data the callback will be called with.  */
  void *data;

  /* The function.  */
  PresentCompletionFunc function;

  /* The presentation ID.  */
  uint32_t id;
};

struct _BackBuffer
{
  /* The picture of this back buffer.  High bit means the back buffer
     is busy.  */
  Picture picture;

  /* The pixmap of this back buffer.  */
  Pixmap pixmap;

  /* The idle fence of this back buffer.  */
  Fence *idle_fence;

  /* The serial of the last presentation, or 0.  */
  uint32_t present_serial;

  /* The age of this back buffer.  0 means it is fresh.  */
  unsigned int age;
};

enum
  {
    /* The buffer is currently busy.  */
    BufferBusy = (1U << 31),
    /* The idle notification has (or will) arrive, but the fence has
       not yet been waited upon.  If this is set on the pixmap as well
       as the picture, then calling XFlush is not necessary.  */
    BufferSync = (1U << 30),
  };

#define IsBufferBusy(buffer)	((buffer)->picture & BufferBusy)
#define SetBufferBusy(buffer)	((buffer)->picture |= BufferBusy)
#define ClearBufferBusy(buffer)	((buffer)->picture &= ~BufferBusy)

struct _PictureTarget
{
  /* The XID of the picture.  */
  Picture picture;

  /* The backing window.  */
  Window window;

  /* The GC used to swap back buffers  */
  GC gc;

  /* Two back buffers.  */
  BackBuffer *back_buffers[2];

  /* Presentation event context.  */
  XID presentation_event_context;

  /* The standard event mask.  */
  unsigned long standard_event_mask;

  /* The last known bounds of this render target.  */
  int width, height;

  /* Flags.  */
  int flags;

  /* The index of the current back buffer.  */
  int current_back_buffer;

  /* List of release records.  */
  PresentRecord pending;

  /* List of idle callbacks.  */
  IdleCallback idle_callbacks;

  /* Ongoing buffer activity.  */
  BufferActivityRecord activity;

  /* List of present completion callbacks.  */
  PresentCompletionCallback completion_callbacks;

  /* List of buffers that were used in the course of an update.  */
  XLList *buffers_used;
};

struct _DrmFormatInfo
{
  /* PictFormat associated with this format, or NULL if none were
     found.  */
  XRenderPictFormat *format;

  /* List of supported screen modifiers.  */
  uint64_t *supported_modifiers;

  /* The DRM format code.  */
  uint32_t format_code;

  /* The X Windows depth.  */
  int depth;

  /* The X Windows green, red, blue, and alpha masks.  */
  int red, green, blue, alpha;

  /* The number of bits per pixel.  */
  int bits_per_pixel;

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

  /* The depth of the pixmap.  */
  int depth;

  /* The width and height.  */
  short width, height;
};

/* Number of format modifiers specified by the user.  */
static int num_specified_modifiers;

/* Array of user-specified format modifiers.  */
static uint64_t *user_specified_modifiers;

/* Hash table mapping between presentation windows and targets.  */
static XLAssocTable *xid_table;

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
    {
      .format_code = DRM_FORMAT_XRGB4444,
      .depth = 15,
      .red = 0xf00,
      .green = 0xf0,
      .blue = 0xf,
      .alpha = 0x0,
      .bits_per_pixel = 16,
    },
    {
      .format_code = DRM_FORMAT_ARGB4444,
      .depth = 16,
      .red = 0xf00,
      .green = 0xf0,
      .blue = 0xf,
      .alpha = 0xf000,
      .bits_per_pixel = 16,
    },
  };

/* Array of all known DRM modifier names.  */
static DrmModifierName known_modifiers[] =
  {
    /* Generated from drm_fourcc.h.  */
    DrmModifiersList
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

/* The serial for PresentNotify events.  */
static uint32_t present_serial;

/* The major opcode of the presentation extension.  */
static int present_opcode;

/* Ongoing buffer activity.  */
static BufferActivityRecord all_activity;

/* List of all presentations that have not yet been completed.  */
static PresentCompletionCallback all_completion_callbacks;

/* Whether or not direct presentation should be used.  */
static Bool use_direct_presentation;

/* XRender, DRI3 and XPresent-based renderer.  A RenderTarget is just
   a Picture.  Here is a rough explanation of how the buffer release
   machinery works.

   Normally, upon a request to composite a buffer to a target, certain
   rendering commands are issued to the X server, after which a
   counter is increased.  Once an event is received confirming the
   increase in the counter (by which time all rendering requests
   should have been processed by the server), the idle callback is run
   for the pertinent target.

   If the target is destroyed, the idle callback is not run.

   However, the buffer is presented to a target instead of being
   composited, that fact is noted, and the idle callback is run upon
   receiving the PresentIdleNotify event instead.

   And here is an explanation of pixmaps for dummies.

   A pixmap is just a pixel buffer.  The X server supports several
   pixel storage formats, each of which maps between a depth, the
   number of bits per pixel, and the scanline pad.  The depth is the
   number of significant bits inside each pixel, the bits per pixel
   contains the number of bits taken up by a pixel (it is always a
   multiple of 8), and the scanline pad is the value (in bits) by
   which the stride must be padded.  So, given a depth of 24, a bpp of
   32, and a scanline_pad of 32, the stride of a pixmap for a given
   width is:

     Pad (WIDTH * (32 / 8), 32 / 8)

   Pixmaps themselves have no format information; they are simply a
   collection of pixel values.  Normally, the visual tells the X
   server how to put the pixel onto the display server, but in the X
   rendering extension a picture format does instead.  The picture
   format specifies the alpha, red, green, and blue channel masks that
   are then used to put together the color corresponding to a pixel.

   A buffer must thus consist of a picture format, and a pixmap with a
   specified depth to be useful.

   X always uses premultiplied alpha.  Thankfully, Wayland does
   too.  */



static uint64_t
SendRoundtripMessage (void)
{
  XEvent event;
  static uint64_t id;

  /* Send a message to the X server with a monotonically increasing
     counter.  This is necessary because the connection to the X
     server does not behave synchronously, and receiving the message
     tells us that the X server has finished processing all requests
     that access the buffer.  */

  memset (&event, 0, sizeof event);

  id += 1;
  event.xclient.type = ClientMessage;
  event.xclient.window = round_trip_window;
  event.xclient.message_type = _XL_BUFFER_RELEASE;
  event.xclient.format = 32;

  event.xclient.data.l[0] = id >> 31 >> 1;
  event.xclient.data.l[1] = id & 0xffffffff;

  XSendEvent (compositor.display, round_trip_window, False,
	      NoEventMask, &event);
  return id;
}

/* Find an existing buffer activity record matching the given buffer
   and target.  */

static BufferActivityRecord *
FindBufferActivityRecord (PictureBuffer *buffer, PictureTarget *target)
{
  BufferActivityRecord *record;

  /* Look through the global activity list for a record matching
     the given values.  */
  record = all_activity.global_next;
  while (record != &all_activity)
    {
      if (record->buffer == buffer
	  && record->target == target)
	return record;

      record = record->global_next;
    }

  return NULL;
}

/* Record buffer activity involving the given buffer and target.  */

static void
RecordBufferActivity (PictureBuffer *buffer, PictureTarget *target,
		      uint64_t roundtrip_id)
{
  BufferActivityRecord *record;

  /* Try to find an existing record.  */
  record = FindBufferActivityRecord (buffer, target);

  if (!record)
    {
      record = XLMalloc (sizeof *record);

      /* Buffer activity is actually linked on 3 different lists:

	 - a global list, which is used to actually look up buffer
           activity in response to events.  A list is faster than a
           hash table, as there is not much activity going on at any
           given time.

	 - a buffer list, which is used to remove buffer activity on
           buffer destruction.

	 - a target list, which is used to remove buffer activity on
           target destruction.  */
      record->buffer_next = buffer->activity.buffer_next;
      record->buffer_last = &buffer->activity;
      record->target_next = target->activity.target_next;
      record->target_last = &target->activity;
      record->global_next = all_activity.global_next;
      record->global_last = &all_activity;
      buffer->activity.buffer_next->buffer_last = record;
      buffer->activity.buffer_next = record;
      target->activity.target_next->target_last = record;
      target->activity.target_next = record;
      all_activity.global_next->global_last = record;
      all_activity.global_next = record;

      /* Set the appropriate values.  */
      record->buffer = buffer;
      record->target = target;
    }

  record->id = roundtrip_id;
}

static void
RunIdleCallbacks (PictureBuffer *buffer, PictureTarget *target)
{
  IdleCallback *callback, *last;

  callback = buffer->idle_callbacks.buffer_next;
  while (callback != &buffer->idle_callbacks)
    {
      if (callback->target == target)
	{
	  /* Run the callback and then free it.  */
	  callback->function ((RenderBuffer) (void *) buffer,
			      callback->data);
	  callback->buffer_next->buffer_last = callback->buffer_last;
	  callback->buffer_last->buffer_next = callback->buffer_next;
	  callback->target_next->target_last = callback->target_last;
	  callback->target_last->target_next = callback->target_next;
	  last = callback;
	  callback = callback->buffer_next;
	  XLFree (last);
	}
      else
	callback = callback->buffer_next;
    }

  return;
}

static void
MaybeRunIdleCallbacks (PictureBuffer *buffer, PictureTarget *target)
{
  BufferActivityRecord *record;
  PresentRecord *presentation;

  /* Look through BUFFER's list of activity and presentation records.
     If there is nothing left pertaining to TARGET, then run idle
     callbacks.  */
  record = buffer->activity.buffer_next;
  while (record != &buffer->activity)
    {
      if (record->target == target)
	/* There is still pending activity.  */
	return;

      record = record->buffer_next;
    }

  /* Next, loop through BUFFER's list of presentation records.  If the
     buffer is still busy on TARGET, then return.  */
  presentation = buffer->pending.buffer_next;
  while (presentation != &buffer->pending)
    {
      if (presentation->target == target)
	/* There is still pending activity.  */
	return;

      presentation = presentation->buffer_next;
    }

  /* Run idle callbacks.  */
  RunIdleCallbacks (buffer, target);
}

static void
UnlinkActivityRecord (BufferActivityRecord *record)
{
  record->buffer_last->buffer_next = record->buffer_next;
  record->buffer_next->buffer_last = record->buffer_last;
  record->target_last->target_next = record->target_next;
  record->target_next->target_last = record->target_last;
  record->global_last->global_next = record->global_next;
  record->global_next->global_last = record->global_last;
}

/* Handle an event saying that the X server has completed everything
   up to ID.  */

static void
HandleActivityEvent (uint64_t counter)
{
  BufferActivityRecord *record, *last;

  /* Look through the global activity list for a record matching
     counter.  */
  record = all_activity.global_next;
  while (record != &all_activity)
    {
      last = record;
      record = record->global_next;

      if (last->id == counter)
	{
	  /* Remove the record.  Then, run any callbacks pertaining to
	     it.  This code mandates that there only be a single
	     activity record for each buffer-target combination on the
	     global list at any given time.  */
	  UnlinkActivityRecord (last);
	  MaybeRunIdleCallbacks (last->buffer, last->target);

	  /* Free the record.  */
	  XLFree (last);
	}
    }
}



static void
FreeBackBuffer (BackBuffer *buffer)
{
  XRenderFreePicture (compositor.display, (buffer->picture
					   & ~BufferSync
					   & ~BufferBusy));
  XFreePixmap (compositor.display, (buffer->pixmap
				    & ~BufferSync));
  FenceRelease (buffer->idle_fence);
  XLFree (buffer);
}

static void
FreeBackBuffers (PictureTarget *target)
{
  int i;

  for (i = 0; i < ArrayElements (target->back_buffers); ++i)
    {
      if (target->back_buffers[i])
	FreeBackBuffer (target->back_buffers[i]);
    }

  /* Also clear target->picture if it is a window target.  */
  if (target->window)
    target->picture = None;

  target->back_buffers[0] = NULL;
  target->back_buffers[1] = NULL;
  target->current_back_buffer = -1;
}

static BackBuffer *
CreateBackBuffer (PictureTarget *target)
{
  BackBuffer *buffer;
  XRenderPictureAttributes attrs;
  Window root_window;

  /* Create a single back buffer.  */
  root_window = DefaultRootWindow (compositor.display);
  buffer = XLMalloc (sizeof *buffer);

  buffer->pixmap
    = XCreatePixmap (compositor.display, root_window,
		     target->width, target->height,
		     compositor.n_planes);
  buffer->picture
    = XRenderCreatePicture (compositor.display, buffer->pixmap,
			    compositor.argb_format, 0, &attrs);
  buffer->idle_fence = GetFence ();

  /* The target is no longer freshly presented.  */
  target->flags &= ~JustPresented;

  return buffer;
}

/* Forward declaration.  */
static XserverRegion ServerRegionFromRegion (pixman_region32_t *);

static PresentCompletionCallback *
MakePresentationCallback (void)
{
  PresentCompletionCallback *callback_rec;

  callback_rec = XLMalloc (sizeof *callback_rec);
  callback_rec->id = present_serial;
  callback_rec->next = all_completion_callbacks.next;
  callback_rec->last = &all_completion_callbacks;
  all_completion_callbacks.next->last = callback_rec;
  all_completion_callbacks.next = callback_rec;

  return callback_rec;
}

static PresentCompletionCallback *
SwapBackBuffers (PictureTarget *target, pixman_region32_t *damage)
{
  XserverRegion region;
  XSyncFence fence;
  BackBuffer *back_buffer;
  int other;
  PresentCompletionCallback *callback;

  /* Swap back buffers according to the damage in region using the
     Present extension.  Return a present completion callback.  */

  if (damage)
    region = ServerRegionFromRegion (damage);
  else
    region = None;

  /* Find the current back buffer.  */
  back_buffer = target->back_buffers[target->current_back_buffer];

  /* Get the idle fence.  */
  fence = FenceToXFence (back_buffer->idle_fence);

  /* Present the current pixmap.  */
  present_serial++;

  /* 0 is not a valid serial here, so don't let it reach that.  */
  if (!present_serial)
    present_serial++;

  /* TODO: handle completion correctly.  */
  XPresentPixmap (compositor.display, target->window,
		  back_buffer->pixmap, present_serial,
		  None, region, 0, 0, None, None, fence,
		  PresentOptionAsync, 0, 0, 0, NULL, 0);

  /* Mark the back buffer as busy, and the other back buffer as having
     been released.  */
  SetBufferBusy (back_buffer);
  back_buffer->present_serial = present_serial;

  /* Set the current back buffer's age to 1, meaning that it reflects
     the contents of the buffer 1 swap ago.  */
  back_buffer->age = 1;

  /* Find the other back buffer and clear its busy flag if set.  */
  other = (target->current_back_buffer ? 0 : 1);

  if (target->back_buffers[other])
    {
      target->back_buffers[other]->picture |= BufferSync;
      ClearBufferBusy (target->back_buffers[other]);

      /* Age the other buffer as well, given that it is not currently
	 garbaged.  */
      if (target->back_buffers[other]->age)
	target->back_buffers[other]->age++;
    }

  if (region)
    XFixesDestroyRegion (compositor.display, region);

  target->current_back_buffer = -1;
  target->picture = None;

  callback = MakePresentationCallback ();
  callback->id = present_serial;

  return callback;
}

static void
SwapBackBuffersWithCopy (PictureTarget *target, pixman_region32_t *damage)
{
  pixman_box32_t *boxes;
  int nboxes, i, other;
  BackBuffer *back_buffer;

  boxes = pixman_region32_rectangles (damage, &nboxes);

  if (nboxes > 20)
    /* Damage is too large; simplify it by using the extents
       instead.  */
    boxes = &damage->extents, nboxes = 1;

  /* Find the current back buffer.  */
  back_buffer = target->back_buffers[target->current_back_buffer];

  for (i = 0; i < nboxes; ++i)
    XCopyArea (compositor.display,
	       back_buffer->pixmap,
	       target->window, target->gc,
	       boxes[i].x1, boxes[i].y1,
	       boxes[i].x2 - boxes[i].x1,
	       boxes[i].y1 - boxes[i].y2,
	       boxes[i].x1, boxes[i].y1);

  /* Age and the other back buffer.  N.B. that presenting and then
     copying is not handled at all, so be sure to only call one or the
     other for any given target.  */

  other = (target->current_back_buffer ? 0 : 1);
  back_buffer->age = 1;

  if (target->back_buffers[other])
    target->back_buffers[other]++;
}

static void
MaybeAwaitBuffer (BackBuffer *buffer)
{
  if (!(buffer->picture & BufferSync))
    return;

  /* Flush any pending presentation requests.  */
  if (!(buffer->pixmap & BufferSync))
    XFlush (compositor.display);

  /* Start waiting on the buffer's idle fence.  */
  FenceAwait (buffer->idle_fence);
  buffer->picture &= ~BufferSync;
  buffer->pixmap &= ~BufferSync;

  /* Set the present serial to 0 so BufferSync is not set again
     afterwards.  */
  buffer->present_serial = 0;
}

static BackBuffer *
GetNextBackBuffer (PictureTarget *target)
{
  /* Return the next back buffer that will be used, but do not create
     any if none exists.  */

  if (target->back_buffers[0]
      && !IsBufferBusy (target->back_buffers[0]))
    return target->back_buffers[0];

  return target->back_buffers[1];
}

static void
EnsurePicture (PictureTarget *target)
{
  BackBuffer *buffer;

  if (target->picture)
    return;

  /* Find a back buffer that isn't busy.  */
  if (!target->back_buffers[0]
      || !IsBufferBusy (target->back_buffers[0]))
    {
      buffer = target->back_buffers[0];
      target->current_back_buffer = 0;

      if (!buffer)
	{
	  /* Create the first back buffer.  */
	  buffer = CreateBackBuffer (target);
	  target->back_buffers[0] = buffer;
	}
    }
  else
    {
      buffer = target->back_buffers[1];
      target->current_back_buffer = 1;

      if (!buffer)
	{
	  /* Create the second back buffer.  */
	  buffer = CreateBackBuffer (target);
	  target->back_buffers[1] = buffer;
	}
    }

  /* The selected buffer must not be busy.  */
  XLAssert (!IsBufferBusy (buffer));

  /* If the fence will be triggered, wait on it now.  */
  MaybeAwaitBuffer (buffer);

  /* Set target->picture.  */
  target->picture = buffer->picture;
}



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

static void
InitSynchronizedPresentation (void)
{
  XrmDatabase rdb;
  XrmName namelist[3];
  XrmClass classlist[3];
  XrmValue value;
  XrmRepresentation type;

  rdb = XrmGetDatabase (compositor.display);

  if (!rdb)
    return;

  namelist[1] = XrmStringToQuark ("useDirectPresentation");
  namelist[0] = app_quark;
  namelist[2] = NULLQUARK;

  classlist[1] = XrmStringToQuark ("UseDirectPresentation");
  classlist[0] = resource_quark;
  classlist[2] = NULLQUARK;

  /* Enable the use of direct presentation if
     *.UseDirectPresentation.*.useDirectPresentation is true.  This is
     still incomplete, as the features necessary for it to play nice
     with frame synchronization have not yet been implemented in the X
     server.  */

  if (XrmQGetResource (rdb, namelist, classlist,
		       &type, &value)
      && type == QString
      && (!strcmp (value.addr, "True")
	  || !strcmp (value.addr, "true")))
    use_direct_presentation = True;
}

static void
AddAdditionalModifier (const char *name)
{
  int i, j;

  for (i = 0; i < ArrayElements (known_modifiers) - 1; ++i)
    {
      if (!strcmp (known_modifiers[i].name, name))
	{
	  /* The modifier was found.  See if it already exists.  */
	  for (j = 0; j < num_specified_modifiers; ++j)
	    {
	      if (user_specified_modifiers[j]
		  == known_modifiers[i].modifier)
		/* The modifier was already specified.  */
		return;
	    }

	  /* Otherwise, increment num_specified_modifiers.  */
	  num_specified_modifiers++;

	  /* Make user_specified_modifiers big enough.  */
	  user_specified_modifiers
	    = XLRealloc (user_specified_modifiers,
			 num_specified_modifiers
			 * sizeof *user_specified_modifiers);

	  /* And add the modifier.  */
	  user_specified_modifiers[num_specified_modifiers - 1]
	    = known_modifiers[i].modifier;
	  return;
	}
    }

  fprintf (stderr, "Unknown buffer format modifier: %s\n", name);
}

static void
ParseAdditionalModifiers (const char *string)
{
  const char *end, *sep;
  char *buffer;

  end = string + strlen (string);

  while (string < end)
    {
      /* Find the next comma.  */
      sep = strchr (string, ',');

      if (!sep)
	sep = end;

      /* Copy the text between string and sep into buffer.  */
      buffer = alloca (sep - string + 1);
      memcpy (buffer, string, sep - string);
      buffer[sep - string] = '\0';

      /* Add this modifier.  */
      AddAdditionalModifier (buffer);

      string = sep + 1;
    }
}

static void
InitAdditionalModifiers (void)
{
  XrmDatabase rdb;
  XrmName namelist[3];
  XrmClass classlist[3];
  XrmValue value;
  XrmRepresentation type;
  char *name;

  rdb = XrmGetDatabase (compositor.display);

  if (!rdb)
    return;

  if (!asprintf (&name, "additionalModifiersOfScreen%d",
		 DefaultScreen (compositor.display)))
    return;

  namelist[1] = XrmStringToQuark (name);
  free (name);

  namelist[0] = app_quark;
  namelist[2] = NULLQUARK;

  classlist[1] = XrmStringToQuark ("AdditionalModifiers");
  classlist[0] = resource_quark;
  classlist[2] = NULLQUARK;

  /* Enable the use of direct presentation if
     *.UseDirectPresentation.*.useDirectPresentation is true.  This is
     still incomplete, as the features necessary for it to play nice
     with frame synchronization have not yet been implemented in the X
     server.  */

  if (XrmQGetResource (rdb, namelist, classlist,
		       &type, &value)
      && type == QString)
    ParseAdditionalModifiers ((const char *) value.addr);
}

/* Forward declaration.  */
static void AddRenderFlag (int);

static Bool
InitRenderFuncs (void)
{
  int major, minor, error_base, event_base;
  XSetWindowAttributes attrs;

  /* Set up the default visual.  */
  compositor.visual = PickVisual (&compositor.n_planes);

  /* Initialize the presentation extension.  */
  if (!XPresentQueryExtension (compositor.display,
			       &present_opcode,
			       &error_base, &event_base)
      || !XPresentQueryVersion (compositor.display,
				&major, &minor))
    {
      fprintf (stderr, "The X presentation extension is not supported"
	       " by this X server\n");
      return False;
    }

  /* Figure out whether or not the user wants synchronized
     presentation.  */
  InitSynchronizedPresentation ();

  /* Find out what additional modifiers the user wants.  */
  InitAdditionalModifiers ();

  if (use_direct_presentation)
    AddRenderFlag (SupportsDirectPresent);

  /* Create an unmapped, InputOnly window, that is used to receive
     roundtrip events.  */
  attrs.override_redirect = True;
  round_trip_window = XCreateWindow (compositor.display,
				     DefaultRootWindow (compositor.display),
				     -1, -1, 1, 1, 0, CopyFromParent, InputOnly,
				     CopyFromParent, CWOverrideRedirect,
				     &attrs);

  /* Initialize the hash table between presentation windows and
     targets.  */
  xid_table = XLCreateAssocTable (256);

  /* Return success if the visual was found.  */
  return compositor.visual != NULL;
}

static RenderTarget
TargetFromDrawable (Drawable drawable, Window window,
		    unsigned long standard_event_mask)
{
  XRenderPictureAttributes picture_attrs;
  PictureTarget *target;
  XGCValues gcvalues;

  /* This is just to pacify GCC; picture_attrs is not used as mask is
     0.  */
  memset (&picture_attrs, 0, sizeof picture_attrs);
  target = XLCalloc (1, sizeof *target);
  target->window = window;

  if (window != None)
    /* Start selecting for presentation events from the given
       window.  */
    target->presentation_event_context
      = XPresentSelectInput (compositor.display, window,
			     PresentIdleNotifyMask
			     | PresentCompleteNotifyMask);
  else
    /* Create the picture corresponding to this drawable.  */
    target->picture = XRenderCreatePicture (compositor.display,
					    drawable,
					    compositor.argb_format,
					    0, &picture_attrs);

  /* Initialize the current back buffer.  */
  target->current_back_buffer = -1;

  /* Initialize the list of release records.  */
  target->pending.target_next = &target->pending;
  target->pending.target_last = &target->pending;

  /* Initialize the list of target activity.  */
  target->activity.target_next = &target->activity;
  target->activity.target_last = &target->activity;

  /* And idle callbacks.  */
  target->idle_callbacks.target_next = &target->idle_callbacks;
  target->idle_callbacks.target_last = &target->idle_callbacks;

  /* And the event mask.  */
  target->standard_event_mask = standard_event_mask;

  if (window)
    {
      /* Add the window to the assoc table.  */
      XLMakeAssoc (xid_table, window, target);

      /* Create the GC used to swap back buffers.  */
      target->gc = XCreateGC (compositor.display,
			      window, 0, &gcvalues);
    }

  return (RenderTarget) (void *) target;
}

static RenderTarget
TargetFromPixmap (Pixmap pixmap)
{
  return TargetFromDrawable (pixmap, None, NoEventMask);
}

static RenderTarget
TargetFromWindow (Window window, unsigned long event_mask)
{
  return TargetFromDrawable (window, window, event_mask);
}

static void
SetStandardEventMask (RenderTarget target, unsigned long standard_event_mask)
{
  PictureTarget *pict_target;

  pict_target = target.pointer;

  /* Set the standard event mask.  This is used to temporarily
     suppress exposures.  */
  pict_target->standard_event_mask = standard_event_mask;
}

static void
NoteTargetSize (RenderTarget target, int width, int height)
{
  PictureTarget *pict_target;

  pict_target = target.pointer;

  if (width != pict_target->width
      || height != pict_target->height)
    /* Recreate all the back buffers for the new target size.  */
    FreeBackBuffers (pict_target);

  pict_target->width = width;
  pict_target->height = height;
}

static Picture
PictureFromTarget (RenderTarget target)
{
  PictureTarget *pict_target;

  pict_target = target.pointer;
  return pict_target->picture;
}

static void
FreePictureFromTarget (Picture picture)
{
  /* There is no need to free these pictures.  */
}

static void
RemovePresentRecord (PresentRecord *record)
{
  record->target_next->target_last = record->target_last;
  record->target_last->target_next = record->target_next;
  record->buffer_next->buffer_last = record->buffer_last;
  record->buffer_last->buffer_next = record->buffer_next;

  XLFree (record);
}

static void
DestroyRenderTarget (RenderTarget target)
{
  PictureTarget *pict_target;
  PresentRecord *record, *last;
  BufferActivityRecord *activity_record, *activity_last;
  IdleCallback *idle, *idle_last;

  pict_target = target.pointer;

  /* Assert that there are no more buffers left in the active buffer
     list.  */
  XLAssert (pict_target->buffers_used == NULL);

  /* Destroy all back buffers.  */
  FreeBackBuffers (pict_target);

  if (pict_target->window)
    {
      /* Delete the window from the assoc table.  */
      XLDeleteAssoc (xid_table, pict_target->window);

      /* Free the GC.  */
      XFreeGC (compositor.display, pict_target->gc);
    }

  /* Free attached presentation records.  */
  record = pict_target->pending.target_next;
  while (record != &pict_target->pending)
    {
      last = record;
      record = record->target_next;

      /* Free the record.  */
      RemovePresentRecord (last);
    }

  /* Free all activity associated with this target.  */
  activity_record = pict_target->activity.target_next;
  while (activity_record != &pict_target->activity)
    {
      activity_last = activity_record;
      activity_record = activity_record->target_next;

      UnlinkActivityRecord (activity_last);
      XLFree (activity_last);
    }

  /* Free all idle callbacks on this target.  */
  idle = pict_target->idle_callbacks.target_next;
  while (idle != &pict_target->idle_callbacks)
    {
      idle_last = idle;
      idle = idle->target_next;

      /* Free the callback without doing anything else with it.  */
      XLFree (idle_last);
    }

  if (pict_target->picture)
    XRenderFreePicture (compositor.display,
			pict_target->picture);

  XFree (pict_target);
}

static void
FillBoxesWithTransparency (RenderTarget target, pixman_box32_t *boxes,
			   int nboxes, int min_x, int min_y)
{
  XRectangle *rects;
  static XRenderColor color;
  int i;
  PictureTarget *pict_target;

  pict_target = target.pointer;

  /* Ensure a back buffer is created or used.  */
  EnsurePicture (pict_target);

  if (nboxes < 256)
    rects = alloca (sizeof *rects * nboxes);
  else
    rects = XLMalloc (sizeof *rects * nboxes);

  /* Pacify GCC.  */
  memset (rects, 0, sizeof *rects * nboxes);

  for (i = 0; i < nboxes; ++i)
    {
      rects[i].x = BoxStartX (boxes[i]) - min_x;
      rects[i].y = BoxStartY (boxes[i]) - min_y;
      rects[i].width = BoxWidth (boxes[i]);
      rects[i].height = BoxHeight (boxes[i]);
    }

  XRenderFillRectangles (compositor.display, PictOpClear,
			 pict_target->picture, &color, rects,
			 nboxes);

  if (nboxes >= 256)
    XLFree (rects);
}

static XserverRegion
ServerRegionFromRegion (pixman_region32_t *region)
{
  XRectangle *rects;
  int i, nboxes;
  pixman_box32_t *boxes;
  XserverRegion server_region;

  boxes = pixman_region32_rectangles (region, &nboxes);

  if (nboxes < 256)
    rects = alloca (sizeof *rects * nboxes);
  else
    rects = XLMalloc (sizeof *rects * nboxes);

  for (i = 0; i < nboxes; ++i)
    {
      rects[i].x = BoxStartX (boxes[i]);
      rects[i].y = BoxStartY (boxes[i]);
      rects[i].width = BoxWidth (boxes[i]);
      rects[i].height = BoxHeight (boxes[i]);
    }

  server_region = XFixesCreateRegion (compositor.display, rects,
				      nboxes);

  if (nboxes >= 256)
    XLFree (rects);

  return server_region;
}

static void
ClearRectangle (RenderTarget target, int x, int y, int width, int height)
{
  PictureTarget *pict_target;
  static XRenderColor color;

  pict_target = target.pointer;

  XRenderFillRectangle (compositor.display, PictOpClear,
			pict_target->picture, &color, x, y,
			width, height);
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

static double
GetScale (DrawParams *params)
{
  if (params->flags & ScaleSet)
    return params->scale;

  return 1.0;
}

static double
GetSourceX (DrawParams *params)
{
  if (params->flags & OffsetSet)
    return params->off_x;

  return 0.0;
}

static double
GetSourceY (DrawParams *params)
{
  if (params->flags & OffsetSet)
    return params->off_y;

  return 0.0;
}

static BufferTransform
GetBufferTransform (DrawParams *params)
{
  if (params->flags & TransformSet)
    return params->transform;

  return Normal;
}

static Bool
CompareStretch (DrawParams *params, DrawParams *other)
{
  if ((params->flags & StretchSet)
      != (other->flags & StretchSet))
    return False;

  if (params->flags & StretchSet)
    return (other->crop_width == params->crop_width
	    && other->crop_height == params->crop_height
	    && other->stretch_width == params->stretch_width
	    && other->stretch_height == params->stretch_height);

  return True;
}

static void
MaybeApplyTransform (PictureBuffer *buffer, DrawParams *params)
{
  XTransform transform;
  Matrix ftransform;

  if (GetScale (params) == GetScale (&buffer->params)
      && GetSourceX (params) == GetSourceX (&buffer->params)
      && GetSourceY (params) == GetSourceY (&buffer->params)
      && (GetBufferTransform (params)
	  == GetBufferTransform (&buffer->params))
      && CompareStretch (params, &buffer->params))
    /* Nothing changed.  */
    return;

  /* Otherwise, compute and apply the new transform. */
  if (!params->flags)
    /* No transform of any kind is set, use the identity matrix.  */
    XRenderSetPictureTransform (compositor.display,
				buffer->picture,
				&identity_transform);
  else
    {
      MatrixIdentity (&ftransform);

      /* The buffer transform must always be applied first.  */

      if (params->flags & TransformSet)
	ApplyInverseTransform (buffer->width, buffer->height,
			       &ftransform, params->transform);

      /* Note that these must be applied in the right order.  First,
	 the scale is applied.  Then, the offset, and finally the
	 stretch.  */

      if (params->flags & ScaleSet)
	MatrixScale (&ftransform, 1.0 / GetScale (params),
		     1.0 / GetScale (params));

      if (params->flags & OffsetSet)
	MatrixTranslate (&ftransform, params->off_x, params->off_y);

      if (params->flags & StretchSet)
	MatrixScale (&ftransform,
		     params->crop_width / params->stretch_width,
		     params->crop_height / params->stretch_height);

      /* Export the matrix to an XTransform.  */
      MatrixExport (&ftransform, &transform);

      /* Set the transform.  The transform maps from dest coords to
	 picture coords, so that [X Y 1] = TRANSFORM * [DX DY 1].  */
      XRenderSetPictureTransform (compositor.display, buffer->picture,
				  &transform);
    }

  /* Save the parameters into buffer.  */
  buffer->params = *params;
}

static void
Composite (RenderBuffer buffer, RenderTarget target,
	   Operation op, int src_x, int src_y, int x, int y,
	   int width, int height, DrawParams *draw_params)
{
  PictureBuffer *picture_buffer;
  PictureTarget *picture_target;
  XLList *tem;

  picture_buffer = buffer.pointer;
  picture_target = target.pointer;

  /* Ensure a back buffer is created.  */
  EnsurePicture (picture_target);

  /* Maybe set the transform if the parameters changed.  (draw_params
     specifies a transform to apply to the buffer, not to the
     target.)  */
  MaybeApplyTransform (picture_buffer, draw_params);

  /* Do the compositing.  */
  XRenderComposite (compositor.display, ConvertOperation (op),
		    picture_buffer->picture, None,
		    picture_target->picture,
		    /* src-x, src-y, mask-x, mask-y.  */
		    src_x, src_y, 0, 0,
		    /* dst-x, dst-y, width, height.  */
		    x, y, width, height);

  for (tem = picture_target->buffers_used; tem; tem = tem->next)
    {
      /* Return if the buffer is already in the buffers_used list.  */

      if (tem->data == picture_buffer)
	return;
    }

  /* Record pending buffer activity; the roundtrip message is then
     sent later.  */

  picture_target->buffers_used
    = XLListPrepend (picture_target->buffers_used, picture_buffer);
}

static RenderCompletionKey
FinishRender (RenderTarget target, pixman_region32_t *damage,
	      RenderCompletionFunc function, void *data)
{
  XLList *tem, *last;
  PictureTarget *pict_target;
  uint64_t roundtrip_id;
  PresentCompletionCallback *callback_rec;

  pict_target = target.pointer;

  if (!pict_target->buffers_used)
    /* No buffers were used.  */
    return NULL;

  /* Finish rendering.  This function then sends a single roundtrip
     message and records buffer activity for each buffer involved in
     the update based on that.  */

  roundtrip_id = SendRoundtripMessage ();
  tem = pict_target->buffers_used;

  while (tem)
    {
      last = tem;
      tem = tem->next;

      /* Record buffer activity on this one buffer.  */
      RecordBufferActivity (last->data, pict_target,
			    roundtrip_id);

      /* Free the list element.  */
      XLFree (last);
    }

  /* Clear buffers_used.  */
  pict_target->buffers_used = NULL;

  /* Swap the back buffer to the screen if it was used.  */

  if (pict_target->current_back_buffer != -1)
    {
      /* If a callback was specified, then use the Present extension,
	 and return a completion callback.  */

      if (function)
	{
	  callback_rec = SwapBackBuffers (pict_target, damage);
	  callback_rec->function = function;
	  callback_rec->data = data;

	  return (RenderCompletionKey) callback_rec;
	}

      /* Otherwise, swap buffers using XCopyArea.  */
      SwapBackBuffersWithCopy (pict_target, damage);
      return NULL;
    }

  /* Otherwise, there is nothing to do.  */
  return NULL;
}

static void
CancelCompletionCallback (RenderCompletionKey key)
{
  PresentCompletionCallback *callback;

  callback = key;
  callback->next->last = callback->last;
  callback->last->next = callback->next;

  XLFree (callback);
}

static int
TargetAge (RenderTarget target)
{
  BackBuffer *buffer;
  PictureTarget *pict_target;

  pict_target = target.pointer;

  if (pict_target->flags & JustPresented)
    return -2;

  buffer = GetNextBackBuffer (pict_target);

  if (!buffer)
    return -1;

  if (buffer->age > INT_MAX)
    return -1;

  return (int) buffer->age - 1;
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

static PresentRecord *
FindPresentRecord (PictureBuffer *buffer, PictureTarget *target)
{
  PresentRecord *record;

  record = buffer->pending.buffer_next;
  while (record != &buffer->pending)
    {
      if (record->target == target)
	return record;

      record = record->buffer_next;
    }

  /* No matching record was found.  */
  return NULL;
}

static PresentRecord *
AllocateRecord (PictureBuffer *buffer, PictureTarget *target)
{
  PresentRecord *record;

  /* Allocate a record and link it onto both BUFFER and TARGET.  */
  record = XLCalloc (1, sizeof *record);
  record->buffer_next = buffer->pending.buffer_next;
  record->buffer_last = &buffer->pending;
  buffer->pending.buffer_next->buffer_last = record;
  buffer->pending.buffer_next = record;
  record->target_next = target->pending.target_next;
  record->target_last = &target->pending;
  target->pending.target_next->target_last = record;
  target->pending.target_next = record;
  record->buffer = buffer;
  record->target = target;

  return record;
}

/* Direct presentation support.  When a surface has no transform
   applied, its pixmap can be presented directly onto a window
   target.  */

static PresentCompletionKey
PresentToWindow (RenderTarget target, RenderBuffer source,
		 pixman_region32_t *damage,
		 PresentCompletionFunc callback, void *data)
{
  PictureBuffer *buffer;
  PictureTarget *pict_target;
  XserverRegion region;
  PresentRecord *record;
  PresentCompletionCallback *callback_rec;

  /* Present SOURCE onto TARGET.  Return False if the presentation is
     not supported.  */
  buffer = source.pointer;
  pict_target = target.pointer;

  if (pict_target->flags & NoPresentation)
    return NULL;

  /* If the depth is not the same as the window, don't present.  */
  if (buffer->depth != compositor.n_planes)
    return NULL;

  if (!(buffer->flags & CanPresent))
    return NULL;

  /* Since presentation has happened, free all the back buffers.  */
  FreeBackBuffers (pict_target);

  /* Build the damage region.  */
  if (damage)
    region = ServerRegionFromRegion (damage);
  else
    region = None;

  if (use_direct_presentation)
    {
      /* Present the pixmap now from the damage; it will complete upon
	 the next vblank if direct presentation is enabled, or
	 immediately if it is not.  */
      XPresentPixmap (compositor.display, pict_target->window, buffer->pixmap,
		      ++present_serial, None, region, 0, 0, None, None, None,
		      PresentOptionNone, 0, 1, 0, NULL, 0);
    }
  else
    /* Direct presentation is off; present the pixmap asynchronously
       at an msc of 0.  */
    XPresentPixmap (compositor.display, pict_target->window, buffer->pixmap,
		    ++present_serial, None, region, 0, 0,  None, None, None,
		    PresentOptionAsync, 0, 0, 0, NULL, 0);

  if (region)
    XFixesDestroyRegion (compositor.display, region);

  /* Now, we must wait for pict_target to release the buffer.  See if
     a presentation to this target has already been recorded.  */
  record = FindPresentRecord (buffer, pict_target);

  /* If it was, just set the serial.  */
  if (record)
    record->serial = present_serial;
  else
    {
      /* Otherwise, allocate and attach a record.  */
      record = AllocateRecord (buffer, pict_target);
      record->serial = present_serial;
    }

  /* Allocate a presentation completion callback.  */
  callback_rec = MakePresentationCallback ();
  callback_rec->function = callback;
  callback_rec->data = data;
  callback_rec->id = present_serial;

  /* Mark the target as just having been presented.  The flag is
     cleared the next time a back buffer is created.  */
  pict_target->flags |= JustPresented;

  return callback_rec;
}

/* Cancel the given presentation callback.  */

static void
CancelPresentationCallback (PresentCompletionKey key)
{
  PresentCompletionCallback *callback;

  callback = key;
  callback->next->last = callback->last;
  callback->last->next = callback->next;

  XLFree (callback);
}

static RenderFuncs picture_render_funcs =
  {
    .init_render_funcs = InitRenderFuncs,
    .target_from_window = TargetFromWindow,
    .target_from_pixmap = TargetFromPixmap,
    .set_standard_event_mask = SetStandardEventMask,
    .note_target_size = NoteTargetSize,
    .picture_from_target = PictureFromTarget,
    .free_picture_from_target = FreePictureFromTarget,
    .destroy_render_target = DestroyRenderTarget,
    .fill_boxes_with_transparency = FillBoxesWithTransparency,
    .clear_rectangle = ClearRectangle,
    .composite = Composite,
    .finish_render = FinishRender,
    .cancel_completion_callback = CancelCompletionCallback,
    .target_age = TargetAge,
    .import_fd_fence = ImportFdFence,
    .wait_fence = WaitFence,
    .delete_fence = DeleteFence,
    .get_finish_fence = GetFinishFence,
    .present_to_window = PresentToWindow,
    .cancel_presentation_callback = CancelPresentationCallback,
  };

static void
AddRenderFlag (int flag)
{
  picture_render_funcs.flags |= flag;
}

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
  int i, length, pair_count, j;
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
	     modifier, and another one for each manually specified
	     modifier.  */
	  pair_count += 1 + num_specified_modifiers;
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

      for (j = 0; j < length; ++j)
	{
	  /* Then, add length for each explicit modifier that wasn't
	     already specified.  */

	  if (mods[j] != DRM_FORMAT_MOD_INVALID
	      && mods[j] != DRM_FORMAT_MOD_LINEAR)
	    pair_count += length;
	}

      memcpy (all_formats[i].supported_modifiers, mods,
	      sizeof *mods * length);
      free (reply);
    }

  *pair_count_return = pair_count;
}

static void
InitDrmFormats (void)
{
  int pair_count, i, j, n, k;

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

      /* And add all of the user-specified modifiers.  */
      for (j = 0; j < num_specified_modifiers; ++j)
	{
	  /* Assert that n < pair_count.  */
	  XLAssert (n < pair_count);

	  drm_formats[n].drm_format = all_formats[i].format_code;
	  drm_formats[n].drm_modifier = user_specified_modifiers[j];
	  n++;
	}

      /* Now add every supported explicit modifier.  */
      for (j = 0; j < all_formats[i].n_supported_modifiers; ++i)
	{
	  /* Ignore previously specified modifiers.  */

	  if ((all_formats[i].supported_modifiers[j]
	       == DRM_FORMAT_MOD_INVALID))
	    continue;

	  /* Ignore user-specified modifiers.  */

	  for (k = 0; k < num_specified_modifiers; ++k)
	    {
	      if (user_specified_modifiers[k]
		  == all_formats[i].supported_modifiers[j])
		continue;
	    }

	  /* Check n < pair_count.  */
	  XLAssert (n < pair_count);

	  /* Add the specified modifier.  */
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

  XLAddFdFlag (fd, FD_CLOEXEC, True);

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

static Bool
PictFormatIsPresentable (XRenderPictFormat *format)
{
  /* If format has the same masks as the visual format, then it is
     presentable.  */
  if (!memcmp (&format->direct, &compositor.argb_format->direct,
	       sizeof format->direct))
    return True;

  return False;
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
  PictureBuffer *buffer;

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

  /* Create the wrapper object.  */
  buffer = XLCalloc (1, sizeof *buffer);
  buffer->picture = picture;
  buffer->pixmap = pixmap;
  buffer->depth = depth;
  buffer->width = attributes->width;
  buffer->height = attributes->height;

  /* Initialize the list of release records.  */
  buffer->pending.buffer_next = &buffer->pending;
  buffer->pending.buffer_last = &buffer->pending;

  /* And the list of idle funcs.  */
  buffer->idle_callbacks.buffer_next = &buffer->idle_callbacks;
  buffer->idle_callbacks.buffer_last = &buffer->idle_callbacks;

  /* And the list of pending activity.  */
  buffer->activity.buffer_next = &buffer->activity;
  buffer->activity.buffer_last = &buffer->activity;

  /* If the format is presentable, mark the buffer as presentable.  */
  if (PictFormatIsPresentable (format))
    buffer->flags |= CanPresent;

  /* And mark it as opaque if it is.  */
  if (!format->direct.alphaMask)
    buffer->flags |= IsOpaque;

  return (RenderBuffer) (void *) buffer;

 error:
  CloseFileDescriptors (attributes);
  *error = True;
  return (RenderBuffer) NULL;
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
  PictureBuffer *buffer;

  if (success)
    {
      /* This is just to pacify GCC.  */
      memset (&picture_attrs, 0, sizeof picture_attrs);

      picture = XRenderCreatePicture (compositor.display,
				      pending->pixmap,
				      pending->format, 0,
				      &picture_attrs);

      /* Create the wrapper structure.  */
      buffer = XLCalloc (1, sizeof *buffer);
      buffer->picture = picture;
      buffer->pixmap = pending->pixmap;
      buffer->depth = pending->depth;
      buffer->width = pending->width;
      buffer->height = pending->height;

      /* Initialize the list of release records.  */
      buffer->pending.buffer_next = &buffer->pending;
      buffer->pending.buffer_last = &buffer->pending;

      /* And the list of idle funcs.  */
      buffer->idle_callbacks.buffer_next = &buffer->idle_callbacks;
      buffer->idle_callbacks.buffer_last = &buffer->idle_callbacks;

      /* And the list of pending activity.  */
      buffer->activity.buffer_next = &buffer->activity;
      buffer->activity.buffer_last = &buffer->activity;

      /* If the format is presentable, mark the buffer as presentable.  */
      if (PictFormatIsPresentable (pending->format))
	buffer->flags |= CanPresent;

      /* And mark it as opaque if it is.  */
      if (!pending->format->direct.alphaMask)
	buffer->flags |= IsOpaque;

      /* Call the creation success function with the new picture.  */
      pending->success_func ((RenderBuffer) (void *) buffer,
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
  record->depth = depth;
  record->width = attributes->width;
  record->height = attributes->height;

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
  PictureBuffer *buffer;
  XRenderPictFormat *pict_format;

  depth = DepthForFormat (attributes->format, &bpp);
  format = attributes->format;

  if (!depth)
    {
      *error = True;
      return (RenderBuffer) NULL;
    }

  /* Duplicate the fd, since XCB closes file descriptors after sending
     them.  */
  fd = fcntl (attributes->fd, F_DUPFD_CLOEXEC, 0);

  if (fd < 0)
    {
      *error = True;
      return (RenderBuffer) NULL;
    }

  /* Obtain the picture format.  */
  pict_format = PictFormatForFormat (format);
  XLAssert (pict_format != NULL);

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

  /* Create the picture for the pixmap.  */
  picture = XRenderCreatePicture (compositor.display, pixmap,
				  pict_format, 0, &picture_attrs);

  /* Create the wrapper object.  */
  buffer = XLCalloc (1, sizeof *buffer);
  buffer->picture = picture;
  buffer->pixmap = pixmap;
  buffer->depth = depth;
  buffer->width = attributes->width;
  buffer->height = attributes->height;

  /* Initialize the list of release records.  */
  buffer->pending.buffer_next = &buffer->pending;
  buffer->pending.buffer_last = &buffer->pending;

  /* And the list of idle funcs.  */
  buffer->idle_callbacks.buffer_next = &buffer->idle_callbacks;
  buffer->idle_callbacks.buffer_last = &buffer->idle_callbacks;

  /* And the list of pending activity.  */
  buffer->activity.buffer_next = &buffer->activity;
  buffer->activity.buffer_last = &buffer->activity;

  /* If the format is presentable, mark the buffer as presentable.  */
  if (PictFormatIsPresentable (pict_format))
    buffer->flags |= CanPresent;

  /* And mark it as opaque if it is.  */
  if (!pict_format->direct.alphaMask)
    buffer->flags |= IsOpaque;

  /* Return the picture.  */
  return (RenderBuffer) (void *) buffer;
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
  XLAssert (depth != 0);

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

static RenderBuffer
BufferFromSinglePixel (uint32_t red, uint32_t green, uint32_t blue,
		       uint32_t alpha, Bool *error)
{
  Picture picture;
  Pixmap pixmap;
  XRenderPictureAttributes picture_attrs;
  XRenderColor color;
  PictureBuffer *buffer;

  /* Create the pixmap.  */
  pixmap = XCreatePixmap (compositor.display,
			  DefaultRootWindow (compositor.display),
			  1, 1, compositor.n_planes);

  /* Create the picture.  */
  picture = XRenderCreatePicture (compositor.display, pixmap,
				  compositor.argb_format, 0,
				  &picture_attrs);

  /* Fill the picture with the single pixel.  */
  color.red = red >> 16;
  color.green = green >> 16;
  color.blue = blue >> 16;
  color.alpha = alpha >> 16;
  XRenderFillRectangle (compositor.display, PictOpSrc,
			picture, &color, 0, 0, 1, 1);

  /* Create the wrapper object.  */
  buffer = XLCalloc (1, sizeof *buffer);
  buffer->picture = picture;
  buffer->pixmap = pixmap;
  buffer->depth = compositor.n_planes;

  /* Initialize the list of release records.  */
  buffer->pending.buffer_next = &buffer->pending;
  buffer->pending.buffer_last = &buffer->pending;

  /* And the list of idle funcs.  */
  buffer->idle_callbacks.buffer_next = &buffer->idle_callbacks;
  buffer->idle_callbacks.buffer_last = &buffer->idle_callbacks;

  /* And the list of pending activity.  */
  buffer->activity.buffer_next = &buffer->activity;
  buffer->activity.buffer_last = &buffer->activity;

  /* Return the picture.  */
  return (RenderBuffer) (void *) buffer;
}

static void
FreeAnyBuffer (RenderBuffer buffer)
{
  PictureBuffer *picture_buffer;
  PresentRecord *record, *last;
  IdleCallback *idle, *last_idle;
  BufferActivityRecord *activity_record, *activity_last;

  picture_buffer = buffer.pointer;

  XFreePixmap (compositor.display,
	       picture_buffer->pixmap);
  XRenderFreePicture (compositor.display,
		      picture_buffer->picture);

  /* Free attached presentation records.  */
  record = picture_buffer->pending.buffer_next;
  while (record != &picture_buffer->pending)
    {
      last = record;
      record = record->buffer_next;

      /* Free the record.  */
      RemovePresentRecord (last);
    }

  /* Free any activity involving this buffer.  */
  activity_record = picture_buffer->activity.buffer_next;
  while (activity_record != &picture_buffer->activity)
    {
      activity_last = activity_record;
      activity_record = activity_record->buffer_next;

      UnlinkActivityRecord (activity_last);
      XLFree (activity_last);
    }

  /* Run and free all idle callbacks.  */
  idle = picture_buffer->idle_callbacks.buffer_next;
  while (idle != &picture_buffer->idle_callbacks)
    {
      last_idle = idle;
      idle = idle->buffer_next;

      /* Unlink the idle callback from the target.  */
      last_idle->target_next->target_last = last_idle->target_last;
      last_idle->target_last->target_next = last_idle->target_next;

      /* Run it.  */
      last_idle->function (buffer, last_idle->data);

      /* Free it.  */
      XLFree (last_idle);
    }

  /* Free the picture buffer itself.  */
  XLFree (picture_buffer);
}

static void
FreeShmBuffer (RenderBuffer buffer)
{
  FreeAnyBuffer (buffer);
}

static void
FreeDmabufBuffer (RenderBuffer buffer)
{
  FreeAnyBuffer (buffer);
}

static void
FreeSinglePixelBuffer (RenderBuffer buffer)
{
  FreeAnyBuffer (buffer);
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

static IdleCallbackKey
AddIdleCallback (RenderBuffer buffer, RenderTarget target,
		 BufferIdleFunc function, void *data)
{
  PictureBuffer *pict_buffer;
  PictureTarget *pict_target;
  IdleCallback *key;

  pict_buffer = buffer.pointer;
  pict_target = target.pointer;

  key = XLMalloc (sizeof *key);
  key->function = function;
  key->data = data;
  key->target = target.pointer;
  key->buffer_next = pict_buffer->idle_callbacks.buffer_next;
  key->buffer_last = &pict_buffer->idle_callbacks;
  key->target_next = pict_target->idle_callbacks.target_next;
  key->target_last = &pict_target->idle_callbacks;
  pict_buffer->idle_callbacks.buffer_next->buffer_last = key;
  pict_buffer->idle_callbacks.buffer_next = key;
  key->target->idle_callbacks.target_next->target_last = key;
  key->target->idle_callbacks.target_next = key;

  return key;
}

static void
CancelIdleCallback (IdleCallbackKey key)
{
  IdleCallback *internal_key;

  internal_key = key;
  internal_key->target_next->target_last = internal_key->target_last;
  internal_key->target_last->target_next = internal_key->target_next;
  internal_key->buffer_next->buffer_last = internal_key->buffer_last;
  internal_key->buffer_last->buffer_next = internal_key->buffer_next;

  XLFree (internal_key);
}

static Bool
IsBufferIdle (RenderBuffer buffer, RenderTarget target)
{
  BufferActivityRecord *record;
  PresentRecord *presentation;
  PictureBuffer *pict_buffer;
  PictureTarget *pict_target;

  pict_buffer = buffer.pointer;
  pict_target = target.pointer;

  /* A buffer is idle if it has no pending activity or
     presentation on the given target.  */
  record = pict_buffer->activity.buffer_next;
  while (record != &pict_buffer->activity)
    {
      if (record->target == pict_target)
	/* There is still pending activity.  */
	return False;

      record = record->buffer_next;
    }

  /* Next, loop through BUFFER's list of presentation records.  If the
     buffer is still busy on TARGET, then return.  */
  presentation = pict_buffer->pending.buffer_next;
  while (presentation != &pict_buffer->pending)
    {
      if (presentation->target == pict_target)
	/* There is still pending activity.  */
	return False;

      presentation = presentation->buffer_next;
    }

  /* The buffer is idle.  */
  return True;
}

static Bool
IdleEventPredicate (Display *display, XEvent *event, XPointer data)
{
  /* Return whether or not the event is relevant to buffer busy
     tracking.  */
  return ((event->type == GenericEvent
	   && event->xgeneric.evtype == PresentIdleNotify)
	  || (event->type == ClientMessage
	      && event->xclient.message_type == _XL_BUFFER_RELEASE));
}

static void
WaitForIdle (RenderBuffer buffer, RenderTarget target)
{
  XEvent event;

  while (!IsBufferIdle (buffer, target))
    {
      XIfEvent (compositor.display, &event, IdleEventPredicate,
		NULL);

      /* We failed to get event data for a generic event, so there's
	 no point in continuing.  */
      if (event.type == GenericEvent
	  && !XGetEventData (compositor.display, &event.xcookie))
	abort ();

      /* Handle the idle event.  */
      HandleOneXEventForPictureRenderer (&event);

      if (event.type == GenericEvent)
	XFreeEventData (compositor.display, &event.xcookie);
    }
}

static void
SetNeedWaitForIdle (RenderTarget target)
{
  PictureTarget *pict_target;

  /* Request that WaitForIdle be valid with buffers presented to the
     given target.  All this normally does is disable presentation
     onto the given target.  */

  pict_target = target.pointer;
  pict_target->flags |= NoPresentation;
}

static Bool
IsBufferOpaque (RenderBuffer buffer)
{
  PictureBuffer *pict_buffer;

  /* Return whether or not the buffer format is opaque, which lets us
     optimize some things out.  */
  pict_buffer = buffer.pointer;

  if (pict_buffer->flags & IsOpaque)
    return True;

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
    .buffer_from_single_pixel = BufferFromSinglePixel,
    .free_shm_buffer = FreeShmBuffer,
    .free_dmabuf_buffer = FreeDmabufBuffer,
    .free_single_pixel_buffer = FreeSinglePixelBuffer,
    .can_release_now = CanReleaseNow,
    .add_idle_callback = AddIdleCallback,
    .cancel_idle_callback = CancelIdleCallback,
    .is_buffer_idle = IsBufferIdle,
    .wait_for_idle = WaitForIdle,
    .set_need_wait_for_idle = SetNeedWaitForIdle,
    .is_buffer_opaque = IsBufferOpaque,
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

static Bool
HandlePresentCompleteNotify (XPresentCompleteNotifyEvent *complete)
{
  PresentCompletionCallback *callback, *last;
#ifdef DEBUG_PRESENT_TIME
  static uint64_t last_ust;
#endif
  Bool rc;

  callback = all_completion_callbacks.next;
  rc = False;

  while (callback != &all_completion_callbacks)
    {
      last = callback;
      callback = callback->next;

      if (last->id == complete->serial_number)
	{
	  /* The presentation is complete.  Run and unlink the
	     callback.  */
	  last->function (last->data);
	  last->next->last = last->last;
	  last->last->next = last->next;

#ifdef DEBUG_PRESENT_TIME
	  fprintf (stderr, "Time taken: %lu us (%g ms) (= 1/%g s)\n",
		   complete->ust - last_ust,
		   (complete->ust - last_ust) / 1000.0,
		   1000.0 / ((complete->ust - last_ust) / 1000.0));
	  last_ust = complete->ust;
#endif

	  XLFree (last);
	  rc = True;
	}
    }

  return rc;
}

static Bool
HandlePresentIdleNotify (XPresentIdleNotifyEvent *idle)
{
  PresentRecord *record;
  PictureTarget *target;
  PictureBuffer *buffer;
  int i;

  /* Handle a single idle notify event.  Find the target corresponding
     to idle->window.  */
  target = XLLookUpAssoc (xid_table, idle->window);

  if (target)
    {
      /* See if the idle record corresponds to any of target's back
	 buffers, and set the BufferSync flag if that is the case.  */
      for (i = 0; i < ArrayElements (target->back_buffers); ++i)
	{
	  if (target->back_buffers[i]
	      && (target->back_buffers[i]->present_serial
		  == idle->serial_number))
	    {
	      /* Now say that the target idle fence must be waited
		 for.  */
	      target->back_buffers[i]->present_serial = 0;
	      target->back_buffers[i]->picture |= BufferSync;
	      target->back_buffers[i]->pixmap |= BufferSync;
	      ClearBufferBusy (target->back_buffers[i]);

	      return True;
	    }
	}

      /* Now, look for a corresponding presentation record.  */
      record = target->pending.target_next;

      while (record != &target->pending)
	{
	  if (record->buffer->pixmap == idle->pixmap)
	    {
	      /* The buffer was found.  Remove the presentation record
		 if the serial matches, and return.  */
	      if (record->serial == idle->serial_number)
		{
		  /* Save away buffer.  */
		  buffer = record->buffer;

		  /* Remove the presentation record.  */
		  RemovePresentRecord (record);

		  /* Run idle callbacks if this is now idle.  */
		  MaybeRunIdleCallbacks (buffer, target);
		}

	      return True;
	    }

	  record = record->target_next;
	}
    }

  return True;
}

static Bool
HandlePresentationEvent (XGenericEventCookie *event)
{
  switch (event->evtype)
    {
    case PresentIdleNotify:
      /* Find which pixmap became idle and note that it is now
	 idle.  */
      return HandlePresentIdleNotify (event->data);

    case PresentCompleteNotify:
      /* Find which presentation completed and call the callback.  */
      return HandlePresentCompleteNotify (event->data);
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

  if (event->type == ClientMessage
      && event->xclient.message_type == _XL_BUFFER_RELEASE)
    {
      /* Values are masked against 0xffffffff, as Xlib sign-extends
	 those longs.  */
      high = event->xclient.data.l[0] & 0xffffffff;
      low = event->xclient.data.l[1] & 0xffffffff;
      id = low | (high << 32);

      /* Handle the activity change.  */
      HandleActivityEvent (id);
      return True;
    }

  if (event->type == GenericEvent
      /* If present_opcode was not initialized, then it is 0, which is
	 not a valid extension opcode.  */
      && event->xgeneric.extension == present_opcode)
    return HandlePresentationEvent (&event->xcookie);

  return False;
}

void
InitPictureRenderer (void)
{
  identity_transform.matrix[0][0] = 1;
  identity_transform.matrix[1][1] = 1;
  identity_transform.matrix[2][2] = 1;

  pending_success.next = &pending_success;
  pending_success.last = &pending_success;
  all_activity.global_next = &all_activity;
  all_activity.global_last = &all_activity;
  all_completion_callbacks.next = &all_completion_callbacks;
  all_completion_callbacks.last = &all_completion_callbacks;

  /* Initialize the renderer with our functions.  */
  RegisterStaticRenderer ("picture", &picture_render_funcs,
			  &picture_buffer_funcs);
}
