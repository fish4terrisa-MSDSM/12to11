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

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/poll.h>

#include <limits.h>

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>

#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/sync.h>

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/shm.h>

#include <wayland-server.h>

#include <pixman.h>

#include "transfer_atoms.h"

typedef struct _Compositor Compositor;

struct _Compositor
{
  /* The X display for this "compositor" instance.  */
  Display *display;

  /* The XCB connection.  */
  xcb_connection_t *conn;

  /* The Wayland display used to communicate with clients.  */
  struct wl_display *wl_display;

  /* Its event loop object.  */
  struct wl_event_loop *wl_event_loop;

  /* The name of the socket used to communicate with clients.  */
  const char *wl_socket;

  /* XRandr event and error base, and versions.  */
  int rr_event_base, rr_error_base, rr_major, rr_minor;

  /* The visual used for all windows.  */
  Visual *visual;

  /* The colormap.  */
  Colormap colormap;

  /* The picture format used for ARGB formats.  */
  XRenderPictFormat *argb_format;

  /* The picture format used for XRGB formats.  */
  XRenderPictFormat *xrgb_format;

  /* The depth of that visual.  */
  int n_planes;

  /* Whether the server time is monotonic.  */
  Bool server_time_monotonic;

  /* The resource and app names.  */
  const char *resource_name, *app_name;
};

/* Forward declarations from seat.c.  */

typedef struct _Seat Seat;
typedef struct _Pointer Pointer;
typedef struct _RelativePointer RelativePointer;
typedef struct _SwipeGesture SwipeGesture;
typedef struct _PinchGesture PinchGesture;

/* Forward declarations from primary_selection.c.  */

typedef struct _PDataSource PDataSource;

/* Forward declarations from wp_viewporter.c.  */

typedef struct _ViewportExt ViewportExt;

/* Defined in 12to11.c.  */

extern Compositor compositor;

/* Defined in time.c.  */

typedef struct _Timestamp Timestamp;
typedef enum _TimestampDifference TimestampDifference;

struct _Timestamp
{
  /* Number of server months passed.  */
  unsigned int months;

  /* Millisecond time into those months.  */
  unsigned int milliseconds;
};

enum _TimestampDifference
  {
    Earlier,
    Same,
    Later,
  };

extern Timestamp TimestampFromServerTime (Time);
extern Timestamp TimestampFromClientTime (Time);
extern TimestampDifference CompareTimestamps (Timestamp, Timestamp);
extern TimestampDifference CompareTimeWith (Time, Timestamp);

#define TimestampIs(a, op, b)	(CompareTimestamps ((a), (b)) == (op))
#define TimeIs(a, op, b)	(CompareTimeWith ((a), (b)) == (op))

extern Bool HandleOneXEventForTime (XEvent *);
extern void InitTime (void);

/* Defined in renderer.c.  */

typedef struct _RenderFuncs RenderFuncs;
typedef struct _BufferFuncs BufferFuncs;

typedef union _RenderTarget RenderTarget;
typedef union _RenderBuffer RenderBuffer;
typedef union _RenderFence RenderFence;

typedef struct _DrawParams DrawParams;

typedef struct _DmaBufAttributes DmaBufAttributes;
typedef struct _SharedMemoryAttributes SharedMemoryAttributes;

typedef struct _DrmFormat DrmFormat;
typedef struct _ShmFormat ShmFormat;

typedef enum _Operation Operation;
typedef enum _BufferTransform BufferTransform;
typedef enum _RenderMode RenderMode;

typedef void *IdleCallbackKey;
typedef void *PresentCompletionKey;
typedef void *RenderCompletionKey;

typedef void (*DmaBufSuccessFunc) (RenderBuffer, void *);
typedef void (*DmaBufFailureFunc) (void *);

typedef void (*BufferIdleFunc) (RenderBuffer, void *);
typedef void (*PresentCompletionFunc) (void *, uint64_t, uint64_t);
typedef void (*RenderCompletionFunc) (void *, uint64_t, uint64_t);

enum _BufferTransform
  {
    Normal,
    CounterClockwise90,
    CounterClockwise180,
    CounterClockwise270,
    Flipped,
    Flipped90,
    Flipped180,
    Flipped270,
  };

enum _Operation
  {
    OperationOver,
    OperationSource,
  };

enum
  {
    /* Scale has been set.  */
    ScaleSet	 = 1,
    /* Transform has been set.  */
    TransformSet = (1 << 1),
    /* Offset has been set.  */
    OffsetSet	 = (1 << 2),
    /* Stretch has been set.  */
    StretchSet	 = (1 << 3),
  };

/* The transforms specified in the draw parameters are applied in the
   following order: transform, scale, off_x, off_y, crop_width,
   crop_height, stretch_width, stretch_height.  */

struct _DrawParams
{
  /* Which fields are set.  */
  int flags;

  /* A transform by which to rotate the buffer.  */
  BufferTransform transform;

  /* A scale factor to apply to the buffer.  */
  double scale;

  /* Offset from which to start sampling from the buffer, after
     scaling.  */
  double off_x, off_y;

  /* The crop, width and height of the buffer.  The buffer will be
     stretched or shrunk to this size, after being cropped to
     crop and being offset by -off_x, -off_y.  */
  double crop_width, crop_height, stretch_width, stretch_height;
};

struct _SharedMemoryAttributes
{
  /* The format of the buffer.  */
  uint32_t format;

  /* The offset, width, height, and stride of the buffer.  */
  int32_t offset, width, height, stride;

  /* The pool file descriptor.  */
  int fd;

  /* Pointer to a pointer to the pool data.  */
  void **data;

  /* Size of the pool.  */
  size_t pool_size;
};

struct _DmaBufAttributes
{
  /* The file descriptors.  They should be closed by the time the
     callback returns.  */
  int fds[4];

  /* The modifier.  */
  uint64_t modifier;

  /* Strides.  */
  unsigned int strides[4];

  /* Offsets.  */
  unsigned int offsets[4];

  /* The number of planes set.  */
  int n_planes;

  /* The width and height of the buffer.  */
  int width, height;

  /* Flags.  */
  int flags;

  /* The DRM format of the buffer.  */
  uint32_t drm_format;
};

union _RenderTarget
{
  /* The XID of the target resource, if that is what it is.  */
  XID xid;

  /* The pointer to the target, if that is what it is.  */
  void *pointer;
};

union _RenderBuffer
{
  /* The XID of the buffer resource, if that is what it is.  */
  XID xid;

  /* The pointer to the buffer, if that is what it is.  */
  void *pointer;
};

union _RenderFence
{
  /* The XID of the fence resource, if that is what it is.  */
  XID xid;

  /* The pointer to the fence, if that is what it is.  */
  void *pointer;
};

enum
  {
    /* The render target always preserves previously drawn contents;
       IOW, target_age always returns 0.  */
    NeverAges		  = 1,
    /* Buffers attached can always be immediately released.  */
    ImmediateRelease	  = 1 << 2,
    /* The render target supports explicit synchronization.  */
    SupportsExplicitSync  = 1 << 3,
    /* The render target supports direct presentation, so it is okay
       for surfaces to be unredirected by the compositing manager.  */
    SupportsDirectPresent = 1 << 4,
  };

enum _RenderMode
  {
    /* Do not synchronize rendering.  */
    RenderModeAsync,
    /* Synchronize rendering with the vertical refresh.  */
    RenderModeVsync,
  };

struct _RenderFuncs
{
  /* Initialize the visual and depth.  */
  Bool (*init_render_funcs) (void);

  /* Create a rendering target for the given window.  */
  RenderTarget (*target_from_window) (Window, unsigned long);

  /* Create a rendering target for the given pixmap.  */
  RenderTarget (*target_from_pixmap) (Pixmap);

  /* Set the rendering mode for the render target.  Return whether or
     not the mode was successfully set.  The default rendering mode is
     RenderModeAsync.  The last argument is the number of the next
     frame as known to the caller.  */
  Bool (*set_render_mode) (RenderTarget, RenderMode, uint64_t);

  /* Set the client associated with the target.  This allows some
     extra pixmap memory allocation tracking to be done.  */
  void (*set_client) (RenderTarget, struct wl_client *);

  /* Set the standard event mask of the target.  */
  void (*set_standard_event_mask) (RenderTarget, unsigned long);

  /* Set the target width and height.  This can be NULL.  */
  void (*note_target_size) (RenderTarget, int, int);

  /* Get an XRender Picture from the given rendering target.  The
     picture should only be used to create a cursor, and must be
     destroyed by calling FreePictureFromTarget immediately
     afterwards.  */
  Picture (*picture_from_target) (RenderTarget);

  /* Free a picture created that way.  */
  void (*free_picture_from_target) (Picture);

  /* Destroy the given rendering target.  */
  void (*destroy_render_target) (RenderTarget);

  /* Begin rendering.  This can be NULL, but if not, must be called
     before any drawing operations.  */
  void (*start_render) (RenderTarget);

  /* Fill the target with transparency in the given rectangles.  */
  void (*fill_boxes_with_transparency) (RenderTarget, pixman_box32_t *, int,
					int, int);

  /* Clear the given rectangle.  */
  void (*clear_rectangle) (RenderTarget, int, int, int, int);

  /* Composite width, height, from the given buffer onto the given
     target, at x, y.  The arguments are: buffer, target, operation,
     source_x, source_y, x, y, width, height, params.  params
     describes how to transform the given buffer.

     Reads outside the texture contents should result in transparency
     being composited.  */
  void (*composite) (RenderBuffer, RenderTarget, Operation, int, int,
		     int, int, int, int, DrawParams *);

  /* Finish rendering, and swap changes in given damage to display.
     May be NULL.  If a callback is passed and a non-NULL key is
     returned, then the rendering will not actually have finished
     until the callback is run.  */
  RenderCompletionKey (*finish_render) (RenderTarget, pixman_region32_t *,
					RenderCompletionFunc, void *);

  /* Cancel the callback returned by finish_render.  */
  void (*cancel_completion_callback) (RenderCompletionKey);

  /* Return the age of the target.  Value is a number not less than
     -2, describing the "age" of the contents of the target.
     
     -1 means the buffer contains no valid contents, and must be
     redrawn from scratch.  0 means the buffer contains the contents
     at the time of the last call to `finish_render', 1 means the
     buffer contains the contents at the time of the second last call
     to `finish_render', and so on.

     -2 means the buffer contents are of the last call to
      FinishRender, but are invalid for any rendering call other than
      present_to_window.

     Note that when a render target is first created, the renderer may
     chose to return 0 instead of 1.  */
  int (*target_age) (RenderTarget);

  /* Create a rendering "fence" object, that serves to explicitly
     specify the order of rendering requests between different
     programs.  Fence-related functions must only be defined if
     SupportsExplicitSync is set.  */
  RenderFence (*import_fd_fence) (int, Bool *);

  /* Wait for the given fence object.  This is guaranteed to make all
     drawing commands execute after the fence has been triggered, but
     does not typically block.  */
  void (*wait_fence) (RenderFence);

  /* Delete the given fence object.  */
  void (*delete_fence) (RenderFence);

  /* Get a file descriptor describing a fence that is triggered upon
     the completion of all drawing commands made before it is
     called.  */
  int (*get_finish_fence) (Bool *);

  /* Directly present the given buffer to the window, without copying,
     and possibly flip the buffer contents to the screen, all while
     possibly being synchronized with the vertical refresh.  Call the
     supplied callback when the presentation completes.  May be
     NULL.  */
  PresentCompletionKey (*present_to_window) (RenderTarget, RenderBuffer,
					     pixman_region32_t *,
					     PresentCompletionFunc, void *);

  /* Call the specified render callback once it is time to display the
     next frame.  May be NULL.  */
  RenderCompletionKey (*notify_msc) (RenderTarget, RenderCompletionFunc,
				     void *);

  /* Cancel the given presentation callback.  */
  void (*cancel_presentation_callback) (PresentCompletionKey);

  /* Some flags.  NeverAges means targets always preserve contents
     that were previously drawn.  */
  int flags;
};

struct _DrmFormat
{
  /* The supported DRM format.  */
  uint32_t drm_format;

  /* The supported modifier.  */
  uint64_t drm_modifier;

  /* Backend specific flags associated with this DRM format.  */
  int flags;
};

struct _ShmFormat
{
  /* The Wayland type code of the format.  */
  uint32_t format;
};

struct _BufferFuncs
{
  /* Get DRM formats and modifiers supported by this renderer.  Return
     0 formats if nothing is supported.  */
  DrmFormat *(*get_drm_formats) (int *);

  /* Get a list of DRM device nodes for each provider.  Return NULL if
     the provider list could not be initialized.  */
  dev_t *(*get_render_devices) (int *);

  /* Get SHM formats supported by this renderer.  */
  ShmFormat *(*get_shm_formats) (int *);

  /* Create a buffer from the given dma-buf attributes.  */
  RenderBuffer (*buffer_from_dma_buf) (DmaBufAttributes *, Bool *);

  /* Create a buffer from the given dma-buf attributes
     asynchronously.  */
  void (*buffer_from_dma_buf_async) (DmaBufAttributes *, DmaBufSuccessFunc,
				     DmaBufFailureFunc, void *);

  /* Create a buffer from the given shared memory attributes.  */
  RenderBuffer (*buffer_from_shm) (SharedMemoryAttributes *, Bool *);

  /* Validate the shared memory attributes passed as args.  Return
     false if they are not valid.  */
  Bool (*validate_shm_params) (uint32_t, uint32_t, uint32_t, int32_t,
			       int32_t, size_t);

  /* Create a buffer from the given RGBA color.  */
  RenderBuffer (*buffer_from_single_pixel) (uint32_t, uint32_t, uint32_t, uint32_t,
					    Bool *);

  /* Free a buffer created from shared memory.  */
  void (*free_shm_buffer) (RenderBuffer);

  /* Free a dma-buf buffer.  */
  void (*free_dmabuf_buffer) (RenderBuffer);

  /* Free a single-pixel buffer.  */
  void (*free_single_pixel_buffer) (RenderBuffer);

  /* Notice that the given buffer has been damaged.  May be NULL.  If
     the given NULL damage, assume that the entire buffer has been
     damaged.  Must be called at least once before any rendering can
     be performed on the buffer.  3rd arg is a DrawParams struct
     describing a transform to inversely apply to the damage
     region.  */
  void (*update_buffer_for_damage) (RenderBuffer, pixman_region32_t *,
				    DrawParams *);

  /* Return whether or not the buffer contents can be released early,
     by being copied to an offscreen buffer.  */
  Bool (*can_release_now) (RenderBuffer);

  /* Run a callback once the buffer contents become idle on the given
     target.  NULL if flags contains ImmediateRelease.  The callback
     is also run when the buffer is destroyed, but not when the target
     is destroyed; in that case, the callback key simply becomes
     invalid.  */
  IdleCallbackKey (*add_idle_callback) (RenderBuffer, RenderTarget,
					BufferIdleFunc, void *);

  /* Cancel the given idle callback.  */
  void (*cancel_idle_callback) (IdleCallbackKey);

  /* Return whether or not the buffer is idle.  NULL if flags contains
     ImmediateRelease.  */
  Bool (*is_buffer_idle) (RenderBuffer, RenderTarget);

  /* Wait for a buffer to become idle on the given target.  May be
     NULL.  */
  void (*wait_for_idle) (RenderBuffer, RenderTarget);

  /* Ensure wait_for_idle can be called.  May be NULL.  */
  void (*set_need_wait_for_idle) (RenderTarget);

  /* Return whether or not the given buffer can be treated as
     completely opaque.  */
  Bool (*is_buffer_opaque) (RenderBuffer);

  /* Called during renderer initialization.  */
  void (*init_buffer_funcs) (void);
};

extern int renderer_flags;

extern void RegisterStaticRenderer (const char *, RenderFuncs *,
				    BufferFuncs *);
extern void InitRenderers (void);

extern RenderTarget RenderTargetFromWindow (Window, unsigned long);
extern RenderTarget RenderTargetFromPixmap (Pixmap);
extern Bool RenderSetRenderMode (RenderTarget, RenderMode, uint64_t);
extern void RenderSetClient (RenderTarget, struct wl_client *);
extern void RenderSetStandardEventMask (RenderTarget, unsigned long);
extern void RenderNoteTargetSize (RenderTarget, int, int);
extern Picture RenderPictureFromTarget (RenderTarget);
extern void RenderFreePictureFromTarget (Picture);
extern void RenderDestroyRenderTarget (RenderTarget);
extern void RenderStartRender (RenderTarget);
extern void RenderFillBoxesWithTransparency (RenderTarget, pixman_box32_t *,
					     int, int, int);
extern void RenderClearRectangle (RenderTarget, int, int, int, int);
extern void RenderComposite (RenderBuffer, RenderTarget, Operation, int,
			     int, int, int, int, int, DrawParams *);
extern RenderCompletionKey RenderFinishRender (RenderTarget,
					       pixman_region32_t *,
					       RenderCompletionFunc,
					       void *);
extern void RenderCancelCompletionCallback (RenderCompletionKey);
extern int RenderTargetAge (RenderTarget);
extern RenderFence RenderImportFdFence (int, Bool *);
extern void RenderWaitFence (RenderFence);
extern void RenderDeleteFence (RenderFence);
extern int RenderGetFinishFence (Bool *);
extern PresentCompletionKey RenderPresentToWindow (RenderTarget, RenderBuffer,
						   pixman_region32_t *,
						   PresentCompletionFunc,
						   void *);
extern RenderCompletionKey RenderNotifyMsc (RenderTarget, RenderCompletionFunc,
					    void *);
extern void RenderCancelPresentationCallback (PresentCompletionKey);

extern DrmFormat *RenderGetDrmFormats (int *);
extern dev_t *RenderGetRenderDevices (int *);
extern ShmFormat *RenderGetShmFormats (int *);
extern RenderBuffer RenderBufferFromDmaBuf (DmaBufAttributes *, Bool *);
extern void RenderBufferFromDmaBufAsync (DmaBufAttributes *, DmaBufSuccessFunc,
					 DmaBufFailureFunc, void *);
extern RenderBuffer RenderBufferFromShm (SharedMemoryAttributes *, Bool *);
extern Bool RenderValidateShmParams (uint32_t, uint32_t, uint32_t, int32_t,
				     int32_t, size_t);
extern RenderBuffer RenderBufferFromSinglePixel (uint32_t, uint32_t, uint32_t,
						 uint32_t, Bool *);
extern void RenderFreeShmBuffer (RenderBuffer);
extern void RenderFreeDmabufBuffer (RenderBuffer);
extern void RenderFreeSinglePixelBuffer (RenderBuffer);
extern void RenderUpdateBufferForDamage (RenderBuffer, pixman_region32_t *,
					 DrawParams *);
extern Bool RenderCanReleaseNow (RenderBuffer);
extern IdleCallbackKey RenderAddIdleCallback (RenderBuffer, RenderTarget,
					      BufferIdleFunc, void *);
extern void RenderCancelIdleCallback (IdleCallbackKey);
extern Bool RenderIsBufferIdle (RenderBuffer, RenderTarget);
extern void RenderWaitForIdle (RenderBuffer, RenderTarget);
extern void RenderSetNeedWaitForIdle (RenderTarget);
extern Bool RenderIsBufferOpaque (RenderBuffer);

/* Defined in run.c.  */

typedef struct _PollFd WriteFd;
typedef struct _PollFd ReadFd;

extern void XLRunCompositor (void);
extern WriteFd *XLAddWriteFd (int, void *, void (*) (int, void *, ReadFd *));
extern ReadFd *XLAddReadFd (int, void *, void (*) (int, void *, ReadFd *));
extern void XLRemoveWriteFd (WriteFd *);
extern void XLRemoveReadFd (ReadFd *);

/* Defined in alloc.c.  */

extern void *XLMalloc (size_t);
extern void *XLRealloc (void *, size_t);
extern void *XLSafeMalloc (size_t);
extern void *XLCalloc (size_t, size_t);
extern char *XLStrdup (const char *);
extern void XLFree (void *);

/* Defined in fns.c.  */

/* Generic singly linked list structure.  */

typedef struct _XLList XLList;
typedef struct _XIDList XIDList;
typedef struct _XLAssoc XLAssoc;
typedef struct _XLAssocTable XLAssocTable;
typedef struct _RootWindowSelection RootWindowSelection;

struct _XLList
{
  XLList *next;
  void *data;
};

/* The same, but for X server resources.  */

struct _XIDList
{
  XIDList *next;
  XID data;
};

extern void XLListFree (XLList *, void (*) (void *));
extern XLList *XLListRemove (XLList *, void *);
extern XLList *XLListPrepend (XLList *, void *);

extern void XIDListFree (XIDList *, void (*) (XID));
extern XIDList *XIDListRemove (XIDList *, XID);
extern XIDList *XIDListPrepend (XIDList *, XID);

struct _XLAssoc
{
  /* Next object in this bucket.  */
  XLAssoc *next;

  /* Last object in this bucket.  */
  XLAssoc *prev;

  /* XID of the object.  */
  XID x_id;

  /* Untyped data.  */
  void *data;
};

/* Map between XID and untyped data.  Implemented the same way as the
   old XAssocTable.  */

struct _XLAssocTable
{
  /* Pointer to first pucket in bucket array.  */
  XLAssoc *buckets;

  /* Table size (number of buckets).  */
  int size;
};

extern XLAssocTable *XLCreateAssocTable (int);
extern void XLMakeAssoc (XLAssocTable *, XID, void *);
extern void *XLLookUpAssoc (XLAssocTable *, XID);
extern void XLDeleteAssoc (XLAssocTable *, XID);
extern void XLDestroyAssocTable (XLAssocTable *);

extern int XLOpenShm (void);

extern void XLScaleRegion (pixman_region32_t *, pixman_region32_t *,
			   float, float);
extern void XLExtendRegion (pixman_region32_t *, pixman_region32_t *,
			    int, int);
extern void XLTransformRegion (pixman_region32_t *, pixman_region32_t *,
			       BufferTransform, int, int);
extern Time XLGetServerTimeRoundtrip (void);

extern RootWindowSelection *XLSelectInputFromRootWindow (unsigned long);
extern void XLDeselectInputFromRootWindow (RootWindowSelection *);

extern void XLRecordBusfault (void *, size_t);
extern void XLRemoveBusfault (void *);
extern Bool XLAddFdFlag (int, int, Bool);

/* Defined in compositor.c.  */

extern void XLInitCompositor (void);

/* Defined in region.c.  */

extern void XLCreateRegion (struct wl_client *,
			    struct wl_resource *,
			    uint32_t);

/* Defined in buffer.c.  */

typedef struct _ExtBuffer ExtBuffer;
typedef struct _ExtBufferFuncs ExtBufferFuncs;
typedef void (*ExtBufferFunc) (ExtBuffer *, void *);

struct _ExtBufferFuncs
{
  void (*retain) (ExtBuffer *);
  void (*dereference) (ExtBuffer *);
  RenderBuffer (*get_buffer) (ExtBuffer *);
  unsigned int (*width) (ExtBuffer *);
  unsigned int (*height) (ExtBuffer *);
  void (*release) (ExtBuffer *);
  void (*print_buffer) (ExtBuffer *);
};

struct _ExtBuffer
{
  /* Functions for this buffer.  */
  ExtBufferFuncs funcs;

  /* Label used for debugging.  */
  char *label;

  /* List of destroy listeners.  */
  XLList *destroy_listeners;
};

extern void XLRetainBuffer (ExtBuffer *);
extern void XLDereferenceBuffer (ExtBuffer *);
extern RenderBuffer XLRenderBufferFromBuffer (ExtBuffer *);
extern unsigned int XLBufferWidth (ExtBuffer *);
extern unsigned int XLBufferHeight (ExtBuffer *);
extern void XLReleaseBuffer (ExtBuffer *);
extern void *XLBufferRunOnFree (ExtBuffer *, ExtBufferFunc,
				void *);
extern void XLBufferCancelRunOnFree (ExtBuffer *, void *);
extern void XLPrintBuffer (ExtBuffer *);

extern void ExtBufferDestroy (ExtBuffer *);

/* Defined in shm.c.  */

extern int render_first_error;

extern void XLInitShm (void);

/* Defined in subcompositor.c.  */

typedef struct _View View;
typedef struct _List List;
typedef struct _Subcompositor Subcompositor;
typedef struct _SubcompositorDestroyCallback SubcompositorDestroyCallback;

typedef enum _FrameMode FrameMode;

enum _FrameMode
  {
    ModeStarted,
    ModeComplete,
    ModePresented,
  };

extern void SubcompositorInit (void);

extern Subcompositor *MakeSubcompositor (void);
extern View *MakeView (void);

extern void SubcompositorSetTarget (Subcompositor *, RenderTarget *);
extern void SubcompositorInsert (Subcompositor *, View *);
extern void SubcompositorInsertBefore (Subcompositor *, View *, View *);
extern void SubcompositorInsertAfter (Subcompositor *, View *, View *);

extern void SubcompositorSetOpaqueCallback (Subcompositor *,
					    void (*) (Subcompositor *,
						      void *,
						      pixman_region32_t *),
					    void *);
extern void SubcompositorSetInputCallback (Subcompositor *,
					   void (*) (Subcompositor *,
						     void *,
						     pixman_region32_t *),
					   void *);
extern void SubcompositorSetBoundsCallback (Subcompositor *,
					    void (*) (void *, int, int,
						      int, int),
					    void *);
extern void SubcompositorSetNoteFrameCallback (Subcompositor *,
					       void (*) (FrameMode, uint64_t,
							 void *, uint64_t,
							 uint64_t),
					       void *);
extern void SubcompositorBounds (Subcompositor *, int *, int *, int *, int *);
extern void SubcompositorSetProjectiveTransform (Subcompositor *, int, int);

extern void SubcompositorUpdate (Subcompositor *);
extern void SubcompositorExpose (Subcompositor *, XEvent *);
extern void SubcompositorGarbage (Subcompositor *);
extern View *SubcompositorLookupView (Subcompositor *, int, int, int *, int *);
extern Bool SubcompositorIsEmpty (Subcompositor *);

extern int SubcompositorWidth (Subcompositor *);
extern int SubcompositorHeight (Subcompositor *);

extern void SubcompositorFree (Subcompositor *);

extern void SubcompositorFreeze (Subcompositor *);
extern void SubcompositorUnfreeze (Subcompositor *);
extern SubcompositorDestroyCallback *SubcompositorOnDestroy (Subcompositor *,
							     void (*) (void *),
							     void *);
extern void SubcompositorRemoveDestroyCallback (SubcompositorDestroyCallback *);
extern void SubcompositorSetAlwaysGarbaged (Subcompositor *);

extern void ViewSetSubcompositor (View *, Subcompositor *);

extern void ViewInsert (View *, View *);
extern void ViewInsertAfter (View *, View *, View *);
extern void ViewInsertBefore (View *, View *, View *);
extern void ViewInsertStart (View *, View *);

extern Bool ViewIsVisible (View *);

extern void ViewUnparent (View *);
extern View *ViewGetParent (View *);
extern void ViewAttachBuffer (View *, ExtBuffer *);
extern void ViewMove (View *, int, int);
extern void ViewDetach (View *);
extern void ViewMap (View *);
extern void ViewUnmap (View *);
extern void ViewMoveFractional (View *, double, double);

extern void ViewSetTransform (View *, BufferTransform);
extern void ViewSetViewport (View *, double, double, double, double,
			     double, double);
extern void ViewClearViewport (View *);

extern void ViewSetData (View *, void *);
extern void *ViewGetData (View *);

extern void ViewSetMaybeResizedFunction (View *, void (*) (View *));

extern void ViewTranslate (View *, int, int, int *, int *);

extern void ViewFree (View *);

extern void ViewDamage (View *, pixman_region32_t *);
extern void ViewDamageBuffer (View *, pixman_region32_t *);
extern void ViewSetOpaque (View *, pixman_region32_t *);
extern void ViewSetInput (View *, pixman_region32_t *);
extern double ViewGetContentScale (View *);
extern int ViewWidth (View *);
extern int ViewHeight (View *);
extern void ViewSetScale (View *, int);

extern Subcompositor *ViewGetSubcompositor (View *);

/* Forward declarations from explicit_synchronization.c.  */

typedef struct _Synchronization Synchronization;
typedef struct _SyncRelease SyncRelease;

/* Defined in surface.c.  */

typedef struct _State State;
typedef struct _FrameCallback FrameCallback;
typedef enum _RoleType RoleType;
typedef enum _FocusMode FocusMode;
typedef enum _PresentationHint PresentationHint;

enum _FocusMode
  {
    SurfaceFocusIn,
    SurfaceFocusOut,
  };

enum _RoleType
  {
    AnythingType,
    SubsurfaceType,
    XdgType,
    CursorType,
    DndIconType,
    TestSurfaceType,
  };

#define RotatesDimensions(transform)		\
  ((transform) == CounterClockwise90		\
   || (transform) == CounterClockwise270	\
   || (transform) == Flipped90			\
   || (transform) == Flipped270)

enum
  {
    PendingNone		    = 0,
    PendingOpaqueRegion	    = 1,
    PendingInputRegion	    = (1 << 2),
    PendingDamage	    = (1 << 3),
    PendingSurfaceDamage    = (1 << 4),
    PendingBuffer	    = (1 << 5),
    PendingFrameCallbacks   = (1 << 6),
    PendingBufferScale	    = (1 << 7),
    PendingAttachments	    = (1 << 8),
    PendingViewportSrc	    = (1 << 9),
    PendingViewportDest	    = (1 << 10),
    PendingBufferTransform  = (1 << 11),
    PendingPresentationHint = (1 << 12),

    /* Flags here are stored in `pending' of the current state for
       space reasons.  */
    BufferAlreadyReleased = (1 << 19),
  };

enum _PresentationHint
  {
    PresentationHintVsync,
    PresentationHintAsync,
  };

struct _FrameCallback
{
  /* The next and last callbacks.  */
  FrameCallback *next, *last;

  /* The wl_resource containing this callback.  */
  struct wl_resource *resource;
};

struct _State
{
  /* Accumulated damage.  */
  pixman_region32_t damage;

  /* Opaque region.  */
  pixman_region32_t opaque;

  /* Input region.  */
  pixman_region32_t input;

  /* Surface damage.  */
  pixman_region32_t surface;

  /* The currently attached buffer.  */
  ExtBuffer *buffer;

  /* What part of this state has been changed, and some more
     flags.  */
  int pending;

  /* The scale of this buffer.  */
  int buffer_scale;

  /* The buffer transform.  */
  BufferTransform transform;

  /* List of frame callbacks.  */
  FrameCallback frame_callbacks;

  /* Attachment position.  */
  int x, y;

  /* Viewport destination width and height.  All viewport coordinates
     and dimensions are specified in terms of the surface coordinate
     system.  */
  int dest_width, dest_height;

  /* Viewport source rectangle.  */
  double src_x, src_y, src_width, src_height;

  /* The presentation hint.  Defaults to PresentationHintVsync.  */
  PresentationHint presentation_hint;
};

typedef enum _ClientDataType ClientDataType;
typedef struct _Surface Surface;
typedef struct _Role Role;
typedef struct _RoleFuncs RoleFuncs;
typedef struct _CommitCallback CommitCallback;
typedef struct _UnmapCallback UnmapCallback;
typedef struct _DestroyCallback DestroyCallback;
typedef struct _ClientData ClientData;

enum _ClientDataType
  {
    SubsurfaceData,
    PointerConfinementData,
    ShortcutInhibitData,
    IdleInhibitData,
    MaxClientData,
    XdgActivationData,
    TearingControlData,
  };

struct _DestroyCallback
{
  /* The next and last destroy callbacks in this list.  */
  DestroyCallback *next, *last;

  /* Function called when the surface is destroyed.  */
  void (*destroy_func) (void *data);

  /* Data for the surface.  */
  void *data;
};

struct _UnmapCallback
{
  /* The next and last callbacks in this list.  */
  UnmapCallback *next, *last;

  /* Function called when the surface is unmapped.  */
  void (*unmap) (void *data);

  /* Data for the surface.  */
  void *data;
};

struct _CommitCallback
{
  /* Function called when the surface is committed, but before any
     role commit function.  */
  void (*commit) (Surface *, void *);

  /* Data that callback is called with.  */
  void *data;

  /* The next and last commit callbacks in this list.  */
  CommitCallback *next, *last;
};

struct _ClientData
{
  /* The next piece of client data attached to this surface.  */
  ClientData *next;

  /* The client data itself.  */
  void *data;

  /* The free function.  */
  void (*free_function) (void *);

  /* The type of the client data.  */
  ClientDataType type;
};

struct _Surface
{
  /* The view associated with this surface.  */
  View *view;

  /* The view used to store subsurfaces below this surface.
     No buffer is ever attached; this is purely a container.  */
  View *under;

  /* The resource corresponding to this surface.  */
  struct wl_resource *resource;

  /* The role corresponding to this surface.  */
  Role *role;

  /* The kind of role allowed for this surface.  */
  RoleType role_type;

  /* The state that's pending a commit.  */
  State pending_state;

  /* The state that's currently in use.  */
  State current_state;

  /* Any state cached for application after early_commit returns
     False.  */
  State cached_state;

  /* List of subsurfaces.  */
  XLList *subsurfaces;

  /* List of client data.  */
  ClientData *client_data;

  /* List of commit callbacks.  */
  CommitCallback commit_callbacks;

  /* List of destroy callbacks.  */
  DestroyCallback destroy_callbacks;

  /* List of unmap callbacks.  */
  UnmapCallback unmap_callbacks;

  /* The outputs this surface is known to be on.  */
  RROutput *outputs;

  /* The number of outputs this surface is known to be on.  */
  int n_outputs;

  /* The number of seats that have this surface focused.  */
  int num_focused_seats;

  /* Bounds inside which the surface output need not be
     recomputed.  */
  pixman_region32_t output_region;

  /* The next and last surfaces in this list.  */
  Surface *next, *last;

  /* The key for the window scalling factor callback.  */
  void *scale_callback_key;

  /* X, Y of the last coordinates that were used to update this
     surface's entered outputs.  */
  int output_x, output_y;

  /* The associated explicit synchronization resource, if any.  */
  Synchronization *synchronization;

  /* The associated sync release resource, if any.  */
  SyncRelease *release;

  /* The associated sync acquire fd, or -1.  */
  int acquire_fence;

  /* The scale factor used to convert from surface coordinates to
     window coordinates.  */
  double factor;

  /* Any associated viewport resource.  */
  ViewportExt *viewport;

  /* Any associated input delta.  This is used to compensate
     for fractional subsurface placement while handling input.  */
  double input_delta_x, input_delta_y;
};

struct _RoleFuncs
{
  /* These are mandatory.  */
  Bool (*setup) (Surface *, Role *);
  void (*teardown) (Surface *, Role *);
  void (*commit) (Surface *, Role *);
  void (*release_buffer) (Surface *, Role *, ExtBuffer *);

  /* These are optional.  */
  Bool (*early_commit) (Surface *, Role *);
  void (*subsurface_update) (Surface *, Role *);
  Window (*get_window) (Surface *, Role *);
  void (*get_resize_dimensions) (Surface *, Role *, int *, int *);
  void (*post_resize) (Surface *, Role *, int, int, int, int);
  void (*move_by) (Surface *, Role *, int, int);
  void (*rescale) (Surface *, Role *);
  void (*parent_rescale) (Surface *, Role *);
  void (*select_extra_events) (Surface *, Role *, unsigned long);
  void (*note_focus) (Surface *, Role *, FocusMode);
  void (*outputs_changed) (Surface *, Role *);
  void (*activate) (Surface *, Role *, int, Timestamp,
		    Surface *);
};

struct _Role
{
  /* Various callbacks for the role function.  */
  RoleFuncs funcs;

  /* The struct wl_resource backing this role.  */
  struct wl_resource *resource;

  /* Surface attached to this role.  */
  Surface *surface;
};

extern Surface all_surfaces;

extern void XLCreateSurface (struct wl_client *,
			     struct wl_resource *,
			     uint32_t);
extern void XLCommitSurface (Surface *, Bool);
extern void XLInitSurfaces (void);
extern Bool XLSurfaceAttachRole (Surface *, Role *);
extern void XLSurfaceReleaseRole (Surface *, Role *);

extern void XLDefaultCommit (Surface *);

extern void XLSurfaceRunFrameCallbacks (Surface *, struct timespec);
extern void XLSurfaceRunFrameCallbacksMs (Surface *, uint32_t);
extern CommitCallback *XLSurfaceRunAtCommit (Surface *,
					     void (*) (Surface *, void *),
					     void *);
extern void XLSurfaceCancelCommitCallback (CommitCallback *);
extern UnmapCallback *XLSurfaceRunAtUnmap (Surface *, void (*) (void *),
					   void *);
extern void XLSurfaceCancelUnmapCallback (UnmapCallback *);
extern DestroyCallback *XLSurfaceRunOnFree (Surface *, void (*) (void *),
					    void *);
extern void XLSurfaceCancelRunOnFree (DestroyCallback *);
extern void *XLSurfaceGetClientData (Surface *, ClientDataType,
				     size_t, void (*) (void *));
extern void *XLSurfaceFindClientData (Surface *, ClientDataType);
extern Bool XLSurfaceGetResizeDimensions (Surface *, int *, int *);
extern void XLSurfacePostResize (Surface *, int, int, int, int);
extern void XLSurfaceMoveBy (Surface *, int, int);
extern Window XLWindowFromSurface (Surface *);
extern void XLUpdateSurfaceOutputs (Surface *, int, int, int, int);
extern void XLSurfaceSelectExtraEvents (Surface *, unsigned long);
extern void XLSurfaceNoteFocus (Surface *, FocusMode);
extern void XLSurfaceMergeCachedState (Surface *);

extern void SurfaceToWindow (Surface *, double, double, double *, double *);
extern void ScaleToWindow (Surface *, double, double, double *, double *);
extern void WindowToSurface (Surface *, double, double, double *, double *);
extern void ScaleToSurface (Surface *, double, double, double *, double *);

extern void TruncateSurfaceToWindow (Surface *, int, int, int *, int *);
extern void TruncateScaleToWindow (Surface *, int, int, int *, int *);
extern void TruncateWindowToSurface (Surface *, int, int, int *, int *);
extern void TruncateScaleToSurface (Surface *, int, int, int *, int *);

/* Defined in output.c.  */

extern int global_scale_factor;

extern void XLInitRROutputs (void);
extern Bool XLHandleOneXEventForOutputs (XEvent *);
extern void XLOutputGetMinRefresh (struct timespec *);
extern Bool XLGetOutputRectAt (int, int, int *, int *, int *, int *);
extern void *XLAddScaleChangeCallback (void *, void (*) (void *, int));
extern void XLRemoveScaleChangeCallback (void *);
extern void XLClearOutputs (Surface *);
extern void XLOutputSetChangeFunction (void (*) (Time));
extern void XLGetMaxOutputBounds (int *, int *, int *, int *);
extern void XLOutputHandleScaleChange (int);

/* Defined in atoms.c.  */

extern Atom _NET_WM_OPAQUE_REGION, _XL_BUFFER_RELEASE,
  _NET_WM_SYNC_REQUEST_COUNTER, _NET_WM_FRAME_DRAWN, WM_DELETE_WINDOW,
  WM_PROTOCOLS, _NET_SUPPORTING_WM_CHECK, _NET_SUPPORTED, _NET_WM_SYNC_REQUEST,
  _MOTIF_WM_HINTS, _NET_WM_STATE_MAXIMIZED_VERT, _NET_WM_STATE_MAXIMIZED_HORZ,
  _NET_WM_STATE_FOCUSED, _NET_WM_STATE_FULLSCREEN, _NET_WM_STATE,
  _NET_WM_MOVERESIZE, _GTK_FRAME_EXTENTS, WM_TRANSIENT_FOR, _XL_DMA_BUF_CREATED,
  _GTK_SHOW_WINDOW_MENU, _NET_WM_ALLOWED_ACTIONS, _NET_WM_ACTION_FULLSCREEN,
  _NET_WM_ACTION_MAXIMIZE_HORZ, _NET_WM_ACTION_MAXIMIZE_VERT,
  _NET_WM_ACTION_MINIMIZE, INCR, CLIPBOARD, TARGETS, UTF8_STRING,
  _XL_SERVER_TIME_ATOM, MULTIPLE, TIMESTAMP, ATOM_PAIR, _NET_WM_NAME, WM_NAME,
  MANAGER, _XSETTINGS_SETTINGS, libinput_Scroll_Methods_Available, XdndAware,
  XdndSelection, XdndTypeList, XdndActionCopy, XdndActionMove, XdndActionLink,
  XdndActionAsk, XdndActionPrivate, XdndActionList, XdndActionDescription,
  XdndProxy, XdndEnter, XdndPosition, XdndStatus, XdndLeave, XdndDrop,
  XdndFinished, _NET_WM_FRAME_TIMINGS, _NET_WM_BYPASS_COMPOSITOR, WM_STATE,
  _NET_WM_WINDOW_TYPE, _NET_WM_WINDOW_TYPE_MENU, _NET_WM_WINDOW_TYPE_DND,
  CONNECTOR_ID, _NET_WM_PID, _NET_WM_PING, libinput_Scrolling_Pixel_Distance,
  _NET_ACTIVE_WINDOW;

extern XrmQuark resource_quark, app_quark, QString;

/* This is automatically generated by mime4.awk.  */
extern Atom DirectTransferAtoms;

extern Atom InternAtom (const char *);
extern void ProvideAtom (const char *, Atom);

extern void XLInitAtoms (void);

/* Defined in xdg_wm.c.  */

typedef struct _XdgWmBase XdgWmBase;
typedef struct _XdgRoleList XdgRoleList;

struct _XdgRoleList
{
  /* The next and last elements in this list.  */
  XdgRoleList *next, *last;

  /* The role.  */
  Role *role;
};

struct _XdgWmBase
{
  /* The associated struct wl_resource.  */
  struct wl_resource *resource;

  /* The latest ping sent.  */
  uint32_t last_ping;

  /* List of all surfaces attached.  */
  XdgRoleList list;
};

extern void XLInitXdgWM (void);
extern void XLXdgWmBaseSendPing (XdgWmBase *);

/* Defined in frame_clock.c.  */

typedef struct _FrameClock FrameClock;

extern FrameClock *XLMakeFrameClockForWindow (Window);
extern Bool XLFrameClockFrameInProgress (FrameClock *);
extern void XLFrameClockAfterFrame (FrameClock *, void (*) (FrameClock *,
							    void *,
							    uint64_t),
				    void *);
extern void XLFrameClockHandleFrameEvent (FrameClock *, XEvent *);
extern Bool XLFrameClockStartFrame (FrameClock *, Bool);
extern void XLFrameClockEndFrame (FrameClock *);
extern Bool XLFrameClockCanBatch (FrameClock *);
extern void XLFreeFrameClock (FrameClock *);
extern Bool XLFrameClockSyncSupported (void);
extern void XLFrameClockSetPredictRefresh (FrameClock *);
extern void XLFrameClockDisablePredictRefresh (FrameClock *);
extern void XLFrameClockSetFreezeCallback (FrameClock *, void (*) (void *,
								   Bool),
					   Bool (*) (void *), void *);
extern void XLFrameClockNoteConfigure (FrameClock *);
extern void *XLAddCursorClockCallback (void (*) (void *, struct timespec),
				       void *);
extern void XLStopCursorClockCallback (void *);
extern void XLStartCursorClock (void);
extern void XLStopCursorClock (void);
extern void XLInitFrameClock (void);

/* Defined in xdg_surface.c.  */

typedef struct _XdgRoleImplementation XdgRoleImplementation;
typedef struct _XdgRoleImplementationFuncs XdgRoleImplementationFuncs;
typedef enum _XdgRoleImplementationType XdgRoleImplementationType;

enum _XdgRoleImplementationType
  {
    TypeUnknown,
    TypeToplevel,
    TypePopup,
  };

struct _XdgRoleImplementationFuncs
{
  void (*attach) (Role *, XdgRoleImplementation *);
  void (*commit) (Role *, Surface *, XdgRoleImplementation *);
  void (*detach) (Role *, XdgRoleImplementation *);
  void (*ack_configure) (Role *, XdgRoleImplementation *, uint32_t);
  void (*note_size) (Role *, XdgRoleImplementation *, int, int);
  void (*note_window_pre_resize) (Role *, XdgRoleImplementation *, int, int);
  void (*note_window_resized) (Role *, XdgRoleImplementation *, int, int);
  void (*handle_geometry_change) (Role *, XdgRoleImplementation *);
  void (*post_resize) (Role *, XdgRoleImplementation *, int, int, int, int);
  Bool (*is_window_mapped) (Role *, XdgRoleImplementation *);
  void (*note_focus) (Role *, XdgRoleImplementation *, FocusMode);
  void (*outputs_changed) (Role *, XdgRoleImplementation *);
  void (*after_commit) (Role *, Surface *, XdgRoleImplementation *);
  void (*activate) (Role *, XdgRoleImplementation *, int, Time,
		    Surface *);
  void (*rescale) (Role *, XdgRoleImplementation *);
};

struct _XdgRoleImplementation
{
  /* Various functions implementing the logic behind this role.  */
  XdgRoleImplementationFuncs funcs;
};

extern unsigned long border_pixel;
extern int shape_base;

extern Bool XLHandleXEventForXdgSurfaces (XEvent *);

extern void XLInitXdgSurfaces (void);
extern void XLGetXdgSurface (struct wl_client *, struct wl_resource *,
			     uint32_t, struct wl_resource *);
extern void XLXdgRoleAttachImplementation (Role *, XdgRoleImplementation *);
extern void XLXdgRoleDetachImplementation (Role *, XdgRoleImplementation *);
extern void XLXdgRoleSendConfigure (Role *, uint32_t);
extern void XLXdgRoleCalcNewWindowSize (Role *, int, int, int *, int *);
extern int XLXdgRoleGetWidth (Role *);
extern int XLXdgRoleGetHeight (Role *);
extern void XLXdgRoleSetBoundsSize (Role *, int, int);
extern void XLXdgRoleGetCurrentGeometry (Role *, int *, int *, int *, int *);
extern void XLXdgRoleNoteConfigure (Role *, XEvent *);
extern void XLRetainXdgRole (Role *);
extern void XLReleaseXdgRole (Role *);
extern void XLXdgRoleCurrentRootPosition (Role *, int *, int *);
extern Bool XLXdgRoleInputRegionContains (Role *, int, int);
extern void XLXdgRoleResizeForMap (Role *);
extern void *XLXdgRoleRunOnReconstrain (Role *, void (*) (void *, XEvent *),
				       void (*) (void *), void *);
extern void XLXdgRoleCancelReconstrainCallback (void *);
extern void XLXdgRoleReconstrain (Role *, XEvent *);
extern void XLXdgRoleMoveBy (Role *, int, int);
extern void XLXdgRoleNoteRejectedConfigure (Role *);

extern Window XLWindowFromXdgRole (Role *);
extern Subcompositor *XLSubcompositorFromXdgRole (Role *);
extern XdgRoleImplementationType XLTypeOfXdgRole (Role *);
extern XdgRoleImplementation *XLImplementationOfXdgRole (Role *);
extern FrameClock *XLXdgRoleGetFrameClock (Role *);
extern XdgRoleImplementation *XLLookUpXdgToplevel (Window);
extern XdgRoleImplementation *XLLookUpXdgPopup (Window);

extern void XLXdgRoleHandlePing (Role *, XEvent *, void (*) (XEvent *));
extern void XLXdgRoleReplyPing (Role *);

/* Defined in positioner.c.  */

typedef struct _Positioner Positioner;

/* This structure is public because positioners must be copied into
   xdg_popups.  */

struct _Positioner
{
  /* The fields below mean what they do in the xdg_shell protocol
     spec.  */
  int width, height;
  int anchor_x, anchor_y;
  int anchor_width, anchor_height;
  unsigned int anchor, gravity, constraint;
  int offset_x, offset_y;
  Bool reactive;
  int parent_width, parent_height;
  uint32_t constraint_adjustment;

  /* The wl_resource corresponding to this positioner.  Not valid when
     embedded in i.e. an xdg_popup.  */
  struct wl_resource *resource;
};

extern void XLCreateXdgPositioner (struct wl_client *, struct wl_resource *,
				   uint32_t);
extern void XLPositionerCalculateGeometry (Positioner *, Role *, int *, int *,
					   int *, int *);
extern void XLCheckPositionerComplete (Positioner *);

/* Defined in xdg_toplevel.c.  */

extern void XLGetXdgToplevel (struct wl_client *, struct wl_resource *,
			      uint32_t);
extern Bool XLHandleXEventForXdgToplevels (XEvent *);
extern Bool XLIsXdgToplevel (Window);
extern void XLInitXdgToplevels (void);
extern void XLXdgToplevelGetDecoration (XdgRoleImplementation *,
					struct wl_resource *, uint32_t);

/* Defined in xdg_popup.c.  */

extern void XLGetXdgPopup (struct wl_client *, struct wl_resource *,
			   uint32_t, struct wl_resource *,
			   struct wl_resource *);
extern Bool XLHandleXEventForXdgPopups (XEvent *);
extern void XLInitPopups (void);

/* Defined in xerror.c.  */

typedef struct _ClientErrorData ClientErrorData;

struct _ClientErrorData
{
  /* The wl_listener used to hang this data from.  */
  struct wl_listener listener;

  /* How many pixels of pixmap data this client has allocated.  This
     data is guaranteed to be present as long as the client still
     exists.  */
  uint64_t n_pixels;

  /* The number of references to this error data.  The problem with is
     that resources are destroyed after the client error data, so
     without reference counting the client data will become invalid by
     the time the destroy listener is called.  */
  int refcount;
};

extern ClientErrorData *ErrorDataForClient (struct wl_client *);
extern void ReleaseClientData (ClientErrorData *);
extern void ProcessPendingDisconnectClients (void);

extern void InitXErrors (void);
extern void CatchXErrors (void);
extern Bool UncatchXErrors (XErrorEvent *);

/* Defined in ewmh.c.  */

extern Bool XLWmSupportsHint (Atom);

/* Defined in timer.c.  */

typedef struct _Timer Timer;

extern Timer *AddTimer (void (*) (Timer *, void *,
				  struct timespec),
			void *, struct timespec);
extern Timer *AddTimerWithBaseTime (void (*) (Timer *, void *,
					      struct timespec),
				    void *, struct timespec,
				    struct timespec);

extern void RemoveTimer (Timer *);
extern void RetimeTimer (Timer *);
extern struct timespec TimerCheck (void);

extern struct timespec CurrentTimespec (void);
extern struct timespec MakeTimespec (time_t, long int);
extern int TimespecCmp (struct timespec, struct timespec);
extern struct timespec TimespecAdd (struct timespec, struct timespec);
extern struct timespec TimespecSub (struct timespec, struct timespec);

extern void XLInitTimers (void);

/* Defined in subsurface.c.  */

extern void XLSubsurfaceParentDestroyed (Role *);
extern void XLSubsurfaceHandleParentCommit (Surface *);
extern void XLInitSubsurfaces (void);
extern void XLUpdateOutputsForChildren (Surface *, int, int);
extern void XLUpdateDesynchronousChildren (Surface *, int *);
extern Surface *XLSubsurfaceGetRoot (Surface *);

/* Defined in data_device.c.  */

typedef struct _DataDevice DataDevice;
typedef struct _DataSource DataSource;
typedef struct _CreateOfferFuncs CreateOfferFuncs;
typedef struct _DndOfferFuncs DndOfferFuncs;

typedef struct wl_resource *(*CreateOfferFunc) (struct wl_client *,
						Timestamp);
typedef void (*SendDataFunc) (struct wl_resource *, Timestamp);

struct _CreateOfferFuncs
{
  /* Function called to create data offers.  */
  CreateOfferFunc create_offer;

  /* Function called to send data offers.  */
  SendDataFunc send_offers;
};

struct _DndOfferFuncs
{
  struct wl_resource *(*create) (struct wl_client *, int);
  void (*send_offers) (struct wl_resource *);
};

extern void XLInitDataDevice (void);
extern void XLRetainDataDevice (DataDevice *);
extern void XLReleaseDataDevice (DataDevice *);
extern void XLDataDeviceClearSeat (DataDevice *);
extern void XLDataDeviceHandleFocusChange (DataDevice *);
extern void XLSetForeignSelection (Timestamp, CreateOfferFuncs);
extern void XLClearForeignSelection (Timestamp);
extern int XLDataSourceTargetCount (DataSource *);
extern void XLDataSourceGetTargets (DataSource *, Atom *);
extern struct wl_resource *XLResourceFromDataSource (DataSource *);
extern Bool XLDataSourceHasAtomTarget (DataSource *, Atom);
extern Bool XLDataSourceHasTarget (DataSource *, const char *);
extern void XLDataSourceAttachDragDevice (DataSource *, DataDevice *);
extern void *XLDataSourceAddDestroyCallback (DataSource *, void (*) (void *),
					     void *);
extern void XLDataSourceCancelDestroyCallback (void *);
extern void XLDataSourceSendDropPerformed (DataSource *);
extern void XLDataSourceSendDropCancelled (DataSource *);
extern Bool XLDataSourceCanDrop (DataSource *);
extern XLList *XLDataSourceGetMimeTypeList (DataSource *);
extern void XLDataSourceUpdateDeviceActions (DataSource *);
extern uint32_t XLDataSourceGetSupportedActions (DataSource *);
extern void XLDataDeviceMakeOffers (Seat *, DndOfferFuncs, Surface *,
				    int, int);
extern void XLDataDeviceSendEnter (Seat *, Surface *, double, double,
				   DataSource *);
extern void XLDataDeviceSendMotion (Seat *, Surface *, double, double, Time);
extern void XLDataDeviceSendLeave (Seat *, Surface *, DataSource *);
extern void XLDataDeviceSendDrop (Seat *, Surface *);

/* Defined in text_input.h.  */

typedef struct _TextInputFuncs TextInputFuncs;

struct _TextInputFuncs
{
  void (*focus_in) (Seat *, Surface *);
  void (*focus_out) (Seat *);

  /* The second-last argument is actually an XIDeviceEvent *.  */
  Bool (*filter_input) (Seat *, Surface *, void *, KeyCode *);
};

extern void XLTextInputDispatchCoreEvent (Surface *, XEvent *);
extern void XLInitTextInput (void);

/* Defined in seat.c.  */

extern int xi2_opcode;
extern int xi_first_event;
extern int xi_first_error;
extern int xi2_major;
extern int xi2_minor;

extern XLList *live_seats;

extern Bool XLHandleOneXEventForSeats (XEvent *);
extern Window XLGetGEWindowForSeats (XEvent *);
extern void XLDispatchGEForSeats (XEvent *, Surface *,
				  Subcompositor *);
extern void XLSelectStandardEvents (Window);
extern void XLInitSeats (void);
extern Bool XLResizeToplevel (Seat *, Surface *, uint32_t, uint32_t);
extern void XLMoveToplevel (Seat *, Surface *, uint32_t);
extern Bool XLSeatExplicitlyGrabSurface (Seat *, Surface *, uint32_t);

extern void *XLSeatRunAfterResize (Seat *, void (*) (void *, void *),
				   void *);
extern void XLSeatCancelResizeCallback (void *);
extern void *XLSeatRunOnDestroy (Seat *, void (*) (void *), void *);
extern void XLSeatCancelDestroyListener (void *);
extern DataDevice *XLSeatGetDataDevice (Seat *);
extern void XLSeatSetDataDevice (Seat *, DataDevice *);
extern Bool XLSeatIsInert (Seat *);
extern Bool XLSeatIsClientFocused (Seat *, struct wl_client *);
extern Surface *XLSeatGetFocus (Seat *);
extern void XLSeatShowWindowMenu (Seat *, Surface *, int, int);
extern Timestamp XLSeatGetLastUserTime (Seat *);
extern void XLSeatBeginDrag (Seat *, DataSource *, Surface *,
			     Surface *, uint32_t);
extern DataSource *XLSeatGetDragDataSource (Seat *);
extern void *XLSeatAddModifierCallback (Seat *, void (*) (unsigned int, void *),
					void *);
extern void XLSeatRemoveModifierCallback (void *);
extern unsigned int XLSeatGetEffectiveModifiers (Seat *);
extern Bool XLSeatResizeInProgress (Seat *);
extern void XLSeatSetTextInputFuncs (TextInputFuncs *);
extern int XLSeatGetKeyboardDevice (Seat *);
extern int XLSeatGetPointerDevice (Seat *);
extern Seat *XLSeatGetInputMethodSeat (void);
extern void XLSeatDispatchCoreKeyEvent (Seat *, Surface *, XEvent *);
extern Seat *XLPointerGetSeat (Pointer *);
extern void XLSeatGetMouseData (Seat *, Surface **, double *, double *,
				double *, double *);
extern void XLSeatLockPointer (Seat *);
extern void XLSeatUnlockPointer (Seat *);
extern RelativePointer *XLSeatGetRelativePointer (Seat *, struct wl_resource *);
extern void XLSeatDestroyRelativePointer (RelativePointer *);
extern Bool XLSeatApplyExternalGrab (Seat *, Surface *);
extern void XLSeatCancelExternalGrab (Seat *);
extern SwipeGesture *XLSeatGetSwipeGesture (Seat *, struct wl_resource *);
extern PinchGesture *XLSeatGetPinchGesture (Seat *, struct wl_resource *);
extern void XLSeatDestroySwipeGesture (SwipeGesture *);
extern void XLSeatDestroyPinchGesture (PinchGesture *);
extern KeyCode XLKeysymToKeycode (KeySym, XEvent *);
extern Bool XLSeatCheckActivationSerial (Seat *, uint32_t);

extern Cursor InitDefaultCursor (void);

/* Defined in dmabuf.c.  */

extern void XLInitDmabuf (void);

/* Defined in select.c.  */

typedef struct _ReadTransfer ReadTransfer;
typedef struct _WriteTransfer WriteTransfer;

typedef enum _ReadStatus ReadStatus;

typedef ReadStatus (*GetDataFunc) (WriteTransfer *, unsigned char *,
				   ptrdiff_t, ptrdiff_t *);

enum _ReadStatus
  {
    ReadOk,
    EndOfFile,
    NeedBiggerBuffer,
  };

extern Window selection_transfer_window;

extern void FinishTransfers (void);
extern long SelectionQuantum (void);
extern Bool HookSelectionEvent (XEvent *);
extern void InitSelections (void);
extern ReadTransfer *ConvertSelectionFuncs (Atom, Atom, Time, void *,
					    void (*) (ReadTransfer *, Atom,
						      int),
					    void (*) (ReadTransfer *, Atom,
						      int, ptrdiff_t),
					    Bool (*) (ReadTransfer *, Bool));
extern void *GetTransferData (ReadTransfer *);
extern Time GetTransferTime (ReadTransfer *);

extern unsigned char *ReadChunk (ReadTransfer *, int, ptrdiff_t *,
				 ptrdiff_t *);
extern void SkipChunk (ReadTransfer *);
extern void CompleteDelayedTransfer (ReadTransfer *);

extern Bool OwnSelection (Timestamp, Atom, GetDataFunc (*) (WriteTransfer *,
							    Atom, Atom *),
			  Atom *, int);
extern void DisownSelection (Atom);

extern void SetWriteTransferData (WriteTransfer *, void *);
extern void *GetWriteTransferData (WriteTransfer *);
extern void StartReading (WriteTransfer *);

/* Defined in xdata.c.  */

extern Bool XLHandleOneXEventForXData (XEvent *);
extern void XLNoteSourceDestroyed (DataSource *);
extern Bool XLNoteLocalSelection (Seat *, DataSource *);
extern void XLReceiveDataFromSelection (Time, Atom, Atom, int);
extern Bool XLOwnDragSelection (Time, DataSource *);
extern void XLNotePrimaryDestroyed (PDataSource *);

extern void XLInitXData (void);

/* Defined in xsettings.c.  */

extern void XLInitXSettings (void);
extern Bool XLHandleOneXEventForXSettings (XEvent *);
extern void XLListenToIntegerSetting (const char *, void (*) (int));

/* Defined in dnd.c.  */

extern void XLDndWriteAwarenessProperty (Window);
extern Bool XLDndFilterClientMessage (Surface *, XEvent *);

extern void XLHandleOneXEventForDnd (XEvent *);

extern void XLDoDragLeave (Seat *);
extern void XLDoDragMotion (Seat *, double, double);
extern void XLDoDragFinish (Seat *);
extern Bool XLDoDragDrop (Seat *);

/* Defined in icon_surface.c.  */

typedef struct _IconSurface IconSurface;

extern IconSurface *XLGetIconSurface (Surface *);
extern Bool XLHandleOneXEventForIconSurfaces (XEvent *);
extern void XLMoveIconSurface (IconSurface *, int, int);
extern void XLInitIconSurfaces (void);
extern void XLReleaseIconSurface (IconSurface *);
extern Bool XLIsWindowIconSurface (Window);

/* Defined in primary_selection.c.  */

extern void XLInitPrimarySelection (void);
extern void XLSetForeignPrimary (Timestamp, CreateOfferFuncs);
extern void XLClearForeignPrimary (Timestamp);
extern Bool XLNoteLocalPrimary (Seat *, PDataSource *);
extern void XLPrimarySelectionHandleFocusChange (Seat *);
extern struct wl_resource *XLResourceFromPDataSource (PDataSource *);
extern Bool XLPDataSourceHasAtomTarget (PDataSource *, Atom);
extern Bool XLPDataSourceHasTarget (PDataSource *, const char *);
extern int XLPDataSourceTargetCount (PDataSource *);
extern void XLPDataSourceGetTargets (PDataSource *, Atom *);

/* Defined in picture_renderer.c.  */

extern Bool HandleErrorForPictureRenderer (XErrorEvent *);
extern Bool HandleOneXEventForPictureRenderer (XEvent *);
extern void InitPictureRenderer (void);

#ifdef HaveEglSupport

/* Defined in egl.c.  */

extern void InitEgl (void);

#endif

/* Defined in explicit_synchronization.c.  */

extern void XLDestroyRelease (SyncRelease *);
extern void XLSyncCommit (Synchronization *);
extern void XLSyncRelease (SyncRelease *);
extern void XLWaitFence (Surface *);
extern void XLInitExplicitSynchronization (void);

/* Defined in transform.c.  */

/* N.B. the data is stored in column-major order.  */
typedef float Matrix[9];

extern void MatrixMultiply (Matrix, Matrix, Matrix *);
extern void MatrixIdentity (Matrix *);
extern void MatrixTranslate (Matrix *, float, float);
extern void MatrixScale (Matrix *, float, float);
extern void MatrixExport (Matrix *, XTransform *);
extern void MatrixRotate (Matrix *, float, float, float);
extern void MatrixMirrorHorizontal (Matrix *, float);

extern void ApplyInverseTransform (int, int, Matrix *,
				   BufferTransform);
extern void TransformBox (pixman_box32_t *, BufferTransform, int, int);
extern BufferTransform InvertTransform (BufferTransform);

/* Defined in wp_viewporter.c.  */

extern void XLInitWpViewporter (void);
extern void XLWpViewportReportBadSize (ViewportExt *);
extern void XLWpViewportReportOutOfBuffer (ViewportExt *);

/* Defined in decoration.c.  */

extern void XLInitDecoration (void);

/* Defined in single_pixel_buffer.c.  */

extern void XLInitSinglePixelBuffer (void);

/* Defined in drm_lease.c.  */

extern void XLInitDrmLease (void);

/* Defined in pointer_constraints.c.  */

extern void XLInitPointerConstraints (void);
extern void XLPointerBarrierCheck (Seat *, Surface *, double, double,
				   double, double);
extern void XLPointerBarrierLeft (Seat *, Surface *);
extern void XLPointerConstraintsSurfaceMovedTo (Surface *, int, int);
extern void XLPointerConstraintsSubsurfaceMoved (Surface *);
extern void XLPointerConstraintsReconfineSurface (Surface *);

/* Defined in relative_pointer.c.  */

extern void XLInitRelativePointer (void);
extern void XLRelativePointerSendRelativeMotion (struct wl_resource *,
						 uint64_t, double, double);

/* Defined in keyboard_shortcuts_inhibit.c.  */

extern void XLInitKeyboardShortcutsInhibit (void);
extern void XLCheckShortcutInhibition (Seat *, Surface *);
extern void XLReleaseShortcutInhibition (Seat *, Surface *);

/* Defined in idle_inhibit.c.  */

extern void XLInitIdleInhibit (void);
extern void XLIdleInhibitNoticeSurfaceFocused (Surface *);
extern void XLDetectSurfaceIdleInhibit (void);

/* Defined in process.c.  */

typedef struct _ProcessQueue ProcessQueue;

extern void ParseProcessString (const char *, char ***, size_t *);
extern void RunProcess (ProcessQueue *, char **);
extern ProcessQueue *MakeProcessQueue (void);
extern int ProcessPoll (struct pollfd *, nfds_t, struct timespec *);

/* Defined in fence_ring.c.  */

typedef struct _Fence Fence;

extern Fence *GetFence (void);
extern Fence *GetLooseFence (void);
extern void FenceTrigger (Fence *);
extern void FenceRetain (Fence *);
extern void FenceAwait (Fence *);
extern void FenceRelease (Fence *);
extern XSyncFence FenceToXFence (Fence *);

/* Defined in pointer_gestures.c.  */

extern void XLInitPointerGestures (void);

/* Defined in test.c.  */

extern int locked_output_scale;

extern void XLInitTest (void);
extern Bool XLHandleOneXEventForTest (XEvent *);
extern Surface *XLLookUpTestSurface (Window, Subcompositor **);

/* Defined in buffer_release.c.  */

typedef void (*AllReleasedCallback) (void *);
typedef struct _BufferReleaseHelper BufferReleaseHelper;

extern BufferReleaseHelper *MakeBufferReleaseHelper (AllReleasedCallback,
						     void *);
extern void FreeBufferReleaseHelper (BufferReleaseHelper *);
extern void ReleaseBufferWithHelper (BufferReleaseHelper *, ExtBuffer *,
				     RenderTarget);

/* Defined in test_seat.c.  */

extern void XLGetTestSeat (struct wl_client *, struct wl_resource *,
			   uint32_t);

/* Defined in xdg_activation.c.  */

extern void XLInitXdgActivation (void);

/* Defined in tearing_control.c.  */

extern void XLInitTearingControl (void);

/* Defined in sync_source.h.  */

typedef struct _SyncHelper SyncHelper;
typedef enum _SynchronizationType SynchronizationType;

extern SyncHelper *MakeSyncHelper (Subcompositor *, Window,
				   RenderTarget,
				   void (*) (void *, uint32_t),
				   Role *);
extern void SyncHelperUpdate (SyncHelper *);
extern void FreeSyncHelper (SyncHelper *);
extern void SyncHelperHandleFrameEvent (SyncHelper *, XEvent *);
extern void SyncHelperSetResizeCallback (SyncHelper *, void (*) (void *,
								 Bool),
					 Bool (*) (void *));
extern void SyncHelperNoteConfigureEvent (SyncHelper *);
extern void SyncHelperCheckFrameCallback (SyncHelper *);
extern void SyncHelperClearPendingFrame (SyncHelper *);

/* Utility functions that don't belong in a specific file.  */

#define ArrayElements(arr) (sizeof (arr) / sizeof (arr)[0])

#define BoxStartX(box)	(MIN ((box).x1, (box).x2))
#define BoxEndX(box)	(MAX ((box).x1, (box).x2) - 1)
#define BoxStartY(box)	(MIN ((box).y1, (box).y2))
#define BoxEndY(box)	(MAX ((box).y1, (box).y2) - 1)

#define BoxWidth(box)	(BoxEndX (box) - BoxStartX (box) + 1)
#define BoxHeight(box)	(BoxEndY (box) - BoxStartY (box) + 1)

#define SafeCmp(n1, n2) (((n1) > (n2)) - ((n1) < (n2)))

#if __GNUC__ >= 7
#define Fallthrough __attribute__ ((fallthrough))
#else
#define Fallthrough ((void) 0)
#endif

/* Taken from intprops.h in gnulib.  */

/* True if the real type T is signed.  */
#define TypeIsSigned(t) (! ((t) 0 < (t) -1))

/* The width in bits of the integer type or expression T.
   Do not evaluate T.  T must not be a bit-field expression.
   Padding bits are not supported; this is checked at compile-time below.  */
#define TypeWidth(t) (sizeof (t) * CHAR_BIT)

/* The maximum and minimum values for the integer type T.  */
#define TypeMinimum(t) ((t) ~ TypeMaximum (t))
#define TypeMaximum(t)						\
  ((t) (! TypeIsSigned (t)					\
        ? (t) -1						\
        : ((((t) 1 << (TypeWidth (t) - 2)) - 1) * 2 + 1)))

#if __GNUC__ >= 7

#define IntAddWrapv(a, b, r) 		__builtin_add_overflow (a, b, r)
#define IntSubtractWrapv(a, b, r)	__builtin_sub_overflow (a, b, r)
#define IntMultiplyWrapv(a, b, r)	__builtin_mul_overflow (a, b, r)

#define Popcount(number)		__builtin_popcount (number)

#endif

/* This is a macro in order to be more static analyzer friendly.  */
#define XLAssert(cond) (!(cond) ? abort () : ((void) 0))

/* Utility structs.  */

typedef struct _Rectangle Rectangle;

struct _Rectangle
{
  /* The X and Y.  */
  int x, y;

  /* The width and height.  */
  int width, height;
};

/* port.h may override ports here.  */

#ifdef PortFile
#include PortFile
#endif
