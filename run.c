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

#include <stdio.h>
#include <poll.h>
#include <alloca.h>

#include "compositor.h"

typedef struct _PollFd PollFd;

struct _PollFd
{
  /* The next and last writable fd in this chain.  */
  PollFd *next, *last;

  /* The file descriptor itself.  */
  int write_fd;

  /* Callback run with the fd number and data when the fd becomes
     writable or readable.  */
  void (*poll_callback) (int, void *, PollFd *);

  /* Data for the callback.  */
  void *data;

  /* The direction; 1 means write, 0 means read.  */
  int direction;
};

/* Number of file descriptors for which we are waiting for something
   to be written.  */
static int num_poll_fd;

/* Linked list of file descriptors and write callbacks.  */
static PollFd poll_fds;

WriteFd *
XLAddWriteFd (int fd, void *data, void (*poll_callback) (int, void *,
							 WriteFd *))
{
  WriteFd *record;

  record = XLMalloc (sizeof *record);
  record->next = poll_fds.next;
  record->last = &poll_fds;
  record->write_fd = fd;
  record->poll_callback = poll_callback;
  record->data = data;
  record->direction = 1;

  poll_fds.next->last = record;
  poll_fds.next = record;

  num_poll_fd++;

  return record;
}

ReadFd *
XLAddReadFd (int fd, void *data, void (*poll_callback) (int, void *,
							ReadFd *))
{
  WriteFd *record;

  record = XLMalloc (sizeof *record);
  record->next = poll_fds.next;
  record->last = &poll_fds;
  record->write_fd = fd;
  record->poll_callback = poll_callback;
  record->data = data;
  record->direction = 0;

  poll_fds.next->last = record;
  poll_fds.next = record;

  num_poll_fd++;

  return record;
}

void
XLRemoveWriteFd (WriteFd *fd)
{
  /* Mark this record as invalid.  Records cannot safely change while
     the event loop is in progress, so we remove all invalid records
     immediately before polling.  */
  fd->write_fd = -1;
}

void
XLRemoveReadFd (ReadFd *fd)
{
  /* Mark this record as invalid.  Records cannot safely change while
     the event loop is in progress, so we remove all invalid records
     immediately before polling.  */
  fd->write_fd = -1;
}

static void
RemoveWriteFd (WriteFd *fd)
{
  fd->next->last = fd->last;
  fd->last->next = fd->next;

  num_poll_fd--;
  XLFree (fd);
}

static void
HandleOneXEvent (XEvent *event)
{
  XLHandleOneXEventForDnd (event);

  /* Filter all non-GenericEvents through the input method
     infrastructure.  */
  if (event->type != GenericEvent
      && XFilterEvent (event, event->xany.window))
    return;

  if (XLHandleXEventForXdgSurfaces (event))
    return;

  if (HandleOneXEventForPictureRenderer (event))
    return;

  if (XLHandleXEventForXdgToplevels (event))
    return;

  if (XLHandleXEventForXdgPopups (event))
    return;

  if (XLHandleOneXEventForSeats (event))
    return;

  if (XLHandleOneXEventForIconSurfaces (event))
    return;

  if (XLHandleOneXEventForXData (event))
    return;

  if (XLHandleOneXEventForOutputs (event))
    return;

  if (XLHandleOneXEventForXSettings (event))
    return;

  if (HandleOneXEventForTime (event))
    return;

  if (XLHandleOneXEventForTest (event))
    return;
}

static void
ReadXEvents (void)
{
  XEvent event;

  while (XPending (compositor.display))
    {
      XNextEvent (compositor.display, &event);

      /* We failed to get event data for a generic event, so there's
	 no point in continuing.  */
      if (event.type == GenericEvent
	  && !XGetEventData (compositor.display, &event.xcookie))
	continue;

      if (!HookSelectionEvent (&event))
	HandleOneXEvent (&event);

      if (event.type == GenericEvent)
	XFreeEventData (compositor.display, &event.xcookie);
    }
}

static void
RunStep (void)
{
  int x_connection, wl_connection, rc, i, j;
  struct timespec timeout;
  struct pollfd *fds;
  PollFd **pollfds, *item, *last;

  fds = alloca (sizeof *fds * (num_poll_fd + 2));

  /* This is used as an optimization to not have to loop over the
     entire descriptor list twice.  */
  pollfds = alloca (sizeof *pollfds * num_poll_fd);

  /* Run timers.  This, and draining selection transfers, must be done
     before setting up poll file descriptors, since timer callbacks
     can change the write fd list.  */
  timeout = TimerCheck ();

  /* Drain complete selection transfers.  */
  FinishTransfers ();

  /* Disconnect clients that have experienced out-of-memory
     errors.  */
  ProcessPendingDisconnectClients ();

  /* FinishTransfers can potentially send events to Wayland clients
     and make X requests.  Flush after it is called.  */
  XFlush (compositor.display);
  wl_display_flush_clients (compositor.wl_display);

  /* Obtain the connections.  */
  x_connection = ConnectionNumber (compositor.display);
  wl_connection = wl_event_loop_get_fd (compositor.wl_event_loop);

  fds[0].fd = x_connection;
  fds[1].fd = wl_connection;
  fds[0].events = POLLIN;
  fds[1].events = POLLIN;
  fds[0].revents = 0;
  fds[1].revents = 0;

  /* Copy valid write file descriptors into the pollfd array, while
     removing invalid ones.  */
  item = poll_fds.next;
  i = 0;

  while (item != &poll_fds)
    {
      last = item;
      item = item->next;

      if (last->write_fd == -1)
	{
	  /* Remove this invalid pollfd.  */
	  RemoveWriteFd (last);
	  continue;
	}

      /* Otherwise, add the fd and bump i.  */

      fds[2 + i].fd = last->write_fd;
      fds[2 + i].events = POLLOUT;
      fds[2 + i].revents = 0;
      pollfds[i] = last;

      if (!last->direction)
	/* See https://www.greenend.org.uk/rjk/tech/poll.html for why
	   POLLHUP.  */
	fds[2 + i].events = POLLIN | POLLHUP;

      i += 1;
    }

  /* Handle any events already in the queue, which can happen if
     something inside ReadXEvents synced.  */
  if (XEventsQueued (compositor.display, QueuedAlready))
    {
      ReadXEvents ();

      XFlush (compositor.display);
      wl_display_flush_clients (compositor.wl_display);
    }

  /* Disconnect clients that have experienced out-of-memory
     errors.  */
  ProcessPendingDisconnectClients ();

  rc = ProcessPoll (fds, 2 + i, &timeout);

  if (rc > 0)
    {
      if (fds[0].revents & POLLIN)
	ReadXEvents ();

      if (fds[1].revents & POLLIN)
	wl_event_loop_dispatch (compositor.wl_event_loop, -1);

      /* Now see how many write fds are set.  */
      for (j = 0; j < i; ++j)
	{
	  if (fds[2 + j].revents & (POLLOUT | POLLIN | POLLHUP)
	      /* Check that pollfds[j] is still valid, and wasn't
		 removed while handling X events.  */
	      && pollfds[j]->write_fd != -1)
	    /* Then call the poll callback.  */
	    pollfds[j]->poll_callback (pollfds[j]->write_fd,
				       pollfds[j]->data, pollfds[j]);
	}
    }

  /* Disconnect clients that have experienced out-of-memory
     errors.  */
  ProcessPendingDisconnectClients ();
}

void __attribute__ ((noreturn))
XLRunCompositor (void)
{
  /* Set up the sentinel node for file descriptors that are being
     polled from.  */
  poll_fds.next = &poll_fds;
  poll_fds.last = &poll_fds;

  while (True)
    RunStep ();
}
