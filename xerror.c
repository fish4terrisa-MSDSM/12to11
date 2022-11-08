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

#include "compositor.h"

#include <X11/extensions/XInput.h>

typedef enum _ClientMemoryCategory ClientMemoryCategory;

/* See the comment in HandleBadAlloc for the meaning of these
   enumerators.  */
enum _ClientMemoryCategory
  {
    MemoryCategoryI,
    MemoryCategoryII,
    MemoryCategoryIII,
    MemoryCategoryIV,
    MemoryCategoryV,
  };

/* X error handling routines.  The entry point into this code is
   CatchXErrors, which starts catching X errors in the following code,
   and UncatchXErrors, which syncs (if necessary) and saves any
   received XErrorEvent into a provided buffer.

   This code is not reentrant since it doesn't have to take care of
   many complicated scenarios that the Emacs code needs.  */

/* First request from which errors should be caught.  -1 if we are not
   currently catching errors.  */
static long long first_error_req;

/* XErrorEvent any error that happens while errors are being caught
   will be saved in.  */
static XErrorEvent error;

/* True if any error was caught.  */
static Bool error_caught;

/* True if any BadAlloc error was encountered.  */
static Bool bad_alloc_experienced;

/* The serial of the last BadAlloc request.  */
static unsigned long next_bad_alloc_serial;

/* Whether or not program execution is currently inside the bad alloc
   handler.  */
static Bool inside_bad_alloc_handler;

/* Clients that are pending disconnect.  */
static XLList *pending_disconnect_clients;

void
CatchXErrors (void)
{
  first_error_req = XNextRequest (compositor.display);
  error_caught = False;
}

Bool
UncatchXErrors (XErrorEvent *event)
{
  /* Try to avoid syncing to obtain errors if we know none could have
     been generated, because either no request has been made, or all
     requests have been processed.  */
  if ((LastKnownRequestProcessed (compositor.display)
       != XNextRequest (compositor.display) - 1)
      && (NextRequest (compositor.display)
	  > first_error_req))
    /* If none of those conditions apply, catch errors now.  */
    XSync (compositor.display, False);

  first_error_req = -1;

  if (!event)
    return error_caught;

  if (!error_caught)
    return False;

  *event = error;
  return True;
}

void
ReleaseClientData (ClientErrorData *data)
{
  if (--data->refcount)
    return;

  XLFree (data);
}

static void
HandleClientDestroy (struct wl_listener *listener, void *data)
{
  struct wl_client *client;

  /* The client has been destroyed.  Remove it from the list of
     clients pending destruction.  */
  client = data;
  pending_disconnect_clients
    = XLListRemove (pending_disconnect_clients, client);

  /* listener is actually the ClientErrorData.  */
  ReleaseClientData ((ClientErrorData *) listener);
}

ClientErrorData *
ErrorDataForClient (struct wl_client *client)
{
  struct wl_listener *listener;
  ClientErrorData *data;

  listener = wl_client_get_destroy_listener (client,
					     HandleClientDestroy);

  if (listener)
    return (ClientErrorData *) listener;

  /* Allocate the data and set it as the client's destroy
     listener.  */

  data = XLMalloc (sizeof *data);
  data->listener.notify = HandleClientDestroy;
  data->n_pixels = 0;

  /* Add a reference to the data.  */
  data->refcount = 1;

  wl_client_add_destroy_listener (client, &data->listener);
  return data;
}

static void
CategorizeClients (struct wl_list *client_list,
		   struct wl_client **clients,
		   ClientMemoryCategory *categories,
		   ClientMemoryCategory *max_category)
{
  struct wl_list *list;
  struct wl_client *client;
  ClientErrorData *data;
  uint64_t total;
  int i;

  total = 0;
  i = 0;
  list = client_list;

  /* The heuristics here are not intended to handle every out of
     memory situation.  Rather, they are designed to handle BadAlloc
     errors caused by clients recently allocating unreasonable chunks
     of memory from the server.  */

  /* Compute the total number of pixels allocated.  */

  wl_client_for_each (client, list)
    {
      data = ErrorDataForClient (client);

      if (IntAddWrapv (total, data->n_pixels, &total))
	total = UINT64_MAX;
    }

  list = client_list;

  wl_client_for_each (client, list)
    {
      data = ErrorDataForClient (client);

      if (data->n_pixels <= total * 0.05)
	{
	  if (*max_category < MemoryCategoryI)
	    *max_category = MemoryCategoryI;

	  categories[i++] = MemoryCategoryI;
	  clients[i - 1] = client;
	}
      else if (data->n_pixels <= total * 0.10)
	{
	  if (*max_category < MemoryCategoryII)
	    *max_category = MemoryCategoryII;

	  categories[i++] = MemoryCategoryII;
	  clients[i - 1] = client;
	}
      else if (data->n_pixels <= total * 0.20)
	{
	  if (*max_category < MemoryCategoryIII)
	    *max_category = MemoryCategoryIII;

	  categories[i++] = MemoryCategoryIII;
	  clients[i - 1] = client;
	}
      else if (data->n_pixels <= total / 2)
	{
	  if (*max_category < MemoryCategoryIV)
	    *max_category = MemoryCategoryIV;

	  categories[i++] = MemoryCategoryIV;
	  clients[i - 1] = client;
	}
      else
	{
	  if (*max_category < MemoryCategoryV)
	    *max_category = MemoryCategoryV;

	  categories[i++] = MemoryCategoryV;
	  clients[i - 1] = client;
	}

      fprintf (stderr,
	       "%p %"PRIu64" %"PRIu64" %u\n", client, total, data->n_pixels,
	       categories[i - 1]);
    }
}

static void
SavePendingDisconnectClient (struct wl_client *client)
{
  XLList *list;

  /* Never link the same client onto the list twice.  */

  list = pending_disconnect_clients;
  for (; list; list = list->next)
    {
      if (list->data == client)
	return;
    }

  pending_disconnect_clients
    = XLListPrepend (pending_disconnect_clients, client);
}

static void
DisconnectOneClient (void *data)
{
  wl_client_post_no_memory (data);
}

void
ProcessPendingDisconnectClients (void)
{
  XLList *head;

  /* Unlink the list onto head.  */
  head = pending_disconnect_clients;
  pending_disconnect_clients = NULL;

  if (head)
    XLListFree (head, DisconnectOneClient);

  /* Ignore future BadAlloc errors caused by requests generated before
     client disconnects were processed.  */
  next_bad_alloc_serial
    = XNextRequest (compositor.display);
}

static void
HandleBadAlloc (XErrorEvent *event)
{
  struct wl_client **clients_by_category;
  struct wl_list *client_list;
  int num_clients, i;
  char buf[256];
  ClientMemoryCategory *categories, max_category;

  if (event->serial < next_bad_alloc_serial)
    return;

  if (inside_bad_alloc_handler)
    /* This function is not reentrant.  */
    return;

  /* Handle a BadAlloc error.  For efficiency reasons, errors are not
     caught around events (CreatePixmap, CreateWindow, etc) that are
     likely to raise BadAlloc errors upon ridiculous requests from
     clients do not have errors caught around them.

     Upon such an error occuring, clients are sorted by the amount of
     pixmap they have allocated.  Each client has its own "badness"
     score for each pixel of pixmap data allocated on its behalf.

     Each client is organized by the percentage of the total badness
     it takes up.  Those clients are then categorized as follows:

       I.   Clients occupying between 0 to 5 percent of the total
            score.
       II.  Clients occupying between 6 to 10 percent of the total
            score.
       III. Clients occupying between 11 to 20 percent of the total
            score.
       IV.  Clients occupying more than 20 percent of the total score.
       V.   Clients occupying more than 50 percent of the total score.

     Finally, all clients falling in the bottom-most category that
     exists are disconnected with out-of-memory errors.  */

  client_list = wl_display_get_client_list (compositor.wl_display);

  if (wl_list_empty (client_list))
    {
      /* If there are no clients, just exit immediately.  */
      XGetErrorText (compositor.display, event->error_code,
		     buf, sizeof buf);
      fprintf (stderr, "X protocol error: %s on protocol request %d\n",
	       buf, event->request_code);
      exit (70);
    }

  /* Count the number of clients.  */
  num_clients = wl_list_length (client_list);

  /* Allocate buffers to hold the client data.  */
  categories
    = alloca (sizeof *categories * num_clients);
  clients_by_category
    = alloca (sizeof *clients_by_category * num_clients);
  max_category
    = MemoryCategoryI;

  /* Organize the clients by category.  */
  CategorizeClients (client_list, clients_by_category,
		     categories, &max_category);

  /* Ignore future BadAlloc errors caused by requests generated before
     this BadAlloc error was processed.  */
  next_bad_alloc_serial = XNextRequest (compositor.display);

  /* Prevent recursive invocations of this error handler when XSync is
     called by resource destructors.  */
  inside_bad_alloc_handler = True;

  /* Now disconnect each client fitting that category.
     wl_client_post_no_memory can call Xlib functions through resource
     destructors, so each client is actually put on a queue that is
     drained in RunStep.  */
  for (i = 0; i < num_clients; ++i)
    {
      if (categories[i] == max_category)
        SavePendingDisconnectClient (clients_by_category[i]);
    }

  /* At this point, start ignoring all BadDrawable, BadWindow,
     BadPixmap and BadPicture errors.  Whatever resource was supposed
     to be created could not be created, so trying to destroy it as
     part of client destruction will fail.  */
  bad_alloc_experienced = True;
  inside_bad_alloc_handler = False;
}

static int
ErrorHandler (Display *display, XErrorEvent *event)
{
  char buf[256];
  unsigned long next_request;

  /* Reset fields that overflowed.  */
  next_request = XNextRequest (compositor.display);

  if (first_error_req != -1
      && next_request < first_error_req)
    first_error_req = 0;

  if (next_request < next_bad_alloc_serial)
    next_bad_alloc_serial = 0;

  if (first_error_req != -1
      && event->serial >= first_error_req)
    {
      error = *event;
      error_caught = True;

      return 0;
    }

  if (HandleErrorForPictureRenderer (event))
    return 0;

  if (event->error_code == (xi_first_error + XI_BadDevice))
    /* Various XI requests can result in XI_BadDevice errors if the
       device has been removed on the X server, but we have not yet
       processed the corresponding hierarchy events.  */
    return 0;

  if (bad_alloc_experienced
      /* See the comment at the end of HandleBadAlloc for why this is
	 done.  */
      && (event->error_code == BadDrawable
	  || event->error_code == BadWindow
	  || event->error_code == BadPixmap
	  || event->error_code == (render_first_error + BadPicture)))
    return 0;

  if (event->error_code == BadAlloc)
    {
      /* A client may have asked for the protocol translator to
	 allocate an unreasonable amount of memory.  */
      HandleBadAlloc (event);

      return 0;
    }

  XGetErrorText (display, event->error_code, buf, sizeof buf);
  fprintf (stderr, "X protocol error: %s on protocol request %d\n",
	   buf, event->request_code);
  exit (70);
}

void
InitXErrors (void)
{
  first_error_req = -1;
  XSetErrorHandler (ErrorHandler);

  /* Allow debugging by setting an environment variable.  */
  if (getenv ("SYNCHRONIZE"))
    XSynchronize (compositor.display, True);
}
