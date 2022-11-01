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
#include <stdlib.h>
#include <time.h>

#include "compositor.h"

typedef struct _FrameClockCallback FrameClockCallback;
typedef struct _CursorClockCallback CursorClockCallback;

enum
  {
    /* 150ms.  */
    MaxPresentationAge	  = 150000,
  };

/* Whether or not the compositor supports frame synchronization.  */

static Bool frame_sync_supported;

/* Timer used for cursor animations.  */

static Timer *cursor_clock;

/* How many cursors want cursor animations.  */

static int cursor_count;

struct _FrameClockCallback
{
  /* Function called once a frame is completely written to display and
     (ideally, whether or not this actually works depends on various
     different factors) enters vblank.  */
  void (*frame) (FrameClock *, void *);

  /* Data that function is called with.  */
  void *data;

  /* Next and last callbacks in this list.  */
  FrameClockCallback *next, *last;
};

struct _FrameClock
{
  /* List of frame clock callbacks.  */
  FrameClockCallback callbacks;

  /* Two sync counters.  */
  XSyncCounter primary_counter, secondary_counter;

  /* The value of the frame currently being drawn in this frame clock,
     and the value of the last frame that was marked as complete.  */
  uint64_t next_frame_id, finished_frame_id;

  /* A timer used as a fake synchronization source if frame
     synchronization is not supported.  */
  Timer *static_frame_timer;

  /* A timer used to end the next frame.  */
  Timer *end_frame_timer;

    /* Callback run when the frame is frozen.  */
  void (*freeze_callback) (void *);

  /* Data for that callback.  */
  void *freeze_callback_data;

  /* The wanted configure value.  */
  uint64_t configure_id;

  /* The time the last frame was drawn.  */
  uint64_t last_frame_time;

  /* Any pending frame synchronization counter value, or 0.  */
  uint64_t pending_sync_value;

  /* The last frame drawn for whom a _NET_WM_FRAME_TIMINGS message has
     not yet arrived.  */
  uint64_t frame_timings_id;

  /* The time the frame at frame_timings_id was drawn.  Used to
     compute the presentation time.  */
  uint64_t frame_timings_drawn_time;

  /* The last known presentation time.  */
  uint64_t last_presentation_time;

  /* The refresh interval.  */
  uint32_t refresh_interval;

  /* The delay between the start of vblank and the redraw point.  */
  uint32_t frame_delay;

  /* The number of configure events received affecting freeze, and the
     number of configure events that should be received after a freeze
     is put in place.  */
  uint32_t got_configure_count, pending_configure_count;

  /* Whether or not configury is in progress, and whether or not this
     is frozen, and whether or not the frame shouldn't actually be
     unfrozen until EndFrame.  */
  Bool need_configure, frozen, frozen_until_end_frame;

  /* Whether or not EndFrame was called after StartFrame.  */
  Bool end_frame_called;

  /* Whether or not we are waiting for a frame to be completely
     painted.  */
  Bool in_frame;

  /* Whether or not this frame clock should try to predict
     presentation times, in order to group frames together.  */
  Bool predict_refresh;
};

struct _CursorClockCallback
{
  /* Function called every time cursors should animate once.  */
  void (*frame) (void *, struct timespec);

  /* Data for that function.  */
  void *data;

  /* Next and last cursor clock callbacks.  */
  CursorClockCallback *next, *last;
};

/* List of cursor frame callbacks.  */

static CursorClockCallback cursor_callbacks;

static void
SetSyncCounter (XSyncCounter counter, uint64_t value)
{
  uint64_t low, high;
  XSyncValue sync_value;

  low = value & 0xffffffff;
  high = value >> 32;

  XSyncIntsToValue (&sync_value, low, high);
  XSyncSetCounter (compositor.display, counter,
		   sync_value);
}

static uint64_t
HighPrecisionTimestamp (struct timespec *clock)
{
  uint64_t timestamp;

  if (IntMultiplyWrapv (clock->tv_sec, 1000000, &timestamp)
      || IntAddWrapv (timestamp, clock->tv_nsec / 1000, &timestamp))
    /* Overflow.  */
    return 0;

  return timestamp;
}

static uint64_t
HighPrecisionTimestamp32 (struct timespec *clock)
{
  uint64_t timestamp, milliseconds;

  /* This function is like CurrentHighPrecisionTimestamp, but the X
     server time portion is limited to 32 bits.  First, the seconds
     are converted to milliseconds.  */
  if (IntMultiplyWrapv (clock->tv_sec, 1000, &milliseconds))
    return 0;

  /* Next, the nanosecond portion is also converted to
     milliseconds.  */
  if (IntAddWrapv (milliseconds, clock->tv_nsec / 1000000,
		   &milliseconds))
    return 0;

  /* Then, the milliseconds are truncated to 32 bits.  */
  milliseconds &= 0xffffffff;

  /* Finally, add the milliseconds to the timestamp.  */
  if (IntMultiplyWrapv (milliseconds, 1000, &timestamp))
    return 0;

  /* And add the remaining nsec portion.  */
  if (IntAddWrapv (timestamp, (clock->tv_nsec % 1000000) / 1000,
		   &timestamp))
    /* Overflow.  */
    return 0;

  return timestamp;
}

static Bool
HighPrecisionTimestampToTimespec (uint64_t timestamp,
				  struct timespec *timespec)
{
  uint64_t remainder, seconds;

  seconds = timestamp / 1000000;
  remainder = timestamp % 1000000;

  if (IntAddWrapv (0, seconds, &timespec->tv_sec))
    return False;

  /* We know that this cannot overflow tv_nsec, which is long int.  */
  timespec->tv_nsec = remainder * 1000;
  return True;
}

/* Forward declaration.  */

static void EndFrame (FrameClock *);

static void
HandleEndFrame (Timer *timer, void *data, struct timespec time)
{
  FrameClock *clock;

  clock = data;

  /* Now that the time allotted for the current frame has run out, end
     the frame.  */
  RemoveTimer (timer);
  clock->end_frame_timer = NULL;

  if (clock->end_frame_called)
    EndFrame (clock);
}

/* Forward declarations.  */

static void RunFrameCallbacks (FrameClock *);
static Bool StartFrame (FrameClock *, Bool, Bool);

static void
FreezeForValue (FrameClock *clock, uint64_t counter_value)
{
  Bool need_empty_frame;

  /* If it took too long (1 second at 60fps) to obtain the counter
     value, and said value is now out of date, don't do anything.  */

  if (clock->next_frame_id > counter_value)
    return;

  need_empty_frame = False;

  /* If ending a frame waits for PresentCompleteNotify, then the
     configure event after this freeze may have been put into effect
     by the time the freeze itself.  Start a new frame to bring up to
     date contents to the display.  */
  if (clock->pending_configure_count <= clock->got_configure_count)
    need_empty_frame = True;

  /* The frame clock is now frozen, and we will have to wait for a
     client to ack_configure and then commit something.  */

  if (clock->end_frame_timer)
    {
      /* End the frame now, and clear in_frame early.  */
      RemoveTimer (clock->end_frame_timer);
      clock->end_frame_timer = NULL;

      if (clock->end_frame_called)
	EndFrame (clock);
    }

  /* counter_value - 240 is the value seen by the compositor when the
     frame contents were frozen in response to a resize.  If it is
     less than finished_frame_id, run frame callbacks now, or clients
     like Chromium are confused and hang waiting for frame callbacks
     to be called.  */
  if (counter_value - 240 <= clock->finished_frame_id)
    RunFrameCallbacks (clock);

  /* The reason for clearing in_frame is that otherwise a future
     Commit after the configuration is acknowledged will not be able
     to start a new frame and restart the frame clock.  */
  clock->in_frame = False;
  clock->need_configure = True;
  clock->configure_id = counter_value;

  if (need_empty_frame)
    {
      /* Request a new frame and don't allow starting frames until it
	 finishes.  See above for why.  clock->in_frame is False for
	 now to really force the frame to happen.  */

      StartFrame (clock, True, False);
      EndFrame (clock);
    }
  else
    clock->frozen = True;

  return;
}

static void
PostEndFrame (FrameClock *clock)
{
  uint64_t target, fallback, now, additional;
  struct timespec timespec, current_time;

  XLAssert (clock->end_frame_timer == NULL);

  if (!clock->refresh_interval
      || !clock->last_presentation_time)
    return;

  /* Obtain the monotonic clock time.  */
  clock_gettime (CLOCK_MONOTONIC, &current_time);

  /* target is now the time the last frame was presented.  This is the
     end of a vertical blanking period. */
  target = clock->last_presentation_time;

  /* now is the current time.  */
  now = HighPrecisionTimestamp (&current_time);

  /* There is no additional offset to add to the time.  */
  additional = 0;

  /* If now is more than UINT32_MAX * 1000, then this timestamp may
     overflow the 32-bit X server time, depending on how the X
     compositing manager implements timestamp generation.  Generate a
     fallback timestamp to use in that situation.

     Use now << 10 instead of now / 1000; the difference is too small
     to be noticeable.  */
  if (now << 10 > UINT32_MAX)
    fallback = HighPrecisionTimestamp32 (&current_time);
  else
    fallback = 0;

  if (!now)
    return;

  /* If the last time the frame time was obtained was that long ago,
     return immediately.  */
  if (now - clock->last_presentation_time >= MaxPresentationAge)
    {
      if ((fallback - clock->last_presentation_time) <= MaxPresentationAge)
	{
	  /* Some compositors wrap around once the X server time
	     overflows the 32-bit Time type.  If now happens to be
	     within the limit after its millisecond portion is
	     truncated to 32 bits, continue, after setting the
	     additional value the difference between the truncated
	     value and the actual time.  */

	  additional = now - fallback;
	  now = fallback;
	}
      else
	return;
    }

  /* Keep adding the refresh interval until target becomes the
     presentation time of a frame in the future.  */

  while (target < now)
    {
      if (IntAddWrapv (target, clock->refresh_interval, &target))
	return;
    }

  /* The vertical blanking period itself can't actually be computed
     based on available data.  However, frame_delay must be inside the
     vertical blanking period for it to make any sense, so use it to
     compute the deadline instead.  Add about 200 us to the frame
     delay to compensate for the roundtrip time.  */
  target -= clock->frame_delay - 200;

  /* Add the remainder of now if it was probably truncated by the
     compositor.  */
  target += additional;

  /* Convert the high precision timestamp to a timespec.  */
  if (!HighPrecisionTimestampToTimespec (target, &timespec))
    return;

  /* Schedule the timer marking the end of this frame for the target
     time.  */
  clock->end_frame_timer = AddTimerWithBaseTime (HandleEndFrame,
						 clock,
						 /* Use no delay; this
						    timer will only
						    run once.  */
						 MakeTimespec (0, 0),
						 timespec);
}

static Bool
StartFrame (FrameClock *clock, Bool urgent, Bool predict)
{
  if (clock->frozen)
    return False;

  if (clock->frozen_until_end_frame)
    return False;

  if (clock->in_frame)
    {
      if (clock->end_frame_timer
	  /* If the end of the frame is still pending but EndFrame has
	     been called, then treat the frame as if it has just been
	     started.  */
	  && clock->end_frame_called)
	{
	  /* In addition, require another EndFrame for the frame to
	     end.  */
	  clock->end_frame_called = False;
	  return True;
	}

      /* Otherwise it genuinely is invalid to call StartFrame here, so
	 return False.  */
      return False;
    }

  if (clock->need_configure)
    {
      clock->next_frame_id = clock->configure_id;
      clock->finished_frame_id = 0;

      /* Don't start the end frame timer if this frame is being drawn
	 in response to configury.  */
      predict = True;
    }

  clock->in_frame = True;
  clock->end_frame_called = False;

  /* Set the clock to an odd value; if we want the compositor to
     redraw this frame immediately (since it is running late), make it
     so that value % 4 == 3.  Otherwise, make it so that value % 4 ==
     1.  */

  if (urgent)
    {
      if (clock->next_frame_id % 4 == 2)
	clock->next_frame_id += 1;
      else
	clock->next_frame_id += 3;
    }
  else
    {
      if (clock->next_frame_id % 4 == 3)
	clock->next_frame_id += 3;
      else
	clock->next_frame_id += 1;
    }

  /* If frame synchronization is not supported, setting the sync
     counter itself isn't necessary; the values are used as a flag to
     tell us whether or not a frame has been completely drawn.  */
  if (!frame_sync_supported)
    return True;

  SetSyncCounter (clock->secondary_counter,
		  clock->next_frame_id);

  if (clock->predict_refresh && predict)
    PostEndFrame (clock);

  clock->need_configure = False;
  return True;
}

static void
EndFrame (FrameClock *clock)
{
  if (clock->frozen)
    return;

  clock->frozen_until_end_frame = False;

  /* Signal that end_frame was called and it is now safe to finish the
     frame from the timer.  */
  clock->end_frame_called = True;

  if (!clock->in_frame
      /* If the end of the frame has already been signalled, this
	 function should just return instead of increasing the counter
	 to an odd value.  */
      || clock->finished_frame_id == clock->next_frame_id)
    return;

  if (clock->end_frame_timer)
    /* If the frame is ending at a predicted time, don't allow ending
       it manually.  */
    return;

  /* Signal to the compositor that the frame is now complete.  When
     the compositor finishes drawing the frame, a callback will be
     received.  */

  if (clock->next_frame_id % 4 == 3)
    clock->next_frame_id += 1;
  else
    clock->next_frame_id += 3;

  clock->finished_frame_id = clock->next_frame_id;

  /* The frame has ended.  Freeze the frame clock if there is a
     pending sync value.  */
  if (clock->pending_sync_value)
    FreezeForValue (clock, clock->pending_sync_value);
  clock->pending_sync_value = 0;

  if (!frame_sync_supported)
    return;

  SetSyncCounter (clock->secondary_counter, clock->next_frame_id);
}

static void
FreeFrameCallbacks (FrameClock *clock)
{
  FrameClockCallback *callback, *last;

  callback = clock->callbacks.next;

  while (callback != &clock->callbacks)
    {
      last = callback;
      callback = callback->next;

      XLFree (last);
    }

  clock->callbacks.next = &clock->callbacks;
  clock->callbacks.last = &clock->callbacks;
}

static void
RunFrameCallbacks (FrameClock *clock)
{
  FrameClockCallback *callback;

  callback = clock->callbacks.next;

  while (callback != &clock->callbacks)
    {
      callback->frame (clock, callback->data);
      callback = callback->next;
    }
}

static void
NoteFakeFrame (Timer *timer, void *data, struct timespec time)
{
  FrameClock *clock;

  clock = data;

  if (clock->in_frame
      && (clock->finished_frame_id == clock->next_frame_id))
    {
      clock->in_frame = False;
      RunFrameCallbacks (clock);
    }
}

void
XLFrameClockAfterFrame (FrameClock *clock,
			void (*frame_func) (FrameClock *, void *),
			void *data)
{
  FrameClockCallback *callback;

  callback = XLCalloc (1, sizeof *callback);

  callback->next = clock->callbacks.next;
  callback->last = &clock->callbacks;

  clock->callbacks.next->last = callback;
  clock->callbacks.next = callback;

  callback->data = data;
  callback->frame = frame_func;
}

Bool
XLFrameClockStartFrame (FrameClock *clock, Bool urgent)
{
  return StartFrame (clock, urgent, True);
}

void
XLFrameClockEndFrame (FrameClock *clock)
{
  EndFrame (clock);
}

Bool
XLFrameClockFrameInProgress (FrameClock *clock)
{
  if (clock->frozen_until_end_frame)
    /* Don't consider a frame as being in progress, since the frame
       counter has been incremented to freeze the display.  */
    return False;

  return clock->in_frame;
}

/* N.B. that this function is called from popups, where normal
   freezing does not work, as the window manager does not
   cooperate.  */

void
XLFrameClockFreeze (FrameClock *clock)
{
  /* Start a frame now, unless one is already in progress, in which
     case it suffices to get rid of the timer.  */
  if (!clock->end_frame_timer)
    StartFrame (clock, False, False);
  else
    {
      RemoveTimer (clock->end_frame_timer);
      clock->end_frame_timer = NULL;
    }

  /* Don't unfreeze until the next EndFrame.  */
  clock->frozen_until_end_frame = True;
  clock->frozen = True;
}

void
XLFrameClockHandleFrameEvent (FrameClock *clock, XEvent *event)
{
  uint64_t low, high, value;

  if (event->xclient.message_type == _NET_WM_FRAME_DRAWN)
    {
      /* Mask these values against 0xffffffff, since Xlib sign-extends
	 these 32 bit values to fit into long, which can be 64
	 bits.  */
      low = event->xclient.data.l[0] & 0xffffffff;
      high = event->xclient.data.l[1] & 0xffffffff;
      value = low | (high << 32);

      if (value == clock->finished_frame_id
	  && clock->in_frame
	  /* If this means the frame has been completely drawn, then
	     clear in_frame and run frame callbacks to i.e. draw any
	     late frame.  */
	  && (clock->finished_frame_id == clock->next_frame_id))
	{
	  /* Record the time at which the frame was drawn.  */
	  low = event->xclient.data.l[2] & 0xffffffff;
	  high = event->xclient.data.l[3] & 0xffffffff;

	  /* Actually compute the time and save it.  */
	  clock->last_frame_time = low | (high << 32);

	  /* Run any frame callbacks, since drawing has finished.  */
	  clock->in_frame = False;
	  RunFrameCallbacks (clock);

	  if (clock->frame_timings_id == -1)
	    {
	      /* Wait for the frame's presentation time to arrive,
		 unless we are already waiting on a previous
		 frame.  */
	      clock->frame_timings_id = value;

	      /* Also save the frame drawn time.  */
	      clock->frame_timings_drawn_time = clock->last_frame_time;
	    }
	}
    }

  if (event->xclient.message_type == _NET_WM_FRAME_TIMINGS)
    {
      /* Check that the frame timings are up to date.  */
      low = event->xclient.data.l[0] & 0xffffffff;
      high = event->xclient.data.l[1] & 0xffffffff;
      value = low | (high << 32);

      if (value != clock->frame_timings_id)
	/* They are not.  */
	return;

      /* The timings message has arrived, so clear
	 frame_timings_id.  */
      clock->frame_timings_id = -1;

      /* And set the last known presentation time.  */
      clock->last_presentation_time = (clock->frame_timings_drawn_time
				       + event->xclient.data.l[2]);

      /* Save the presentation time and refresh interval.  There is no
	 need to mask these values, since they are being put into
	 (u)int32_t.  */
      clock->refresh_interval = event->xclient.data.l[3];
      clock->frame_delay = event->xclient.data.l[4];

      if (clock->refresh_interval & (1U << 31)
	  || clock->frame_delay == 0x80000000)
	{
	  /* This means frame timing information is unavailable.  */
	  clock->refresh_interval = 0;
	  clock->frame_delay = 0;
	  clock->last_presentation_time = 0;
	}
    }

  if (event->xclient.message_type == WM_PROTOCOLS
      && event->xclient.data.l[0] == _NET_WM_SYNC_REQUEST
      && event->xclient.data.l[4] == 1)
    {
      low = event->xclient.data.l[2];
      high = event->xclient.data.l[3];
      value = low | (high << 32);

      /* Ensure that value is even.  */
      if (value % 2)
	value += 1;

      /* Set the number of configure events that should be received by
	 the time the freeze is put into effect.  */
      clock->pending_configure_count
	= clock->got_configure_count + 1;

      /* If a frame is in progress, postpone this frame
	 synchronization message.  */
      if (clock->in_frame && !clock->end_frame_called)
	clock->pending_sync_value = value;
      else
	FreezeForValue (clock, value);

      if (clock->freeze_callback)
	/* Call the freeze callback in any case.  */
	clock->freeze_callback (clock->freeze_callback_data);
    }
}

void
XLFreeFrameClock (FrameClock *clock)
{
  FreeFrameCallbacks (clock);

  if (frame_sync_supported)
    {
      XSyncDestroyCounter (compositor.display,
			   clock->primary_counter);
      XSyncDestroyCounter (compositor.display,
			   clock->secondary_counter);
    }
  else
    RemoveTimer (clock->static_frame_timer);

  if (clock->end_frame_timer)
    RemoveTimer (clock->end_frame_timer);

  XLFree (clock);
}

FrameClock *
XLMakeFrameClockForWindow (Window window)
{
  FrameClock *clock;
  XSyncValue initial_value;
  struct timespec default_refresh_rate;

  clock = XLCalloc (1, sizeof *clock);
  clock->next_frame_id = 0;

  /* Set this to an invalid value.  */
  clock->frame_timings_id = -1;

  XLOutputGetMinRefresh (&default_refresh_rate);

  XSyncIntToValue (&initial_value, 0);

  if (frame_sync_supported)
    {
      clock->primary_counter
	= XSyncCreateCounter (compositor.display,
			      initial_value);
      clock->secondary_counter
	= XSyncCreateCounter (compositor.display,
			      initial_value);
    }
  else
    clock->static_frame_timer
      = AddTimer (NoteFakeFrame, clock,
		  default_refresh_rate);

  /* Initialize sentinel link.  */
  clock->callbacks.next = &clock->callbacks;
  clock->callbacks.last = &clock->callbacks;

  if (frame_sync_supported)
    XChangeProperty (compositor.display, window,
		     _NET_WM_SYNC_REQUEST_COUNTER, XA_CARDINAL, 32,
		     PropModeReplace,
		     (unsigned char *) &clock->primary_counter, 2);

  if (getenv ("DEBUG_REFRESH_PREDICTION"))
    clock->predict_refresh = True;

  return clock;
}

void
XLFrameClockUnfreeze (FrameClock *clock)
{
  clock->frozen = False;
}

Bool
XLFrameClockNeedConfigure (FrameClock *clock)
{
  return clock->need_configure;
}

Bool
XLFrameClockSyncSupported (void)
{
  return frame_sync_supported;
}

Bool
XLFrameClockIsFrozen (FrameClock *clock)
{
  return clock->frozen;
}

Bool
XLFrameClockCanBatch (FrameClock *clock)
{
  /* Hmm... this doesn't seem very accurate.  Maybe it would be a
     better to test against the target presentation time instead.  */

  return clock->end_frame_timer != NULL;
}

void
XLFrameClockSetPredictRefresh (FrameClock *clock)
{
  /* This sets whether or not the frame clock should try to predict
     when the compositing manager will draw a frame to display.

     It is useful when multiple subsurfaces are trying to start
     subframes on the same toplevel at the same time; in that case,
     the subframes will be grouped into a single synchronized frame,
     instead of being postponed.  */

  if (compositor.server_time_monotonic)
    clock->predict_refresh = True;
}

void
XLFrameClockDisablePredictRefresh (FrameClock *clock)
{
  /* This sets whether or not the frame clock should try to predict
     when the compositing manager will draw a frame to display.

     It is useful when multiple subsurfaces are trying to start
     subframes on the same toplevel at the same time; in that case,
     the subframes will be grouped into a single synchronized frame,
     instead of being postponed.  */

  clock->predict_refresh = False;
}

void
XLFrameClockSetFreezeCallback (FrameClock *clock, void (*callback) (void *),
			       void *data)
{
  clock->freeze_callback = callback;
  clock->freeze_callback_data = data;
}

uint64_t
XLFrameClockGetFrameTime (FrameClock *clock)
{
  /* Only return the time if it is actually a valid clock time.  */
  if (!compositor.server_time_monotonic)
    return 0;

  return clock->last_frame_time;
}

void
XLFrameClockNoteConfigure (FrameClock *clock)
{
  /* This value is to track resize event validity.  */
  clock->got_configure_count += 1;
}


/* Cursor animation clock-related functions.  */

static void
NoteCursorFrame (Timer *timer, void *data, struct timespec time)
{
  CursorClockCallback *callback;

  callback = cursor_callbacks.next;

  while (callback != &cursor_callbacks)
    {
      callback->frame (callback->data, time);
      callback = callback->next;
    }
}

void *
XLAddCursorClockCallback (void (*frame_func) (void *, struct timespec),
			  void *data)
{
  CursorClockCallback *callback;

  callback = XLMalloc (sizeof *callback);

  callback->next = cursor_callbacks.next;
  callback->last = &cursor_callbacks;

  cursor_callbacks.next->last = callback;
  cursor_callbacks.next = callback;

  callback->frame = frame_func;
  callback->data = data;

  return callback;
}

void
XLStopCursorClockCallback (void *key)
{
  CursorClockCallback *callback;

  callback = key;

  /* First, make the list skip past CALLBACK.  */
  callback->last->next = callback->next;
  callback->next->last = callback->last;

  /* Then, free CALLBACK.  */
  XLFree (callback);
}

void
XLStartCursorClock (void)
{
  struct timespec cursor_refresh_rate;

  if (cursor_count++)
    return;

  cursor_refresh_rate.tv_sec = 0;
  cursor_refresh_rate.tv_nsec = 60000000;
  cursor_clock = AddTimer (NoteCursorFrame, NULL,
			   cursor_refresh_rate);
}

void
XLStopCursorClock (void)
{
  if (--cursor_count)
    return;

  RemoveTimer (cursor_clock);
  cursor_clock = NULL;
}

void
XLInitFrameClock (void)
{
  if (!getenv ("DISABLE_FRAME_SYNCHRONIZATION"))
    frame_sync_supported = XLWmSupportsHint (_NET_WM_FRAME_DRAWN);

  /* Initialize cursor callbacks.  */
  cursor_callbacks.next = &cursor_callbacks;
  cursor_callbacks.last = &cursor_callbacks;
}
