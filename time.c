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

#include "compositor.h"

/* The latest known time.  */
static Timestamp current_time;

/* Half a month; used as a threshold in various places.  */
#define HalfMonth (1U << 31)

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
