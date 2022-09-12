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

/* Some of this file was taken from timespec-add.c and timespec-sub.c,
   part of gnulib, written by Paul Eggert <eggert@cs.ucla.edu>.  */

#include "compositor.h"

/* Linked list of all timers.  */
static Timer all_timers;

struct _Timer
{
  /* The next and last timers in this list.  */
  Timer *next, *last;

  /* The repeat of this timer.  */
  struct timespec repeat;

  /* The next time this timer should be run.  */
  struct timespec next_time;

  /* The function that should be called when the timer is run.  */
  void (*function) (Timer *, void *, struct timespec);

  /* User data associated with the timer.  */
  void *timer_data;
};

struct timespec
CurrentTimespec (void)
{
  struct timespec timespec;

  clock_gettime (CLOCK_MONOTONIC, &timespec);
  return timespec;
}

struct timespec
MakeTimespec (time_t s, long int ns)
{
  struct timespec r;

  r.tv_sec = s;
  r.tv_nsec = ns;

  return r;
}

int
TimespecCmp (struct timespec a, struct timespec b)
{
  return (2 * SafeCmp (a.tv_sec, b.tv_sec)
	  + SafeCmp (a.tv_nsec, b.tv_nsec));
}

struct timespec
TimespecAdd (struct timespec a, struct timespec b)
{
  time_t rs, bs, bs1;
  int ns, nsd, rns;

  rs = a.tv_sec;
  bs = b.tv_sec;
  ns = a.tv_nsec + b.tv_nsec;
  nsd = ns - 1000000000;
  rns = ns;

  if (0 < nsd)
    {
      rns = nsd;

      if (!IntAddWrapv (bs, 1, &bs1))
	bs = bs1;
      else if (rs < 0)
	rs++;
      else
	goto high_overflow;
    }

  if (IntAddWrapv (rs, bs, &rs))
    {
      if (bs < 0)
	{
	  rs = TypeMinimum (time_t);
	  rns = 0;
	}
      else
	{
	high_overflow:
	  rs = TypeMaximum (time_t);
	  rns = 1000000000 - 1;
	}
    }

  return MakeTimespec (rs, rns);
}

struct timespec
TimespecSub (struct timespec a, struct timespec b)
{
  time_t rs, bs, bs1;
  int ns, rns;

  rs = a.tv_sec;
  bs = b.tv_sec;
  ns = a.tv_nsec - b.tv_nsec;
  rns = ns;

  if (ns < 0)
    {
      rns = ns + 1000000000;
      if (!IntAddWrapv (bs, 1, &bs1))
	bs = bs1;
      else if (- TypeIsSigned (time_t) < rs)
	rs--;
      else
	goto low_overflow;
    }

  if (IntSubtractWrapv (rs, bs, &rs))
    {
      if (0 < bs)
	{
	low_overflow:
	  rs = TypeMinimum (time_t);
	  rns = 0;
	}
      else
	{
	  rs = TypeMaximum (time_t);
	  rns = 1000000000 - 1;
	}
    }

  return MakeTimespec (rs, rns);
}

Timer *
AddTimer (void (*function) (Timer *, void *, struct timespec),
	  void *data, struct timespec delay)
{
  Timer *timer;

  timer = XLMalloc (sizeof *timer);
  timer->function = function;
  timer->timer_data = data;
  timer->repeat = delay;
  timer->next_time = TimespecAdd (CurrentTimespec (),
				  delay);

  /* Chain the timer onto our list of timers.  */
  timer->next = all_timers.next;
  timer->last = &all_timers;

  all_timers.next->last = timer;
  all_timers.next = timer;

  return timer;
}

Timer *
AddTimerWithBaseTime (void (*function) (Timer *, void *, struct timespec),
		      void *data, struct timespec delay, struct timespec base)
{
  Timer *timer;

  timer = XLMalloc (sizeof *timer);
  timer->function = function;
  timer->timer_data = data;
  timer->repeat = delay;
  timer->next_time = TimespecAdd (base, delay);

  /* Chain the timer onto our list of timers.  */
  timer->next = all_timers.next;
  timer->last = &all_timers;

  all_timers.next->last = timer;
  all_timers.next = timer;

  return timer;
}

void
RemoveTimer (Timer *timer)
{
  /* Start by removing the timer from the list of timers.  This is
     only safe inside a timer callback our outside TimerCheck.  */
  timer->next->last = timer->last;
  timer->last->next = timer->next;

  /* Then, free the timer.  */
  XLFree (timer);
}

void
RetimeTimer (Timer *timer)
{
  timer->next_time = TimespecAdd (CurrentTimespec (),
				  timer->repeat);
}

struct timespec
TimerCheck (void)
{
  struct timespec now, wait, temp;
  Timer *timer, *next;
  Bool flag;

  now = CurrentTimespec ();
  wait = MakeTimespec (TypeMaximum (time_t),
		       1000000000 - 1);
  timer = all_timers.next;

  while (timer != &all_timers)
    {
      /* Move the list forward first, so the timer callback can
	 safely remove itself from the list.  */
      next = timer->next;
      flag = False;

      if (TimespecCmp (timer->next_time, now) <= 0)
	{
	  timer->next_time = TimespecAdd (timer->next_time,
					  timer->repeat);
	  flag = True;
	}

      temp = TimespecSub (timer->next_time, now);

      if (TimespecCmp (temp, wait) < 0)
	/* Wait is the time to wait until the next timer might
	   fire.  */
	wait = temp;

      if (flag)
	/* Run this function here instead, since it might remove the
	   timer from the list.  */
	timer->function (timer, timer->timer_data, now);

      timer = next;
    }

  return wait;
}

void
XLInitTimers (void)
{
  all_timers.next = &all_timers;
  all_timers.last = &all_timers;
}
