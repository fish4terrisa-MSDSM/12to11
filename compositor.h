/* Wayland compositor running on top of an X serer.

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

#include <sys/param.h>
#include <sys/stat.h>

#include <limits.h>

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

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
};

/* Forward declarations from seat.c.  */

typedef struct _Seat Seat;

/* Defined in 12to11.c.  */

extern Compositor compositor;

/* Defined in run.c.  */

typedef struct _PollFd WriteFd;
typedef struct _PollFd ReadFd;

extern void XLRunCompositor (void);
extern WriteFd *XLAddWriteFd (int, void *, void (*) (int, void *));
extern ReadFd *XLAddReadFd (int, void *, void (*) (int, void *));
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

extern void XLAssert (Bool);
extern int XLOpenShm (void);

extern void XLScaleRegion (pixman_region32_t *, pixman_region32_t *,
			   float, float);
extern Time XLGetServerTimeRoundtrip (void);

extern RootWindowSelection *XLSelectInputFromRootWindow (unsigned long);
extern void XLDeselectInputFromRootWindow (RootWindowSelection *);

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
  Picture (*get_picture) (ExtBuffer *);
  Pixmap (*get_pixmap) (ExtBuffer *);
  unsigned int (*width) (ExtBuffer *);
  unsigned int (*height) (ExtBuffer *);
  void (*release) (ExtBuffer *);
  void (*print_buffer) (ExtBuffer *);
};

struct _ExtBuffer
{
  /* Functions for this buffer.  */
  ExtBufferFuncs funcs;

  /* List of destroy listeners.  */
  XLList *destroy_listeners;
};

extern void XLRetainBuffer (ExtBuffer *);
extern void XLDereferenceBuffer (ExtBuffer *);
extern Picture XLPictureFromBuffer (ExtBuffer *);
extern Pixmap XLPixmapFromBuffer (ExtBuffer *);
extern unsigned int XLBufferWidth (ExtBuffer *);
extern unsigned int XLBufferHeight (ExtBuffer *);
extern void XLReleaseBuffer (ExtBuffer *);
extern void *XLBufferRunOnFree (ExtBuffer *, ExtBufferFunc,
				void *);
extern void XLBufferCancelRunOnFree (ExtBuffer *, void *);
extern void XLPrintBuffer (ExtBuffer *);

extern void ExtBufferDestroy (ExtBuffer *);

/* Defined in shm.c.  */

extern void XLInitShm (void);

/* Defined in subcompositor.c.  */

typedef struct _View View;
typedef struct _List List;
typedef struct _Subcompositor Subcompositor;

extern void SubcompositorInit (void);

extern Subcompositor *MakeSubcompositor (void);
extern View *MakeView (void);

extern void SubcompositorSetTarget (Subcompositor *, Picture);
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

extern void ViewSetSubcompositor (View *, Subcompositor *);

extern void ViewInsert (View *, View *);
extern void ViewInsertAfter (View *, View *, View *);
extern void ViewInsertBefore (View *, View *, View *);
extern void ViewInsertStart (View *, View *);

extern void ViewUnparent (View *);
extern View *ViewGetParent (View *);
extern void ViewAttachBuffer (View *, ExtBuffer *);
extern void ViewMove (View *, int, int);
extern void ViewDetach (View *);
extern void ViewMap (View *);
extern void ViewUnmap (View *);

extern void ViewSetData (View *, void *);
extern void *ViewGetData (View *);

extern void ViewTranslate (View *, int, int, int *, int *);

extern void ViewFree (View *);

extern void ViewDamage (View *, pixman_region32_t *);
extern void ViewSetOpaque (View *, pixman_region32_t *);
extern void ViewSetInput (View *, pixman_region32_t *);
extern int ViewWidth (View *);
extern int ViewHeight (View *);
extern void ViewSetScale (View *, int);

extern Subcompositor *ViewGetSubcompositor (View *);

/* Defined in surface.c.  */

typedef struct _State State;
typedef struct _FrameCallback FrameCallback;
typedef enum _RoleType RoleType;

enum _RoleType
  {
    AnythingType,
    SubsurfaceType,
    XdgType,
    CursorType,
    DndIconType,
  };

enum
  {
    PendingNone		  = 0,
    PendingOpaqueRegion	  = 1,
    PendingInputRegion	  = (1 << 2),
    PendingDamage	  = (1 << 3),
    PendingSurfaceDamage  = (1 << 4),
    PendingBuffer	  = (1 << 5),
    PendingFrameCallbacks = (1 << 6),
    PendingBufferScale	  = (1 << 7),
    PendingAttachments	  = (1 << 8),
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

  /* What part of this state has been changed.  */
  int pending;

  /* The scale of this buffer.  */
  int buffer_scale;

  /* List of frame callbacks.  */
  FrameCallback frame_callbacks;

  /* Attachment position.  */
  int x, y;
};

typedef enum _ClientDataType ClientDataType;
typedef struct _Surface Surface;
typedef struct _Role Role;
typedef struct _RoleFuncs RoleFuncs;
typedef struct _CommitCallback CommitCallback;
typedef struct _UnmapCallback UnmapCallback;
typedef struct _DestroyCallback DestroyCallback;

enum _ClientDataType
  {
    SubsurfaceData,
    MaxClientData,
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

  /* Array of "client data".  */
  void *client_data[MaxClientData];

  /* List of functions for freeing "client data".  */
  void (*free_client_data[MaxClientData]) (void *);

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
  Bool (*subframe) (Surface *, Role *);
  void (*end_subframe) (Surface *, Role *);
  Window (*get_window) (Surface *, Role *);
  void (*get_resize_dimensions) (Surface *, Role *, int *, int *);
  void (*post_resize) (Surface *, Role *, int, int, int, int);
  void (*move_by) (Surface *, Role *, int, int);
  void (*rescale) (Surface *, Role *);
  void (*note_desync_child) (Surface *, Role *);
  void (*note_child_synced) (Surface *, Role *);
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

extern void XLStateAttachBuffer (State *, ExtBuffer *);
extern void XLStateDetachBuffer (State *);
extern void XLSurfaceRunFrameCallbacks (Surface *, struct timespec);
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
extern Bool XLSurfaceGetResizeDimensions (Surface *, int *, int *);
extern void XLSurfacePostResize (Surface *, int, int, int, int);
extern void XLSurfaceMoveBy (Surface *, int, int);
extern Window XLWindowFromSurface (Surface *);
extern void XLUpdateSurfaceOutputs (Surface *, int, int, int, int);

/* Defined in output.c.  */

extern int global_scale_factor;

extern void XLInitRROutputs (void);
extern Bool XLHandleOneXEventForOutputs (XEvent *);
extern void XLOutputGetMinRefresh (struct timespec *);
extern Bool XLGetOutputRectAt (int, int, int *, int *, int *, int *);
extern void *XLAddScaleChangeCallback (void *, void (*) (void *, int));
extern void XLRemoveScaleChangeCallback (void *);
extern void XLClearOutputs (Surface *);

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
  _NET_WM_WINDOW_TYPE, _NET_WM_WINDOW_TYPE_MENU, _NET_WM_WINDOW_TYPE_DND;

/* This is automatically generated by mime4.awk.  */
extern Atom DirectTransferAtoms;

extern Atom InternAtom (const char *);
extern void ProvideAtom (const char *, Atom);

extern void XLInitAtoms (void);

/* Defined in xdg_wm.c.  */

extern void XLInitXdgWM (void);

/* Defined in frame_clock.c.  */

typedef struct _FrameClock FrameClock;

extern FrameClock *XLMakeFrameClockForWindow (Window);
extern Bool XLFrameClockFrameInProgress (FrameClock *);
extern void XLFrameClockAfterFrame (FrameClock *, void (*) (FrameClock *,
							    void *),
				    void *);
extern void XLFrameClockHandleFrameEvent (FrameClock *, XEvent *);
extern void XLFrameClockStartFrame (FrameClock *, Bool);
extern void XLFrameClockEndFrame (FrameClock *);
extern Bool XLFrameClockIsFrozen (FrameClock *);
extern Bool XLFrameClockCanBatch (FrameClock *);
extern Bool XLFrameClockNeedConfigure (FrameClock *);
extern void XLFrameClockFreeze (FrameClock *);
extern void XLFrameClockUnfreeze (FrameClock *);
extern void XLFreeFrameClock (FrameClock *);
extern Bool XLFrameClockSyncSupported (void);
extern Bool XLFrameClockCanRunFrame (FrameClock *);
extern void XLFrameClockSetPredictRefresh (FrameClock *);
extern void XLFrameClockDisablePredictRefresh (FrameClock *);
extern void XLFrameClockSetFreezeCallback (FrameClock *, void (*) (void *),
					   void *);
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
  void (*commit_inside_frame) (Role *, XdgRoleImplementation *);
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

extern Window XLWindowFromXdgRole (Role *);
extern Subcompositor *XLSubcompositorFromXdgRole (Role *);
extern XdgRoleImplementationType XLTypeOfXdgRole (Role *);
extern XdgRoleImplementation *XLImplementationOfXdgRole (Role *);
extern FrameClock *XLXdgRoleGetFrameClock (Role *);
extern XdgRoleImplementation *XLLookUpXdgToplevel (Window);
extern XdgRoleImplementation *XLLookUpXdgPopup (Window);

/* Defined in positioner.c.  */

typedef struct _Positioner Positioner;

extern void XLCreateXdgPositioner (struct wl_client *, struct wl_resource *,
				   uint32_t);
extern void XLPositionerCalculateGeometry (Positioner *, Role *, int *, int *,
					   int *, int *);
extern void XLRetainPositioner (Positioner *);
extern void XLReleasePositioner (Positioner *);
extern Bool XLPositionerIsReactive (Positioner *);

/* Defined in xdg_toplevel.c.  */

extern void XLGetXdgToplevel (struct wl_client *, struct wl_resource *,
			      uint32_t);
extern Bool XLHandleXEventForXdgToplevels (XEvent *);
extern Bool XLIsXdgToplevel (Window);
extern void XLInitXdgToplevels (void);

/* Defined in xdg_popup.c.  */

extern void XLGetXdgPopup (struct wl_client *, struct wl_resource *,
			   uint32_t, struct wl_resource *,
			   struct wl_resource *);
extern Bool XLHandleXEventForXdgPopups (XEvent *);
extern Bool XLHandleButtonForXdgPopups (Seat *, Surface *);
extern void XLInitPopups (void);

/* Defined in xerror.c.  */

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

/* Defined in data_device.c.  */

typedef struct _DataDevice DataDevice;
typedef struct _DataSource DataSource;
typedef struct _CreateOfferFuncs CreateOfferFuncs;
typedef struct _DndOfferFuncs DndOfferFuncs;

typedef struct wl_resource *(*CreateOfferFunc) (struct wl_client *, Time);
typedef void (*SendDataFunc) (struct wl_resource *, Time);

struct _CreateOfferFuncs
{
  CreateOfferFunc create_offer;
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
extern void XLSetForeignSelection (Time, CreateOfferFuncs);
extern void XLClearForeignSelection (Time);
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

/* Defined in seat.c.  */

extern int xi2_opcode;
extern int xi_first_event;
extern int xi_first_error;

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
extern void XLSeatShowWindowMenu (Seat *, Surface *, int, int);
extern Time XLSeatGetLastUserTime (Seat *);
extern void XLSeatBeginDrag (Seat *, DataSource *, Surface *,
			     Surface *, uint32_t);
extern DataSource *XLSeatGetDragDataSource (Seat *);
extern void *XLSeatAddModifierCallback (Seat *, void (*) (unsigned int, void *),
					void *);
extern void XLSeatRemoveModifierCallback (void *);
extern unsigned int XLSeatGetEffectiveModifiers (Seat *);

extern Cursor InitDefaultCursor (void);

/* Defined in dmabuf.c.  */

extern void XLInitDmabuf (void);
extern Bool XLHandleErrorForDmabuf (XErrorEvent *);
extern Bool XLHandleOneXEventForDmabuf (XEvent *);

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

extern Bool OwnSelection (Time, Atom, GetDataFunc (*) (WriteTransfer *,
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
extern void XLPrimarySelectionHandleFocusChange (Seat *);

/* Utility functions that don't belong in a specific file.  */

#define ArrayElements(arr) (sizeof (arr) / sizeof (arr)[0])

#define BoxStartX(box)	(MIN ((box).x1, (box).x2))
#define BoxEndX(box)	(MAX ((box).x1, (box).x2) - 1)
#define BoxStartY(box)	(MIN ((box).y1, (box).y2))
#define BoxEndY(box)	(MAX ((box).y1, (box).y2) - 1)

#define BoxWidth(box)	(BoxEndX (box) - BoxStartX (box) + 1)
#define BoxHeight(box)	(BoxEndY (box) - BoxStartY (box) + 1)

#define SafeCmp(n1, n2) (((n1) > (n2)) - ((n1) < (n2)))

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

#define IntAddWrapv(a, b, r) 		__builtin_add_overflow (a, b, r)
#define IntSubtractWrapv(a, b, r)	__builtin_sub_overflow (a, b, r)
#define IntMultiplyWrapv(a, b, r)	__builtin_mul_overflow (a, b, r)

#define ConfigureWidth(event)	((event)->xconfigure.width / global_scale_factor)
#define ConfigureHeight(event)	((event)->xconfigure.height / global_scale_factor)

