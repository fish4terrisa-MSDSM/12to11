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

#include <string.h>
#include <stdio.h>

#include "compositor.h"

#include <X11/extensions/sync.h>

/* The latest known time.  */
static Timestamp current_time;

/* Alarms used to keep track of the server time.  */
static XSyncAlarm alarm_a, alarm_b;

/* The server time counter.  */
static XSyncCounter counter;

/* The event and error bases of the synchronization extension.  */
static int xsync_event_base, xsync_error_base;

/* Half a month; used as a threshold in various places.  */
#define HalfMonth (1U << 31)

/* The max value of Time.  */
#define MaxTime 0xffffffff

/* The protocol translator can run for more than 48 days.  That makes
   normal X timestamp handling unsafe, as the X server wraps around
   timestamps after that much time.  This function creates a Timestamp
   from the server time while handling overflow.  */

Timestamp
TimestampFromServerTime (Time time)
{
  if (time >= current_time.milliseconds)
    {
      /* No overflow happened.  */
      current_time.milliseconds = time;
      return current_time;
    }

  /* The server time wrapped around.  */
  current_time.months++;
  current_time.milliseconds = time;

  return current_time;
}

Timestamp
TimestampFromClientTime (Time time)
{
  Timestamp timestamp;

  timestamp.months = current_time.months;
  timestamp.milliseconds = time;

  /* Create a timestamp from a Time that may lie in the past.
     Compensate for wraparound if the difference between the
     timestamps is more than half a month.  */

  if (time < current_time.milliseconds
      && (current_time.milliseconds - time) >= HalfMonth)
    timestamp.months += 1;
  else if (time > current_time.milliseconds
	   && (time - current_time.milliseconds) >= HalfMonth)
    timestamp.months -= 1;

  return timestamp;
}

TimestampDifference
CompareTimestamps (Timestamp a, Timestamp b)
{
  if (a.months < b.months)
    return Earlier;

  if (a.months > b.months)
    return Later;

  if (a.milliseconds < b.milliseconds)
    return Earlier;

  if (a.milliseconds > b.milliseconds)
    return Later;

  return Same;
}

TimestampDifference
CompareTimeWith (Time a, Timestamp b)
{
  return CompareTimestamps (TimestampFromClientTime (a), b);
}


/* Timestamp tracking.  The code below treats INT64 as unsigned, since
   that won't overflow until long in the far future.  */

static XSyncCounter
FindSystemCounter (const char *name)
{
  XSyncSystemCounter *system_counters;
  int i, num_counters;
  XSyncCounter counter;

  num_counters = 0;
  system_counters = XSyncListSystemCounters (compositor.display,
					     &num_counters);
  counter = None;

  for (i = 0; i < num_counters; ++i)
    {
      if (!strcmp (system_counters[i].name, name))
	{
	  counter = system_counters[i].counter;
	  break;
	}

      /* Continue looking at the next counter.  */
    }

  if (system_counters)
    XSyncFreeSystemCounterList (system_counters);

  return counter;
}

static uint64_t
ValueToScalar (XSyncValue value)
{
  uint64_t low, high;

  low = XSyncValueLow32 (value);
  high = XSyncValueHigh32 (value);

  return low | (high << 32);
}

static void
ScalarToValue (uint64_t scalar, XSyncValue *value)
{
  XSyncIntsToValue (value, scalar & 0xffffffff, scalar >> 32);
}

static void
StartAlarms (XSyncCounter counter, XSyncValue current_value)
{
  uint64_t scalar_value, target;
  XSyncTrigger trigger;
  XSyncAlarmAttributes attributes;
  unsigned long value_mask;

  scalar_value = ValueToScalar (current_value);

  /* Delete existing alarms.  */

  if (alarm_a)
    XSyncDestroyAlarm (compositor.display, alarm_a);

  if (alarm_b)
    XSyncDestroyAlarm (compositor.display, alarm_b);

  value_mask = (XSyncCACounter | XSyncCATestType
		| XSyncCAValue | XSyncCAEvents);

  /* Start the first kind of alarm.  This alarm assumes that the
     counter does wrap around along with the server time.

     The protocol allows for more kinds of server behavior, but all
     servers either implement the counter as one that wraps around
     after Time ends, as defined here... */

  if (scalar_value >= HalfMonth)
    {
      /* value exceeds HalfMonth.  Wait for value to overflow back to
	 0.  */
      trigger.counter = counter;
      trigger.test_type = XSyncNegativeComparison;
      trigger.wait_value = current_value;

      /* Set the trigger and ask for events.  */
      attributes.trigger = trigger;
      attributes.events = True;
      XSyncIntToValue (&attributes.delta, -1);

      /* Create the alarm.  */
      alarm_a = XSyncCreateAlarm (compositor.display,
				  value_mask | XSyncCADelta,
				  &attributes);
      XSync (compositor.display, False);
    }
  else
    {
      /* value is not yet HalfMonth.  Wait for value to exceed
	 HalfMonth - 1.  */
      trigger.counter = counter;
      trigger.test_type = XSyncPositiveComparison;
      ScalarToValue (HalfMonth, &trigger.wait_value);

      /* Set the trigger and ask for events.  */
      attributes.trigger = trigger;
      attributes.events = True;

      /* Create the alarm.  */
      alarm_a = XSyncCreateAlarm (compositor.display,
				  value_mask, &attributes);
    }

  /* ...or the counter increases indefinitely, with its lower 32 bits
     representing the server time, which this counter takes into
     account.  */
  if ((scalar_value & MaxTime) >= HalfMonth)
    {
      /* The time exceeds HalfMonth.  Wait for the value to overflow
	 time again.  */
      target = (scalar_value & ~MaxTime) + MaxTime + 1;
      trigger.counter = counter;
      trigger.test_type = XSyncPositiveComparison;
      ScalarToValue (target, &trigger.wait_value);

      /* Set the trigger and ask for events.  */
      attributes.trigger = trigger;
      attributes.events = True;

      /* Create the alarm.  */
      alarm_b = XSyncCreateAlarm (compositor.display,
				  value_mask, &attributes);
    }
  else
    {
      /* The time is not yet HalfMonth.  Wait for the time to exceed
	 HalfMonth - 1.  */
      target = (scalar_value & ~MaxTime) + HalfMonth;
      trigger.counter = counter;
      trigger.test_type = XSyncPositiveComparison;
      ScalarToValue (target, &trigger.wait_value);

      /* Set the trigger and ask for events.  */
      attributes.trigger = trigger;
      attributes.events = True;

      /* Create the alarm.  */
      alarm_b = XSyncCreateAlarm (compositor.display,
				  value_mask, &attributes);
    }

  /* Now wait for alarm notifications to arrive.  */
}

static Bool
HandleAlarmNotify (XSyncAlarmNotifyEvent *notify)
{
  if (notify->alarm != alarm_a
      || notify->alarm != alarm_b)
    /* We are not interested in this outdated or irrelevant alarm.  */
    return False;

  /* First, synchronize our local time with the server time in the
     notification.  */
  TimestampFromServerTime (notify->time);

  /* Next, recreate the alarms for the new time.  */
  StartAlarms (counter, notify->counter_value);
  return True;
}

Bool
HandleOneXEventForTime (XEvent *event)
{
  if (event->type == xsync_event_base + XSyncAlarmNotify)
    return HandleAlarmNotify ((XSyncAlarmNotifyEvent *) event);

  return False;
}

void
InitTime (void)
{
  XSyncValue value;
  Bool supported;
  int xsync_major, xsync_minor;

  supported = XSyncQueryExtension (compositor.display,
				   &xsync_event_base,
				   &xsync_error_base);

  if (supported)
    supported = XSyncInitialize (compositor.display,
				 &xsync_major, &xsync_minor);

  if (!supported)
    {
      fprintf (stderr, "A compatible version of the synchronization"
	       " extension was not found\n");
      exit (1);
    }

  if (xsync_major < 3 || (xsync_major == 3 && xsync_minor < 1))
    {
      fprintf (stderr, "Sync fences are not supported by this X server\n");
      exit (1);
    }

  /* Initialize server timestamp tracking.  In order for server time
     accounting to be absolutely reliable, we must receive an event
     detailing each change every time it reaches HalfMonth and 0.  Set
     up multiple counter alarms to do that.  */

  counter = FindSystemCounter ("SERVERTIME");

  if (!counter)
    fprintf (stderr, "Server missing required system counter SERVERTIME\n");
  else
    {
      /* Now, obtain the current value of the counter.  This cannot
	 fail without calling the error (or IO error) handler.  */
      XSyncQueryCounter (compositor.display, counter, &value);

      /* Start the alarms.  */
      StartAlarms (counter, value);
    }
}
