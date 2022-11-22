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

#include <stdio.h>

#include "compositor.h"

/* Generic sync helper.  There are two methods for the protocol
   translator to synchronize its redraw with the X compositing manager
   and server; the protocol translator dynamically selects the right
   method depending on which options have been specified by the client
   upon commit.

   The first is based on the _NET_WM_SYNC_REQUEST and
   _NET_WM_FRAME_DRAWN protocols, and is only present when there is a
   compositing manager.  The second is not yet implemented.  */

enum _SynchronizationType
  {
    SyncTypeFrameClock,
    SyncTypePresent,
  };

struct _SyncHelper
{
  /* The pending frame ID.  */
  uint64_t pending_frame;

  /* The associated subcompositor.  */
  Subcompositor *subcompositor;

  /* The associated rendering target.  */
  RenderTarget target;

  /* The associated window.  */
  Window window;

  /* The associated frame clock.  */
  FrameClock *clock;

  /* Callback called to run frame callbacks.  */
  void (*frame_callback) (void *, uint32_t);

  /* Callback called to start resize.  */
  void (*resize_callback) (void *, Bool);

  /* Callback called to decide whether or not it is ok to fast forward
     a frame.  */
  Bool (*fast_forward_callback) (void *);

  /* Role associated with the sync helper.  */
  Role *role;

  /* Clock synchronization part of sync helper.  The sync helper has
     to seamlessly switch between two different clocks: the monotonic
     X server time, in microseconds, and the system time.

     The switching is done by maintaining two counters.  The first is
     a 64-bit microsecond-precision counter containing the last
     reported time with the milliseconds part truncated to 32 bits,
     and the second is the struct timespec at which that time
     arrived.  */
  uint64_t server_time, arrival_time;

  /* What kind of synchronization is being used.  Switching to a given
     synchronization type can only happen when starting a frame.  */
  SynchronizationType used;

  /* The last msc and ust.  This is only set upon ModePresented, and
     used to drive async presentation.  */
  uint64_t last_msc, last_ust;

  /* Various flags.  */
  int flags;
};

enum
  {
    FrameStarted      = 1,
    FramePending      = 1 << 1,
    FrameSynchronized = 1 << 2,
    FrameResize	      = 1 << 3,
  };

static SynchronizationType
GetWantedSynchronizationType (SyncHelper *helper)
{
  if (helper->flags & FrameSynchronized)
    return SyncTypeFrameClock;

  if (helper->flags & FrameResize)
    {
      /* Confirming a resize must be done using the regular frame
	 clock.  */
      helper->flags &= ~FrameResize;
      return SyncTypeFrameClock;
    }

#ifdef AllowPresent /* TODO: make this work.  */
  /* Otherwise, use Present.  */
  return SyncTypePresent;
#else
  return SyncTypeFrameClock;
#endif
}

static uint64_t
ServerTimeFromTimespec (struct timespec *clock)
{
  uint64_t timestamp;

  if (IntMultiplyWrapv (clock->tv_sec, 1000000, &timestamp)
      || IntAddWrapv (timestamp, clock->tv_nsec / 1000, &timestamp))
    /* Overflow.  */
    return 0;

  return timestamp;
}

static uint64_t
ConfineTime (uint64_t time)
{
  uint32_t milliseconds;

  /* Given a microsecond time, confine the millisecond part to
     CARD32.  */
  milliseconds = time / 1000;

  return (milliseconds * (uint64_t) 1000
	  + time % 1000);
}

static Bool
TimestampGreaterThan (uint64_t time_a, uint64_t time_b)
{
  uint32_t ms_a, ms_b;

  /* Compare the millisecond part, handling wraparound.  */

  ms_a = time_a / 1000;
  ms_b = time_b / 1000;

  if (ms_a > ms_b)
    return ms_b - ms_a > UINT32_MAX / 2;

  if (ms_a == ms_b)
    return (time_a % 1000 > time_b % 1000);

  return ms_b - ms_a > UINT32_MAX / 2;
}

static uint64_t
ConsiderFrameTime (SyncHelper *helper, uint64_t frame_time_us)
{
  struct timespec current_time;
  uint64_t old_arrival_time;

  /* Get the time the previous timestamp arrived for future
     reference.  */
  old_arrival_time = helper->arrival_time;

  /* Store the time at which the specified timestamp arrived.  */
  clock_gettime (CLOCK_MONOTONIC, &current_time);
  helper->arrival_time = ServerTimeFromTimespec (&current_time);

  if (frame_time_us == (uint64_t) -1)
    {
    use_future_time:
      /* Some frame time in the future should be computed and
	 used.  */

      if (IntAddWrapv (helper->server_time,
		       helper->arrival_time - old_arrival_time,
		       &helper->server_time))
	return (uint64_t) -1;

      helper->server_time = ConfineTime (helper->server_time);
      return helper->server_time;
    }

  /* If frame_time_us is larger than helper->server_time, great!  Just
     use it instead.  */

  if (TimestampGreaterThan (frame_time_us, helper->server_time))
    {
      helper->server_time = ConfineTime (frame_time_us);
      return helper->server_time;
    }

  /* Otherwise, go to the -1 branch.  */
  goto use_future_time;
}

static void
FrameCompleted (SyncHelper *helper, uint64_t frame_time_us)
{
  uint64_t time;

  /* The frame completed.  Run frame callbacks should there be no
     pending frame, or start a new update again mailbox-style.  */

  time = ConsiderFrameTime (helper, frame_time_us);

  if (helper->flags & FramePending)
    {
      /* Clear the frame pending flag.  */
      helper->flags &= ~FramePending;

      /* Start a new update.  */
      SubcompositorUpdate (helper->subcompositor);
    }
  else
    /* Run the frame callback.  */
    helper->frame_callback (helper->role, time / 1000);
}

static void
EndFrame (SyncHelper *helper)
{
  XLFrameClockEndFrame (helper->clock);
}

static Bool
UpdateFrameRefreshPrediction (SyncHelper *helper)
{
  int desync_children;

  /* Count the number of desynchronous children attached to this
     surface, directly or indirectly.  When this number is more than
     1, enable frame refresh prediction, which allows separate frames
     from subsurfaces to be batched together.  */

  if (helper->role->surface)
    {
      desync_children = 0;
      XLUpdateDesynchronousChildren (helper->role->surface,
				     &desync_children);

      if (desync_children)
	XLFrameClockSetPredictRefresh (helper->clock);
      else
	XLFrameClockDisablePredictRefresh (helper->clock);

      return desync_children > 0;
    }

  return False;
}

static void
NoteFrame (FrameMode mode, uint64_t id, void *data,
	   uint64_t msc, uint64_t ust)
{
  SyncHelper *helper;
  Bool success;
  SynchronizationType wanted;

  helper = data;

  switch (mode)
    {
    case ModeStarted:
      /* Record this frame counter as the pending frame.  */
      helper->pending_frame = id;

      if (helper->flags & FrameStarted)
	break;

      helper->flags |= FrameStarted;

      wanted = GetWantedSynchronizationType (helper);

      if (wanted == SyncTypeFrameClock)
	{
	frame_clock:
	  /* Check whether or not frame refresh prediction should be
	     used.  */
	  UpdateFrameRefreshPrediction (helper);

	  /* Use async presentation.  */
	  RenderSetRenderMode (helper->target, RenderModeAsync,
			       helper->last_msc + 1);

	  /* Start frame clock-based synchronization.  If I were more
	     confident in this code, then the call would be allowed to
	     fail, but as it stands I'm not.  */
	  success = XLFrameClockStartFrame (helper->clock, False);
	  XLAssert (success);

	  helper->flags |= FrameSynchronized;
	}
      else if (wanted == SyncTypePresent)
	{
	  /* Since presentation is wanted, switch the renderer to
	     vsync mode.  */

	  if (!RenderSetRenderMode (helper->target, RenderModeVsync,
				    helper->last_msc + 1))
	    {
	      wanted = SyncTypeFrameClock;
	      goto frame_clock;
	    }

	  /* Now, presentation will implicitly be synchronized.  */
	}

      helper->used = wanted;
      break;

    case ModePresented:
      helper->last_msc = msc;
      helper->last_ust = ust;
      Fallthrough;

    case ModeComplete:

      /* The frame was completed.  */
      if (id == helper->pending_frame)
	{
	  /* End the frame if a frame clock was used for
	     synchronization.  */
	  if (helper->used == SyncTypeFrameClock)
	    EndFrame (helper);

	  /* Clear the frame completed flag.  FrameSynchronized will
	     still be set, until AfterFrame is called.  */
	  helper->flags &= ~FrameStarted;

	  if (!(helper->flags & FrameSynchronized))
	    /* But this means the frame was not synchronized.  Run
	       frame callbacks or start a new update now.  */
	    FrameCompleted (helper, (uint64_t) -1);

	  /* This value means that there is no frame currently being
	     displayed.  */
	  helper->pending_frame = (uint64_t) -1;
	}

      break;

    default:
    }
}

static void
AfterFrame (FrameClock *clock, void *data, uint64_t frame_time_us)
{
  SyncHelper *helper;

  helper = data;

  /* The frame completed.  */
  helper->flags &= ~FrameSynchronized;
  FrameCompleted (helper, frame_time_us);
}

static void
HandleFreeze (void *data, Bool only_frame)
{
  SyncHelper *helper;

  helper = data;

  /* The helper is now frozen.  Cancel any late frame and run the
     resize callback.

     Make sure that the next update will be done via the frame
     clock.  */

  helper->flags &= ~FramePending;
  helper->flags |= FrameResize;

  if (helper->resize_callback)
    helper->resize_callback (helper->role, only_frame);
}

static Bool
CheckFrame (SyncHelper *helper)
{
  Bool rc;

  /* Return whether or not it is ok to perform an update now.  It is
     not ok when frame-clock synchronization is being used, and the
     compositing manager is reading from the contents of the back
     buffer.  */
  rc = (helper->used != SyncTypeFrameClock
	|| !XLFrameClockFrameInProgress (helper->clock)
	|| XLFrameClockCanBatch (helper->clock));

  return rc;
}

static Bool
QueryFastForward (void *data)
{
  SyncHelper *helper;

  helper = data;

  if (helper->fast_forward_callback)
    return helper->fast_forward_callback (helper->role);

  return False;
}



SyncHelper *
MakeSyncHelper (Subcompositor *subcompositor, Window window,
		RenderTarget target,
		void (*frame_callback) (void *, uint32_t),
		Role *role)
{
  SyncHelper *helper;
  struct timespec current_time;

  /* Create a sync helper for the given subcompositor, which
     synchronizes to the specified window.  */
  helper = XLCalloc (1, sizeof *helper);
  helper->clock = XLMakeFrameClockForWindow (window);
  helper->subcompositor = subcompositor;
  helper->pending_frame = -1;
  helper->frame_callback = frame_callback;
  helper->role = role;
  helper->target = target;

  /* Set the note frame callback.  */
  SubcompositorSetNoteFrameCallback (helper->subcompositor,
				     NoteFrame, helper);

  /* Set the frame clock callbacks.  */
  XLFrameClockAfterFrame (helper->clock, AfterFrame, helper);
  XLFrameClockSetFreezeCallback (helper->clock, HandleFreeze,
				 QueryFastForward, helper);

  /* Initialize the sync helper time.  */

  clock_gettime (CLOCK_MONOTONIC, &current_time);
  helper->arrival_time = ServerTimeFromTimespec (&current_time) + 0xffffffff*1000;

  if (compositor.server_time_monotonic)
    helper->server_time = helper->arrival_time;
  else
    /* This can never overflow because the X server time is limited to
       0xffffffff.  */
    helper->server_time = XLGetServerTimeRoundtrip () * 1000;

  return helper;
}

void
SyncHelperUpdate (SyncHelper *helper)
{
  /* Perform a subcompositor update on helper.  If the update will
     happen while the compositing manager is still drawing the
     results, schedule the update for when the frame completes.  */

  if (!CheckFrame (helper))
    helper->flags |= FramePending;
  else
    SubcompositorUpdate (helper->subcompositor);
}

void
FreeSyncHelper (SyncHelper *helper)
{
  XLFreeFrameClock (helper->clock);
  SubcompositorSetNoteFrameCallback (helper->subcompositor,
				     NULL, NULL);
  XLFree (helper);
}

void
SyncHelperHandleFrameEvent (SyncHelper *helper, XEvent *event)
{
  XLFrameClockHandleFrameEvent (helper->clock, event);
}

/* Much of the code below is only necessary in the xdg_toplevel
   role.  */



void
SyncHelperSetResizeCallback (SyncHelper *helper,
			     void (*resize_start) (void *, Bool),
			     Bool (*check_fast_forward) (void *))
{
  /* Set a resize callback.  It is called to begin a resize.  Upon
     being called, the sync helper becomes "frozen", and will not
     display frames until the next call to SyncHelperUpdate.  */

  helper->resize_callback = resize_start;
  helper->fast_forward_callback = check_fast_forward;
}

void
SyncHelperNoteConfigureEvent (SyncHelper *helper)
{
  /* Tell the frame clock about the arrival of a ConfigureNotify
     event.  This is used to determine whether a synchronization event
     is up-to-date.  */

  XLFrameClockNoteConfigure (helper->clock);
}

void
SyncHelperCheckFrameCallback (SyncHelper *helper)
{
  uint64_t time;

  /* Prevent deadlocks when the client is waiting for a frame callback
     while the frame clock is frozen, which can happen if it submits
     frame callbacks before calling Commit.  If the frame clock is frozen,
     meaning that a resize is in progress, generate a frame.  */

  time = ConsiderFrameTime (helper, -1);
  helper->frame_callback (helper->role, time / 1000);
}

void
SyncHelperClearPendingFrame (SyncHelper *helper)
{
  /* Clear any frame that is waiting to be displayed.  This should be
     called prior to a configure event for clients which must handle
     interactive resize.  */

  helper->flags &= ~FramePending;
}



#if 0

static void __attribute__((constructor))
SyncHelperSelftest (void)
{
  uint64_t timestampa, timestampb;

  timestampa = 1000 * 1000 + 500;
  timestampb = 1000 * 1000 + 550;

  XLAssert (TimestampGreaterThan (timestampb, timestampa));
  XLAssert (!TimestampGreaterThan (timestampa, timestampb));

  timestampa = (uint64_t) 0xffffffff * 1000 + 500;
  timestampb = (uint64_t) 0xffffffff * 1000 + 550;

  XLAssert (TimestampGreaterThan (timestampb, timestampa));
  XLAssert (!TimestampGreaterThan (timestampa, timestampb));

  timestampa = (uint64_t) 0xffffffff * 1000 + 500;
  timestampb = (((uint64_t) 0xffffffff) + 1) * 1000 + 500;

  XLAssert (TimestampGreaterThan (timestampb, timestampa));
  XLAssert (!TimestampGreaterThan (timestampa, timestampb));
}

#endif
