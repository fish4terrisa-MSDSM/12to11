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

#include <sys/stat.h>
#include <sys/fcntl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>

#include <errno.h>

#include "compositor.h"

#include <X11/extensions/shape.h>

#include <X11/XKBlib.h>

#include <X11/extensions/XKBfile.h>
#include <X11/extensions/XKM.h>

#include <X11/extensions/XInput2.h>
#include <linux/input-event-codes.h>

#include "xdg-shell.h"
#include "pointer-gestures-unstable-v1.h"

/* X11 event opcode, event base, and error base for the input
   extension.  */

int xi2_opcode, xi_first_event, xi_first_error;

/* The version of the input extension in use.  */

int xi2_major, xi2_minor;

/* The current keymap file descriptor.  */

static int keymap_fd;

/* XKB event type.  */

static int xkb_event_type;

/* Keymap currently in use.  */

static XkbDescPtr xkb_desc;

/* Assocation between device IDs and seat objects.  This includes both
   keyboard and and pointer devices.  */

static XLAssocTable *seats;

/* Association between device IDs and "source device info" objects.
   This includes both pointer and keyboard devices.  */
static XLAssocTable *devices;

/* List of all seats that are not inert.  */

XLList *live_seats;

/* This is a mask of all keyboard state.  */
#define AllKeyMask							\
  (ShiftMask | LockMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask	\
   | Mod4Mask)

enum
  {
    IsInert		  = 1,
    IsWindowMenuShown	  = (1 << 2),
    IsDragging		  = (1 << 3),
    IsDropped		  = (1 << 4),
    IsTextInputSeat	  = (1 << 5),
    IsPointerLocked	  = (1 << 6),
    IsSurfaceCoordSet	  = (1 << 7),
    IsExternalGrabApplied = (1 << 8),
    IsInPinchGesture	  = (1 << 9),
    IsInSwipeGesture	  = (1 << 10),
    IsTestSeat		  = (1 << 11),
    IsTestDeviceSpecified = (1 << 12),
  };

enum
  {
    StateIsRaw = 1,
  };

enum
  {
    AnyVerticalAxis   = 1,
    AnyHorizontalAxis = (1 << 1),
  };

typedef struct _Seat Seat;
typedef struct _SeatClientInfo SeatClientInfo;
typedef struct _Pointer Pointer;
typedef struct _Keyboard Keyboard;
typedef struct _RelativePointer RelativePointer;
typedef struct _SeatCursor SeatCursor;
typedef struct _ResizeDoneCallback ResizeDoneCallback;
typedef struct _ScrollValuator ScrollValuator;
typedef struct _DestroyListener DestroyListener;
typedef struct _DeviceInfo DeviceInfo;
typedef struct _ModifierChangeCallback ModifierChangeCallback;
typedef struct _CursorRing CursorRing;

typedef enum _ResizeEdge ResizeEdge;
typedef enum _WhatEdge WhatEdge;
typedef enum _Direction Direction;

enum _ResizeEdge
  {
    NoneEdge	    = 65535,
    TopLeftEdge	    = 0,
    TopEdge	    = 1,
    TopRightEdge    = 2,
    RightEdge	    = 3,
    BottomRightEdge = 4,
    BottomEdge	    = 5,
    BottomLeftEdge  = 6,
    LeftEdge	    = 7,
    MoveEdge	    = 8,
  };

enum _WhatEdge
  {
    APointerEdge,
    AKeyboardEdge,
  };

enum _Direction
  {
    Vertical,
    Horizontal,
  };

enum
  {
    ResizeAxisTop    = 1,
    ResizeAxisLeft   = (1 << 1),
    ResizeAxisRight  = (1 << 2),
    ResizeAxisBottom = (1 << 3),
    ResizeAxisMove   = (1 << 16),
  };

enum
  {
    DeviceCanFingerScroll = 1,
    DeviceCanEdgeScroll	  = 2,
  };

/* Array indiced by ResizeEdge containing axes along which the edge
   resizes.  */

static int resize_edges[] =
  {
    ResizeAxisTop | ResizeAxisLeft,
    ResizeAxisTop,
    ResizeAxisTop | ResizeAxisRight,
    ResizeAxisRight,
    ResizeAxisRight | ResizeAxisBottom,
    ResizeAxisBottom,
    ResizeAxisBottom | ResizeAxisLeft,
    ResizeAxisLeft,
    ResizeAxisMove,
  };

#define CursorRingElements	2
#define CursorRingBusy		3

struct _CursorRing
{
  /* The width and height of the RenderTargets within.  */
  int width, height;

  /* Array of render targets.  */
  RenderTarget targets[CursorRingElements];

  /* Array of pixmaps.  */
  Pixmap pixmaps[CursorRingElements];

  /* Index of target being used.  -1 means nothing is being used.  */
  short used;
};

struct _DestroyListener
{
  /* Function called when seat is destroyed.  */
  void (*destroy) (void *);

  /* Data for that function.  */
  void *data;

  /* Next and last destroy listeners in this list.  */
  DestroyListener *next, *last;
};

struct _SeatCursor
{
  /* The parent role.  Note that there is no wl_resource associated
     with it.  */
  Role role;

  /* The current cursor.  */
  Cursor cursor;

  /* The seat this cursor is for.  */
  Seat *seat;

  /* The subcompositor for this cursor.  */
  Subcompositor *subcompositor;

  /* The frame callback for this cursor.  */
  void *cursor_frame_key;

  /* Ring of render targets for cursors.  This allows updating the
     cursor while not creating a new render target each time.  */
  CursorRing *cursor_ring;

  /* The hotspot of the cursor.  */
  int hotspot_x, hotspot_y;

  /* Whether or not this cursor is currently keeping the cursor clock
     active.  */
  Bool holding_cursor_clock;
};

struct _ResizeDoneCallback
{
  /* Function called when a resize operation finishes.  */
  void (*done) (void *, void *);

  /* Data for this callback.  */
  void *data;

  /* The next and last callbacks in this list.  */
  ResizeDoneCallback *next, *last;
};

struct _ScrollValuator
{
  /* The next scroll valuator in this list.  */
  ScrollValuator *next;

  /* The serial of the last event to have updated this valuator.  */
  unsigned long enter_serial;

  /* The current value of this valuator.  */
  double value;

  /* The increment of this valuator.  */
  double increment;

  /* The number of this valuator.  */
  int number;

  /* The direction of this valuator.  */
  Direction direction;
};

struct _Pointer
{
  /* The seat this pointer object refers to.  */
  Seat *seat;

  /* The struct wl_resource associated with this pointer.  */
  struct wl_resource *resource;

  /* The next and last pointer devices attached to the seat client
     info.  */
  Pointer *next, *last;

  /* The seat client info associated with this pointer resource.  */
  SeatClientInfo *info;

  /* Some state.  */
  int state;
};

struct _Keyboard
{
  /* The seat this keyboard object refers to.  */
  Seat *seat;

  /* The struct wl_resource associated with this keyboard.  */
  struct wl_resource *resource;

  /* The seat client info associated with this keyboard resource.  */
  SeatClientInfo *info;

  /* The next and last keyboard attached to the seat client info and
     the seat.  */
  Keyboard *next, *next1, *last, *last1;
};

struct _RelativePointer
{
  /* The seat this relative pointer refers to.  */
  Seat *seat;

  /* The struct wl_resource associated with this relative pointer.  */
  struct wl_resource *resource;

  /* The seat client info associated with this relative pointer
     resource.  */
  SeatClientInfo *info;

  /* The next and last relative pointers attached to the seat client
     info.  */
  RelativePointer *next, *last;
};

struct _SwipeGesture
{
  /* The seat this swipe gesture refers to.  */
  Seat *seat;

  /* The struct wl_resource associated with this swipe gesture.  */
  struct wl_resource *resource;

  /* The seat client info associated with this swipe gesture
     resource.  */
  SeatClientInfo *info;

  /* The next and last swipe gestures attached to the seat client
     info.  */
  SwipeGesture *next, *last;
};

struct _PinchGesture
{
  /* The seat this pinch gesture refers to.  */
  Seat *seat;

  /* The struct wl_resource associated with this pinch gesture.  */
  struct wl_resource *resource;

  /* The seat client info associated with this pinch gesture.  */
  SeatClientInfo *info;

  /* The next and last pinch gestures attached to the seat client
     info.  */
  PinchGesture *next, *last;
};

struct _SeatClientInfo
{
  /* The next and last structures in the client info chain.  */
  SeatClientInfo *next, *last;

  /* The client corresponding to this object.  */
  struct wl_client *client;

  /* Number of references to this seat client information.  */
  int refcount;

  /* The serial of the last enter event sent.  */
  uint32_t last_enter_serial;

  /* List of pointer objects on this seat for this client.  */
  Pointer pointers;

  /* List of keyboard objects on this seat for this client.  */
  Keyboard keyboards;

  /* List of relative pointers on this seat for this client.  */
  RelativePointer relative_pointers;

  /* List of swipe gestures on this seat for this client.  */
  SwipeGesture swipe_gestures;

  /* List of pinch gestures on this seat for this client.  */
  PinchGesture pinch_gestures;
};

struct _ModifierChangeCallback
{
  /* Callback run when modifiers change.  */
  void (*changed) (unsigned int, void *);

  /* Data for the callback.  */
  void *data;

  /* Next and last callbacks in this list.  */
  ModifierChangeCallback *next, *last;
};

struct _Seat
{
  /* The last user time.  */
  Timestamp last_user_time;

  /* The last time the focus changed into a surface.  */
  Timestamp last_focus_time;

  /* When the last external grab was applied.  */
  Time external_grab_time;

  /* wl_global associated with this seat.  */
  struct wl_global *global;

  /* XI device ID of the master keyboard device.  */
  int master_keyboard;

  /* XI device ID of the master pointer device.  */
  int master_pointer;

  /* Number of references to this seat.  */
  int refcount;

  /* Some flags associated with this seat.  */
  int flags;

  /* The currently focused surface.  */
  Surface *focus_surface;

  /* The destroy callback attached to that surface.  */
  DestroyCallback *focus_destroy_callback;

  /* The last surface seen.  */
  Surface *last_seen_surface;

  /* The destroy callback attached to that surface.  */
  DestroyCallback *last_seen_surface_callback;

  /* The surface on which the last pointer click was made.  */
  Surface *last_button_press_surface;

  /* The destroy callback attached to that surface.  */
  DestroyCallback *last_button_press_surface_callback;

  /* Unmap callback used for cancelling the grab.  */
  UnmapCallback *grab_unmap_callback;

  /* The subcompositor that the mouse pointer is inside.  */
  Subcompositor *last_seen_subcompositor;

  /* The window for that subcompositor.  */
  Window last_seen_subcompositor_window;

  /* The destroy callback for the subcompositor.  */
  SubcompositorDestroyCallback *subcompositor_callback;

  /* How many times the grab is held on this seat.  */
  int grab_held;

  /* Modifier masks.  */
  unsigned int base, locked, latched;

  /* Current base, locked and latched group.  Normalized by the X
     server.  */
  int base_group, locked_group, latched_group;

  /* Current effective group.  Also normalized.  */
  int effective_group;

  /* Bitmask of whether or not a key was pressed.  Length of the
     mask is the max_keycode >> 3 + 1.  */
  unsigned char *key_pressed;

  /* The current cursor attached to this seat.  */
  SeatCursor *cursor;

  /* The icon surface.  */
  IconSurface *icon_surface;

  /* Callbacks run after a resize completes.  */
  ResizeDoneCallback resize_callbacks;

  /* The drag-and-drop grab window.  This is a 1x1 InputOnly window
     with an empty input region at 0, 0, used to differentiate between
     events delivered to a surface during drag and drop, and events
     delivered due to the grab.  */
  Window grab_window;

  /* List of scroll valuators on this seat.  */
  ScrollValuator *valuators;

  /* Serial of the last crossing event.  */
  unsigned long last_crossing_serial;

  /* List of destroy listeners.  */
  DestroyListener destroy_listeners;

  /* Surface currently being resized, if any.  */
  Surface *resize_surface;

  /* Unmap callback for that surface.  */
  UnmapCallback *resize_surface_callback;

  /* The last edge used to obtain a grab.  */
  WhatEdge last_grab_edge;

  /* The last timestamp used to obtain a grab.  */
  Time last_grab_time;

  /* When it was sent.  */
  Time its_press_time;

  /* The time of the last key event sent.  */
  Time its_depress_time;

  /* The name of the seat.  */
  char *name;

  /* The grab surface.  While it exists, events for different clients
     will be reported relative to it.  */
  Surface *grab_surface;

  /* The unmap callback.  */
  UnmapCallback *grab_surface_callback;

  /* The data source for drag-and-drop.  */
  DataSource *data_source;

  /* The destroy callback for the data source.  */
  void *data_source_destroy_callback;

  /* The surface on which this drag operation started.  */
  Surface *drag_start_surface;

  /* The UnmapCallback for that surface.  */
  UnmapCallback *drag_start_unmap_callback;

  /* The last surface to be entered during drag-and-drop.  */
  Surface *drag_last_surface;

  /* The destroy callback for that surface.  */
  DestroyCallback *drag_last_surface_destroy_callback;

  /* The time the active grab was acquired.  */
  Time drag_grab_time;

  /* The button of the last button event sent, and the root_x and
     root_y of the last button or motion event.  */
  int last_button, its_root_x, its_root_y;

  /* The serial of the last button event sent.  */
  uint32_t last_button_serial;

  /* The serial of the last button press event sent.  GTK 4 sends this
     even when grabbing a popup in response to a button release
     event.  */
  uint32_t last_button_press_serial;

  /* The last serial used to obtain a grab.  */
  uint32_t last_grab_serial;

  /* The serial of the last key event sent.  */
  uint32_t last_keyboard_serial;

  /* Whether or not a resize is in progress.  */
  Bool resize_in_progress;

  /* Where that resize started.  */
  int resize_start_root_x, resize_start_root_y;

  /* Where the pointer was last seen.  */
  int resize_last_root_x, resize_last_root_y;

  /* The dimensions of the surface when it was first seen.  */
  int resize_width, resize_height;

  /* The axises.  */
  int resize_axis_flags;

  /* The button for the resize.  */
  int resize_button;

  /* The time used to obtain the resize grab.  */
  Time resize_time;

  /* The attached data device, if any.  */
  DataDevice *data_device;

  /* List of seat client information.  */
  SeatClientInfo client_info;

  /* List of all attached keyboards.  */
  Keyboard keyboards;

  /* The root_x and root_y of the last motion or crossing event.  */
  double last_motion_x, last_motion_y;

  /* The x and y of the last surface movement.  */
  double last_surface_x, last_surface_y;

  /* List of all modifier change callbacks attached to this seat.  */
  ModifierChangeCallback modifier_callbacks;

  /* Array of keys currently held down.  */
  struct wl_array keys;
};

struct _DeviceInfo
{
  /* Some flags associated with this device.  */
  int flags;

  /* The libinput scroll pixel distance, if available.  Else 15.  */
  int scroll_pixel_distance;
};

#define SetMask(ptr, event)						\
  (((unsigned char *) (ptr))[(event) >> 3] |= (1 << ((event) & 7)))
#define ClearMask(ptr, event)						\
  (((unsigned char *) (ptr))[(event) >> 3] &= ~(1 << ((event) & 7)))
#define MaskIsSet(ptr, event)						\
  (((unsigned char *) (ptr))[(event) >> 3] &   (1 << ((event) & 7)))
#define MaskLen(event)							\
  (((event) >> 3) + 1)

/* Text input functions.  */
static TextInputFuncs *input_funcs;

#define CursorFromRole(role)	((SeatCursor *) (role))



static Bool
QueryPointer (Seat *seat, Window relative_to, double *x, double *y)
{
  XIButtonState buttons;
  XIModifierState modifiers;
  XIGroupState group;
  double root_x, root_y, win_x, win_y;
  Window root, child;
  Bool same_screen;

  buttons.mask = NULL;
  same_screen = False;

  /* First, initialize default values in case the pointer is on a
     different screen.  */
  *x = 0;
  *y = 0;

  if (XIQueryPointer (compositor.display, seat->master_pointer,
		      relative_to, &root, &child, &root_x, &root_y,
		      &win_x, &win_y, &buttons, &modifiers,
		      &group))
    {
      *x = win_x;
      *y = win_y;
      same_screen = True;
    }

  /* buttons.mask must be freed manually, even if the pointer is on a
     different screen.  */
  if (buttons.mask)
    XFree (buttons.mask);

  return same_screen;
}

static void
FinalizeSeatClientInfo (Seat *seat)
{
  SeatClientInfo *info, *last;

  info = seat->client_info.next;

  while (info != &seat->client_info)
    {
      last = info;
      info = info->next;

      /* Mark this as invalid, so it won't be unchained later on.  */
      last->last = NULL;
      last->next = NULL;
    }
}

static SeatClientInfo *
GetSeatClientInfo (Seat *seat, struct wl_client *client)
{
  SeatClientInfo *info;

  info = seat->client_info.next;

  while (info != &seat->client_info)
    {
      if (info->client == client)
	return info;

      info = info->next;
    }

  return NULL;
}

static SeatClientInfo *
CreateSeatClientInfo (Seat *seat, struct wl_client *client)
{
  SeatClientInfo *info;

  /* See if client has already created something on the seat.  */
  info = GetSeatClientInfo (seat, client);

  /* Otherwise, create it ourselves.  */
  if (!info)
    {
      info = XLCalloc (1, sizeof *info);
      info->next = seat->client_info.next;
      info->last = &seat->client_info;
      seat->client_info.next->last = info;
      seat->client_info.next = info;

      info->client = client;
      info->pointers.next = &info->pointers;
      info->pointers.last = &info->pointers;
      info->keyboards.next = &info->keyboards;
      info->keyboards.last = &info->keyboards;
      info->relative_pointers.next = &info->relative_pointers;
      info->relative_pointers.last = &info->relative_pointers;
      info->swipe_gestures.next = &info->swipe_gestures;
      info->swipe_gestures.last = &info->swipe_gestures;
      info->pinch_gestures.next = &info->pinch_gestures;
      info->pinch_gestures.last = &info->pinch_gestures;
    }

  /* Increase the reference count of info.  */
  info->refcount++;

  /* Return info.  */
  return info;
}

static void
ReleaseSeatClientInfo (SeatClientInfo *info)
{
  if (--info->refcount)
    return;

  /* Assert that there are no more keyboards or pointers attached.  */
  XLAssert (info->keyboards.next == &info->keyboards);
  XLAssert (info->pointers.next == &info->pointers);
  XLAssert (info->relative_pointers.next == &info->relative_pointers);

  /* Unlink the client info structure if it is still linked.  */
  if (info->next)
    {
      info->next->last = info->last;
      info->last->next = info->next;
    }

  /* Free the client info.  */
  XLFree (info);
}

static void
RetainSeat (Seat *seat)
{
  seat->refcount++;
}

static CursorRing *
MakeCursorRing (int width, int height)
{
  CursorRing *ring;

  ring = XLCalloc (1, sizeof *ring);
  ring->width = width;
  ring->height = height;
  ring->used = -1;

  return ring;
}

static void
MaybeCreateCursor (CursorRing *ring, int index)
{
  XLAssert (index < CursorRingElements);

  /* If the cursor has already been created, return.  */
  if (ring->pixmaps[index])
    return;

  ring->pixmaps[index]
    = XCreatePixmap (compositor.display,
		     DefaultRootWindow (compositor.display),
		     ring->width, ring->height,
		     compositor.n_planes);
  ring->targets[index]
    = RenderTargetFromPixmap (ring->pixmaps[index]);

  /* For simplicity reasons we do not handle idle notifications
     asynchronously.  */
  RenderSetNeedWaitForIdle (ring->targets[index]);
}

static int
GetUnusedCursor (CursorRing *ring)
{
  int i;

  for (i = 0; i < CursorRingElements; ++i)
    {
      if (ring->used != i)
	{
	  /* Create the cursor contents if they have not yet been
	     created.  */
	  MaybeCreateCursor (ring, i);

	  return i;
	}
    }

  return CursorRingBusy;
}

static void
FreeCursorRing (CursorRing *ring)
{
  int i;

  for (i = 0; i < CursorRingElements; ++i)
    {
      if (!ring->pixmaps[i])
	/* This element wasn't created.  */
	continue;

      /* Free the target and pixmap.  */
      RenderDestroyRenderTarget (ring->targets[i]);
      XFreePixmap (compositor.display, ring->pixmaps[i]);
    }

  /* Free the ring itself.  */
  XLFree (ring);
}

static void
ResizeCursorRing (CursorRing *ring, int width, int height)
{
  int i;

  if (width == ring->width && height == ring->height)
    return;

  /* Destroy the pixmaps currently in the cursor ring.  */

  for (i = 0; i < CursorRingElements; ++i)
    {
      if (!ring->pixmaps[i])
	/* This element wasn't created.  */
	continue;

      /* Free the target and pixmap.  */
      RenderDestroyRenderTarget (ring->targets[i]);
      XFreePixmap (compositor.display, ring->pixmaps[i]);

      /* Mark this element as free.  */
      ring->pixmaps[i] = None;
    }

  /* Reinitialize the cursor ring with new data.  */
  ring->width = width;
  ring->height = height;
  ring->used = -1;
}

static void
UpdateCursorOutput (SeatCursor *cursor, int root_x, int root_y)
{
  int hotspot_x, hotspot_y;

  /* Scale the hotspot coordinates up by the scale factor specified in
     the surface.  */
  hotspot_x = cursor->hotspot_x * cursor->role.surface->factor;
  hotspot_y = cursor->hotspot_y * cursor->role.surface->factor;

  /* We use a rectangle 1 pixel wide and tall, originating from the
     hotspot of the pointer.  */
  XLUpdateSurfaceOutputs (cursor->role.surface, root_x + hotspot_x,
			  root_y + hotspot_y, 1, 1);
}

static Window
CursorWindow (SeatCursor *cursor)
{
  /* When dragging, use the surface on which the active grab is
     set.  */
  if (cursor->seat->flags & IsDragging)
    return cursor->seat->grab_window;

  /* The cursor should be cleared along with
     seat->last_seen_surface.  */
  XLAssert (cursor->seat->last_seen_surface != NULL);

  return XLWindowFromSurface (cursor->seat->last_seen_surface);
}

static void
HandleCursorFrame (void *data, struct timespec time)
{
  SeatCursor *cursor;

  cursor = data;

  if (cursor->role.surface)
    XLSurfaceRunFrameCallbacks (cursor->role.surface, time);
}

static void
StartCursorClock (SeatCursor *cursor)
{
  if (cursor->holding_cursor_clock)
    return;

  cursor->cursor_frame_key
    = XLAddCursorClockCallback (HandleCursorFrame,
				cursor);
  cursor->holding_cursor_clock = True;
}

static void
EndCursorClock (SeatCursor *cursor)
{
  if (!cursor->holding_cursor_clock)
    return;

  XLStopCursorClockCallback (cursor->cursor_frame_key);
  cursor->holding_cursor_clock = False;
}

static void
FreeCursor (SeatCursor *cursor)
{
  Window window;

  if (cursor->role.surface)
    XLSurfaceReleaseRole (cursor->role.surface,
			  &cursor->role);

  /* Now any attached views should have been released, so free the
     subcompositor.  */
  SubcompositorFree (cursor->subcompositor);

  cursor->seat->cursor = NULL;

  window = CursorWindow (cursor);

  if (cursor->cursor != None)
    XFreeCursor (compositor.display, cursor->cursor);

  if (!(cursor->seat->flags & IsInert) && window)
    XIDefineCursor (compositor.display,
		    cursor->seat->master_pointer,
		    window, InitDefaultCursor ());

  /* And release the cursor ring.  */
  if (cursor->cursor_ring)
    FreeCursorRing (cursor->cursor_ring);

  /* Maybe release the cursor clock if it was active for this
     cursor.  */
  EndCursorClock (cursor);
  XLFree (cursor);
}

static void
FreeValuators (Seat *seat)
{
  ScrollValuator *last, *tem;

  tem = seat->valuators;

  while (tem)
    {
      last = tem;
      tem = tem->next;

      XLFree (last);
    }

  seat->valuators = NULL;
}

static void
FreeDestroyListeners (Seat *seat)
{
  DestroyListener *listener, *last;

  listener = seat->destroy_listeners.next;

  while (listener != &seat->destroy_listeners)
    {
      last = listener;
      listener = listener->next;

      XLFree (last);
    }
}

static void
FreeModifierCallbacks (Seat *seat)
{
  ModifierChangeCallback *callback, *last;

  callback = seat->modifier_callbacks.next;

  while (callback != &seat->modifier_callbacks)
    {
      last = callback;
      callback = callback->next;

      XLFree (last);
    }
}

static void
ReleaseSeat (Seat *seat)
{
  if (--seat->refcount)
    return;

  if (seat->icon_surface)
    XLReleaseIconSurface (seat->icon_surface);

  if (seat->focus_destroy_callback)
    XLSurfaceCancelRunOnFree (seat->focus_destroy_callback);

  if (seat->last_seen_surface_callback)
    XLSurfaceCancelRunOnFree (seat->last_seen_surface_callback);

  if (seat->last_button_press_surface_callback)
    XLSurfaceCancelRunOnFree (seat->last_button_press_surface_callback);

  if (seat->drag_last_surface_destroy_callback)
    XLSurfaceCancelRunOnFree (seat->drag_last_surface_destroy_callback);

  if (seat->grab_surface_callback)
    XLSurfaceCancelUnmapCallback (seat->grab_surface_callback);

  if (seat->grab_unmap_callback)
    XLSurfaceCancelUnmapCallback (seat->grab_unmap_callback);

  if (seat->resize_surface_callback)
    XLSurfaceCancelUnmapCallback (seat->resize_surface_callback);

  if (seat->drag_start_unmap_callback)
    XLSurfaceCancelUnmapCallback (seat->drag_start_unmap_callback);

  if (seat->data_source_destroy_callback)
    XLDataSourceCancelDestroyCallback (seat->data_source_destroy_callback);

  if (seat->subcompositor_callback)
    SubcompositorRemoveDestroyCallback (seat->subcompositor_callback);

  if (seat->grab_window != None)
    XDestroyWindow (compositor.display, seat->grab_window);

  wl_array_release (&seat->keys);

  if (seat->cursor)
    FreeCursor (seat->cursor);

  if (seat->data_device)
    {
      XLDataDeviceClearSeat (seat->data_device);
      XLReleaseDataDevice (seat->data_device);
    }

  FinalizeSeatClientInfo (seat);
  FreeValuators (seat);
  FreeDestroyListeners (seat);
  FreeModifierCallbacks (seat);

  XLFree (seat->name);
  XLFree (seat->key_pressed);
  XLFree (seat);
}

static void
ComputeHotspot (SeatCursor *cursor, int min_x, int min_y,
		int *x, int *y)
{
  int dx, dy;
  int hotspot_x, hotspot_y;

  if (!cursor->role.surface)
    {
      /* Can this really happen? */
      *x = min_x + cursor->hotspot_x;
      *y = min_y + cursor->hotspot_y;
      return;
    }

  /* Scale the hotspot coordinates up by the scale.  */
  hotspot_x = cursor->hotspot_x * cursor->role.surface->factor;
  hotspot_y = cursor->hotspot_y * cursor->role.surface->factor;

  /* Apply the surface offsets to the hotspot as well.  */
  dx = (cursor->role.surface->current_state.x
	* cursor->role.surface->factor);
  dy = (cursor->role.surface->current_state.y
	* cursor->role.surface->factor);

  *x = min_x + hotspot_x - dx;
  *y = min_y + hotspot_y - dy;
}

static void
ApplyCursor (SeatCursor *cursor, RenderTarget target,
	     int min_x, int min_y)
{
  Window window;
  int x, y;
  Picture picture;

  if (cursor->cursor)
    XFreeCursor (compositor.display, cursor->cursor);

  ComputeHotspot (cursor, min_x, min_y, &x, &y);

  picture = RenderPictureFromTarget (target);
  cursor->cursor = XRenderCreateCursor (compositor.display,
					picture, MAX (0, x),
					MAX (0, y));
  RenderFreePictureFromTarget (picture);

  window = CursorWindow (cursor);

  if (!(cursor->seat->flags & IsInert) && window != None)
    XIDefineCursor (compositor.display,
		    cursor->seat->master_pointer,
		    window, cursor->cursor);
}

static void
UpdateCursorFromSubcompositor (SeatCursor *cursor)
{
  RenderTarget target;
  int min_x, min_y, max_x, max_y, width, height, x, y;
  Bool need_clear;
  int index;

  /* First, compute the bounds of the subcompositor.  */
  SubcompositorBounds (cursor->subcompositor,
		       &min_x, &min_y, &max_x, &max_y);

  /* Then, its width and height.  */
  width = max_x - min_x + 1;
  height = max_y - min_y + 1;

  /* If the cursor hotspot extends outside width and height, extend
     the picture.  */
  ComputeHotspot (cursor, min_x, min_y, &x, &y);

  if (x < 0 || y < 0 || x >= width || y >= height)
    {
      if (x >= width)
	width = x;
      if (y >= height)
	height = y;

      if (x < 0)
	width += -x;
      if (y < 0)
	height += -y;

      need_clear = True;
    }
  else
    need_clear = False;

  if (cursor->cursor_ring)
    /* If the width or height of the cursor ring changed, resize its
       contents.  */
    ResizeCursorRing (cursor->cursor_ring, width, height);
  else
    /* Otherwise, there is not yet a cursor ring.  Create one.  */
    cursor->cursor_ring = MakeCursorRing (width, height);

  /* Get an unused cursor from the cursor ring.  */
  index = GetUnusedCursor (cursor->cursor_ring);
  XLAssert (index != CursorRingBusy);

  /* Get the target and pixmap.  */
  target = cursor->cursor_ring->targets[index];

  /* If the bounds extend beyond the subcompositor, clear the
     picture.  */
  if (need_clear)
    RenderClearRectangle (target, 0, 0, width, height);

  /* Garbage the subcompositor, since cursor contents are not
     preserved.  */
  SubcompositorGarbage (cursor->subcompositor);

  /* Set the right transform if the hotspot is negative.  */
  SubcompositorSetProjectiveTransform (cursor->subcompositor,
				       MAX (0, -x), MAX (0, -x));

  SubcompositorSetTarget (cursor->subcompositor, &target);
  SubcompositorUpdate (cursor->subcompositor);
  SubcompositorSetTarget (cursor->subcompositor, NULL);

  /* Apply the new cursor.  */
  ApplyCursor (cursor, target, min_x, min_y);

  /* Set it as the cursor being used.  */
  cursor->cursor_ring->used = index;
}

static void
UpdateCursor (SeatCursor *cursor, int x, int y)
{
  cursor->hotspot_x = x;
  cursor->hotspot_y = y;

  UpdateCursorFromSubcompositor (cursor);
}

static void
ApplyEmptyCursor (SeatCursor *cursor)
{
  Window window;

  if (cursor->cursor)
    XFreeCursor (compositor.display, cursor->cursor);

  cursor->cursor = None;
  window = CursorWindow (cursor);

  if (window != None)
    XIDefineCursor (compositor.display,
		    cursor->seat->master_pointer,
		    window, InitDefaultCursor ());

  if (cursor->cursor_ring)
    /* This means no cursor in the ring is currently being used.  */
    cursor->cursor_ring->used = -1;
}

static void
Commit (Surface *surface, Role *role)
{
  SeatCursor *cursor;

  cursor = CursorFromRole (role);

  if (SubcompositorIsEmpty (cursor->subcompositor))
    {
      ApplyEmptyCursor (cursor);
      return;
    }

  UpdateCursorFromSubcompositor (cursor);

  /* If the surface now has frame callbacks, start the cursor frame
     clock.  */

  if (surface->current_state.frame_callbacks.next
      != &surface->current_state.frame_callbacks)
    StartCursorClock (cursor);
}

static void
Teardown (Surface *surface, Role *role)
{
  role->surface = NULL;

  ViewUnparent (surface->view);
  ViewUnparent (surface->under);

  ViewSetSubcompositor (surface->view, NULL);
  ViewSetSubcompositor (surface->under, NULL);
}

static Bool
Setup (Surface *surface, Role *role)
{
  SeatCursor *cursor;

  cursor = CursorFromRole (role);
  role->surface = surface;

  /* First, attach the subcompositor.  */
  ViewSetSubcompositor (surface->under, cursor->subcompositor);
  ViewSetSubcompositor (surface->view, cursor->subcompositor);

  /* Then, insert the view.  */
  SubcompositorInsert (cursor->subcompositor, surface->under);
  SubcompositorInsert (cursor->subcompositor, surface->view);

  return True;
}

static void
ReleaseBuffer (Surface *surface, Role *role, ExtBuffer *buffer)
{
  SeatCursor *cursor;
  int i;

  cursor = CursorFromRole (role);

  /* Cursors are generally committed only once, so syncing here is
     OK in terms of efficiency.  */
  for (i = 0; i < CursorRingElements; ++i)
    {
      if (cursor->cursor_ring->pixmaps[i])
	RenderWaitForIdle (XLRenderBufferFromBuffer (buffer),
			   cursor->cursor_ring->targets[i]);
    }

  XLReleaseBuffer (buffer);
}

static void
SubsurfaceUpdate (Surface *surface, Role *role)
{
  SeatCursor *cursor;

  cursor = CursorFromRole (role);

  /* A desync subsurface's contents changed.  Update the cursor
     again.  */
  UpdateCursorFromSubcompositor (cursor);
}

static void
MakeCurrentCursor (Seat *seat, Surface *surface, int x, int y)
{
  SeatCursor *role;
  Window window;

  window = XLWindowFromSurface (seat->last_seen_surface);

  if (window == None || (seat->flags & IsInert))
    return;

  role = XLCalloc (1, sizeof *role);
  XIDefineCursor (compositor.display,
		  seat->master_pointer,
		  window,
		  InitDefaultCursor ());

  role->hotspot_x = x;
  role->hotspot_y = y;
  role->seat = seat;

  ApplyEmptyCursor (role);

  /* Set up role callbacks.  */

  role->role.funcs.commit = Commit;
  role->role.funcs.teardown = Teardown;
  role->role.funcs.setup = Setup;
  role->role.funcs.release_buffer = ReleaseBuffer;
  role->role.funcs.subsurface_update = SubsurfaceUpdate;

  /* Set up the subcompositor.  */

  role->subcompositor = MakeSubcompositor ();

  if (!XLSurfaceAttachRole (surface, &role->role))
    abort ();

  seat->cursor = role;

  /* Tell the cursor surface what output(s) it is in.  */

  UpdateCursorOutput (role, seat->last_motion_x,
		      seat->last_motion_y);

  /* If something was committed, update the cursor now.  */

  if (!SubcompositorIsEmpty (role->subcompositor))
    UpdateCursorFromSubcompositor (role);
}

static void
SetCursor (struct wl_client *client, struct wl_resource *resource,
	   uint32_t serial, struct wl_resource *surface_resource,
	   int32_t hotspot_x, int32_t hotspot_y)
{
  Surface *surface, *seen;
  Pointer *pointer;
  Seat *seat;

  pointer = wl_resource_get_user_data (resource);
  seat = pointer->seat;
  seen = seat->last_seen_surface;

  if (serial < pointer->info->last_enter_serial)
    return;

  if (!surface_resource)
    {
      if (!seen || (wl_resource_get_client (seen->resource)
		    != client))
	return;

      if (seat->cursor)
	FreeCursor (seat->cursor);

      return;
    }

  surface = wl_resource_get_user_data (surface_resource);

  /* Do nothing at all if the last seen surface isn't owned by client
     and we are not updating the current pointer surface.  */

  if (!seat->cursor
      || surface->role != &seat->cursor->role)
    {
      if (!seen || (wl_resource_get_client (seen->resource)
		    != client))
	return;
    }

  /* If surface already has another role, raise an error.  */

  if (surface->role_type != AnythingType
      && surface->role_type != CursorType)
    {
      wl_resource_post_error (resource, WL_POINTER_ERROR_ROLE,
			      "surface already has or had a different role");
      return;
    }

  if (surface->role && !seat->cursor
      && surface->role != &seat->cursor->role)
    {
      wl_resource_post_error (resource, WL_POINTER_ERROR_ROLE,
			      "surface already has a cursor role"
			      " on another seat");
      return;
    }

  if (surface->role)
    {
      UpdateCursor (CursorFromRole (surface->role),
		    hotspot_x, hotspot_y);
      return;
    }

  /* Free any cursor that already exists.  */
  if (seat->cursor)
    FreeCursor (seat->cursor);

  MakeCurrentCursor (seat, surface, hotspot_x, hotspot_y);
}

static void
ReleasePointer (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
ReleaseKeyboard (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_pointer_interface wl_pointer_impl =
  {
    .set_cursor = SetCursor,
    .release = ReleasePointer,
  };

static const struct wl_keyboard_interface wl_keyboard_impl =
  {
    .release = ReleaseKeyboard,
  };

static void
HandlePointerResourceDestroy (struct wl_resource *resource)
{
  Pointer *pointer;

  pointer = wl_resource_get_user_data (resource);
  pointer->last->next = pointer->next;
  pointer->next->last = pointer->last;

  ReleaseSeatClientInfo (pointer->info);
  ReleaseSeat (pointer->seat);

  XLFree (pointer);
}

static void
HandleKeyboardResourceDestroy (struct wl_resource *resource)
{
  Keyboard *keyboard;

  keyboard = wl_resource_get_user_data (resource);
  keyboard->last->next = keyboard->next;
  keyboard->next->last = keyboard->last;
  keyboard->last1->next1 = keyboard->next1;
  keyboard->next1->last1 = keyboard->last1;

  ReleaseSeatClientInfo (keyboard->info);
  ReleaseSeat (keyboard->seat);

  XLFree (keyboard);
}

static void
GetPointer (struct wl_client *client, struct wl_resource *resource,
	    uint32_t id)
{
  Seat *seat;
  SeatClientInfo *info;
  Pointer *pointer;
  struct wl_resource *pointer_resource;

  pointer_resource
    = wl_resource_create (client, &wl_pointer_interface,
			  wl_resource_get_version (resource),
			  id);

  if (!pointer_resource)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  pointer = XLSafeMalloc (sizeof *pointer);

  if (!pointer)
    {
      wl_resource_post_no_memory (resource);
      wl_resource_destroy (pointer_resource);

      return;
    }

  seat = wl_resource_get_user_data (resource);
  RetainSeat (seat);

  memset (pointer, 0, sizeof *pointer);

  info = CreateSeatClientInfo (seat, client);
  pointer->resource = pointer_resource;
  pointer->seat = seat;
  pointer->info = info;
  pointer->next = info->pointers.next;
  pointer->last = &info->pointers;

  /* This flag means the pointer object has just been created, and
     button presses should send a corresponding entry event.  */
  pointer->state |= StateIsRaw;

  info->pointers.next->last = pointer;
  info->pointers.next = pointer;

  wl_resource_set_implementation (pointer_resource, &wl_pointer_impl,
				  pointer, HandlePointerResourceDestroy);
}

static void
SendRepeatKeys (struct wl_resource *resource)
{
  if (wl_resource_get_version (resource) < 4)
    return;

  wl_keyboard_send_repeat_info (resource,
				1000 / xkb_desc->ctrls->repeat_interval,
				xkb_desc->ctrls->repeat_delay);
}

static void
UpdateSingleKeyboard (Keyboard *keyboard)
{
  struct stat statb;

  if (fstat (keymap_fd, &statb) < 0)
    {
      perror ("fstat");
      exit (0);
    }

  wl_keyboard_send_keymap (keyboard->resource,
			   WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
			   keymap_fd, statb.st_size);

  SendRepeatKeys (keyboard->resource);
}

static void
GetKeyboard (struct wl_client *client, struct wl_resource *resource,
	     uint32_t id)
{
  SeatClientInfo *info;
  Seat *seat;
  Keyboard *keyboard;
  struct wl_resource *keyboard_resource;

  keyboard_resource
    = wl_resource_create (client, &wl_keyboard_interface,
			  wl_resource_get_version (resource),
			  id);

  if (!keyboard_resource)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  keyboard = XLSafeMalloc (sizeof *keyboard);

  if (!keyboard)
    {
      wl_resource_post_no_memory (resource);
      wl_resource_destroy (keyboard_resource);

      return;
    }

  seat = wl_resource_get_user_data (resource);
  RetainSeat (seat);

  memset (keyboard, 0, sizeof *keyboard);

  info = CreateSeatClientInfo (seat, client);
  keyboard->resource = keyboard_resource;

  /* First, link the keyboard onto the seat client info.  */
  keyboard->info = info;
  keyboard->next = info->keyboards.next;
  keyboard->last = &info->keyboards;
  info->keyboards.next->last = keyboard;
  info->keyboards.next = keyboard;

  /* Then, the seat.  */
  keyboard->seat = seat;
  keyboard->next1 = seat->keyboards.next1;
  keyboard->last1 = &seat->keyboards;
  seat->keyboards.next1->last1 = keyboard;
  seat->keyboards.next1 = keyboard;

  wl_resource_set_implementation (keyboard_resource, &wl_keyboard_impl,
				  keyboard, HandleKeyboardResourceDestroy);

  UpdateSingleKeyboard (keyboard);

  /* Update the keyboard's focus surface too.  */

  if (seat->focus_surface
      && (wl_resource_get_client (seat->focus_surface->resource)
	  == client))
    wl_keyboard_send_enter (keyboard_resource,
			    wl_display_next_serial (compositor.wl_display),
			    seat->focus_surface->resource, &seat->keys);
}

static void
GetTouch (struct wl_client *client, struct wl_resource *resource,
	  uint32_t id)
{
  wl_resource_post_error (resource, WL_SEAT_ERROR_MISSING_CAPABILITY,
			  "touch support not yet implemented");
}

static void
Release (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_seat_interface wl_seat_impl =
  {
    .get_pointer = GetPointer,
    .get_keyboard = GetKeyboard,
    .get_touch = GetTouch,
    .release = Release,
  };

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  Seat *seat;

  seat = wl_resource_get_user_data (resource);
  ReleaseSeat (seat);
}

static void
HandleBind1 (struct wl_client *client, Seat *seat, uint32_t version,
	     uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_seat_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &wl_seat_impl, seat,
				  HandleResourceDestroy);

  wl_seat_send_capabilities (resource, (WL_SEAT_CAPABILITY_POINTER
					| WL_SEAT_CAPABILITY_KEYBOARD));

  if (wl_resource_get_version (resource) > 2)
    wl_seat_send_name (resource, seat->name);

  RetainSeat (seat);
}

static void
HandleBind (struct wl_client *client, void *data, uint32_t version,
	    uint32_t id)
{
  HandleBind1 (client, data, version, id);
}

static void
AddValuator (Seat *seat, XIScrollClassInfo *info)
{
  ScrollValuator *valuator;

  valuator = XLCalloc (1, sizeof *valuator);
  valuator->next = seat->valuators;
  valuator->increment = info->increment;
  valuator->number = info->number;

  if (info->scroll_type == XIScrollTypeHorizontal)
    valuator->direction = Horizontal;
  else
    valuator->direction = Vertical;

  seat->valuators = valuator;
}

static void
UpdateValuators (Seat *seat, XIDeviceInfo *device)
{
  int i;

  /* First, free any existing valuators.  */
  FreeValuators (seat);

  /* Next, add each valuator.  */
  for (i = 0; i < device->num_classes; ++i)
    {
      if (device->classes[i]->type == XIScrollClass)
	AddValuator (seat, (XIScrollClassInfo *) device->classes[i]);
    }
}

static void
InitSeatCommon (Seat *seat)
{
  seat->client_info.next = &seat->client_info;
  seat->client_info.last = &seat->client_info;

  seat->keyboards.next1 = &seat->keyboards;
  seat->keyboards.last1 = &seat->keyboards;

  seat->resize_callbacks.next = &seat->resize_callbacks;
  seat->resize_callbacks.last = &seat->resize_callbacks;

  seat->destroy_listeners.next = &seat->destroy_listeners;
  seat->destroy_listeners.last = &seat->destroy_listeners;

  seat->modifier_callbacks.next = &seat->modifier_callbacks;
  seat->modifier_callbacks.last = &seat->modifier_callbacks;

  wl_array_init (&seat->keys);
}

static void
MakeSeatForDevicePair (int master_keyboard, int master_pointer,
		       XIDeviceInfo *pointer_info)
{
  Seat *seat;
  XkbStateRec state;
  unsigned long mask;

  seat = XLCalloc (1, sizeof *seat);
  seat->master_keyboard = master_keyboard;
  seat->master_pointer = master_pointer;
  seat->name = XLStrdup (pointer_info->name);
  seat->global = wl_global_create (compositor.wl_display,
				   &wl_seat_interface, 8,
				   seat, HandleBind);

  InitSeatCommon (seat);

  XLMakeAssoc (seats, master_keyboard, seat);
  XLMakeAssoc (seats, master_pointer, seat);

  if (!live_seats)
    /* This is the first seat; make it the input seat.  */
    seat->flags |= IsTextInputSeat;

  live_seats = XLListPrepend (live_seats, seat);

  /* Now update the seat state from the X server.  */
  CatchXErrors ();

  XkbGetState (compositor.display, master_keyboard, &state);

  if (UncatchXErrors (NULL))
    /* If the device was disabled or removed, a HierarchyChange event
       will be sent shortly afterwards, causing the seat to be
       destroyed.  In that case, not selecting for modifier changes
       will be inconsequential.  */
    return;

  seat->base = state.base_mods;
  seat->locked = state.locked_mods;
  seat->latched = state.latched_mods;
  seat->base_group = state.base_group;
  seat->locked_group = state.locked_group;
  seat->latched_group = state.latched_group;
  seat->effective_group = state.group;

  /* And select for XKB events from the master keyboard device.  If
     the server does not support accessing input extension devices
     with Xkb, an error will result.  */

  mask = 0;

  mask |= XkbModifierStateMask;
  mask |= XkbModifierBaseMask;
  mask |= XkbModifierLatchMask;
  mask |= XkbModifierLockMask;
  mask |= XkbGroupStateMask;
  mask |= XkbGroupBaseMask;
  mask |= XkbGroupLatchMask;
  mask |= XkbGroupLockMask;

  CatchXErrors ();
  XkbSelectEventDetails (compositor.display, master_keyboard,
			 /* Now enable everything in that mask.  */
			 XkbStateNotify, mask, mask);
  UncatchXErrors (NULL);

  UpdateValuators (seat, pointer_info);
  RetainSeat (seat);
}

static void
UpdateScrollMethods (DeviceInfo *info, int deviceid)
{
  unsigned char *data;
  Status rc;
  Atom actual_type;
  int actual_format;
  unsigned long nitems, bytes_after;

  data = NULL;

  /* This only works with the libinput driver.  */
  rc = XIGetProperty (compositor.display, deviceid,
		      libinput_Scroll_Methods_Available,
		      0, 3, False, XIAnyPropertyType,
		      &actual_type, &actual_format,
		      &nitems, &bytes_after, &data);

  /* If there aren't enough items in the data, or the format is wrong,
     return.  */
  if (rc != Success || nitems < 3 || actual_format != 8 || !data)
    {
      if (data)
	XFree (data);

      return;
    }

  /* First, clear all flags that this function sets.  */
  info->flags &= ~DeviceCanFingerScroll;
  info->flags &= ~DeviceCanEdgeScroll;

  if (data[0])
    info->flags |= DeviceCanFingerScroll;

  if (data[1])
    info->flags |= DeviceCanEdgeScroll;

  if (data)
    XFree (data);
}

static void
UpdateScrollPixelDistance (DeviceInfo *info, int deviceid)
{
  unsigned char *data;
  Status rc;
  Atom actual_type;
  int actual_format;
  unsigned long nitems, bytes_after;

  data = NULL;

  /* This only works with the libinput driver.  */
  rc = XIGetProperty (compositor.display, deviceid,
		      libinput_Scrolling_Pixel_Distance,
		      0, 1, False, XIAnyPropertyType,
		      &actual_type, &actual_format,
		      &nitems, &bytes_after, &data);

  /* If there aren't enough items in the data, or the format is wrong,
     return.  */
  if (rc != Success || nitems < 1 || actual_format != 32 || !data)
    {
      if (data)
	XFree (data);

      /* Set the distance to the default, 15.  */
      info->scroll_pixel_distance = 15;

      return;
    }

  /* Now set the scroll pixel distance.  */
  info->scroll_pixel_distance = ((long *) data)[0];

  /* And free the data.  */
  XLFree (data);
}

static void
RecordDeviceInformation (XIDeviceInfo *deviceinfo)
{
  DeviceInfo *info;

  info = XLLookUpAssoc (devices, deviceinfo->deviceid);

  /* If info doesn't exist, allocate it now.  */
  if (!info)
    {
      info = XLMalloc (sizeof *info);
      XLMakeAssoc (devices, deviceinfo->deviceid, info);
    }

  /* Now clear info->flags, and determine what the device can do.  */
  info->flags = 0;

  /* Initialize scrolling attributes pertinent to pointer devices.  */
  if (deviceinfo->use == XISlavePointer)
    {
      /* Catch errors in case the device disappears.  */
      CatchXErrors ();

      /* Obtain the "libinput Scroll Methods Enabled" property and use it
	 to compute what scroll methods are available.  This naturally
	 only works with the libinput driver.  */
      UpdateScrollMethods (info, deviceinfo->deviceid);

      /* Obtain the "libinput Scrolling Pixel Distance" property and
	 use it if available.  If not, default to 15.  */
      UpdateScrollPixelDistance (info, deviceinfo->deviceid);

      /* Uncatch errors.  */
      UncatchXErrors (NULL);
    }
}

static void
SetupInitialDevices (void)
{
  XIDeviceInfo *deviceinfo;
  int ndevices, i;

  deviceinfo = XIQueryDevice (compositor.display,
			      XIAllDevices, &ndevices);

  if (!deviceinfo)
    return;

  for (i = 0; i < ndevices; ++i)
    {
      if (deviceinfo[i].use == XIMasterPointer)
	MakeSeatForDevicePair (deviceinfo[i].attachment,
			       deviceinfo[i].deviceid,
			       &deviceinfo[i]);

      RecordDeviceInformation (&deviceinfo[i]);
    }

  XIFreeDeviceInfo (deviceinfo);
}

static void
RunResizeDoneCallbacks (Seat *seat)
{
  ResizeDoneCallback *callback, *last;

  callback = seat->resize_callbacks.next;

  while (callback != &seat->resize_callbacks)
    {
      last = callback;
      callback = callback->next;

      last->done (last, last->data);
      XLFree (last);
    }

  /* Empty the list since all elements are free again.  */
  seat->resize_callbacks.next = &seat->resize_callbacks;
  seat->resize_callbacks.last = &seat->resize_callbacks;
}

/* Forward declarations.  */
static void TransformToSurface (Surface *, double, double, double *, double *);
static Surface *FindSurfaceUnder (Subcompositor *, double, double);
static void EnteredSurface (Seat *, Surface *, Time, double, double, Bool);

static void
CancelResizeOperation (Seat *seat, Time time, Subcompositor *subcompositor,
		       XIDeviceEvent *xev)
{
  Surface *dispatch;
  double x, y;

  /* Stop the resize operation.  */
  XLSurfaceCancelUnmapCallback (seat->resize_surface_callback);
  seat->resize_surface = NULL;

  /* Run resize completion callbacks.  */
  RunResizeDoneCallbacks (seat);

  /* Ungrab the pointer.  */
  XIUngrabDevice (compositor.display, seat->master_pointer,
		  time);

  if (!subcompositor)
    return;

  /* If there's no focus surface, look up the surface and enter
     it.  The grab should not be held at this point.  */
  dispatch = FindSurfaceUnder (subcompositor, xev->event_x, xev->event_y);

  if (dispatch)
    {
      TransformToSurface (dispatch, xev->event_x, xev->event_y, &x, &y);

      /* Enter the surface.  */
      EnteredSurface (seat, dispatch, xev->time, x, y, False);
    }

  /* Otherwise, there should be no need to leave any entered surface,
     since the entered surface should be NULL already.  */
}

static Bool
InterceptButtonEventForResize (Seat *seat, Subcompositor *subcompositor,
			       XIDeviceEvent *xev)
{
  if (xev->type == XI_ButtonPress)
    return True;

  /* If the button starting the resize has been released, cancel the
     resize operation.  */
  if (xev->detail == seat->resize_button)
    CancelResizeOperation (seat, xev->time, subcompositor, xev);

  return True;
}

/* Forward declaration.  */

static Bool HandleValuatorMotion (Seat *, Surface *, double, double,
				  XIDeviceEvent *);

#define MoveLeft(flags, i)	((flags) & ResizeAxisLeft ? (i) : 0)
#define MoveTop(flags, i)	((flags) & ResizeAxisTop  ? (i) : 0)

static void
HandleMovement (Seat *seat, int west, int north)
{
  XLSurfaceMoveBy (seat->resize_surface, west, north);
}

static Bool
InterceptMotionEventForResize (Seat *seat, XIDeviceEvent *xev)
{
  int root_x, root_y, diff_x, diff_y, abs_diff_x, abs_diff_y;

  root_x = lrint (xev->root_x);
  root_y = lrint (xev->root_y);

  /* Handle valuator motion anyway.  Otherwise, the values could get
     out of date.  */
  HandleValuatorMotion (seat, NULL, xev->event_x, xev->event_y, xev);

  if (root_x == seat->resize_last_root_x
      && root_y == seat->resize_last_root_y)
    /* No motion really happened.  */
    return True;

  /* If this is a move and not a resize, simply move the surface's
     window.  */
  if (seat->resize_axis_flags & ResizeAxisMove)
    {
      HandleMovement (seat, seat->resize_last_root_x - root_x,
		      seat->resize_last_root_y - root_y);

      seat->resize_last_root_x = root_x;
      seat->resize_last_root_y = root_y;
      return True;
    }

  /* Compute the amount by which to move the window.  The movement is
     towards the geographical north and west.  */
  diff_x = seat->resize_last_root_x - root_x;
  diff_y = seat->resize_last_root_y - root_y;

  abs_diff_x = 0;
  abs_diff_y = 0;

  if (seat->resize_axis_flags & ResizeAxisLeft)
    /* diff_x will move the surface leftwards.  This is by how
       much to extend the window the other way as well.  */
    abs_diff_x = seat->resize_start_root_x - root_x;

  if (seat->resize_axis_flags & ResizeAxisTop)
    /* Likewise for diff_y.  */
    abs_diff_y = seat->resize_start_root_y - root_y;

  if (seat->resize_axis_flags & ResizeAxisRight)
    /* diff_x is computed differently here, since root_x grows in the
       correct resize direction.  */
    abs_diff_x = root_x - seat->resize_start_root_x;

  if (seat->resize_axis_flags & ResizeAxisBottom)
    /* The same applies for the direction of root_y.  */
    abs_diff_y = root_y - seat->resize_start_root_y;

  if (!abs_diff_x && !abs_diff_y)
    /* No resizing has to take place.  */
    return True;

  seat->resize_last_root_x = root_x;
  seat->resize_last_root_y = root_y;

  /* Now, post a new configure event.  Upon ack, also move the window
     leftwards and topwards by diff_x and diff_y, should the resize
     direction go that way.  */
  XLSurfacePostResize (seat->resize_surface,
		       MoveLeft (seat->resize_axis_flags, diff_x),
		       MoveTop (seat->resize_axis_flags, diff_y),
		       seat->resize_width + abs_diff_x,
		       seat->resize_height + abs_diff_y);

  return True;
}

static Bool
InterceptResizeEvent (Seat *seat, Subcompositor *subcompositor,
		      XIDeviceEvent *xev)
{
  if (!seat->resize_surface)
    return False;

  switch (xev->evtype)
    {
    case XI_ButtonRelease:
      return InterceptButtonEventForResize (seat, subcompositor, xev);

    case XI_Motion:
      return InterceptMotionEventForResize (seat, xev);
    }

  return True;
}

static void
RunDestroyListeners (Seat *seat)
{
  DestroyListener *listeners;

  listeners = seat->destroy_listeners.next;

  while (listeners != &seat->destroy_listeners)
    {
      listeners->destroy (listeners->data);
      listeners = listeners->next;
    }
}

/* Forward declaration.  */
static void SetFocusSurface (Seat *, Surface *);

static void
NoticeDeviceDisabled (int deviceid)
{
  Seat *seat, *new;
  DeviceInfo *info;

  /* First, see if there is any deviceinfo related to the disabled
     device.  If there is, free it.  */
  info = XLLookUpAssoc (devices, deviceid);

  if (info)
    {
      XLDeleteAssoc (devices, deviceid);
      XLFree (info);
    }

  /* It doesn't matter if this is the keyboard or pointer, since
     paired master devices are always destroyed together.  */

  seat = XLLookUpAssoc (seats, deviceid);

  /* Test seats should not be destroyed here.  */

  if (seat && !(seat->flags & IsTestSeat))
    {
      /* The device has been disabled, mark the seat inert and
	 dereference it.  The seat is still referred to by the
	 global.  */

      seat->flags |= IsInert;

      /* Set the focus surface to NULL, so surfaces don't mistakenly
	 treat themselves as still focused.  */

      SetFocusSurface (seat, NULL);

      /* Run destroy handlers.  */

      RunDestroyListeners (seat);

      /* Since the seat is now inert, remove it from the assoc
	 table and destroy the global.  */

      XLDeleteAssoc (seats, seat->master_keyboard);
      XLDeleteAssoc (seats, seat->master_pointer);

      /* Also remove it from the list of live seats.  */

      live_seats = XLListRemove (live_seats, seat);

      /* Run and remove all resize completion callbacks.  */

      RunResizeDoneCallbacks (seat);

      /* Finally, destroy the global.  */

      wl_global_destroy (seat->global);

      /* If it was the input seat, then find a new seat to take its
	 place.  */
      if (seat->flags & IsTextInputSeat
	  && live_seats)
	{
	  new = live_seats->data;

	  /* This results in nondeterministic selection of input
	     seats, and as such can be confusing to the user.  */
	  new->flags |= IsTextInputSeat;
	}

      /* And release the seat.  */

      ReleaseSeat (seat);
    }
}

static void
NoticeDeviceEnabled (int deviceid)
{
  XIDeviceInfo *info;
  int ndevices;

  CatchXErrors ();
  info = XIQueryDevice (compositor.display, deviceid,
			&ndevices);
  UncatchXErrors (NULL);

  if (info && info->use == XIMasterPointer)
    /* ndevices doesn't have to checked here.  */
    MakeSeatForDevicePair (info->attachment, deviceid, info);

  if (info)
    {
      /* Update device information for this device.  */
      RecordDeviceInformation (info);

      /* And free the device.  */
      XIFreeDeviceInfo (info);
    }
}

static void
NoticeSlaveAttached (int deviceid)
{
  XIDeviceInfo *info;
  int ndevices;

  CatchXErrors ();
  info = XIQueryDevice (compositor.display, deviceid,
			&ndevices);
  UncatchXErrors (NULL);

  /* A slave device was attached.  Take this opportunity to update its
     device information.  */

  if (info)
    {
      /* Update device information for this device.  */
      RecordDeviceInformation (info);

      /* And free the device.  */
      XIFreeDeviceInfo (info);
    }
}

static void
HandleHierarchyEvent (XIHierarchyEvent *event)
{
  int i;

  for (i = 0; i < event->num_info; ++i)
    {
      if (event->info[i].flags & XIDeviceDisabled)
	NoticeDeviceDisabled (event->info[i].deviceid);
      else if (event->info[i].flags & XIDeviceEnabled)
	NoticeDeviceEnabled (event->info[i].deviceid);
      else if (event->info[i].flags & XISlaveAttached)
	NoticeSlaveAttached (event->info[i].deviceid);
    }
}

#define KeyIsPressed(seat, keycode)					\
  (MaskIsSet ((seat)->key_pressed,					\
	      (keycode) - xkb_desc->min_key_code))
#define KeySetPressed(seat, keycode, pressed)				\
  (!pressed								\
   ? ClearMask ((seat)->key_pressed,					\
		(keycode) - xkb_desc->min_key_code)			\
   : SetMask ((seat)->key_pressed,					\
	      (keycode) - xkb_desc->min_key_code))			\

#define WaylandKeycode(keycode) ((keycode) - 8)

static void
InsertKeyIntoSeat (Seat *seat, int32_t keycode)
{
  int32_t *data;

  data = wl_array_add (&seat->keys, sizeof *data);

  if (data)
    *data = keycode;
}

static void
ArrayRemove (struct wl_array *array, void *item, size_t size)
{
  size_t bytes;
  char *arith;

  arith = item;

  bytes = array->size - (arith + size
			 - (char *) array->data);
  if (bytes > 0)
    memmove (item, arith + size, bytes);
  array->size -= size;
}

static void
RemoveKeyFromSeat (Seat *seat, int32_t keycode)
{
  int32_t *data;

  wl_array_for_each (data, &seat->keys)
    {
      if (*data == keycode)
	{
	  ArrayRemove (&seat->keys, data, sizeof *data);
	  break;
	}
    }
}

static SeatClientInfo *
ClientInfoForResource (Seat *seat, struct wl_resource *resource)
{
  return GetSeatClientInfo (seat, wl_resource_get_client (resource));
}

static void
SendKeyboardKey (Seat *seat, Surface *focus, Time time,
		 uint32_t key, uint32_t state)
{
  Keyboard *keyboard;
  SeatClientInfo *info;
  uint32_t serial;

  serial = wl_display_next_serial (compositor.wl_display);
  seat->last_keyboard_serial = serial;

  info = ClientInfoForResource (seat, focus->resource);

  if (!info)
    return;

  keyboard = info->keyboards.next;

  for (; keyboard != &info->keyboards; keyboard = keyboard->next)
    wl_keyboard_send_key (keyboard->resource, serial, time,
			  key, state);
}

static void
HandleKeyPressed (Seat *seat, KeyCode keycode, Time time)
{
  if (KeyIsPressed (seat, keycode))
    return;

  KeySetPressed (seat, keycode, True);
  InsertKeyIntoSeat (seat, WaylandKeycode (keycode));
}

static void
HandleKeyReleased (Seat *seat, KeyCode keycode, Time time)
{
  if (!KeyIsPressed (seat, keycode))
    return;

  KeySetPressed (seat, keycode, False);
  RemoveKeyFromSeat (seat, WaylandKeycode (keycode));
}

static void
HandleRawKey (XIRawEvent *event)
{
  Seat *seat;

  /* We select for raw events from the X server in order to track the
     keys that are currently pressed.  In order to respect grabs, key
     press and release events are only reported in response to
     regular device events.  */

  seat = XLLookUpAssoc (seats, event->deviceid);

  if (!seat)
    return;

  if (event->detail < xkb_desc->min_key_code
      || event->detail > xkb_desc->max_key_code)
    return;

  if (event->evtype == XI_RawKeyPress)
    HandleKeyPressed (seat, event->detail, event->time);
  else
    HandleKeyReleased (seat, event->detail, event->time);

  /* This is used for tracking grabs.  */
  seat->its_depress_time = event->time;

  /* Update the last user time.  send_event events can have a
     different timestamp not synchronized with that of the server.  */
  if (!event->send_event)
    seat->last_user_time = TimestampFromServerTime (event->time);
}

static void
HandleResizeComplete (Seat *seat)
{
  Surface *surface;
  XEvent msg;

  surface = seat->last_button_press_surface;

  if (!surface || !XLWindowFromSurface (surface))
    goto finish;

  /* We might have gotten the button release before the window manager
     set the grab.  Cancel the resize operation in that case.  */

  memset (&msg, 0, sizeof msg);
  msg.xclient.type = ClientMessage;
  msg.xclient.window = XLWindowFromSurface (surface);
  msg.xclient.format = 32;
  msg.xclient.message_type = _NET_WM_MOVERESIZE;
  msg.xclient.data.l[0] = seat->its_root_x;
  msg.xclient.data.l[1] = seat->its_root_y;
  msg.xclient.data.l[2] = 11; /* _NET_WM_MOVERESIZE_CANCEL.  */
  msg.xclient.data.l[3] = seat->last_button;
  msg.xclient.data.l[4] = 1; /* Source indication.  */

  XSendEvent (compositor.display,
	      DefaultRootWindow (compositor.display),
	      False,
	      SubstructureRedirectMask | SubstructureNotifyMask,
	      &msg);

 finish:

  /* Now say that resize operations have stopped.  */
  seat->resize_in_progress = False;

  /* And run callbacks.  */
  RunResizeDoneCallbacks (seat);
}

/* Forward declarations.  */

static int GetXButton (int);
static void SendButton (Seat *, Surface *, Time, uint32_t, uint32_t,
			double, double);

static void
HandleRawButton (XIRawEvent *event)
{
  Seat *seat;
  int button;
  double win_x, win_y;
  double dispatch_x, dispatch_y;
  Window window;

  seat = XLLookUpAssoc (seats, event->deviceid);

  if (!seat)
    return;

  if (seat->resize_in_progress
      || seat->flags & IsWindowMenuShown)
    {
      if (seat->last_seen_surface)
	{
	  window = XLWindowFromSurface (seat->last_seen_surface);

	  if (window == None)
	    goto complete;

	  button = GetXButton (event->detail);

	  if (button < 0)
	    goto complete;

	  /* When a RawButtonPress is received while resizing is still
	     in progress, release the button on the current surface.

	     Since leave and entry events generated by grabs are
	     ignored, the client will not get leave events correctly
	     while Metacity is resizing a frame.  This results in
	     programs such as GTK tracking the button state
	     incorrectly if the pointer never leaves the surface
	     during a resize operation.

	     Something similar applies to the window menu.

	     (It would be good to avoid this sync by fetching the
	     actual modifier values from the raw event.)  */

	  if (QueryPointer (seat, window, &win_x, &win_y))
	    {
	      /* Otherwise, the pointer is on a different screen!  */
	      TransformToSurface (seat->last_seen_surface,
				  win_x, win_y, &dispatch_x, &dispatch_y);

	      /* And finally the button release.  */
	      SendButton (seat, seat->last_seen_surface, event->time,
			  button, WL_POINTER_BUTTON_STATE_RELEASED,
			  dispatch_x, dispatch_y);
	    }
	}

    complete:

      if (event->detail == seat->last_button
	  && seat->resize_in_progress)
	HandleResizeComplete (seat);
    }
}

static void
HandleDeviceChanged (XIDeviceChangedEvent *event)
{
  Seat *seat;
  XIDeviceInfo *info;
  int ndevices;

  seat = XLLookUpAssoc (seats, event->deviceid);

  if (!seat || event->deviceid != seat->master_pointer)
    return;

  /* Now, update scroll valuators from the new device info.  */

  CatchXErrors ();
  info = XIQueryDevice (compositor.display, event->deviceid,
			&ndevices);
  UncatchXErrors (NULL);

  if (!info)
    /* The device was disabled, return now.  */
    return;

  UpdateValuators (seat, info);
  XIFreeDeviceInfo (info);
}

static void
HandlePropertyChanged (XIPropertyEvent *event)
{
  DeviceInfo *info;

  info = XLLookUpAssoc (devices, event->deviceid);

  if (!info)
    return;

  if (event->property == libinput_Scroll_Methods_Available)
    /* Update scroll methods for the device whose property
       changed.  */
    UpdateScrollMethods (info, event->deviceid);
  else if (event->property == libinput_Scrolling_Pixel_Distance)
    /* Update the scroll pixel distance.  */
    UpdateScrollPixelDistance (info, event->deviceid);
}

static Seat *
FindSeatByDragWindow (Window window)
{
  Seat *seat;
  XLList *tem;

  for (tem = live_seats; tem; tem = tem->next)
    {
      seat = tem->data;

      if (seat->grab_window == window)
	return seat;
    }

  return NULL;
}

static Bool
HandleDragMotionEvent (XIDeviceEvent *xev)
{
  Seat *seat;

  seat = FindSeatByDragWindow (xev->event);

  if (!seat)
    return False;

  /* When an event is received for the drag window, it means the event
     is outside any surface.  Dispatch it to the external drag and
     drop code.  */

  /* Move the drag-and-drop icon window.  */
  if (seat->icon_surface)
    XLMoveIconSurface (seat->icon_surface, xev->root_x,
		       xev->root_y);

  /* Update information used for resize tracking.  */
  seat->its_root_x = xev->root_x;
  seat->its_root_y = xev->root_y;

  /* Dispatch the drag motion to external programs.  */
  if (seat->data_source)
    XLDoDragMotion (seat, xev->root_x, xev->root_y);

  return True;
}

/* Forward declaration.  */

static void DragButton (Seat *, XIDeviceEvent *);

static Bool
HandleDragButtonEvent (XIDeviceEvent *xev)
{
  Seat *seat;

  seat = FindSeatByDragWindow (xev->event);

  if (!seat)
    return False;

  DragButton (seat, xev);
  return True;
}

static Bool
HandleOneGenericEvent (XGenericEventCookie *xcookie)
{
  switch (xcookie->evtype)
    {
    case XI_HierarchyChanged:
      HandleHierarchyEvent (xcookie->data);
      return True;

    case XI_DeviceChanged:
      HandleDeviceChanged (xcookie->data);
      return True;

    case XI_PropertyEvent:
      HandlePropertyChanged (xcookie->data);
      return True;

    case XI_RawKeyPress:
    case XI_RawKeyRelease:
      HandleRawKey (xcookie->data);
      return True;

    case XI_RawButtonRelease:
      HandleRawButton (xcookie->data);
      return True;

    case XI_Motion:
      return HandleDragMotionEvent (xcookie->data);

    case XI_ButtonPress:
    case XI_ButtonRelease:
      return HandleDragButtonEvent (xcookie->data);
    }

  return False;
}

static void
SelectDeviceEvents (void)
{
  XIEventMask mask;
  size_t length;

  length = XIMaskLen (XI_LASTEVENT);
  mask.mask = alloca (length);
  mask.mask_len = length;
  mask.deviceid = XIAllDevices;

  memset (mask.mask, 0, length);

  XISetMask (mask.mask, XI_PropertyEvent);
  XISetMask (mask.mask, XI_HierarchyChanged);
  XISetMask (mask.mask, XI_DeviceChanged);

  XISelectEvents (compositor.display,
		  DefaultRootWindow (compositor.display),
		  &mask, 1);

  memset (mask.mask, 0, length);

  mask.deviceid = XIAllMasterDevices;

  XISetMask (mask.mask, XI_RawKeyPress);
  XISetMask (mask.mask, XI_RawKeyRelease);
  XISetMask (mask.mask, XI_RawButtonRelease);

  XISelectEvents (compositor.display,
		  DefaultRootWindow (compositor.display),
		  &mask, 1);
}

static void
ClearFocusSurface (void *data)
{
  Seat *seat;

  seat = data;

  seat->focus_surface = NULL;
  seat->focus_destroy_callback = NULL;

  XLPrimarySelectionHandleFocusChange (seat);

  /* Tell any input method about the focus change.  */
  if (input_funcs)
    input_funcs->focus_out (seat);
}

static void
SendKeyboardLeave (Seat *seat, Surface *focus)
{
  Keyboard *keyboard;
  uint32_t serial;
  SeatClientInfo *info;

  serial = wl_display_next_serial (compositor.wl_display);
  info = ClientInfoForResource (seat, focus->resource);

  if (!info)
    return;

  keyboard = info->keyboards.next;

  for (; keyboard != &info->keyboards; keyboard = keyboard->next)
    wl_keyboard_send_leave (keyboard->resource,
			    serial, focus->resource);
}

static void
UpdateSingleModifiers (Seat *seat, Keyboard *keyboard, uint32_t serial)
{
  wl_keyboard_send_modifiers (keyboard->resource, serial,
			      seat->base, seat->latched,
			      seat->locked,
			      seat->effective_group);
}

static void
SendKeyboardEnter (Seat *seat, Surface *enter)
{
  Keyboard *keyboard;
  uint32_t serial;
  SeatClientInfo *info;

  serial = wl_display_next_serial (compositor.wl_display);
  info = ClientInfoForResource (seat, enter->resource);

  if (!info)
    return;

  keyboard = info->keyboards.next;

  for (; keyboard != &info->keyboards; keyboard = keyboard->next)
    {
      wl_keyboard_send_enter (keyboard->resource, serial,
			      enter->resource, &seat->keys);
      UpdateSingleModifiers (seat, keyboard, serial);
    }
}

static void
SendKeyboardModifiers (Seat *seat, Surface *focus)
{
  Keyboard *keyboard;
  uint32_t serial;
  SeatClientInfo *info;

  serial = wl_display_next_serial (compositor.wl_display);
  info = ClientInfoForResource (seat, focus->resource);

  if (!info)
    return;

  keyboard = info->keyboards.next;

  for (; keyboard != &info->keyboards; keyboard = keyboard->next)
    UpdateSingleModifiers (seat, keyboard, serial);
}

static void
HackKeyboardModifiers (Seat *seat, Surface *focus, int effective,
		       int group)
{
  Keyboard *keyboard;
  uint32_t serial;
  SeatClientInfo *info;

  serial = wl_display_next_serial (compositor.wl_display);
  info = ClientInfoForResource (seat, focus->resource);

  if (!info)
    return;

  keyboard = info->keyboards.next;

  for (; keyboard != &info->keyboards; keyboard = keyboard->next)
    /* It is wrong to send the new modifiers in seat->based, but I
       don't know anything better.  */
    wl_keyboard_send_modifiers (keyboard->resource, serial,
				effective, 0, 0, group);
}

static void
SendUpdatedModifiers (Seat *seat)
{
  ModifierChangeCallback *callback;

  for (callback = seat->modifier_callbacks.next;
       callback != &seat->modifier_callbacks;
       callback = callback->next)
    /* Send the effective modifiers.  */
    callback->changed (seat->base | seat->locked | seat->latched,
		       callback->data);

  /* If drag and drop is in progress, update the data source
     actions.  */
  if (seat->flags & IsDragging && seat->data_source
      /* Don't do this during external drag and drop.  */
      && seat->drag_last_surface)
    XLDataSourceUpdateDeviceActions (seat->data_source);

  if (seat->focus_surface)
    SendKeyboardModifiers (seat, seat->focus_surface);
}

static void
UpdateModifiersForSeat (Seat *seat,
			unsigned int base, unsigned int locked,
			unsigned int latched, int base_group,
			int locked_group, int latched_group,
			int effective_group)
{
  seat->base = base;
  seat->locked = locked;
  seat->latched = latched;
  seat->base_group = base_group;
  seat->locked_group = locked_group;
  seat->latched_group = latched_group;
  seat->effective_group = effective_group;

  SendUpdatedModifiers (seat);
}

static void
SetFocusSurface (Seat *seat, Surface *focus)
{
  if (focus == seat->focus_surface)
    return;

  if (seat->focus_surface)
    {
      SendKeyboardLeave (seat, seat->focus_surface);

      /* Tell the surface it's no longer focused.  */
      XLSurfaceNoteFocus (seat->focus_surface, SurfaceFocusOut);

      /* Cancel any grab that may be associated with shortcut
	 inhibition.  */
      XLReleaseShortcutInhibition (seat, seat->focus_surface);

      XLSurfaceCancelRunOnFree (seat->focus_destroy_callback);
      seat->focus_destroy_callback = NULL;
      seat->focus_surface = NULL;

      if (input_funcs)
	/* Tell any input method about the change.  */
	input_funcs->focus_out (seat);
    }

  if (!focus)
    {
      /* These changes must be handled even if there is no more focus
	 surface.  */
      XLPrimarySelectionHandleFocusChange (seat);
      return;
    }

  /* Apply any shortcut inhibition.  */
  XLCheckShortcutInhibition (seat, focus);

  if (input_funcs)
    /* Tell any input method about the change.  */
    input_funcs->focus_in (seat, focus);

  seat->focus_surface = focus;
  seat->focus_destroy_callback
    = XLSurfaceRunOnFree (focus, ClearFocusSurface, seat);

  SendKeyboardEnter (seat, focus);

  /* Tell the surface it's now focused.  */
  XLSurfaceNoteFocus (seat->focus_surface, SurfaceFocusIn);

  XLPrimarySelectionHandleFocusChange (seat);

  if (seat->data_device)
    XLDataDeviceHandleFocusChange (seat->data_device);
}

static void
DispatchFocusIn (Surface *surface, XIFocusInEvent *event)
{
  Seat *seat;

  seat = XLLookUpAssoc (seats, event->deviceid);

  if (!seat)
    return;

  /* Record the time the focus changed for the external grab.  */

  if (!event->send_event)
    seat->last_focus_time = TimestampFromServerTime (event->time);

  SetFocusSurface (seat, surface);
}

static void
DispatchFocusOut (Surface *surface, XIFocusOutEvent *event)
{
  Seat *seat;

  seat = XLLookUpAssoc (seats, event->deviceid);

  if (!seat)
    return;

  if (seat->focus_surface == surface)
    SetFocusSurface (seat, NULL);
}

static Surface *
FindSurfaceUnder (Subcompositor *subcompositor, double x, double y)
{
  int x_off, y_off;
  View *view;

  /* Do not round these figures.  Instead, cut off the fractional,
     like the X server does when deciding when to set the cursor.  */
  view = SubcompositorLookupView (subcompositor, x, y,
				  &x_off, &y_off);

  if (view)
    return ViewGetData (view);

  return NULL;
}

/* Forward declaration.  */

static void CancelDrag (Seat *, Window, double, double);

static void
DragLeave (Seat *seat)
{
  if (seat->drag_last_surface)
    {
      if (seat->flags & IsDragging)
	XLDataDeviceSendLeave (seat, seat->drag_last_surface,
			       seat->data_source);
      else
	/* If nothing is being dragged anymore, avoid sending flags to
	   the source after drop or cancel.  */
	XLDataDeviceSendLeave (seat, seat->drag_last_surface,
			       NULL);

      XLSurfaceCancelRunOnFree (seat->drag_last_surface_destroy_callback);

      seat->drag_last_surface_destroy_callback = NULL;
      seat->drag_last_surface = NULL;
    }
}

static void
HandleDragLastSurfaceDestroy (void *data)
{
  Seat *seat;

  seat = data;

  /* Unfortunately there's no way to send a leave message to the
     client, as the surface's resource no longer exists.  Oh well.  */

  seat->drag_last_surface = NULL;
  seat->drag_last_surface_destroy_callback = NULL;
}

static void
DragEnter (Seat *seat, Surface *surface, double x, double y)
{
  if (seat->drag_last_surface)
    DragLeave (seat);

  /* If no data source is specified, only send motion events to
     surfaces created by the same client.  */
  if (!seat->data_source
      && (wl_resource_get_client (seat->drag_start_surface->resource)
	  != wl_resource_get_client (surface->resource)))
    return;

  seat->drag_last_surface = surface;
  seat->drag_last_surface_destroy_callback
    = XLSurfaceRunOnFree (surface, HandleDragLastSurfaceDestroy,
			  seat);

  XLDataDeviceSendEnter (seat, surface, x, y, seat->data_source);
}

static void
DragMotion (Seat *seat, Surface *surface, double x, double y,
	    Time time)
{
  if (!seat->drag_last_surface)
    return;

  if (surface != seat->drag_last_surface)
    return;

  XLDataDeviceSendMotion (seat, surface, x, y, time);
}

static int
MaskPopCount (XIButtonState *mask)
{
  int population, i;

  population = 0;

  for (i = 0; i < mask->mask_len; ++i)
    population += Popcount (mask->mask[i]);

  return population;
}

static void
DragButton (Seat *seat, XIDeviceEvent *xev)
{
  if (xev->evtype != XI_ButtonRelease)
    return;

  /* If a button release event is received with only 1 button
     remaining, then the drag is complete; send the drop.  */
  if (MaskPopCount (&xev->buttons) == 1)
    {
      /* Drop on any external drag and drop that may be in
	 progress.  */
      if (seat->data_source && XLDoDragDrop (seat))
	XLDataSourceSendDropPerformed (seat->data_source);
      else
	{
	  if (seat->drag_last_surface)
	    {
	      if (!seat->data_source
		  || XLDataSourceCanDrop (seat->data_source))
		{
		  XLDataDeviceSendDrop (seat, seat->drag_last_surface);
		  XLDataSourceSendDropPerformed (seat->data_source);
		}
	      else
		/* Otherwise, the data source is not eligible for
		   dropping; simply send cancel.  */
		XLDataSourceSendDropCancelled (seat->data_source);
	    }
	  else if (seat->data_source)
	    XLDataSourceSendDropCancelled (seat->data_source);
	}

      /* This means that CancelDrag will not send the drop cancelled
	 event to the data source again.  */
      seat->flags |= IsDropped;

      CancelDrag (seat, xev->event, xev->event_x,
		  xev->event_y);
    }
}

static void
SendMotion (Seat *seat, Surface *surface, double x, double y,
	    Time time)
{
  Pointer *pointer;
  uint32_t serial;
  SeatClientInfo *info;

  serial = wl_display_next_serial (compositor.wl_display);
  info = ClientInfoForResource (seat, surface->resource);

  if (!info)
    return;

  pointer = info->pointers.next;

  for (; pointer != &info->pointers; pointer = pointer->next)
    {
      if (pointer->state & StateIsRaw)
	{
	  wl_pointer_send_enter (pointer->resource, serial,
				 surface->resource,
				 wl_fixed_from_double (x),
				 wl_fixed_from_double (y));
	  pointer->info->last_enter_serial = serial;
	}

      /* If the seat is locked, don't send any motion events at
	 all.  */

      if (!(seat->flags & IsPointerLocked))
	wl_pointer_send_motion (pointer->resource, time,
				wl_fixed_from_double (x),
				wl_fixed_from_double (y));

      if (wl_resource_get_version (pointer->resource) >= 5)
	wl_pointer_send_frame (pointer->resource);

      pointer->state &= ~StateIsRaw;
    }
}

static void
SendRelativeMotion (Seat *seat, Surface *surface, double dx, double dy,
		    Time time)
{
  SeatClientInfo *info;
  uint64_t microsecond_time;
  RelativePointer *relative_pointer;

  /* Unfortunately there is no way to determine whether or not a
     valuator specified in a raw event really corresponds to pointer
     motion, so we can't get unaccelerated deltas.  It may be worth
     wiring up raw events for the X.org server, since we do know how
     it specifically behaves.  */

  info = ClientInfoForResource (seat, surface->resource);

  if (!info)
    return;

  /* Hmm... */
  microsecond_time = time * 1000;

  relative_pointer = info->relative_pointers.next;
  while (relative_pointer != &info->relative_pointers)
    {
      /* Send the relative deltas.  */
      XLRelativePointerSendRelativeMotion (relative_pointer->resource,
					   microsecond_time, dx, dy);

      /* Move to the next relative pointer.  */
      relative_pointer = relative_pointer->next;
    }
}

static void
SendLeave (Seat *seat, Surface *surface)
{
  Pointer *pointer;
  uint32_t serial;
  SeatClientInfo *info;

  serial = wl_display_next_serial (compositor.wl_display);
  info = ClientInfoForResource (seat, surface->resource);

  if (!info)
    return;

  pointer = info->pointers.next;

  for (; pointer != &info->pointers; pointer = pointer->next)
    {
      wl_pointer_send_leave (pointer->resource, serial,
			     surface->resource);

      /* Apparently this is necessary on both leave and enter
	 events.  */
      if (wl_resource_get_version (pointer->resource) >= 5)
	wl_pointer_send_frame (pointer->resource);
    }
}

static Bool
SendEnter (Seat *seat, Surface *surface, double x, double y)
{
  Pointer *pointer;
  uint32_t serial;
  Bool sent;
  SeatClientInfo *info;

  serial = wl_display_next_serial (compositor.wl_display);
  sent = False;
  info = ClientInfoForResource (seat, surface->resource);

  if (!info)
    return False;

  pointer = info->pointers.next;

  if (pointer != &info->pointers)
    /* If no pointer devices have been created, don't set the
       serial.  */
    info->last_enter_serial = serial;

  for (; pointer != &info->pointers; pointer = pointer->next)
    {
      pointer->state &= ~StateIsRaw;

      wl_pointer_send_enter (pointer->resource, serial,
			     surface->resource,
			     wl_fixed_from_double (x),
			     wl_fixed_from_double (y));

      /* Apparently this is necessary on both leave and enter
	 events.  */
      if (wl_resource_get_version (pointer->resource) >= 5)
	wl_pointer_send_frame (pointer->resource);

      sent = True;
    }

  return sent;
}

static void
SendButton (Seat *seat, Surface *surface, Time time,
	    uint32_t button, uint32_t state, double x,
	    double y)
{
  Pointer *pointer;
  uint32_t serial;
  SeatClientInfo *info;

  serial = wl_display_next_serial (compositor.wl_display);

  /* This is later used to track the seat for resize operations.  */
  seat->last_button_serial = serial;

  if (state == WL_POINTER_BUTTON_STATE_PRESSED)
    /* This is used for popup grabs.  */
    seat->last_button_press_serial = serial;

  info = ClientInfoForResource (seat, surface->resource);

  if (!info)
    return;

  pointer = info->pointers.next;

  for (; pointer != &info->pointers; pointer = pointer->next)
    {
      if (pointer->state & StateIsRaw)
	{
	  wl_pointer_send_enter (pointer->resource, serial,
				 surface->resource,
				 wl_fixed_from_double (x),
				 wl_fixed_from_double (y));
	  pointer->info->last_enter_serial = serial;
	}

      wl_pointer_send_button (pointer->resource,
			      serial, time, button, state);

      if (wl_resource_get_version (pointer->resource) >= 5)
	wl_pointer_send_frame (pointer->resource);

      pointer->state &= ~StateIsRaw;
    }
}

static void
ClearGrabSurface (void *data)
{
  Seat *seat;

  seat = data;

  /* Cancel the unmap callback.  */
  XLSurfaceCancelUnmapCallback (seat->grab_surface_callback);

  seat->grab_surface = NULL;
  seat->grab_surface_callback = NULL;
}

static void
SwapGrabSurface (Seat *seat, Surface *surface)
{
  if (seat->grab_surface == surface)
    return;

  if (seat->grab_surface)
    {
      XLSurfaceCancelUnmapCallback (seat->grab_surface_callback);
      seat->grab_surface = NULL;
      seat->grab_surface_callback = NULL;
    }

  if (surface)
    {
      seat->grab_surface = surface;
      seat->grab_surface_callback
	= XLSurfaceRunAtUnmap (surface, ClearGrabSurface, seat);
    }
}

static void
ClearLastSeenSurface (void *data)
{
  Seat *seat;

  seat = data;

  /* The surface underneath the pointer was destroyed, so clear the
     cursor.  */
  if (seat->cursor)
    FreeCursor (seat->cursor);

  seat->last_seen_surface = NULL;
  seat->last_seen_surface_callback = NULL;
}

static void
UndefineCursorOn (Seat *seat, Surface *surface)
{
  Window window;

  window = XLWindowFromSurface (surface);

  if (window == None)
    return;

  XIUndefineCursor (compositor.display,
		    seat->master_pointer,
		    window);

  /* In addition to undefining the seat specific cursor, also undefine
     the core cursor specified during window creation.  */
  XUndefineCursor (compositor.display, window);
}

/* Forward declaration.  */

static void SendGesturePinchEnd (Seat *, Surface *, Time, int);
static void SendGestureSwipeEnd (Seat *, Surface *, Time, int);

static void
EnteredSurface (Seat *seat, Surface *surface, Time time,
		double x, double y, Bool preserve_cursor)
{
  Time gesture_time;

  if (seat->grab_held && surface != seat->last_seen_surface)
    /* If the seat is grabbed, delay this for later.  */
    return;

  if (seat->last_seen_surface == surface)
    return;

  /* The surface currently entered changed (or will change).  Cancel
     any ongoing gestures.  */

  if (seat->flags & IsInPinchGesture
      /* Not sure if this can actually be NULL here.  */
      && seat->last_seen_surface)
    {
      /* If time is 0 (CurrentTime), then just use the last user
	 time.  */
      gesture_time = time ? time : seat->last_user_time.milliseconds;

      /* Send the gesture end event.  */
      SendGesturePinchEnd (seat, seat->last_seen_surface,
			   gesture_time, 1);

      /* And clear the flag so further updates are not sent.  */
      seat->flags &= ~IsInPinchGesture;
    }

  if (seat->flags & IsInSwipeGesture
      /* Not sure if this can actually be NULL here.  */
      && seat->last_seen_surface)
    {
      /* If time is 0 (CurrentTime), then just use the last user
	 time.  */
      gesture_time = time ? time : seat->last_user_time.milliseconds;

      /* Send the gesture end event.  */
      SendGestureSwipeEnd (seat, seat->last_seen_surface,
			   gesture_time, 1);

      /* And clear the flag so further updates are not sent.  */
      seat->flags &= ~IsInSwipeGesture;
    }

  if (seat->last_seen_surface)
    {
      if (seat->flags & IsDragging)
	DragLeave (seat);
      else
	{
	  SendLeave (seat, seat->last_seen_surface);

	  /* The surface underneath the pointer was destroyed, so
	     clear the cursor.  */
	  if (seat->cursor && !preserve_cursor)
	    FreeCursor (seat->cursor);
	}

      /* Cancel any pointer confinement.  */
      XLPointerBarrierLeft (seat, seat->last_seen_surface);

      /* Clear the surface.  */
      XLSurfaceCancelRunOnFree (seat->last_seen_surface_callback);
      seat->last_seen_surface = NULL;
      seat->last_seen_surface_callback = NULL;

      /* Mark the last surface motion coords as no longer set.  */
      seat->flags &= ~IsSurfaceCoordSet;
    }

  if (surface)
    {
      seat->last_seen_surface = surface;
      seat->last_seen_surface_callback
	= XLSurfaceRunOnFree (surface, ClearLastSeenSurface, seat);
      seat->last_surface_x = x;
      seat->last_surface_y = y;

      if (seat->flags & IsDragging)
	DragEnter (seat, surface, x, y);
      else if (!SendEnter (seat, surface, x, y))
	/* Apparently what is done by other compositors when no
	   wl_pointer object exists for the surface's client is to
	   revert back to the default cursor.  */
	UndefineCursorOn (seat, surface);
    }
}

static void
TransformToSurface (Surface *surface, double event_x, double event_y,
		    double *view_x_out, double *view_y_out)
{
  int int_x, int_y, x, y;
  double view_x, view_y;
  View *view;

  /* Use the surface's view.  */
  view = surface->view;

  /* Even though event_x and event_y are doubles, they cannot exceed
     65535.0, so this cannot overflow.  */
  int_x = (int) event_x;
  int_y = (int) event_y;

  ViewTranslate (view, int_x, int_y, &x, &y);

  /* Add the fractional part back to the final result.  */
  view_x = ((double) x) + event_x - int_x;
  view_y = ((double) y) + event_y - int_y;

  /* Finally, transform the coordinates by the global output
     scale.  */
  *view_x_out = view_x / surface->factor;
  *view_y_out = view_y / surface->factor;
}

static Bool
CanDeliverEvents (Seat *seat, Surface *dispatch)
{
  if (!seat->grab_surface)
    return True;

  /* Otherwise, an owner-events grab is in effect; only dispatch
     events to the client who owns the grab.  */
  return (wl_resource_get_client (dispatch->resource)
	  == wl_resource_get_client (seat->grab_surface->resource));
}

static void
TranslateCoordinates (Window source, Window target, double x, double y,
		      double *x_out, double *y_out)
{
  Window child_return;
  int int_x, int_y, t1, t2;

  int_x = (int) x;
  int_y = (int) y;

  XTranslateCoordinates (compositor.display, source,
			 target, int_x, int_y, &t1, &t2,
			 &child_return);

  /* Add the fractional part back.  */
  *x_out = (x - int_x) + t1;
  *y_out = (y - int_y) + t2;
}

static Surface *
ComputeGrabPosition (Seat *seat, Surface *dispatch,
		     double *event_x, double *event_y)
{
  Window toplevel, grab;

  toplevel = XLWindowFromSurface (dispatch);
  grab = XLWindowFromSurface (seat->grab_surface);

  TranslateCoordinates (toplevel, grab, *event_x, *event_y,
			event_x, event_y);
  return seat->grab_surface;
}

static void
TranslateGrabPosition (Seat *seat, Window window, double *event_x,
		       double *event_y)
{
  Window grab;

  grab = XLWindowFromSurface (seat->grab_surface);

  TranslateCoordinates (window, grab, *event_x, *event_y,
			event_x, event_y);
  return;
}

static void
HandleSubcompositorDestroy (void *data)
{
  Seat *seat;

  seat = data;
  seat->last_seen_subcompositor = NULL;
  seat->subcompositor_callback = NULL;
}

static void
DispatchEntryExit (Subcompositor *subcompositor, XIEnterEvent *event)
{
  Seat *seat;
  Surface *dispatch;
  double x, y, event_x, event_y;

  seat = XLLookUpAssoc (seats, event->deviceid);

  if (!seat)
    return;

  if (event->mode != XINotifyGrab
      && event->mode != XINotifyUngrab)
    {
      /* This is not an event generated by grab activation or
	 deactivation.  Set the last seen subcompositor, or clear it
	 on XI_Leave.  The last seen subcompositor is used to
	 determine the surface to which a grab will be released.  */

      if (event->evtype == XI_Leave
	  || subcompositor != seat->last_seen_subcompositor)
	{
	  if (seat->last_seen_subcompositor)
	    SubcompositorRemoveDestroyCallback (seat->subcompositor_callback);

	  seat->last_seen_subcompositor = NULL;
	  seat->subcompositor_callback = NULL;

	  if (event->evtype == XI_Enter)
	    {
	      /* Attach the new subcompositor.  */
	      seat->last_seen_subcompositor = subcompositor;
	      seat->subcompositor_callback
		= SubcompositorOnDestroy (subcompositor,
					  HandleSubcompositorDestroy,
					  seat);

	      /* Also set the window used.  */
	      seat->last_seen_subcompositor_window = event->event;
	    }
	}
    }

  if (event->mode == XINotifyUngrab
      && seat->grab_surface)
    /* Any explicit grab was released, so release the grab surface as
       well.  */
    SwapGrabSurface (seat, NULL);

  if (event->mode == XINotifyUngrab
      && seat->flags & IsDragging)
    /* The active grab was released.  */
    CancelDrag (seat, event->event, event->event_x,
		event->event_y);

  if (event->evtype == XI_Leave
      && (event->mode == XINotifyGrab
	  || event->mode == XINotifyUngrab))
    /* Ignore grab-related weirdness in XI_Leave events.  */
    return;

  if (event->evtype == XI_Enter
      && event->mode == XINotifyGrab)
    /* Accepting entry events with XINotifyGrab leads to bad results
       when they arrive on a popup that has just been grabbed.  */
    return;

  seat->flags &= ~IsWindowMenuShown;
  seat->last_crossing_serial = event->serial;

  if (event->evtype == XI_Leave)
    dispatch = NULL;
  else
    dispatch = FindSurfaceUnder (subcompositor, event->event_x,
				 event->event_y);

  event_x = event->event_x;
  event_y = event->event_y;

  if (seat->grab_surface)
    {
      /* If the grab surface is set, translate the coordinates to
	 it and use it instead.  */
      TranslateGrabPosition (seat, event->event,
			     &event_x, &event_y);
      dispatch = seat->grab_surface;

      goto after_dispatch_set;
    }

  if (!dispatch)
    EnteredSurface (seat, NULL, event->time, 0, 0, False);
  else
    {
      /* If dispatching during an active grab, and the event is for
	 the wrong client, translate the coordinates to the grab
	 window.  */
      if (!CanDeliverEvents (seat, dispatch))
	dispatch = ComputeGrabPosition (seat, dispatch,
					&event_x, &event_y);

    after_dispatch_set:

      TransformToSurface (dispatch, event_x, event_y, &x, &y);
      EnteredSurface (seat, dispatch, event->time, x, y, False);

      /* If this entry event was the result of a grab, also send
	 motion now, as a corresponding XI_Motion will not arrive
	 later.  */
      if (event->mode == XINotifyUngrab)
	SendMotion (seat, dispatch, x, y, event->time);
    }

  seat->last_motion_x = event->root_x;
  seat->last_motion_y = event->root_y;
}

static Bool
ProcessValuator (Seat *seat, XIDeviceEvent *event, ScrollValuator *valuator,
		 double value, double *total_x, double *total_y, int *flags)
{
  double diff;
  Bool valid;

  valid = False;

  if (seat->last_crossing_serial > valuator->enter_serial)
    /* The valuator is out of date.  Set its serial, value, and
       return.  */
    goto out;

  diff = value - valuator->value;

  if (valuator->direction == Horizontal)
    *total_x += diff / valuator->increment;
  else
    *total_y += diff / valuator->increment;

  if (valuator->direction == Horizontal)
    *flags |= AnyVerticalAxis;
  else
    *flags |= AnyHorizontalAxis;

  valid = True;

 out:
  valuator->value = value;
  valuator->enter_serial = event->serial;

  return valid;
}

static ScrollValuator *
FindScrollValuator (Seat *seat, int number)
{
  ScrollValuator *valuator;

  valuator = seat->valuators;

  for (; valuator; valuator = valuator->next)
    {
      if (valuator->number == number)
	return valuator;
    }

  return NULL;
}

static void
InterpolateAxes (Surface *surface, DeviceInfo *info,
		 double movement_x, double movement_y,
		 double *x_out, double *y_out)
{
  if (!info)
    {
      /* Multiply the deltas by 15 if no device was found.  */
      *x_out = movement_x * 15;
      *y_out = movement_y * 15;

      return;
    }

  /* Multiply these deltas by the scrolling pixel distance to obtain
     the original delta.  */
  *x_out = movement_x * info->scroll_pixel_distance;
  *y_out = movement_y * info->scroll_pixel_distance;
}

static void
SendScrollAxis (Seat *seat, Surface *surface, Time time,
		double x, double y, double axis_x, double axis_y,
		int flags, int sourceid)
{
  Pointer *pointer;
  uint32_t serial;
  SeatClientInfo *info;
  DeviceInfo *deviceinfo;

  serial = wl_display_next_serial (compositor.wl_display);
  info = ClientInfoForResource (seat, surface->resource);

  if (!info)
    return;

  pointer = info->pointers.next;
  deviceinfo = XLLookUpAssoc (devices, sourceid);

  for (; pointer != &info->pointers; pointer = pointer->next)
    {
      if (pointer->state & StateIsRaw)
	{
	  wl_pointer_send_enter (pointer->resource, serial,
				 surface->resource,
				 wl_fixed_from_double (x),
				 wl_fixed_from_double (y));
	  pointer->info->last_enter_serial = serial;
	}

      if (wl_resource_get_version (pointer->resource) < 8
	  /* Send pixel-wise axis events from devices that are most
	     likely touchpads.  */
	  || (deviceinfo
	      && (deviceinfo->flags & DeviceCanFingerScroll
		  || deviceinfo->flags & DeviceCanEdgeScroll)))
	{
	  /* Interpolate the increment-relative axis values to pixel
	     values.  */
	  InterpolateAxes (surface, deviceinfo, axis_x, axis_y,
			   &axis_x, &axis_y);

	  if (axis_x != 0.0)
	    wl_pointer_send_axis (pointer->resource, time,
				  WL_POINTER_AXIS_HORIZONTAL_SCROLL,
				  wl_fixed_from_double (axis_x));

	  if (axis_y != 0.0)
	    wl_pointer_send_axis (pointer->resource, time,
				  WL_POINTER_AXIS_VERTICAL_SCROLL,
				  wl_fixed_from_double (axis_y));
	}
      else
        {
	  /* Send value120 events if they are available; those events
	     make more sense.  */

	  if (axis_x != 0.0)
	    wl_pointer_send_axis_value120 (pointer->resource,
					   WL_POINTER_AXIS_HORIZONTAL_SCROLL,
					   axis_x * 120);

	  if (axis_y != 0.0)
	    wl_pointer_send_axis_value120 (pointer->resource,
					   WL_POINTER_AXIS_VERTICAL_SCROLL,
					   axis_y * 120);
	}

      if (axis_y == 0.0 && axis_x == 0.0)
	{
	  /* This behavior is specific to a few X device
	     drivers! */

	  if (wl_resource_get_version (pointer->resource) >= 5)
	    {
	      /* wl_pointer_send_axis_stop is only present on
		 version 5 or later.  */

	      if (flags & AnyVerticalAxis)
		wl_pointer_send_axis_stop (pointer->resource, time,
					   WL_POINTER_AXIS_VERTICAL_SCROLL);

	      if (flags & AnyHorizontalAxis)
		wl_pointer_send_axis_stop (pointer->resource, time,
					   WL_POINTER_AXIS_HORIZONTAL_SCROLL);
	    }
	}

      if (wl_resource_get_version (pointer->resource) >= 5)
	{
	  /* Send the source of this axis movement if it can be
	     determined.  We assume that axis movement from any
	     device capable finger or edge scrolling comes from a
	     touchpad.  */

	  if (deviceinfo
	      && (deviceinfo->flags & DeviceCanFingerScroll
		  || deviceinfo->flags & DeviceCanEdgeScroll))
	    wl_pointer_send_axis_source (pointer->resource,
					 WL_POINTER_AXIS_SOURCE_FINGER);
	}

      if (axis_x != 0.0 || axis_y != 0.0
	  || flags || pointer->state & StateIsRaw)
	{
	  if (wl_resource_get_version (pointer->resource) >= 5)
	    wl_pointer_send_frame (pointer->resource);
	}

      pointer->state &= ~StateIsRaw;
    }
}

static Bool
HandleValuatorMotion (Seat *seat, Surface *dispatch, double x, double y,
		      XIDeviceEvent *event)
{
  double total_x, total_y, *values;
  ScrollValuator *valuator;
  int i, flags;
  Bool value;

  total_x = 0.0;
  total_y = 0.0;
  value = False;
  values = event->valuators.values;
  flags = 0;

  for (i = 0; i < event->valuators.mask_len * 8; ++i)
    {
      if (!XIMaskIsSet (event->valuators.mask, i))
	continue;

      valuator = FindScrollValuator (seat, i);

      if (!valuator)
	/* We still have to increment values even if we don't know
	   about the valuator in question.  */
	goto next;

      value |= ProcessValuator (seat, event, valuator, *values,
				&total_x, &total_y, &flags);

    next:
      values++;
    }

  if (value && dispatch)
    SendScrollAxis (seat, dispatch, event->time, x, y, total_x,
		    total_y, flags,
		    /* Also pass the event source device ID, which is
		       used in an attempt to determine the axis
		       source.  */
		    event->sourceid);
  return value;
}

static void
CheckPointerBarrier (Seat *seat, Surface *dispatch, double x, double y,
		     double root_x, double root_y)
{
  /* Check if DISPATCH has a pointer confinement that would be
     activated by this motion.  */

  XLPointerBarrierCheck (seat, dispatch, x, y, root_x, root_y);
}

static void
DispatchMotion (Subcompositor *subcompositor, XIDeviceEvent *xev)
{
  Seat *seat;
  Surface *dispatch, *actual_dispatch;
  double x, y, event_x, event_y;

  seat = XLLookUpAssoc (seats, xev->deviceid);

  if (!seat)
    return;

  if (InterceptResizeEvent (seat, subcompositor, xev))
    return;

  /* Move the drag-and-drop icon window.  */
  if (seat->icon_surface)
    XLMoveIconSurface (seat->icon_surface, xev->root_x,
		       xev->root_y);

  /* Update information used for resize tracking.  */
  seat->its_root_x = xev->root_x;
  seat->its_root_y = xev->root_y;
  seat->its_press_time = xev->time;

  /* Update the last user time.  */

  if (!xev->send_event)
    seat->last_user_time = TimestampFromServerTime (xev->time);

  actual_dispatch = FindSurfaceUnder (subcompositor, xev->event_x,
				      xev->event_y);

  if (seat->grab_held)
    dispatch = seat->last_seen_surface;
  else
    dispatch = actual_dispatch;

  event_x = xev->event_x;
  event_y = xev->event_y;

  if (!dispatch)
    {
      if (seat->grab_surface)
	{
	  /* If the grab surface is set, translate the coordinates to
	     it and use it instead.  */
	  TranslateGrabPosition (seat, xev->event,
				 &event_x, &event_y);
	  dispatch = seat->grab_surface;

	  goto after_dispatch_set;
	}

      EnteredSurface (seat, dispatch, xev->time, 0, 0, False);

      /* If drag and drop is in progress, handle "external" drag and
	 drop.  */
      if (seat->flags & IsDragging
	  && seat->data_source)
	XLDoDragMotion (seat, xev->root_x, xev->root_y);

      return;
    }

  /* Update the outputs the pointer surface is currently displayed
     inside, since it evidently moved.  */
  if (seat->cursor)
    UpdateCursorOutput (seat->cursor, xev->root_x,
			xev->root_y);

  /* If dispatching during an active grab, and the event is for the
     wrong client, translate the coordinates to the grab window.  */
  if (!CanDeliverEvents (seat, dispatch))
    dispatch = ComputeGrabPosition (seat, dispatch,
				    &event_x, &event_y);

 after_dispatch_set:

  if (seat->flags & IsDragging
      && seat->data_source)
    /* Inside a surface; cancel external drag and drop.  */
    XLDoDragLeave (seat);

  TransformToSurface (dispatch, event_x, event_y, &x, &y);
  EnteredSurface (seat, dispatch, xev->time, x, y, False);

  if (!HandleValuatorMotion (seat, dispatch, x, y, xev))
    {
      if (seat->flags & IsDragging)
	DragMotion (seat, dispatch, x, y, xev->time);
      else
	{
	  /* Send the motion event.  */
	  SendMotion (seat, dispatch, x, y, xev->time);

	  /* Send relative motion.  Relative motion is handled by
	     subtracting x and y from seat->last_surface_x and
	     seat->last_surface_y, unless pointer motion reporting is
	     locked, in which case XI barrier motion events are used
	     instead.  */
	  if (x - seat->last_surface_x != 0.0
	      || y - seat->last_surface_y != 0.0)
	    SendRelativeMotion (seat, dispatch, x - seat->last_surface_x,
				y - seat->last_surface_y, xev->time);

	  /* Check if this motion would cause a pointer constraint to
	     activate.  */
	  CheckPointerBarrier (seat, dispatch, x, y, xev->root_x,
			       xev->root_y);
	}

      /* Set the last movement location.  */
      seat->last_surface_x = x;
      seat->last_surface_y = y;
      seat->flags |= IsSurfaceCoordSet;
    }

  /* These values are for tracking the output that a cursor is in.  */

  seat->last_motion_x = xev->root_x;
  seat->last_motion_y = xev->root_y;
}

static int
GetXButton (int detail)
{
  switch (detail)
    {
    case Button1:
      return BTN_LEFT;

    case Button2:
      return BTN_MIDDLE;

    case Button3:
      return BTN_RIGHT;

    default:
      return -1;
    }
}

static void
CancelGrab1 (Seat *seat, Subcompositor *subcompositor,
	     Time time, double x, double y)
{
  Surface *surface;

  /* Look up the surface under subcompositor at x, y and enter it, in
     response to implicit grab termination.  */

  surface = FindSurfaceUnder (subcompositor, x, y);

  if (surface)
    TransformToSurface (surface, x, y, &x, &y);

  EnteredSurface (seat, surface, time, x, y, False);
}

static void
CancelGrab (Seat *seat, Time time, Window source, double x,
	    double y)
{
  if (!seat->grab_held)
    return;

  if (--seat->grab_held)
    return;

  /* More or less how this works: translate x and y from source to
     last_seen_subcompositor_window, and look up the surface in the
     last_seen_subcompositor, if present.  */
  if (seat->last_seen_subcompositor)
    {
      /* Avoid translating coordinates if not necessary.  */
      if (source != seat->last_seen_subcompositor_window)
	TranslateCoordinates (source, seat->last_seen_subcompositor_window,
			      x, y, &x, &y);

      /* And cancel the grab.  */
      CancelGrab1 (seat, seat->last_seen_subcompositor, time, x, y);
    }
  else
    /* Otherwise, just leave the surface.  */
    EnteredSurface (seat, NULL, time, 0, 0, False);

  /* Cancel the unmap callback.  */
  XLSurfaceCancelUnmapCallback (seat->grab_unmap_callback);
  seat->grab_unmap_callback = NULL;
}

static void
CancelGrabEarly (Seat *seat)
{
  /* Do this to make sure the grab is immediately canceled.  */
  seat->grab_held = 1;

  /* Cancelling the grab should also result in the unmap callback
     being cancelled.  */
  CancelGrab (seat, seat->its_press_time,
	      DefaultRootWindow (compositor.display),
	      seat->its_root_x, seat->its_root_y);
}

static void
HandleGrabUnmapped (void *data)
{
  CancelGrabEarly (data);
}

static void
LockSurfaceFocus (Seat *seat)
{
  UnmapCallback *callback;

  /* As long as an active grab is held, ignore the passive grab.  */
  if (seat->grab_surface)
    return;

  seat->grab_held++;

  if (seat->grab_held == 1)
    {
      /* Also cancel the grab upon the surface being unmapped.  */
      callback = XLSurfaceRunAtUnmap (seat->last_seen_surface,
				      HandleGrabUnmapped, seat);
      seat->grab_unmap_callback = callback;
    }
}

static void
ClearLastButtonPressSurface (void *data)
{
  Seat *seat;

  seat = data;

  seat->last_button_press_surface = NULL;
  seat->last_button_press_surface_callback = NULL;
}

static void
SetButtonSurface (Seat *seat, Surface *surface)
{
  DestroyCallback *callback;

  if (surface == seat->last_button_press_surface)
    return;

  callback = seat->last_button_press_surface_callback;

  if (seat->last_button_press_surface)
    {
      XLSurfaceCancelRunOnFree (callback);
      seat->last_button_press_surface_callback = NULL;
      seat->last_button_press_surface = NULL;
    }

  if (!surface)
    return;

  seat->last_button_press_surface = surface;
  seat->last_button_press_surface_callback
    = XLSurfaceRunOnFree (surface, ClearLastButtonPressSurface, seat);
}

static void
DispatchButton (Subcompositor *subcompositor, XIDeviceEvent *xev)
{
  Seat *seat;
  Surface *dispatch, *actual_dispatch;
  double x, y, event_x, event_y;
  int button;
  uint32_t state;

  if (xev->flags & XIPointerEmulated)
    return;

  seat = XLLookUpAssoc (seats, xev->deviceid);

  if (!seat)
    return;

  if (InterceptResizeEvent (seat, subcompositor, xev))
    return;

  if (seat->flags & IsDragging)
    {
      DragButton (seat, xev);
      return;
    }

  button = GetXButton (xev->detail);

  if (button < 0)
    return;

  actual_dispatch = FindSurfaceUnder (subcompositor, xev->event_x,
				      xev->event_y);

  if (seat->grab_held)
    dispatch = seat->last_seen_surface;
  else
    dispatch = actual_dispatch;

  event_x = xev->event_x;
  event_y = xev->event_y;

  if (!dispatch)
    {
      if (seat->grab_surface)
	{
	  /* If the grab surface is set, translate the coordinates to
	     it and use it instead.  */
	  TranslateGrabPosition (seat, xev->event,
				 &event_x, &event_y);
	  dispatch = seat->grab_surface;

	  goto after_dispatch_set;
	}

      EnteredSurface (seat, dispatch, xev->time, 0, 0, False);
      return;
    }

  /* If dispatching during an active grab, and the event is for the
     wrong client, translate the coordinates to the grab window.  */
  if (!CanDeliverEvents (seat, dispatch))
    dispatch = ComputeGrabPosition (seat, dispatch,
				    &event_x, &event_y);

 after_dispatch_set:

  TransformToSurface (dispatch, xev->event_x, xev->event_y,
		      &x, &y);
  EnteredSurface (seat, dispatch, xev->time, x, y,
		  False);

  state = (xev->evtype == XI_ButtonPress
	   ? WL_POINTER_BUTTON_STATE_PRESSED
	   : WL_POINTER_BUTTON_STATE_RELEASED);

  SendButton (seat, dispatch, xev->time, button,
	      state, x, y);

  if (xev->evtype == XI_ButtonPress)
    {
      /* These values are used for resize grip tracking.  */
      seat->its_root_x = lrint (xev->root_x);
      seat->its_root_y = lrint (xev->root_y);
      seat->its_press_time = xev->time;
      seat->last_button = xev->detail;

      SetButtonSurface (seat, dispatch);
    }

  if (xev->evtype == XI_ButtonPress)
    LockSurfaceFocus (seat);
  else
    CancelGrab (seat, xev->time, xev->event,
		xev->event_x, xev->event_y);
}

static void
DispatchKey (XIDeviceEvent *xev)
{
  Seat *seat;
  KeyCode keycode;

  seat = XLLookUpAssoc (seats, xev->deviceid);

  if (!seat)
    return;

  /* Report key state changes here.  A side effect is that the key
     state reported in enter events will include grabbed keys, but
     that seems to be an acceptable tradeoff.  */

  if (seat->focus_surface)
    {
      keycode = 0;

      if (input_funcs
	  && seat->flags & IsTextInputSeat
	  && input_funcs->filter_input (seat, seat->focus_surface,
					xev, &keycode))
	/* The input method decided to filter the key.  */
	return;

      /* Ignore repeated keys.  */
      if (xev->flags & XIKeyRepeat)
	return;

      /* If the input method specified a keycode, use it.  */
      if (!keycode)
	keycode = xev->detail;

      if (xev->evtype == XI_KeyPress)
	SendKeyboardKey (seat, seat->focus_surface,
			 xev->time, WaylandKeycode (keycode),
			 WL_KEYBOARD_KEY_STATE_PRESSED);
      else
	SendKeyboardKey (seat, seat->focus_surface,
			 xev->time, WaylandKeycode (keycode),
			 WL_KEYBOARD_KEY_STATE_RELEASED);
    }
}

static void
DispatchBarrierHit (XIBarrierEvent *barrier)
{
  Seat *seat;

  seat = XLLookUpAssoc (seats, barrier->deviceid);

  if (!seat)
    return;

  /* Report a barrier hit event as relative motion.  */

  if (seat->last_seen_surface)
    SendRelativeMotion (seat, seat->last_seen_surface,
			barrier->dx, barrier->dy,
			barrier->time);

  /* Set the last user time.  */

  if (!barrier->send_event)
    seat->last_user_time = TimestampFromServerTime (barrier->time);
}

static void
SendGesturePinchBegin (Seat *seat, Surface *dispatch, Time time,
		       int detail)
{
  PinchGesture *gesture;
  uint32_t serial;
  SeatClientInfo *info;

  serial = wl_display_next_serial (compositor.wl_display);
  info = ClientInfoForResource (seat, dispatch->resource);

  if (!info)
    return;

  gesture = info->pinch_gestures.next;

  for (; gesture != &info->pinch_gestures; gesture = gesture->next)
    zwp_pointer_gesture_pinch_v1_send_begin (gesture->resource,
					     serial, time,
					     dispatch->resource,
					     detail);
}

static void
SendGesturePinchUpdate (Seat *seat, Surface *dispatch, Time time,
			double dx, double dy, double scale, double rotation)
{
  PinchGesture *gesture;
  SeatClientInfo *info;

  info = ClientInfoForResource (seat, dispatch->resource);

  if (!info)
    return;

  gesture = info->pinch_gestures.next;

  for (; gesture != &info->pinch_gestures; gesture = gesture->next)
    zwp_pointer_gesture_pinch_v1_send_update (gesture->resource,
					      time,
					      wl_fixed_from_double (dx),
					      wl_fixed_from_double (dy),
					      wl_fixed_from_double (scale),
					      wl_fixed_from_double (rotation));
}

static void
SendGesturePinchEnd (Seat *seat, Surface *dispatch, Time time, int cancelled)
{
  PinchGesture *gesture;
  SeatClientInfo *info;
  uint32_t serial;

  info = ClientInfoForResource (seat, dispatch->resource);

  if (!info)
    return;

  gesture = info->pinch_gestures.next;
  serial = wl_display_next_serial (compositor.wl_display);

  for (; gesture != &info->pinch_gestures; gesture = gesture->next)
    zwp_pointer_gesture_pinch_v1_send_end (gesture->resource,
					   serial, time, cancelled);
}

static void
DispatchGesturePinch (Subcompositor *subcompositor, XIGesturePinchEvent *pinch)
{
  Seat *seat;
  Surface *dispatch, *actual_dispatch;
  double x, y, event_x, event_y;

  seat = XLLookUpAssoc (seats, pinch->deviceid);

  if (!seat)
    return;

  /* Move the icon surface.  */
  if (seat->icon_surface)
    XLMoveIconSurface (seat->icon_surface, pinch->root_x,
		       pinch->root_y);

  /* Update information used for resize tracking.  */
  seat->its_root_x = pinch->root_x;
  seat->its_root_y = pinch->root_y;
  seat->its_press_time = pinch->time;

  /* Update the last user time.  */

  if (!pinch->send_event)
    seat->last_user_time = TimestampFromServerTime (pinch->time);

  /* Now find the dispatch surface so we can enter it if required.
     Most of this code is copied from DispatchMotion; it should
     probably be moved to a separate function.  */
  actual_dispatch = FindSurfaceUnder (subcompositor, pinch->event_x,
				      pinch->event_y);

  if (seat->grab_held)
    dispatch = seat->last_seen_surface;
  else
    dispatch = actual_dispatch;

  event_x = pinch->event_y;
  event_y = pinch->event_y;

  if (!dispatch)
    {
      if (seat->grab_surface)
	{
	  /* If the grab surface is set, translate the coordinates to
	     it and use it instead.  */
	  TranslateGrabPosition (seat, pinch->event,
				 &event_x, &event_y);
	  dispatch = seat->grab_surface;

	  goto after_dispatch_set;
	}

      EnteredSurface (seat, dispatch, pinch->time, 0, 0, False);
      return;
    }

 after_dispatch_set:
  TransformToSurface (dispatch, event_x, event_y, &x, &y);
  EnteredSurface (seat, dispatch, pinch->time, x, y, False);

  /* Now do the actual event dispatch.  */
  switch (pinch->evtype)
    {
    case XI_GesturePinchBegin:
      /* Send a motion event, in case the position changed.  */
      SendMotion (seat, dispatch, x, y, pinch->time);

      /* Send a begin event.  */
      SendGesturePinchBegin (seat, dispatch, pinch->time, pinch->detail);

      /* Say that the seat is in the middle of a pinch gesture, so it
	 can be cancelled should the pointer move out of this
	 surface.  */
      seat->flags |= IsInPinchGesture;
      break;

    case XI_GesturePinchUpdate:
      /* The gesture sequence was cancelled for some other reason.  */
      if (!(seat->flags & IsInPinchGesture))
	return;

      /* Send an update event.  */
      SendGesturePinchUpdate (seat, dispatch, pinch->time,
			      pinch->delta_x, pinch->delta_y,
			      pinch->scale, pinch->delta_angle);
      break;

    case XI_GesturePinchEnd:
      /* The gesture sequence was cancelled for some other reason.  */
      if (!(seat->flags & IsInPinchGesture))
	return;

      /* Send an end event.  */
      SendGesturePinchEnd (seat, dispatch, pinch->time,
			   pinch->flags & XIGesturePinchEventCancelled);
      break;
    }
}

static void
SendGestureSwipeBegin (Seat *seat, Surface *dispatch, Time time,
		       int detail)
{
  SwipeGesture *gesture;
  uint32_t serial;
  SeatClientInfo *info;

  serial = wl_display_next_serial (compositor.wl_display);
  info = ClientInfoForResource (seat, dispatch->resource);

  if (!info)
    return;

  gesture = info->swipe_gestures.next;

  for (; gesture != &info->swipe_gestures; gesture = gesture->next)
    zwp_pointer_gesture_swipe_v1_send_begin (gesture->resource,
					     serial, time,
					     dispatch->resource,
					     detail);
}

static void
SendGestureSwipeUpdate (Seat *seat, Surface *dispatch, Time time,
			double dx, double dy)
{
  SwipeGesture *gesture;
  SeatClientInfo *info;

  info = ClientInfoForResource (seat, dispatch->resource);

  if (!info)
    return;

  gesture = info->swipe_gestures.next;

  for (; gesture != &info->swipe_gestures; gesture = gesture->next)
    zwp_pointer_gesture_swipe_v1_send_update (gesture->resource,
					      time,
					      wl_fixed_from_double (dx),
					      wl_fixed_from_double (dy));
}

static void
SendGestureSwipeEnd (Seat *seat, Surface *dispatch, Time time, int cancelled)
{
  SwipeGesture *gesture;
  SeatClientInfo *info;
  uint32_t serial;

  info = ClientInfoForResource (seat, dispatch->resource);

  if (!info)
    return;

  gesture = info->swipe_gestures.next;
  serial = wl_display_next_serial (compositor.wl_display);

  for (; gesture != &info->swipe_gestures; gesture = gesture->next)
    zwp_pointer_gesture_swipe_v1_send_end (gesture->resource,
					   serial, time, cancelled);
}

static void
DispatchGestureSwipe (Subcompositor *subcompositor, XIGestureSwipeEvent *swipe)
{
  Seat *seat;
  Surface *dispatch, *actual_dispatch;
  double x, y, event_x, event_y;

  seat = XLLookUpAssoc (seats, swipe->deviceid);

  if (!seat)
    return;

  /* Move the icon surface.  */
  if (seat->icon_surface)
    XLMoveIconSurface (seat->icon_surface, swipe->root_x,
		       swipe->root_y);

  /* Update information used for resize tracking.  */
  seat->its_root_x = swipe->root_x;
  seat->its_root_y = swipe->root_y;
  seat->its_press_time = swipe->time;

  /* Update the last user time.  */

  if (!swipe->send_event)
    seat->last_user_time = TimestampFromServerTime (swipe->time);

  /* Now find the dispatch surface so we can enter it if required.
     Most of this code is copied from DispatchMotion; it should
     probably be moved to a separate function.  */
  actual_dispatch = FindSurfaceUnder (subcompositor, swipe->event_x,
				      swipe->event_y);

  if (seat->grab_held)
    dispatch = seat->last_seen_surface;
  else
    dispatch = actual_dispatch;

  event_x = swipe->event_y;
  event_y = swipe->event_y;

  if (!dispatch)
    {
      if (seat->grab_surface)
	{
	  /* If the grab surface is set, translate the coordinates to
	     it and use it instead.  */
	  TranslateGrabPosition (seat, swipe->event,
				 &event_x, &event_y);
	  dispatch = seat->grab_surface;

	  goto after_dispatch_set;
	}

      EnteredSurface (seat, dispatch, swipe->time, 0, 0, False);
      return;
    }

 after_dispatch_set:
  TransformToSurface (dispatch, event_x, event_y, &x, &y);
  EnteredSurface (seat, dispatch, swipe->time, x, y, False);

  /* Now do the actual event dispatch.  */
  switch (swipe->evtype)
    {
    case XI_GestureSwipeBegin:
      /* Send a motion event, in case the position changed.  */
      SendMotion (seat, dispatch, x, y, swipe->time);

      /* Send a begin event.  */
      SendGestureSwipeBegin (seat, dispatch, swipe->time, swipe->detail);

      /* Say that the seat is in the middle of a swipe gesture, so it
	 can be cancelled should the pointer move out of this
	 surface.  */
      seat->flags |= IsInSwipeGesture;
      break;

    case XI_GestureSwipeUpdate:
      /* The gesture sequence was cancelled for some other reason.  */
      if (!(seat->flags & IsInSwipeGesture))
	return;

      /* Send an update event.  */
      SendGestureSwipeUpdate (seat, dispatch, swipe->time,
			      swipe->delta_x, swipe->delta_y);
      break;

    case XI_GestureSwipeEnd:
      /* The gesture sequence was cancelled for some other reason.  */
      if (!(seat->flags & IsInSwipeGesture))
	return;

      /* Send an end event.  */
      SendGestureSwipeEnd (seat, dispatch, swipe->time,
			   swipe->flags & XIGestureSwipeEventCancelled);
      break;
    }
}

static void
WriteKeymap (void)
{
  FILE *file;
  XkbFileInfo result;
  Bool ok;
  int fd;

  if (keymap_fd != -1)
    close (keymap_fd);

  keymap_fd = XLOpenShm ();

  if (keymap_fd < 0)
    {
      fprintf (stderr, "Failed to allocate keymap fd\n");
      exit (1);
    }

  memset (&result, 0, sizeof result);
  result.type = XkmKeymapFile;
  result.xkb = xkb_desc;

  fd = fcntl (keymap_fd, F_DUPFD_CLOEXEC, 0);

  if (fd < 0)
    {
      perror ("fcntl");
      exit (1);
    }

  file = fdopen (fd, "w");

  if (!file)
    {
      perror ("fdopen");
      exit (1);
    }

  ok = XkbWriteXKBFile (file, &result,
			/* libxkbcommon doesn't read comments in
			   virtual_modifier lines.  */
			False, NULL, NULL);

  if (!ok)
    fprintf (stderr, "Warning: the XKB keymap could not be written\n"
	     "Programs might not continue to interpret keyboard input"
	     " correctly.\n");

  fclose (file);
}

static void
AfterMapUpdate (void)
{
  if (XkbGetIndicatorMap (compositor.display, ~0, xkb_desc) != Success)
    {
      fprintf (stderr, "Could not load indicator map\n");
      exit (1);
    }

  if (XkbGetControls (compositor.display,
		      XkbAllControlsMask, xkb_desc) != Success)
    {
      fprintf (stderr, "Could not load keyboard controls\n");
      exit (1);
    }

  if (XkbGetCompatMap (compositor.display,
		       XkbAllCompatMask, xkb_desc) != Success)
    {
      fprintf (stderr, "Could not load compatibility map\n");
      exit (1);
    }

  if (XkbGetNames (compositor.display,
		   XkbAllNamesMask, xkb_desc) != Success)
    {
      fprintf (stderr, "Could not load names\n");
      exit (1);
    }
}

static void
UpdateKeymapInfo (void)
{
  XLList *tem;
  Seat *seat;
  Keyboard *keyboard;

  for (tem = live_seats; tem; tem = tem->next)
    {
      seat = tem->data;

      if (!seat->key_pressed)
	/* max_keycode is small enough for this to not matter
	   memory-wise.  */
	seat->key_pressed
	  = XLCalloc (MaskLen (xkb_desc->max_key_code
			       - xkb_desc->min_key_code), 1);
      else
	seat->key_pressed
	  = XLRealloc (seat->key_pressed,
		       MaskLen (xkb_desc->max_key_code
				- xkb_desc->min_key_code));

      for (keyboard = seat->keyboards.next1;
	   keyboard != &seat->keyboards;
	   keyboard = keyboard->next1)
	UpdateSingleKeyboard (keyboard);
    }
}

static void
SetupKeymap (void)
{
  int xkb_major, xkb_minor, xkb_op, xkb_error_code;
  xkb_major = XkbMajorVersion;
  xkb_minor = XkbMinorVersion;

  if (!XkbLibraryVersion (&xkb_major, &xkb_minor)
      || !XkbQueryExtension (compositor.display, &xkb_op, &xkb_event_type,
			     &xkb_error_code, &xkb_major, &xkb_minor))
    {
      fprintf (stderr, "Failed to set up Xkb\n");
      exit (1);
    }

  xkb_desc = XkbGetMap (compositor.display,
			XkbAllMapComponentsMask,
			XkbUseCoreKbd);

  if (!xkb_desc)
    {
      fprintf (stderr, "Failed to retrieve keymap from X server\n");
      exit (1);
    }

  AfterMapUpdate ();
  WriteKeymap ();

  XkbSelectEvents (compositor.display, XkbUseCoreKbd,
		   XkbMapNotifyMask | XkbNewKeyboardNotifyMask,
		   XkbMapNotifyMask | XkbNewKeyboardNotifyMask);
  UpdateKeymapInfo ();
}

static Bool
HandleXkbEvent (XkbEvent *event)
{
  Seat *seat;

  if (event->any.xkb_type == XkbMapNotify
      || event->any.xkb_type == XkbNewKeyboardNotify)
    {
      XkbRefreshKeyboardMapping (&event->map);
      XkbFreeKeyboard (xkb_desc, XkbAllMapComponentsMask,
		       True);

      xkb_desc = XkbGetMap (compositor.display,
			    XkbAllMapComponentsMask,
			    XkbUseCoreKbd);

      if (!xkb_desc)
	{
	  fprintf (stderr, "Failed to retrieve keymap from X server\n");
	  exit (1);
	}

      AfterMapUpdate ();
      WriteKeymap ();
      UpdateKeymapInfo ();

      return True;
    }
  else if (event->any.xkb_type == XkbStateNotify)
    {
      /* Look up the seat to which this event corresponds.  */
      seat = XLLookUpAssoc (seats, event->state.device);

      /* And update the modifiers for that seat.  */
      if (seat)
	UpdateModifiersForSeat (seat,
				event->state.base_mods,
				event->state.locked_mods,
				event->state.latched_mods,
				event->state.base_group,
				event->state.locked_group,
				event->state.latched_group,
				event->state.group);
      return True;
    }

  return False;
}

static Seat *
IdentifySeat (WhatEdge *edge, uint32_t serial)
{
  Seat *seat;
  XLList *tem;

  for (tem = live_seats; tem; tem = tem->next)
    {
      seat = tem->data;

      if (seat->last_button_serial == serial
	  || seat->last_button_press_serial == serial)
	{
	  /* This serial belongs to a button press.  */
	  *edge = APointerEdge;
	  return seat;
	}

      if (seat->last_keyboard_serial == serial)
	{
	  /* This serial belongs to a keyboard press.  */
	  *edge = AKeyboardEdge;
	  return seat;
	}
    }

  /* No seat was found.  */
  return NULL;
}

static Timestamp
GetLastUserTime (Seat *seat)
{
  return seat->last_user_time;
}

static Bool
HandleKeyboardEdge (Seat *seat, Surface *target, uint32_t serial,
		    ResizeEdge edge)
{
  Surface *surface;
  XEvent msg;

  surface = seat->last_button_press_surface;

  if (!surface || surface != target)
    return False;

  memset (&msg, 0, sizeof msg);
  msg.xclient.type = ClientMessage;
  msg.xclient.window = XLWindowFromSurface (surface);
  msg.xclient.format = 32;
  msg.xclient.message_type = _NET_WM_MOVERESIZE;
  msg.xclient.data.l[0] = seat->its_root_x;
  msg.xclient.data.l[1] = seat->its_root_y;
  msg.xclient.data.l[2] = edge;
  msg.xclient.data.l[3] = seat->last_button;
  msg.xclient.data.l[4] = edge == MoveEdge ? 10 : 9;

  /* Release all grabs to the pointer device in question.  */
  XIUngrabDevice (compositor.display, seat->master_pointer,
		  seat->its_press_time);

  /* Also release all grabs to the keyboard device.  */
  XIUngrabDevice (compositor.display, seat->master_keyboard,
		  seat->its_press_time);

  /* Clear the grab immediately since it is no longer used.  */

  if (seat->grab_held)
    CancelGrabEarly (seat);

  /* Send leave to any surface that the pointer is currently within.
     The position of the pointer is then restored upon entry.  */
  EnteredSurface (seat, NULL, seat->its_press_time, 0, 0, False);

  /* Send the message to the window manager.  */
  XSendEvent (compositor.display,
	      DefaultRootWindow (compositor.display),
	      False,
	      SubstructureRedirectMask | SubstructureNotifyMask,
	      &msg);

  /* There's no way to determine whether or not a keyboard resize has
     ended.  */
  return False;
}

static void
HandleResizeUnmapped (void *data)
{
  Seat *seat;

  seat = data;
  CancelResizeOperation (seat, seat->resize_time,
			 NULL, NULL);
}

static Bool
FakePointerEdge (Seat *seat, Surface *target, uint32_t serial,
		 ResizeEdge edge)
{
  Cursor cursor;
  Status state;
  Window window;
  XIEventMask mask;
  size_t length;

  if (edge == NoneEdge)
    return False;

  if (seat->resize_surface)
    /* Some surface is already being resized.  Prohibit this resize
       request.  */
    return False;

  window = XLWindowFromSurface (target);

  if (window == None)
    /* No window exists.  */
    return False;

  seat->resize_start_root_x = seat->its_root_x;
  seat->resize_start_root_y = seat->its_root_y;

  seat->resize_last_root_x = seat->its_root_x;
  seat->resize_last_root_y = seat->its_root_y;

  /* Get an appropriate cursor.  */
  cursor = (seat->cursor ? seat->cursor->cursor : None);

  /* Get the dimensions of the surface when it was first seen.
     This can fail if the surface does not support the operation.  */
  if (!XLSurfaceGetResizeDimensions (target, &seat->resize_width,
				     &seat->resize_height))
    return False;

  /* Set up the event mask for the pointer grab.  */
  length = XIMaskLen (XI_LASTEVENT);
  mask.mask = alloca (length);
  mask.mask_len = length;
  mask.deviceid = XIAllMasterDevices;

  memset (mask.mask, 0, length);

  XISetMask (mask.mask, XI_FocusIn);
  XISetMask (mask.mask, XI_FocusOut);
  XISetMask (mask.mask, XI_Enter);
  XISetMask (mask.mask, XI_Leave);
  XISetMask (mask.mask, XI_Motion);
  XISetMask (mask.mask, XI_ButtonPress);
  XISetMask (mask.mask, XI_ButtonRelease);

  /* Grab the pointer, and don't let go until the button is
     released.  */
  state = XIGrabDevice (compositor.display, seat->master_pointer,
			window, seat->its_press_time, cursor,
			XIGrabModeAsync, XIGrabModeAsync, False, &mask);

  if (state != Success)
    return False;

  /* On the other hand, cancel focus locking and leave the surface,
     since we will not be reporting motion events until the resize
     operation completes.  */

  if (seat->grab_held)
    CancelGrabEarly (seat);

  /* Set the surface as the surface undergoing resize.  */
  seat->resize_surface = target;
  seat->resize_surface_callback
    = XLSurfaceRunAtUnmap (seat->resize_surface,
			   HandleResizeUnmapped, seat);
  seat->resize_axis_flags = resize_edges[edge];
  seat->resize_button = seat->last_button;
  seat->resize_time = seat->its_press_time;

  return True;
}

static Bool
HandlePointerEdge (Seat *seat, Surface *target, uint32_t serial,
		   ResizeEdge edge)
{
  Surface *surface;
  XEvent msg;

  surface = seat->last_button_press_surface;

  if (!surface || surface != target)
    return False;

  if (!XLWmSupportsHint (_NET_WM_MOVERESIZE)
      || getenv ("USE_BUILTIN_RESIZE"))
    return FakePointerEdge (seat, target, serial, edge);

  memset (&msg, 0, sizeof msg);
  msg.xclient.type = ClientMessage;
  msg.xclient.window = XLWindowFromSurface (surface);
  msg.xclient.format = 32;
  msg.xclient.message_type = _NET_WM_MOVERESIZE;
  msg.xclient.data.l[0] = seat->its_root_x;
  msg.xclient.data.l[1] = seat->its_root_y;
  msg.xclient.data.l[2] = edge;
  msg.xclient.data.l[3] = seat->last_button;
  msg.xclient.data.l[4] = 1; /* Source indication.  */

  /* Release all grabs to the pointer device in question.  */
  XIUngrabDevice (compositor.display, seat->master_pointer,
		  seat->its_press_time);

  /* Also clear the core grab, even though it's not used anywhere.  */
  XUngrabPointer (compositor.display, seat->its_press_time);

  /* Clear the grab immediately since it is no longer used.  */
  if (seat->grab_held)
    CancelGrabEarly (seat);

  /* Send the message to the window manager.  */
  XSendEvent (compositor.display,
	      DefaultRootWindow (compositor.display),
	      False,
	      SubstructureRedirectMask | SubstructureNotifyMask,
	      &msg);

  /* Assume a resize is in progress.  Stop resizing upon
     seat->last_button being released.  */
  return seat->resize_in_progress = True;
}

static Bool
StartResizeTracking (Seat *seat, Surface *surface, uint32_t serial,
		     ResizeEdge edge)
{
  WhatEdge type;

  /* Seat cannot be NULL here, but -Wanalyzer disagrees.  */
  XLAssert (seat != NULL);

  if (seat != IdentifySeat (&type, serial))
    return False;

  if (type == AKeyboardEdge)
    return HandleKeyboardEdge (seat, surface, serial, edge);
  else
    return HandlePointerEdge (seat, surface, serial, edge);
}

Bool
XLHandleOneXEventForSeats (XEvent *event)
{
  if (event->type == GenericEvent
      && event->xgeneric.extension == xi2_opcode)
    return HandleOneGenericEvent (&event->xcookie);

  if (event->type == xkb_event_type)
    return HandleXkbEvent ((XkbEvent *) event);

  return False;
}

Window
XLGetGEWindowForSeats (XEvent *event)
{
  XIFocusInEvent *focusin;
  XIEnterEvent *enter;
  XIDeviceEvent *xev;
  XIBarrierEvent *barrier;
  XIGesturePinchEvent *pinch;
  XIGestureSwipeEvent *swipe;

  if (event->type == GenericEvent
      && event->xgeneric.extension == xi2_opcode)
    {
      switch (event->xgeneric.evtype)
	{
	case XI_FocusIn:
	case XI_FocusOut:
	  focusin = event->xcookie.data;
	  return focusin->event;

	case XI_Motion:
	case XI_ButtonPress:
	case XI_ButtonRelease:
	case XI_KeyPress:
	case XI_KeyRelease:
	  xev = event->xcookie.data;
	  return xev->event;

	case XI_Enter:
	case XI_Leave:
	  enter = event->xcookie.data;
	  return enter->event;

	case XI_BarrierHit:
	  barrier = event->xcookie.data;
	  return barrier->event;

	case XI_GesturePinchBegin:
	case XI_GesturePinchEnd:
	case XI_GesturePinchUpdate:
	  pinch = event->xcookie.data;
	  return pinch->event;

	case XI_GestureSwipeBegin:
	case XI_GestureSwipeEnd:
	case XI_GestureSwipeUpdate:
	  swipe = event->xcookie.data;
	  return swipe->event;
	}
    }

  return None;
}

void
XLSelectStandardEvents (Window window)
{
  XIEventMask mask;
  size_t length;

  length = XIMaskLen (XI_LASTEVENT);
  mask.mask = alloca (length);
  mask.mask_len = length;
  mask.deviceid = XIAllMasterDevices;

  memset (mask.mask, 0, length);

  XISetMask (mask.mask, XI_FocusIn);
  XISetMask (mask.mask, XI_FocusOut);
  XISetMask (mask.mask, XI_Enter);
  XISetMask (mask.mask, XI_Leave);
  XISetMask (mask.mask, XI_Motion);
  XISetMask (mask.mask, XI_ButtonPress);
  XISetMask (mask.mask, XI_ButtonRelease);
  XISetMask (mask.mask, XI_KeyPress);
  XISetMask (mask.mask, XI_KeyRelease);
  XISetMask (mask.mask, XI_BarrierHit);

  if (xi2_major > 2 || xi2_minor >= 4)
    {
      /* Select for gesture events whenever supported.  */

      XISetMask (mask.mask, XI_GesturePinchBegin);
      XISetMask (mask.mask, XI_GesturePinchUpdate);
      XISetMask (mask.mask, XI_GesturePinchEnd);
      XISetMask (mask.mask, XI_GestureSwipeBegin);
      XISetMask (mask.mask, XI_GestureSwipeUpdate);
      XISetMask (mask.mask, XI_GestureSwipeEnd);
    }

  XISelectEvents (compositor.display, window, &mask, 1);
}

void
XLDispatchGEForSeats (XEvent *event, Surface *surface,
		      Subcompositor *subcompositor)
{
  if (event->xgeneric.evtype == XI_FocusIn)
    DispatchFocusIn (surface, event->xcookie.data);
  else if (event->xgeneric.evtype == XI_FocusOut)
    DispatchFocusOut (surface, event->xcookie.data);
  else if (event->xgeneric.evtype == XI_Enter
	   || event->xgeneric.evtype == XI_Leave)
    DispatchEntryExit (subcompositor, event->xcookie.data);
  else if (event->xgeneric.evtype == XI_Motion)
    DispatchMotion (subcompositor, event->xcookie.data);
  else if (event->xgeneric.evtype == XI_ButtonPress
	   || event->xgeneric.evtype == XI_ButtonRelease)
    DispatchButton (subcompositor, event->xcookie.data);
  else if (event->xgeneric.evtype == XI_KeyPress
	   || event->xgeneric.evtype == XI_KeyRelease)
    DispatchKey (event->xcookie.data);
  else if (event->xgeneric.evtype == XI_BarrierHit)
    DispatchBarrierHit (event->xcookie.data);
  else if (event->xgeneric.evtype == XI_GesturePinchBegin
	   || event->xgeneric.evtype == XI_GesturePinchUpdate
	   || event->xgeneric.evtype == XI_GesturePinchEnd)
    DispatchGesturePinch (subcompositor, event->xcookie.data);
  else if (event->xgeneric.evtype == XI_GestureSwipeBegin
	   || event->xgeneric.evtype == XI_GestureSwipeUpdate
	   || event->xgeneric.evtype == XI_GestureSwipeEnd)
    DispatchGestureSwipe (subcompositor, event->xcookie.data);
}

Cursor
InitDefaultCursor (void)
{
  static Cursor empty_cursor;
  Pixmap pixmap;
  char no_data[1];
  XColor color;

  if (empty_cursor == None)
    {
      no_data[0] = 0;

      pixmap = XCreateBitmapFromData (compositor.display,
				      DefaultRootWindow (compositor.display),
				      no_data, 1, 1);
      color.pixel = 0;
      color.red = 0;
      color.green = 0;
      color.blue = 0;
      color.flags = DoRed | DoGreen | DoBlue;

      empty_cursor = XCreatePixmapCursor (compositor.display,
					  pixmap, pixmap,
					  &color, &color, 0, 0);

      XFreePixmap (compositor.display, pixmap);
    }

  return empty_cursor;
}

Bool
XLResizeToplevel (Seat *seat, Surface *surface, uint32_t serial,
		  uint32_t xdg_edge)
{
  ResizeEdge edge;

  if (seat->resize_in_progress)
    return False;

  switch (xdg_edge)
    {
    case XDG_TOPLEVEL_RESIZE_EDGE_NONE:
      edge = NoneEdge;
      break;

    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT:
      edge = TopLeftEdge;
      break;

    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT:
      edge = TopRightEdge;
      break;

    case XDG_TOPLEVEL_RESIZE_EDGE_TOP:
      edge = TopEdge;
      break;

    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT:
      edge = RightEdge;
      break;

    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM:
      edge = BottomEdge;
      break;

    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT:
      edge = BottomRightEdge;
      break;

    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT:
      edge = BottomLeftEdge;
      break;

    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT:
      edge = LeftEdge;
      break;

    default:
      edge = NoneEdge;
      break;
    }

  return StartResizeTracking (seat, surface, serial, edge);
}

void
XLMoveToplevel (Seat *seat, Surface *surface, uint32_t serial)
{
  StartResizeTracking (seat, surface, serial, MoveEdge);
}

void *
XLSeatRunAfterResize (Seat *seat, void (*func) (void *, void *),
		      void *data)
{
  ResizeDoneCallback *callback;

  callback = XLMalloc (sizeof *callback);
  callback->next = seat->resize_callbacks.next;
  callback->last = &seat->resize_callbacks;

  seat->resize_callbacks.next->last = callback;
  seat->resize_callbacks.next = callback;

  callback->data = data;
  callback->done = func;

  return callback;
}

void
XLSeatCancelResizeCallback (void *key)
{
  ResizeDoneCallback *callback;

  callback = key;
  callback->last->next = callback->next;
  callback->next->last = callback->last;

  callback->last = callback;
  callback->next = callback;

  XLFree (callback);
}

void *
XLSeatRunOnDestroy (Seat *seat, void (*destroy_func) (void *),
		    void *data)
{
  DestroyListener *listener;

  if (seat->flags & IsInert)
    return NULL;

  listener = XLMalloc (sizeof *listener);
  listener->next = seat->destroy_listeners.next;
  listener->last = &seat->destroy_listeners;

  listener->destroy = destroy_func;
  listener->data = data;

  seat->destroy_listeners.next->last = listener;
  seat->destroy_listeners.next = listener;

  return listener;
}

void
XLSeatCancelDestroyListener (void *key)
{
  DestroyListener *listener;

  listener = key;
  listener->next->last = listener->last;
  listener->last->next = listener->next;

  XLFree (listener);
}

Bool
XLSeatExplicitlyGrabSurface (Seat *seat, Surface *surface, uint32_t serial)
{
  Status state;
  Window window;
  WhatEdge edge;
  Time time;
  XIEventMask mask;
  size_t length;
  Cursor cursor;

  if (seat->flags & IsInert
      /* This would interfere with the drag-and-drop grab.  */
      || seat->flags & IsDragging)
    return False;

  window = XLWindowFromSurface (surface);

  if (!window)
    return False;

  if (serial && serial == seat->last_grab_serial)
    {
      /* This probably means we are trying to revert the grab to a
	 popup's parent after the child is destroyed.  */

      edge = seat->last_grab_edge;
      time = seat->last_grab_time;
    }
  else
    {
      if (seat != IdentifySeat (&edge, serial))
	return False;

      if (edge == AKeyboardEdge)
	time = seat->its_depress_time;
      else
	time = seat->its_press_time;
    }

  /* Record these values; they can be used to revert the grab to the
     parent.  */
  seat->last_grab_serial = serial;
  seat->last_grab_edge = edge;
  seat->last_grab_time = time;

  length = XIMaskLen (XI_LASTEVENT);
  mask.mask = alloca (length);
  mask.mask_len = length;
  mask.deviceid = XIAllMasterDevices;

  memset (mask.mask, 0, length);

  XISetMask (mask.mask, XI_FocusIn);
  XISetMask (mask.mask, XI_FocusOut);
  XISetMask (mask.mask, XI_Enter);
  XISetMask (mask.mask, XI_Leave);
  XISetMask (mask.mask, XI_Motion);
  XISetMask (mask.mask, XI_ButtonPress);
  XISetMask (mask.mask, XI_ButtonRelease);
  XISetMask (mask.mask, XI_KeyPress);
  XISetMask (mask.mask, XI_KeyRelease);

  cursor = (seat->cursor ? seat->cursor->cursor : None);

  state = XIGrabDevice (compositor.display, seat->master_pointer,
			window, time, cursor, XIGrabModeAsync,
			XIGrabModeAsync, True, &mask);

  if (state != Success)
    return False;

  /* The grab was obtained.  Since this grab is owner_events, remove
     any focus locking.  */
  if (seat->grab_held)
    CancelGrabEarly (seat);

  /* Now, grab the keyboard.  Note that we just grab the keyboard so
     that keyboard focus cannot be changed, which is not very crucial,
     so it is allowed to fail.  The keyboard grab is not owner_events,
     which is important for input method events to be filtered
     correctly.  */

  state = XIGrabDevice (compositor.display, seat->master_keyboard,
			window, time, None, XIGrabModeAsync,
			XIGrabModeAsync, False, &mask);

  /* Cancel any external grab that might be applied if the keyboard
     grab succeeded.  */
  if (state == Success)
    seat->flags &= ~IsExternalGrabApplied;

  /* And record the grab surface, so that owner_events can be
     implemented correctly.  */
  SwapGrabSurface (seat, surface);

  return True;
}

DataDevice *
XLSeatGetDataDevice (Seat *seat)
{
  return seat->data_device;
}

void
XLSeatSetDataDevice (Seat *seat, DataDevice *data_device)
{
  seat->data_device = data_device;

  XLRetainDataDevice (data_device);
}

Bool
XLSeatIsInert (Seat *seat)
{
  return seat->flags & IsInert;
}

Bool
XLSeatIsClientFocused (Seat *seat, struct wl_client *client)
{
  struct wl_client *surface_client;

  if (!seat->focus_surface)
    return False;

  surface_client
    = wl_resource_get_client (seat->focus_surface->resource);

  return client == surface_client;
}

Surface *
XLSeatGetFocus (Seat *seat)
{
  return seat->focus_surface;
}

void
XLSeatShowWindowMenu (Seat *seat, Surface *surface, int root_x,
		      int root_y)
{
  XEvent msg;
  Window window;

  if (!XLWmSupportsHint (_GTK_SHOW_WINDOW_MENU))
    return;

  if (seat->flags & IsDragging)
    /* The window menu cannot be displayed while the drag-and-drop
       grab is in effect.  */
    return;

  window = XLWindowFromSurface (surface);

  if (window == None)
    return;

  /* Ungrab the pointer.  Also cancel any focus locking, if
     active.  */
  XIUngrabDevice (compositor.display, seat->master_pointer,
		  seat->its_press_time);

  /* Also clear the core grab, even though it's not used anywhere.  */
  XUngrabPointer (compositor.display, seat->its_press_time);

  /* Cancel focus locking.  */
  if (seat->grab_held)
    CancelGrabEarly (seat);

  /* Signal that the window menu is now shown.  The assumption is that
     the window manager will grab the pointer device; the flag is then
     cleared once once any kind of crossing event is received.

     This is race-prone for two reasons.  If the window manager does
     not receive the event in time, the last-grab-time could have
     changed.  Since there is no timestamp provided in the
     _GTK_SHOW_WINDOW_MENU message, there is no way for the window
     manager to know if the time it issued the grab is valid, as it
     will need to obtain the grab time via a server roundtrip.  In
     addition, there is no way for the client to cancel the window
     menu grab, should it receive an event changing the last-grab-time
     before the window manager has a chance to grab the pointer.

     So the conclusion is that _GTK_SHOW_WINDOW_MENU is defective from
     improper design.  In the meantime, the solution mentioned above
     seems to work well enough in most cases.  */
  seat->flags |= IsWindowMenuShown;

  /* Send the message to the window manager with the device to grab,
     and the coordinates where the window menu should be shown.  */
  memset (&msg, 0, sizeof msg);
  msg.xclient.type = ClientMessage;
  msg.xclient.window = window;
  msg.xclient.format = 32;
  msg.xclient.message_type = _GTK_SHOW_WINDOW_MENU;
  msg.xclient.data.l[0] = seat->master_pointer;
  msg.xclient.data.l[1] = root_x;
  msg.xclient.data.l[2] = root_y;

  XSendEvent (compositor.display,
	      DefaultRootWindow (compositor.display),
	      False,
	      SubstructureRedirectMask | SubstructureNotifyMask,
	      &msg);
}

static void
ForceEntry (Seat *seat, Window source, double x, double y)
{
  Surface *surface;
  Window target;

  if (seat->last_seen_surface)
    {
      surface = seat->last_seen_surface;
      target = XLWindowFromSurface (surface);

      if (target == None)
	{
	  if (source != target)
	    /* If the source is something other than the target,
	       translate the coordinates to the target.  */
	    TranslateCoordinates (source, target, x, y, &x, &y);

	  /* Finally, translate the coordinates to the target
	     view.  */
	  TransformToSurface (surface, x, y, &x, &y);
	}
    }
  else
    return;

  if (!SendEnter (seat, surface, x, y))
    /* Apparently what is done by other compositors when no
       wl_pointer object exists for the surface's client is to
       revert back to the default cursor.  */
    UndefineCursorOn (seat, surface);
}

static void
CancelDrag (Seat *seat, Window event_source, double x, double y)
{
  if (!(seat->flags & IsDragging))
    return;

  /* If the last seen surface is now different from the drag start
     surface, clear the cursor on the latter.  */
  if (seat->drag_start_surface != seat->last_seen_surface
      && seat->cursor)
    FreeCursor (seat->cursor);

  if (seat->data_source)
    XLDoDragFinish (seat);

  /* And cancel the drag flag.  */
  seat->flags &= ~IsDragging;

  /* Clear the surface and free its unmap callback.  */
  seat->drag_start_surface = NULL;
  XLSurfaceCancelUnmapCallback (seat->drag_start_unmap_callback);

  /* Release the active grab as well, and leave any surface we
     entered.  */
  if (seat->drag_last_surface)
    DragLeave (seat);

  XIUngrabDevice (compositor.display, seat->master_pointer,
		  seat->drag_grab_time);

  if (seat->data_source)
    {
      /* Attach a NULL drag device to this source.  */
      XLDataSourceAttachDragDevice (seat->data_source, NULL);

      /* Cancel the destroy callback.  */
      XLDataSourceCancelDestroyCallback (seat->data_source_destroy_callback);

      /* If a data source is attached, clear it now.  */
      seat->data_source = NULL;
      seat->data_source_destroy_callback = NULL;
    }

  /* If nothing was dropped, emit the cancelled event.  */
  if (seat->data_source && !(seat->flags & IsDropped))
    XLDataSourceSendDropCancelled (seat->data_source);

  /* Next, enter the last seen surface.  This is necessary when
     releasing the pointer on top of a different surface after the
     drag and drop operation completes.  */
  ForceEntry (seat, event_source, x, y);

  /* Destroy the grab window.  */
  XDestroyWindow (compositor.display, seat->grab_window);
  seat->grab_window = None;

  /* Cancel the icon surface.  */
  if (seat->icon_surface)
    XLReleaseIconSurface (seat->icon_surface);
  seat->icon_surface = NULL;
}

static void
HandleDragSurfaceUnmapped (void *data)
{
  Seat *seat;
  double root_x, root_y;

  seat = data;

  /* Cancel the drag and drop operation.  We don't know where the
     pointer is ATM, so query for that information.  */

  QueryPointer (seat, DefaultRootWindow (compositor.display),
		&root_x, &root_y);

  /* And cancel the drag with the pointer position.  */

  CancelDrag (seat, DefaultRootWindow (compositor.display),
	      root_x, root_y);
}

static void
HandleDataSourceDestroyed (void *data)
{
  Seat *seat;
  double root_x, root_y;

  seat = data;

  /* Clear those fields first, since their contents are now
     destroyed.  */
  seat->data_source = NULL;
  seat->data_source_destroy_callback = NULL;

  /* Cancel the drag and drop operation.  We don't know where the
     pointer is ATM, so query for that information.  */

  QueryPointer (seat, DefaultRootWindow (compositor.display),
		&root_x, &root_y);

  /* And cancel the drag with the pointer position.  */

  CancelDrag (seat, DefaultRootWindow (compositor.display),
	      root_x, root_y);
}

static Window
MakeGrabWindow (void)
{
  Window window;
  XSetWindowAttributes attrs;

  /* Make the window override redirect.  */
  attrs.override_redirect = True;

  /* The window has to be mapped and visible, or the grab will
     fail.  */
  window = XCreateWindow (compositor.display,
			  DefaultRootWindow (compositor.display),
			  0, 0, 1, 1, 0, CopyFromParent, InputOnly,
			  CopyFromParent, CWOverrideRedirect, &attrs);

  /* Clear the input region of the window.  */
  XShapeCombineRectangles (compositor.display, window,
			   ShapeInput, 0, 0, NULL, 0, ShapeSet,
			   Unsorted);

  /* Map and return it.  */
  XMapRaised (compositor.display, window);
  return window;
}

void
XLSeatBeginDrag (Seat *seat, DataSource *data_source, Surface *start_surface,
		 Surface *icon_surface, uint32_t serial)
{
  Window window;
  Time time;
  XIEventMask mask;
  size_t length;
  WhatEdge edge;
  Status state;

  /* If the surface is unmapped or window-less, don't allow dragging
     from it.  */

  window = XLWindowFromSurface (start_surface);

  if (window == None)
    return;

  /* To begin a drag and drop session, first look up the given serial.
     Then, acquire an active grab on the pointer with owner_events set
     to true.

     While the grab is active, all crossing and motion event dispatch
     is hijacked to send events to the appropriate data device manager
     instead.

     If, for some reason, data_source is destroyed, or the source
     surface is destroyed, the grab is cancelled and the drag and drop
     operation terminates.  Otherwise, drop is sent to the correct
     data device manager once all the pointer buttons are
     released.  */

  if (seat->flags & IsDragging)
    return;

  if (seat != IdentifySeat (&edge, serial))
    return;

  if (edge == AKeyboardEdge)
    return;

  /* Use the time of the last button press or release.  */
  time = seat->its_press_time;

  /* Initialize the event mask used for the grab.  */
  length = XIMaskLen (XI_LASTEVENT);
  mask.mask = alloca (length);
  mask.mask_len = length;
  mask.deviceid = XIAllMasterDevices;

  memset (mask.mask, 0, length);

  /* These are just the events we want reported relative to the grab
     window.  */

  XISetMask (mask.mask, XI_Enter);
  XISetMask (mask.mask, XI_Leave);
  XISetMask (mask.mask, XI_Motion);
  XISetMask (mask.mask, XI_ButtonPress);
  XISetMask (mask.mask, XI_ButtonRelease);

  /* Create the window that will be used for the grab.  */

  XLAssert (seat->grab_window == None);
  seat->grab_window = MakeGrabWindow ();

  /* Record that the drag has started, temporarily, for cursor
     updates.  */
  seat->flags |= IsDragging;

  /* Move the cursor to the drag grab window.  */
  if (seat->cursor)
    UpdateCursorFromSubcompositor (seat->cursor);

  /* Unset the drag flag again.  */
  seat->flags &= ~IsDragging;

  /* Now, try to grab the pointer device with events reported relative
     to the grab window.  */

  state = XIGrabDevice (compositor.display, seat->master_pointer,
		        seat->grab_window, time, None, XIGrabModeAsync,
			XIGrabModeAsync, True, &mask);

  if (state != Success)
    {
      /* Destroy the grab window.  */
      XDestroyWindow (compositor.display, seat->grab_window);
      seat->grab_window = None;

      return;
    }

  /* Since the active grab is now held and is owner_events, remove
     focus locking.  */
  if (seat->grab_held)
    CancelGrabEarly (seat);

  /* Record the originating surface of the grab and add an unmap
     callback.  */
  seat->drag_start_surface = start_surface;
  seat->drag_start_unmap_callback
    = XLSurfaceRunAtUnmap (start_surface, HandleDragSurfaceUnmapped,
			   seat);

  seat->flags &= ~IsDragging;

  /* Since dragging has started, leave the last seen surface now.
     Preserve the cursor, since that surface is where the cursor is
     currently set.  */
  EnteredSurface (seat, NULL, CurrentTime, 0, 0, True);

  if (data_source)
    {
      /* Record the data source.  */
      XLDataSourceAttachDragDevice (data_source,
				    seat->data_device);
      seat->data_source = data_source;

      /* Add a destroy callback.  */
      seat->data_source_destroy_callback
	= XLDataSourceAddDestroyCallback (data_source,
					  HandleDataSourceDestroyed,
					  seat);
    }
  else
    /* This should've been destroyed by CancelGrab.  */
    XLAssert (seat->data_source == NULL);

  /* If the icon surface was specified, give it the right type and
     attach the role.  */
  if (icon_surface)
    {
      /* Note that the caller is responsible for validating the type
	 of the icon surface.  */
      icon_surface->role_type = DndIconType;
      seat->icon_surface = XLGetIconSurface (icon_surface);

      /* Move the icon surface to the last known root window position
	 of the pointer.  */
      XLMoveIconSurface (seat->icon_surface, seat->its_root_x,
			 seat->its_root_y);
    }

  /* Record that the drag has really started.  */
  seat->drag_grab_time = time;
  seat->flags |= IsDragging;
  seat->flags &= ~IsDropped;
}

Timestamp
XLSeatGetLastUserTime (Seat *seat)
{
  return GetLastUserTime (seat);
}

void
XLInitSeats (void)
{
  int rc;

  /* This is the version of the input extension that we want.  */
  xi2_major = 2;
  xi2_minor = 4;

  if (XQueryExtension (compositor.display, "XInputExtension",
		       &xi2_opcode, &xi_first_event, &xi_first_error))
    {
      rc = XIQueryVersion (compositor.display, &xi2_major, &xi2_minor);

      if (xi2_major < 2 || (xi2_major == 2 && xi2_minor < 3) || rc)
	{
	  fprintf (stderr, "version 2.3 or later of of the X Input Extension is"
		   " not present on the X server\n");
	  exit (1);
	}
    }

  seats = XLCreateAssocTable (25);
  devices = XLCreateAssocTable (25);
  keymap_fd = -1;

  SelectDeviceEvents ();
  SetupInitialDevices ();
  SetupKeymap ();
}

DataSource *
XLSeatGetDragDataSource (Seat *seat)
{
  return seat->data_source;
}

void *
XLSeatAddModifierCallback (Seat *seat, void (*changed) (unsigned int, void *),
			   void *data)
{
  ModifierChangeCallback *callback;

  callback = XLMalloc (sizeof *callback);
  callback->next = seat->modifier_callbacks.next;
  callback->last = &seat->modifier_callbacks;
  seat->modifier_callbacks.next->last = callback;
  seat->modifier_callbacks.next = callback;

  callback->changed = changed;
  callback->data = data;

  return callback;
}

void
XLSeatRemoveModifierCallback (void *key)
{
  ModifierChangeCallback *callback;

  callback = key;
  callback->next->last = callback->last;
  callback->last->next = callback->next;

  XLFree (callback);
}

unsigned int
XLSeatGetEffectiveModifiers (Seat *seat)
{
  return seat->base | seat->locked | seat->latched;
}

Bool
XLSeatResizeInProgress (Seat *seat)
{
  return seat->resize_in_progress;
}

void
XLSeatSetTextInputFuncs (TextInputFuncs *funcs)
{
  input_funcs = funcs;
}

int
XLSeatGetKeyboardDevice (Seat *seat)
{
  return seat->master_keyboard;
}

int
XLSeatGetPointerDevice (Seat *seat)
{
  return seat->master_pointer;
}

Seat *
XLSeatGetInputMethodSeat (void)
{
  XLList *list;
  Seat *seat;

  for (list = live_seats; list; list = list->next)
    {
      seat = list->data;

      if (seat->flags & IsTextInputSeat)
	return seat;
    }

  return NULL;
}

void
XLSeatDispatchCoreKeyEvent (Seat *seat, Surface *surface, XEvent *event)
{
  unsigned int effective;
  unsigned int state, group;

  /* Dispatch a core event generated by an input method to SEAT.  If
     SURFACE is no longer the focus surface, refrain from doing
     anything.  If a keycode can be found for KEYSYM, use that
     keycode.  */

  if (surface != seat->focus_surface)
    return;

  /* Get the group and state of the key event.  */
  group = event->xkey.state >> 13;
  state = event->xkey.state & AllKeyMask;

  /* Get the effective state of the seat.  */
  effective = seat->base | seat->latched | seat->locked;

  if (group != seat->effective_group || state != effective)
    /* The modifiers in the provided core event are different from
       what the focus surface was previously sent.  Send a new
       modifier event with the effective state provided in the give
       core event.  */
    HackKeyboardModifiers (seat, surface, effective, group);

  /* Then send the event.  */
  if (event->xkey.type == KeyPress)
    SendKeyboardKey (seat, seat->focus_surface, event->xkey.time,
		     WaylandKeycode (event->xkey.keycode),
		     WL_KEYBOARD_KEY_STATE_PRESSED);
  else
    SendKeyboardKey (seat, seat->focus_surface, event->xkey.time,
		     WaylandKeycode (event->xkey.keycode),
		     WL_KEYBOARD_KEY_STATE_RELEASED);

  /* Restore the modifiers.  */
  if (group != seat->effective_group || state != effective)
    SendKeyboardModifiers (seat, surface);
}

Seat *
XLPointerGetSeat (Pointer *pointer)
{
  return pointer->seat;
}

void
XLSeatGetMouseData (Seat *seat, Surface **last_seen_surface,
		    double *last_surface_x, double *last_surface_y,
		    double *its_root_x, double *its_root_y)
{
  *last_seen_surface = seat->last_seen_surface;
  *last_surface_x = seat->last_surface_x;
  *last_surface_y = seat->last_surface_y;
  *its_root_x = seat->its_root_x;
  *its_root_y = seat->its_root_y;
}

void
XLSeatLockPointer (Seat *seat)
{
  seat->flags |= IsPointerLocked;
}

void
XLSeatUnlockPointer (Seat *seat)
{
  seat->flags &= ~IsPointerLocked;
}

RelativePointer *
XLSeatGetRelativePointer (Seat *seat, struct wl_resource *resource)
{
  RelativePointer *relative_pointer;
  SeatClientInfo *info;

  /* Create a relative pointer object for the relative pointer
     resource RESOURCE.  */

  relative_pointer = XLCalloc (1, sizeof *relative_pointer);
  info = CreateSeatClientInfo (seat, wl_resource_get_client (resource));

  /* Link the relative pointer onto the seat client info.  */
  relative_pointer->next = info->relative_pointers.next;
  relative_pointer->last = &info->relative_pointers;
  info->relative_pointers.next->last = relative_pointer;
  info->relative_pointers.next = relative_pointer;
  relative_pointer->info = info;

  /* Then, the seat.  */
  relative_pointer->seat = seat;
  RetainSeat (seat);

  /* Add the resource.  */
  relative_pointer->resource = resource;

  return relative_pointer;
}

void
XLSeatDestroyRelativePointer (RelativePointer *relative_pointer)
{
  relative_pointer->last->next = relative_pointer->next;
  relative_pointer->next->last = relative_pointer->last;

  ReleaseSeatClientInfo (relative_pointer->info);
  ReleaseSeat (relative_pointer->seat);

  XLFree (relative_pointer);
}

SwipeGesture *
XLSeatGetSwipeGesture (Seat *seat, struct wl_resource *resource)
{
  SwipeGesture *swipe_gesture;
  SeatClientInfo *info;

  /* Create a swipe gesture object for the resource RESOURCE.  */
  swipe_gesture = XLCalloc (1, sizeof *swipe_gesture);

  /* Obtain or create the seat client info.  */
  info = CreateSeatClientInfo (seat, wl_resource_get_client (resource));

  /* Link the gesture onto it.  */
  swipe_gesture->next = info->swipe_gestures.next;
  swipe_gesture->last = &info->swipe_gestures;
  info->swipe_gestures.next->last = swipe_gesture;
  info->swipe_gestures.next = swipe_gesture;
  swipe_gesture->info = info;

  /* Set the seat and resource.  */
  swipe_gesture->seat = seat;
  swipe_gesture->resource = resource;
  RetainSeat (seat);

  return swipe_gesture;
}

PinchGesture *
XLSeatGetPinchGesture (Seat *seat, struct wl_resource *resource)
{
  PinchGesture *pinch_gesture;
  SeatClientInfo *info;

  /* Create a pinch gesture object for the resource RESOURCE.  */
  pinch_gesture = XLCalloc (1, sizeof *pinch_gesture);

  /* Obtain or create the seat client info.  */
  info = CreateSeatClientInfo (seat, wl_resource_get_client (resource));

  /* Link the gesture onto it.  */
  pinch_gesture->next = info->pinch_gestures.next;
  pinch_gesture->last = &info->pinch_gestures;
  info->pinch_gestures.next->last = pinch_gesture;
  info->pinch_gestures.next = pinch_gesture;
  pinch_gesture->info = info;

  /* Set the seat and resource.  */
  pinch_gesture->seat = seat;
  pinch_gesture->resource = resource;
  RetainSeat (seat);

  return pinch_gesture;
}

void
XLSeatDestroySwipeGesture (SwipeGesture *swipe_gesture)
{
  swipe_gesture->last->next = swipe_gesture->next;
  swipe_gesture->next->last = swipe_gesture->last;

  ReleaseSeatClientInfo (swipe_gesture->info);
  ReleaseSeat (swipe_gesture->seat);

  XLFree (swipe_gesture);
}

void
XLSeatDestroyPinchGesture (PinchGesture *pinch_gesture)
{
  pinch_gesture->last->next = pinch_gesture->next;
  pinch_gesture->next->last = pinch_gesture->last;

  ReleaseSeatClientInfo (pinch_gesture->info);
  ReleaseSeat (pinch_gesture->seat);

  XLFree (pinch_gesture);
}

Bool
XLSeatApplyExternalGrab (Seat *seat, Surface *surface)
{
  Window window;
  Status state;
  XIEventMask mask;
  size_t length;

  /* Grab the toplevel SURFACE on SEAT.  */

  window = XLWindowFromSurface (surface);

  if (!window)
    return None;

  length = XIMaskLen (XI_LASTEVENT);
  mask.mask = alloca (length);
  mask.mask_len = length;
  mask.deviceid = XIAllMasterDevices;

  memset (mask.mask, 0, length);

  /* Grab focus and key events.  */
  XISetMask (mask.mask, XI_FocusIn);
  XISetMask (mask.mask, XI_FocusOut);
  XISetMask (mask.mask, XI_KeyPress);
  XISetMask (mask.mask, XI_KeyRelease);

  state = XIGrabDevice (compositor.display, seat->master_keyboard,
			window, seat->last_focus_time.milliseconds, None,
			XIGrabModeAsync, XIGrabModeAsync, True, &mask);
  if (state == Success)
    {
      /* Mark an external grab as having been applied.  */
      seat->flags |= IsExternalGrabApplied;

      /* Record the time when it was applied.  */
      seat->external_grab_time = seat->last_focus_time.milliseconds;

      return True;
    }

  return False;
}

void
XLSeatCancelExternalGrab (Seat *seat)
{
  if (!(seat->flags & IsExternalGrabApplied))
    return;

  /* Cancel the external grab.  */
  XIUngrabDevice (compositor.display, seat->master_keyboard,
		  seat->external_grab_time);
}

KeyCode
XLKeysymToKeycode (KeySym keysym, XEvent *event)
{
  unsigned int i, mods_return;
  KeySym keysym_return;

  if (!xkb_desc)
    return 0;

  /* Look up the keycode correspnding to the given keysym.  Return 0
     if there is none.  */

  for (i = xkb_desc->min_key_code; i <= xkb_desc->max_key_code; ++i)
    {
      if (XkbTranslateKeyCode (xkb_desc, i, event->xkey.state,
			       &mods_return, &keysym_return)
	  && keysym_return == keysym)
	return i;
    }

  return 0;
}

/* This is a particularly ugly hack, but there is no other way to
   expose all the internals needed by test_seat.c.  */

#include "test_seat.c"
