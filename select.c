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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compositor.h"

/* Caveat: the MULTIPLE target implementation is completely untested
   and probably doesn't work.  */

typedef struct _PropertyAtom PropertyAtom;
typedef struct _SelectInputData SelectInputData;
typedef struct _SelectionOwnerInfo SelectionOwnerInfo;
typedef struct _QueuedTransfer QueuedTransfer;
typedef struct _MultipleRecord MultipleRecord;

/* Structure representing a single property allocated for selection
   transfers.

   Since Wayland data sources are not read from a wl_surface, there is
   no way to associate each transfer with a specific window.  In
   addition, the compositor might want to obtain selection data in the
   background as well.  Because of this, we must rely on all ongoing
   data transfers from other owners having a unique property.  */

struct _PropertyAtom
{
  /* The atom in question.  */
  Atom atom;

  /* The id used to generate the atom.  */
  uint32_t counter;

  /* The next and last atoms in this chain.  */
  PropertyAtom *next, *last;
};

struct _SelectionOwnerInfo
{
  /* When this selection was last owned.  */
  Timestamp time;

  /* The targets of this atom.  */
  Atom *targets;

  /* The number of targets.  */
  int ntargets;

  /* The callback for this selection.  NULL means the selection is no
     longer owned.  */
  GetDataFunc (*get_transfer_function) (WriteTransfer *,
					Atom, Atom *);
};

enum
  {
    IsFinished	       = 1,
    IsIncr	       = (1 << 1),
    IsWaitingForChunk  = (1 << 2),
    IsStarted	       = (1 << 3),
    IsFailed	       = (1 << 4),
    IsWaitingForDelete = (1 << 5),
    IsWaitingForIncr   = (1 << 6),
    IsReadable	       = (1 << 7),
    IsFlushed	       = (1 << 8),
  };

struct _ReadTransfer
{
  /* The selection owner.  */
  Window owner;

  /* Some state associated with this transfer.  */
  int state;

  /* The atom being used to transfer data from the selection
     owner.  */
  PropertyAtom *property;

  /* The timestamp at which the selection request was issued.  */
  Time time;

  /* The selection and target that are being requested.  */
  Atom selection, target;

  /* The current offset, in 4-byte multiples, into the property data.
     Reset each time a new chunk of data is available.  */
  unsigned long read_offset;

  /* The format of the property data.  */
  unsigned long read_format;

  /* A function called once selection data begins to be read from the
     selection.  The second arg is the type of the selection data, and
     the third arg is the format of the selection data.  */
  void (*data_start_func) (ReadTransfer *, Atom, int);

  /* A function called once data some data can be read from the
     selection.  It may be called multiple times, depending on how big
     the response from the owner is.

     The args are as follows: a pointer to the transfer, the atom
     describing the type of the data, the format of the data (see the
     doc for XGetWindowProperty), and the size of the data that can be
     read in bytes.  */
  void (*data_read_func) (ReadTransfer *, Atom, int, ptrdiff_t);

  /* A function called after the selection transfer completes.  The
     second arg means whether or not the transfer was successful.  */
  Bool (*data_finish_func) (ReadTransfer *, Bool);

  /* Caller-specified data for those functions.  */
  void *data;

  /* The next and last transfers on this chain.  */
  ReadTransfer *next, *last;

  /* A timer that times out after 5 seconds of inactivity.  */
  Timer *timeout;
};

struct _WriteTransfer
{
  /* The next and last transfers on this chain.  */
  WriteTransfer *next, *last;

  /* The requestor of the transfer.  Spelt with an "O" because that's
     what the ICCCM does.  */
  Window requestor;

  /* Some state associated with this transfer.  */
  int state;

  /* The selection being requested from us, the target, the property,
     and the type of the data.  */
  Atom selection, target, property, type;

  /* The time of the request.  */
  Time time;

  /* Data buffer.  */
  unsigned char *buffer;

  /* The size of the data buffer, and how much has been read from
     it.  */
  ptrdiff_t size, offset;

  /* The SelectionNotify event that should be sent.  */
  XEvent event;

  /* Or a MultipleRecord that should be worked on.  */
  MultipleRecord *record;

  /* The offset of this transfer into record->atoms.  */
  unsigned long multiple_offset;

  /* Function called to return a piece of selection data in a buffer.
     If size arg is -1, free associated data.  Return False if EOF was
     reached.  */
  GetDataFunc transfer_function;

  /* User data for the write transfer.  */
  void *data;

  /* A timer that times out after 5 seconds of inactivity.  */
  Timer *timeout;

#ifdef DEBUG
  /* Total bytes of data written to the X server as part of this
     transfer.  */
  size_t total_written;
#endif
};

struct _QueuedTransfer
{
  /* Queued event.  */
  XEvent event;

  /* Next and last items in the queue.  */
  QueuedTransfer *next, *last;
};

struct _SelectInputData
{
  /* Number of requests from this requestor that are in progress.  */
  int refcount;
};

struct _MultipleRecord
{
  /* Number of conversions that still have not been made.  */
  int pending;

  /* The notification event that should be sent.  */
  XEvent event;

  /* The atom pair array.  Free this with XFree!  */
  Atom *atoms;

  /* The number of atoms in the atom pair array.  */
  unsigned long nitems;
};

/* Window used for selection data transfer.  */
Window selection_transfer_window;

/* Assoc table used to keep track of PropertyNotify selections.  */
static XLAssocTable *foreign_notify_table;

/* Counter used for determining the selection property atom.  */
static uint32_t prop_counter;

/* Chain of selection property atoms currently in use.  */
static PropertyAtom prop_atoms;

/* Chain of selection property atoms that can be reused.  */
static PropertyAtom free_list;

/* Circular queue of all outstanding selection data transfers to this
   client.  */
static ReadTransfer read_transfers;

/* List of all outstanding selection data transfers to other
   clients.  */
static WriteTransfer write_transfers;

/* Table of atoms to selection owner info.  */
static XLAssocTable *selection_owner_info;

/* Circular queue of SelectionRequests, to be drained once all
   outstanding write transfers finish.  */
static QueuedTransfer queued_transfers;

#ifdef DEBUG

static void __attribute__ ((__format__ (gnu_printf, 1, 2)))
DebugPrint (const char *format, ...)
{
  va_list ap;

  va_start (ap, format);
  vfprintf (stderr, format, ap);
  va_end (ap);
}

#else
#define DebugPrint(fmt, ...) ((void) 0)
#endif

/* Allocate a new atom to use for selection property transfers.  This
   atom is guaranteed to not be currently in use for a selection
   transfer to selection_transfer_window.

   Once the transfer completes, ReleasePropAtom must be called with
   the returned PropertyAtom object to allow reusing the atom in the
   future.  */

static PropertyAtom *
AllocPropAtom (void)
{
  PropertyAtom *atom;
  char name[sizeof "_XL_UXXXXXXXX" + 1];

  if (free_list.next != &free_list)
    {
      /* There is a free atom that we can use.  Remove it from the
	 free list.  */
      atom = free_list.next;
      atom->next->last = atom->last;
      atom->last->next = atom->next;
    }
  else
    {
      if (IntAddWrapv (prop_counter, 1, &prop_counter))
	{
	  fprintf (stderr, "Failed to allocate selection property"
		   " after running out of valid values!");
	  abort ();
	}

      sprintf (name, "_XL_U%x", prop_counter);
      atom = XLMalloc (sizeof *atom);

      /* Use XInternAtom instead of InternAtom.  These atoms should
	 only be interned once, so there is no point allocating memory
	 in the global atoms table.  */
      atom->atom = XInternAtom (compositor.display, name, False);
      atom->counter = prop_counter;
    }

  /* Now, link atom onto the used list and return it.  */
  atom->last = &prop_atoms;
  atom->next = prop_atoms.next;

  prop_atoms.next->last = atom;
  prop_atoms.next = atom;

  return atom;
}

static void
ReleasePropAtom (PropertyAtom *atom)
{
  /* Move atom from the used list onto the free list.  */

  atom->next->last = atom->last;
  atom->last->next = atom->next;

  atom->last = &free_list;
  atom->next = free_list.next;

  free_list.next->last = atom;
  free_list.next = atom;

  /* Now the atom has been released and can also be used for future
     selection data transfers.  */
}

static void
FinishReadTransfer (ReadTransfer *transfer, Bool success)
{
  Bool delay;

  if (transfer->data_finish_func
      /* This means to delay deallocating the transfer for a
	 while.  */
      && !transfer->data_finish_func (transfer, success))
    delay = True;
  else
    delay = False;

  transfer->next->last = transfer->last;
  transfer->last->next = transfer->next;

  ReleasePropAtom (transfer->property);
  RemoveTimer (transfer->timeout);

  if (!delay)
    XLFree (transfer);
}

static void
HandleTimeout (Timer *timer, void *data, struct timespec time)
{
  ReadTransfer *transfer;

  transfer = data;

  /* Delete the data transfer property from the window.  */
  XDeleteProperty (compositor.display,
		   selection_transfer_window,
		   transfer->property->atom);

  /* Finish the read transfer.  */
  FinishReadTransfer (transfer, False);
}

static void
CancelTransferEarly (ReadTransfer *transfer)
{
  /* Delete the data transfer property from the window.  */
  XDeleteProperty (compositor.display,
		   selection_transfer_window,
		   transfer->property->atom);

  /* Mark the transfer as finished.  */
  transfer->state |= IsFailed | IsFinished;
}

/* After calling this function, set the appropriate functions!  */

static ReadTransfer *
ConvertSelection (Atom selection, Atom target, Time time)
{
  ReadTransfer *transfer;

  /* If time is CurrentTime, obtain the time from the X server.  */
  if (time == CurrentTime)
    time = XLGetServerTimeRoundtrip ();

  transfer = XLCalloc (1, sizeof *transfer);
  transfer->property = AllocPropAtom ();
  transfer->time = time;
  transfer->selection = selection;
  transfer->target = target;

  /* Link the transfer onto the list of outstanding selection
     transfers.  */
  transfer->next = read_transfers.next;
  transfer->last = &read_transfers;

  /* Add a timeout for 5 seconds.  */
  transfer->timeout = AddTimer (HandleTimeout, transfer,
				MakeTimespec (5, 0));

  read_transfers.next->last = transfer;
  read_transfers.next = transfer;

  /* Delete the property from the window beforehand.  The property
     might be left over from a failed transfer.  */
  XDeleteProperty (compositor.display, selection_transfer_window,
		   transfer->property->atom);

  /* Now issue the ConvertSelection request.  */
  XConvertSelection (compositor.display, selection, target,
		     transfer->property->atom,
		     selection_transfer_window, time);

  return transfer;
}

ReadTransfer *
ConvertSelectionFuncs (Atom selection, Atom target, Time time, void *data,
		       void (*data_start) (ReadTransfer *, Atom, int),
		       void (*data_read) (ReadTransfer *, Atom, int, ptrdiff_t),
		       Bool (*data_finish) (ReadTransfer *, Bool))
{
  ReadTransfer *transfer;

  transfer = ConvertSelection (selection, target, time);
  transfer->data = data;
  transfer->data_start_func = data_start;
  transfer->data_read_func = data_read;
  transfer->data_finish_func = data_finish;

  return transfer;
}

/* Find the first outstanding read transfer for SELECTION, TARGET and
   TIME in the queue of outstanding selection transfers.  */

static ReadTransfer *
FindReadTransfer (Atom selection, Atom target, Time time)
{
  ReadTransfer *transfer;

  transfer = read_transfers.last;

  while (transfer != &read_transfers)
    {
      if (transfer->selection == selection
	  && transfer->target == target
	  && transfer->time == time)
	return transfer;

      transfer = transfer->last;
    }

  return NULL;
}

long
SelectionQuantum (void)
{
  long request;

  request = XExtendedMaxRequestSize (compositor.display);

  if (!request)
    request = XMaxRequestSize (compositor.display);

  return request;
}

static ptrdiff_t
FormatTypeSize (int format)
{
  switch (format)
    {
    case 8:
      return sizeof (char);

    case 16:
      return sizeof (short);

    case 32:
      return sizeof (long);
    }

  abort ();
}

void
FinishTransfers (void)
{
  ReadTransfer *transfer, *last;

  transfer = read_transfers.last;

  while (transfer != &read_transfers)
    {
      last = transfer;
      transfer = transfer->last;

      if (last->state & IsFinished)
	FinishReadTransfer (last, !(last->state & IsFailed));
    }
}

/* Signal that the current chunk of data from TRANSFER has been
   completely read.  */

static void
FinishChunk (ReadTransfer *transfer)
{
  if (transfer->state & IsIncr)
    {
      transfer->state |= IsWaitingForChunk;
      return;
    }

  /* If INCR transfer is not in progress, post a timer to finish
     TRANSFER entirely, the next time the event loop is entered.  */
  transfer->state |= IsFinished;
}

void
SkipChunk (ReadTransfer *transfer)
{
  /* Just delete the property.  */
  XDeleteProperty (compositor.display,
		   selection_transfer_window,
		   transfer->property->atom);

  /* And mark the chunk as finished.  */
  FinishChunk (transfer);
}

/* Read a chunk of data from TRANSFER.  LONG_LENGTH gives the length
   of the data to read.  Return a pointer to the data, or NULL if
   reading the data failed, and return the actual length of the data
   in *NBYTES.  Free the data returned using Xlib functions!  */

unsigned char *
ReadChunk (ReadTransfer *transfer, int long_length, ptrdiff_t *nbytes,
	   ptrdiff_t *bytes_after_return)
{
  unsigned char *prop_data;
  Atom actual_type;
  int actual_format, rc;
  unsigned long nitems, bytes_after;

  prop_data = NULL;

  /* Now read the actual property data.  */
  rc = XGetWindowProperty (compositor.display, selection_transfer_window,
			   transfer->property->atom, transfer->read_offset,
			   long_length, True, AnyPropertyType, &actual_type,
			   &actual_format, &nitems, &bytes_after, &prop_data);

  /* Reading the property data failed.  Signal failure by returning
     NULL.  Also, cancel the whole transfer here too.  */
  if (!prop_data)
    {
      CancelTransferEarly (transfer);
      return NULL;
    }

  if (actual_type == None
      || actual_format != transfer->read_format
      || rc != Success)
    {
      /* Same goes here.  */
      if (prop_data)
	XFree (prop_data);

      CancelTransferEarly (transfer);
      return NULL;
    }

  if (!bytes_after)
    /* The property has now been deleted, so finish the chunk.  */
    FinishChunk (transfer);

  /* Now return the number of bytes read.  */
  *nbytes = nitems * FormatTypeSize (actual_format);

  /* Bump the read offset.  */
  transfer->read_offset += long_length;

  /* Return bytes_after to the caller.  */
  if (bytes_after_return)
    {
      if (actual_format == 32)
	*bytes_after_return = bytes_after * (sizeof (long) / 4);
      else
	*bytes_after_return = bytes_after;
    }

  return prop_data;
}

static void
StartSelectionRead (ReadTransfer *transfer)
{
  unsigned char *prop_data;
  Atom actual_type;
  int actual_format, rc;
  unsigned long nitems, bytes_after;

  prop_data = NULL;

  /* First, figure out how big the property data is.  */
  rc = XGetWindowProperty (compositor.display, selection_transfer_window,
			   transfer->property->atom, 0, 0, True, AnyPropertyType,
			   &actual_type, &actual_format, &nitems, &bytes_after,
			   &prop_data);

  if (prop_data)
    XFree (prop_data);

  if (rc != Success)
    {
      FinishReadTransfer (transfer, False);
      return;
    }

  /* If the property is of type INCR, this is the beginning of an INCR
     transfer.  In this case, delete prop_data, the property, and wait
     for a the property to be set with a new value by the owner.  */

  if (actual_type == INCR)
    {
      transfer->state |= IsIncr;
      transfer->state |= IsWaitingForChunk;

      XDeleteProperty (compositor.display, selection_transfer_window,
		       transfer->property->atom);

      return;
    }

  /* If actual_type is None, the property does not exist.  Signal
     failure.  */
  if (actual_type == None || !actual_format)
    {
      FinishReadTransfer (transfer, False);
      return;
    }

  /* If bytes_after is 0, the property is empty.  Immediately call
     data_finish_func.  If the transfer is incremental, then an empty
     property means the data has been fully transferred.  */
  if (!bytes_after)
    goto finish_early;

  /* Otherwise, we now know how long the selection is, and the type of
     the data.  Announce that information, along with the beginning of
     this chunk.  */

  transfer->read_offset = 0;

  /* Also announce the format.  */
  transfer->read_format = actual_format;

  if (transfer->data_start_func
      /* Make sure this isn't run multiple times in the case of INCR
	 selections.  */
      && !(transfer->state & IsStarted))
    transfer->data_start_func (transfer, actual_type,
			       actual_format);

  transfer->state |= IsStarted;

  if (actual_format == 32)
    /* Give actual data size, depending on the size of long.  */
    bytes_after = bytes_after * (sizeof (long) / 4);

  if (transfer->data_read_func)
    transfer->data_read_func (transfer, actual_type, actual_format,
			      bytes_after);

  return;

 finish_early:
  FinishReadTransfer (transfer, True);
  return;
}

static ReadTransfer *
FindReadTransferByProp (Atom atom)
{
  ReadTransfer *transfer;

  transfer = read_transfers.last;

  while (transfer != &read_transfers)
    {
      if (transfer->property->atom == atom)
	return transfer;

      transfer = transfer->last;
    }

  return NULL;
}

static Bool
SendEvent (XEvent *event)
{
  CatchXErrors ();
  XSendEvent (compositor.display, event->xany.window,
	      False, NoEventMask, event);
  return UncatchXErrors (NULL);
}

static void
SendEventUnsafe (XEvent *event)
{
  XSendEvent (compositor.display, event->xany.window,
	      False, NoEventMask, event);
}

static Bool
CanConvertTarget (SelectionOwnerInfo *info, Atom target)
{
  int i;

  if (target == TARGETS
      || target == TIMESTAMP)
    return True;

  for (i = 0; i < info->ntargets; ++i)
    {
      if (target == info->targets[i])
	return True;
    }

  return False;
}

static GetDataFunc
GetTransferFunction (SelectionOwnerInfo *info,
		     WriteTransfer *transfer,
		     Atom target, Atom *type)
{
  return info->get_transfer_function (transfer, target, type);
}

static WriteTransfer *
FindWriteTransfer (Window requestor, Atom property,
		   int ignore_state)
{
  WriteTransfer *transfer;

  transfer = write_transfers.last;

  while (transfer != &write_transfers)
    {
      if (transfer->requestor == requestor
	  && transfer->property == property
	  && (!ignore_state
	      || ((transfer->state & ignore_state)
		  != ignore_state)))
	return transfer;

      transfer = transfer->last;
    }

  return NULL;
}

static Bool
FindQueuedTransfer (Window requestor, Atom property)
{
  QueuedTransfer *transfer;

  transfer = queued_transfers.next;

  while (transfer != &queued_transfers)
    {
      if (transfer->event.xselectionrequest.requestor == requestor
	  || transfer->event.xselectionrequest.property == property)
	return True;

      transfer = transfer->next;
    }

  return False;
}

static void
SignalConversionPerformed (WriteTransfer *transfer)
{
  if (transfer->record)
    {
      /* This conversion is part of a larger conversion to MULTIPLE.
	 Subtract this conversion from the number of conversions that
	 are still pending.  If there are no more pending conversions,
	 send the SelectionNotify event and return.  */

      DebugPrint ("Conversion complete; %d conversions are still pending\n",
		  transfer->record->pending);

      if (!(--transfer->record->pending))
	{
	  SendEventUnsafe (&transfer->record->event);

	  XFree (transfer->record->atoms);
	  XLFree (transfer->record);
	}

      /* In any case, make transfer->record NULL.  */
      transfer->record = NULL;
    }

  /* If the SelectionNotify has not yet been sent, send it now.  */
  if (transfer->event.type)
    SendEventUnsafe (&transfer->event);
  transfer->event.type = 0;
}

static void
FlushTransfer (WriteTransfer *transfer, Bool force)
{
  long size;

  /* Flushing the buffer while waiting for property deletion is an
     invalid operation.  */
  XLAssert (!(transfer->state & IsWaitingForDelete));

  if ((transfer->state & IsStarted) || force)
    {
      if (force)
	DebugPrint ("Forcing transfer\n");
      else
	DebugPrint ("Starting transfer\n");

      /* If the transfer has already started, or FORCE is set, write
	 the property to the window.  */
      CatchXErrors ();
      DebugPrint ("Writing property of size %zd\n",
		  transfer->offset);
      XChangeProperty (compositor.display, transfer->requestor,
		       transfer->property, transfer->type, 8,
		       PropModeReplace, transfer->buffer,
		       transfer->offset);
#ifdef DEBUG
      transfer->total_written += transfer->offset;
#endif
      SignalConversionPerformed (transfer);
      UncatchXErrors (NULL);

      transfer->offset = 0;
    }
  else
    {
      DebugPrint ("Writing INCR property...\n");

      /* Otherwise, write an INCR property and wait for it to be
	 deleted.  */
      size = transfer->size;

      CatchXErrors ();
      XChangeProperty (compositor.display, transfer->requestor,
		       transfer->property, INCR, 32,
		       PropModeReplace, (unsigned char *) &size, 1);
      SignalConversionPerformed (transfer);
      UncatchXErrors (NULL);

      transfer->state |= IsWaitingForIncr;
    }

  /* Now, begin to wait for the property to be deleted.  */
  transfer->state |= IsWaitingForDelete;

  /* And mark the transfer as having been flushed.  */
  transfer->state |= IsFlushed;
}

static void
SelectPropertyNotify (Window window)
{
  SelectInputData *data;

  data = XLLookUpAssoc (foreign_notify_table, window);

  if (data)
    data->refcount++;
  else
    {
      data = XLMalloc (sizeof *data);
      data->refcount = 1;
      XLMakeAssoc (foreign_notify_table, window, data);

      /* Actually select for input from the window.  */
      CatchXErrors ();
      XSelectInput (compositor.display, window,
		    PropertyChangeMask);
      UncatchXErrors (NULL);

      DebugPrint ("Selecting for PropertyChangeMask on %lu\n",
		  window);
    }
}

static void
DeselectPropertyNotify (Window window)
{
  SelectInputData *data;

  data = XLLookUpAssoc (foreign_notify_table, window);
  XLAssert (data != NULL);

  if (--data->refcount)
    return;

  DebugPrint ("De-selecting for PropertyChangeMask on %lu\n",
	      window);

  CatchXErrors ();
  XSelectInput (compositor.display, window, NoEventMask);
  UncatchXErrors (NULL);

  XLDeleteAssoc (foreign_notify_table, window);
  XLFree (data);
}

/* Forward declaration.  */

static void DrainQueuedTransfers (void);

static void
FreeTransfer (WriteTransfer *transfer)
{
  DebugPrint ("Deallocating transfer data\n");

  /* The transfer completed.  Unlink it, and also deselect for
     PropertyNotify events from the requestor.  */

  transfer->next->last = transfer->last;
  transfer->last->next = transfer->next;

  if (transfer->state & IsStarted)
    {
#ifdef DEBUG
      DebugPrint ("Writing zero-length property data; total: %zu\n",
		  transfer->total_written);
#endif

      CatchXErrors ();
      /* Write zero-length property data to complete the INCR
	 transfer.  */
      XChangeProperty (compositor.display, transfer->requestor,
		       transfer->property, transfer->type, 8,
		       PropModeReplace, NULL, 0);
      UncatchXErrors (NULL);
    }

  if (transfer->record)
    {
      /* Since FreeTransfer was called without transfer->record having
	 being released earlier, the conversion associated with this
	 transfer has failed.  Write the conversion failure.  */

      transfer->record->atoms[transfer->multiple_offset] = None;

      DebugPrint ("Conversion at offset %lu failed\n",
		  transfer->multiple_offset);

      CatchXErrors ();
      XChangeProperty (compositor.display, transfer->requestor,
		       transfer->record->event.xselection.property,
		       ATOM_PAIR, 32, PropModeReplace,
		       (unsigned char *) transfer->record->atoms,
		       transfer->record->nitems);
      UncatchXErrors (NULL);

      /* Now, dereference the record, and send the SelectionNotify if
	 there are no more pending conversions.  */

      if (!(--transfer->record->pending))
	{
	  DebugPrint ("Completing MULTIPLE transfer\n");

	  SendEventUnsafe (&transfer->record->event);

	  XFree (transfer->record->atoms);
	  XLFree (transfer->record);
	}
    }

  /* If there are no more transfers, drain queued ones.  */
  if (write_transfers.next == &write_transfers)
    DrainQueuedTransfers ();

  DeselectPropertyNotify (transfer->requestor);
  RemoveTimer (transfer->timeout);
  XLFree (transfer->buffer);
  XLFree (transfer);
}

static void
FinishTransferEarly (WriteTransfer *transfer)
{
  /* Call the transfer function with negative values, to force it to
     free data.  */

  transfer->transfer_function (transfer, NULL, -1, NULL);

  /* Finish the transfer now.  */
  FreeTransfer (transfer);
}

static void
TransferFinished (WriteTransfer *transfer)
{
  if (transfer->state & IsWaitingForDelete)
    {
      /* If we are still waiting for property deletion, simply set the
	 IsFinished flag.  Zero-length property data will be written the
	 next time the property is deleted to mark the completion of
	 this transfer.  */
      transfer->state |= IsFinished;

      DebugPrint ("Transfer finished; waiting for property deletion\n");
    }
  else if (transfer->offset
	   /* Even if there is no more data, we must still write
	      zero-length property data to complete the selection
	      transfer if nothing has previously been written.  */
	   || !(transfer->state & IsFlushed))
    {
      DebugPrint ("Transfer finished, but there is still property data"
		  " unwritten (offset %td)\n", transfer->offset);

      /* There is still property data left to be written.  */
      FlushTransfer (transfer, True);
      transfer->state |= IsFinished;
    }
  else
    /* The transfer is really finished.  */
    FreeTransfer (transfer);
}

static void
TransferBecameReadable (WriteTransfer *transfer)
{
  ptrdiff_t bytes_read;
  ReadStatus status;

  /* If we are still waiting for the INCR property to be deleted,
     don't read anything, but flag the read as pending.  */

  if (transfer->state & IsWaitingForDelete)
    {
      DebugPrint ("Transfer became readable, but we are still waiting"
		  " for a property deletion\n");
      transfer->state |= IsReadable;
      return;
    }

  DebugPrint ("Reading from transfer function\n");

  /* The data source became readable.  Every time the source becomes
     readable, try to fill up the transfer buffer.

     If we get EOF without filling up the buffer, simply set the
     property to the data and the correct type and finish the
     transfer; otherwise, initiate INCR transfer.  */

  transfer->state &= ~IsReadable;

  status = transfer->transfer_function (transfer,
					(transfer->buffer
					 + transfer->offset),
					(transfer->size
					 - transfer->offset),
					&bytes_read);

  if (status != EndOfFile)
    {
      transfer->offset += bytes_read;
      DebugPrint ("Read %td bytes, offset is now %td into %td\n",
		  bytes_read, transfer->offset, transfer->size);

      /* Make sure nothing overflowed.  */
      XLAssert (transfer->offset <= transfer->size);

      if (transfer->offset == transfer->size
	  /* If a bigger buffer is required, flush the transfer now,
	     so the entire transfer buffer can be reused.  */
	  || status == NeedBiggerBuffer)
	{
	  if (status == NeedBiggerBuffer)
	    /* Since a bigger buffer is needed, that means there is
	       still data buffered up to be read.  */
	    transfer->state |= IsReadable;

	  FlushTransfer (transfer, False);
	}
    }
  else
    {
      DebugPrint ("Transfer complete, bytes read as part of EOF: %td, "
		  "off: %td size: %td\n", bytes_read, transfer->offset,
		  transfer->size);

      transfer->offset += bytes_read;
      TransferFinished (transfer);
    }
}

static void
ConvertSelectionTargets1 (SelectionOwnerInfo *info,
			  Window requestor, Atom property)
{
  Atom *targets;

  targets = alloca ((3 + info->ntargets)
		    * sizeof *targets);

  /* Add the required targets, TARGETS and MULTIPLE.  */
  targets[0] = TARGETS;
  targets[1] = MULTIPLE;
  targets[2] = TIMESTAMP;

  /* And the rest of the targets.  */
  memcpy (&targets[3], info->targets,
	  info->ntargets * sizeof *targets);

  XChangeProperty (compositor.display, requestor,
		   property, XA_ATOM, 32, PropModeReplace,
		   (unsigned char *) targets,
		   2 + info->ntargets);
}

static void
ConvertSelectionTargets (SelectionOwnerInfo *info, XEvent *notify)
{

  /* Set the property and send the SelectionNotify event.  */
  CatchXErrors ();
  ConvertSelectionTargets1 (info, notify->xselection.requestor,
			    notify->xselection.property);
  SendEventUnsafe (notify);
  UncatchXErrors (NULL);
}

static void
ConvertSelectionTimestamp1 (SelectionOwnerInfo *info, Window requestor,
			    Window property)
{
  XChangeProperty (compositor.display, requestor, property,
		   XA_ATOM, 32, PropModeReplace,
		   (unsigned char *) &info->time, 1);
}

static void
ConvertSelectionTimestamp (SelectionOwnerInfo *info, XEvent *notify)
{
  /* Set the property and send the SelectionNotify event.  */
  CatchXErrors ();
  ConvertSelectionTimestamp1 (info, notify->xselection.requestor,
			      notify->xselection.property);
  SendEventUnsafe (notify);
  UncatchXErrors (NULL);
}

static void
QueueTransfer (XEvent *event)
{
  QueuedTransfer *transfer;

  transfer = XLMalloc (sizeof *transfer);
  transfer->event = *event;
  transfer->next = queued_transfers.next;
  transfer->last = &queued_transfers;
  queued_transfers.next->last = transfer;
  queued_transfers.next = transfer;
}

static void
HandleWriteTimeout (Timer *timer, void *data, struct timespec time)
{
  DebugPrint ("Transfer timeout\n");

  FinishTransferEarly (data);
}

static void
ConvertSelectionMultiple (SelectionOwnerInfo *info, XEvent *event,
			  XEvent *notify)
{
  Status rc;
  unsigned char *prop_data;
  Atom *atoms;
  Atom actual_type;
  int actual_format;
  unsigned long nitems, bytes_after, i, quantum;
  WriteTransfer *transfer;
  Bool prop_data_changed;
  MultipleRecord *record;

  prop_data = NULL;
  prop_data_changed = False;

  /* Try to get the ATOM_PAIRs describing the targets and properties
     from the source.  */
  CatchXErrors ();
  rc = XGetWindowProperty (compositor.display,
			   event->xselectionrequest.requestor,
			   event->xselectionrequest.property,
			   0, 65535, False, ATOM_PAIR,
			   &actual_type, &actual_format,
			   &nitems, &bytes_after, &prop_data);
  UncatchXErrors (NULL);

  if (rc != Success || actual_format != 32 || nitems % 2
      || actual_type != ATOM_PAIR || !prop_data)
    {
      if (prop_data)
	XFree (prop_data);

      DebugPrint ("Failed to retrieve ATOM_PAIR parameter\n");
      return;
    }

  atoms = (Atom *) prop_data;

  DebugPrint ("Number of items in atom pair: %lu\n", nitems / 2);

  for (i = 0; i < nitems; i += 2)
    {
      DebugPrint ("Verifying MULTIPLE transfer; target = %lu, property = %lu\n",
		  atoms[i + 0], atoms[i + 1]);

      if (FindWriteTransfer (event->xselectionrequest.requestor, atoms[1], 0)
	  || FindQueuedTransfer (event->xselectionrequest.requestor, atoms[1]))
	{
	  DebugPrint ("Found ongoing selection transfer with same requestor "
		      "and property; this MULTIPLE request will have to be "
		      "queued.\n");

	  QueueTransfer (event);

	  if (prop_data)
	    XLFree (prop_data);

	  return;
	}
    }

  record = XLMalloc (sizeof *record);
  record->pending = 0;
  record->event = *notify;
  record->nitems = nitems;
  record->atoms = atoms;

  CatchXErrors ();

  for (i = 0; i < nitems; i += 2)
    {
      /* Fortunately, we don't have to verify every transfer here to
	 make sure the client didn't specify duplicate property names,
	 as the ICCCM doesn't explicitly specify how programs should
	 behave in that case.  Things will simply go wrong later on,
	 and the transfer will time out.  */

      DebugPrint ("Starting MULTIPLE transfer; target = %lu, property = %lu\n",
		  atoms[i + 0], atoms[i + 1]);

      if (atoms[0] == MULTIPLE)
	{
	  DebugPrint ("Saw nested MULTIPLE transfer; "
		      "such conversions are not allowed\n");
	  atoms[0] = None;
	  prop_data_changed = True;

	  continue;
	}

      if (!CanConvertTarget (info, atoms[0]))
	{
	  DebugPrint ("Couldn't convert to target for a simple reason;"
		      " replacing atom with NULL\n");
	  atoms[0] = None;
	  prop_data_changed = True;

	  continue;
	}

      if (atoms[0] == TARGETS)
	{
	  DebugPrint ("Converting to special target TARGETS...\n");
	  ConvertSelectionTargets1 (info, event->xselectionrequest.requestor,
				    atoms[1]);

	  continue;
	}

      if (atoms[0] == TIMESTAMP)
	{
	  DebugPrint ("Converting to special target TIMESTAMP...\n");
	  ConvertSelectionTimestamp1 (info, event->xselectionrequest.requestor,
				      atoms[1]);

	  continue;
	}

      /* Create the write transfer and link it onto the list.  */
      transfer = XLCalloc (1, sizeof *transfer);

      /* This seems to be a sufficiently reasonable value.  */
      quantum = MIN (SelectionQuantum (), 65535 * 2);

      transfer->next = write_transfers.next;
      transfer->last = &write_transfers;
      transfer->requestor = event->xselectionrequest.requestor;
      transfer->selection = event->xselectionrequest.selection;
      transfer->target = event->xselectionrequest.target;
      transfer->property = atoms[1];
      transfer->time = event->xselectionrequest.time;
      transfer->buffer = XLMalloc (quantum);
      transfer->size = quantum;

      /* Add a timeout for 5 seconds.  */
      transfer->timeout = AddTimer (HandleWriteTimeout, transfer,
				    MakeTimespec (5, 0));

      write_transfers.next->last = transfer;
      write_transfers.next = transfer;

      SelectPropertyNotify (transfer->requestor);

      transfer->transfer_function
	= GetTransferFunction (info, transfer,
			       transfer->target,
			       &transfer->type);

      if (!transfer->transfer_function)
	{
	  /* Something failed (most probably, we ran out of fds to
	     make the pipe).  Cancel the transfer and signal
	     failure.  */
	  atoms[0] = None;
	  prop_data_changed = True;

	  FreeTransfer (transfer);
	}
      else
	{
	  /* Otherwise, add the transfer record.  */
	  record->pending++;
	  transfer->record = record;
	  transfer->multiple_offset = i;
	}
    }

  /* If prop_data changed, which happens when a conversion fails,
     write its new contents back onto the requestor to let it know
     which conversions could not be made.  */
  if (prop_data_changed)
    XChangeProperty (compositor.display,
		     event->xselectionrequest.requestor,
		     event->xselectionrequest.property,
		     ATOM_PAIR, 32, PropModeReplace,
		     prop_data, nitems);

  if (prop_data && !record->pending)
    /* If record->pending, then the individual write transfers still
       reference the atom pair data.  */
    XFree (prop_data);

  /* If no pending conversions remain, immediately send success and
     free the conversion.  */
  if (!record->pending)
    {
      SendEvent (&record->event);
      XLFree (record);
    }

  UncatchXErrors (NULL);
}

static Bool
HandleSelectionRequest (XEvent *event)
{
  XEvent notify;
  WriteTransfer *transfer, *existing_transfer;
  long quantum;
  SelectionOwnerInfo *info;

  DebugPrint ("Received SelectionRequest.  Time: %lu, requestor: %lu"
	      ", target: %lu, selection: %lu, property: %lu, serial: %lu\n",
	      event->xselectionrequest.time,
	      event->xselectionrequest.requestor,
	      event->xselectionrequest.target,
	      event->xselectionrequest.selection,
	      event->xselectionrequest.property,
	      event->xselectionrequest.serial);

  notify.xselection.type = SelectionNotify;
  notify.xselection.requestor = event->xselectionrequest.requestor;
  notify.xselection.time = event->xselectionrequest.time;
  notify.xselection.target = event->xselectionrequest.target;
  notify.xselection.selection = event->xselectionrequest.selection;
  notify.xselection.property = None;

  info = XLLookUpAssoc (selection_owner_info,
			event->xselectionrequest.selection);

  if (!info
      || !info->get_transfer_function
      /* The ICCCM prohibits clients from using CurrentTime, but some
	 do anyway.  */
      || (event->xselectionrequest.time != CurrentTime
	  && TimeIs (event->xselectionrequest.time, Earlier, info->time))
      || !CanConvertTarget (info, event->xselectionrequest.target))
    {
      DebugPrint ("Couldn't convert selection due to simple reason\n");

      /* The event is out of date, or the selection cannot be
	 converted to the given target.  Send failure.  */
      SendEvent (&notify);
      return True;
    }

  /* If a selection request with the same property and window already
     exists, delay this request for later.  */
  existing_transfer
    = FindWriteTransfer (event->xselectionrequest.requestor,
			 event->xselectionrequest.property,
			 /* Ignore write transfers that are finished
			    but pending property deletion.  If the
			    existing transfer is finished, but we are
			    still waiting for property deletion, allow
			    the new transfer to take place.  This is
			    because some very popular programs ask for
			    TARGETS, and then ask for STRING with the
			    same property, but only delete the
			    property for the first request after the
			    data for the second request arrives.  This
			    is against the ICCCM, as it says:

			      The requestor should set the property
			      argument to the name of a property that
			      the owner can use to report the value of
			      the selection.  Requestors should ensure
			      that the named property does not exist
			      on the window before issuing the
			      ConvertSelection request.

			    and:

			      Once all the data in the selection has
			      been retrieved (which may require
			      getting the values of several properties
			      -- see the section called "Use of
			      Selection Properties". ), the requestor
			      should delete the property in the
			      SelectionNotify request by using a
			      GetProperty request with the delete
			      argument set to True.  As previously
			      discussed, the owner has no way of
			      knowing when the data has been
			      transferred to the requestor unless the
			      property is removed.

			    Both paragraphs mean that the property
			    should have been deleted by the time the
			    second request is made! */
			 IsFinished | IsWaitingForDelete);

  if (existing_transfer
      /* We need to look at the queue too; otherwise, events from the
	 future might be handled out of order, if the original write
	 transfer is gone, but some events are still queued.  */
      || FindQueuedTransfer (event->xselectionrequest.requestor,
			     event->xselectionrequest.property))
    {
      DebugPrint ("Queueing this selection request for later, because"
		  " an identical transfer is already taking place\n");
      QueueTransfer (event);

      return True;
    }

  /* Otherwise, begin a selection transfer to the requestor.  */
  notify.xselection.property = event->xselectionrequest.property;

  if (notify.xselection.property == None)
    /* This is an obsolete client; use the target as the property.  */
    notify.xselection.property = event->xselectionrequest.target;

  /* Handle special targets, such as TARGETS and TIMESTAMP.  */
  if (notify.xselection.target == TARGETS)
    {
      DebugPrint ("Converting selection to special target TARGETS\n");
      ConvertSelectionTargets (info, &notify);
      return True;
    }
  else if (notify.xselection.target == TIMESTAMP)
    {
      DebugPrint ("Converting selection to special target TIMESTAMP\n");
      ConvertSelectionTimestamp (info, &notify);
      return True;
    }
  else if (notify.xselection.target == MULTIPLE)
    {
      if (event->xselectionrequest.property == None)
	{
	  DebugPrint ("Got malformed MULTIPLE request with no property\n");
	  notify.xselection.property = None;
	  SendEvent (&notify);

	  return True;
	}

      DebugPrint ("Converting selection to special target MULTIPLE\n");
      ConvertSelectionMultiple (info, event, &notify);
      return True;
    }

  DebugPrint ("Starting selection transfer\n");

  /* Create the write transfer and link it onto the list.  */
  transfer = XLCalloc (1, sizeof *transfer);

  /* This seems to be a sufficiently reasonable value.  */
  quantum = MIN (SelectionQuantum (), 65535 * 2);

  transfer->next = write_transfers.next;
  transfer->last = &write_transfers;
  transfer->requestor = event->xselectionrequest.requestor;
  transfer->selection = event->xselectionrequest.selection;
  transfer->target = event->xselectionrequest.target;
  transfer->property = notify.xselection.property;
  transfer->time = event->xselectionrequest.time;
  transfer->event = notify;
  transfer->buffer = XLMalloc (quantum);
  transfer->size = quantum;

  /* Add a timeout for 5 seconds.  */
  transfer->timeout = AddTimer (HandleWriteTimeout, transfer,
				MakeTimespec (5, 0));

  write_transfers.next->last = transfer;
  write_transfers.next = transfer;

  SelectPropertyNotify (transfer->requestor);

  transfer->transfer_function
    = GetTransferFunction (info, transfer,
			   transfer->target,
			   &transfer->type);

  if (!transfer->transfer_function)
    {
      /* Something failed (most likely, we ran out of fds to make the
	 pipe).  Cancel the transfer and signal failure.  */
      notify.xselection.property = None;
      SendEvent (&notify);

      FreeTransfer (transfer);
    }

  return True;
}

static void
DrainQueuedTransfers (void)
{
  QueuedTransfer temp, *item, *last;

  /* If nothing is queued, return.  */

  if (queued_transfers.next == &queued_transfers)
    return;

  /* First, relink everything onto this temp sentinel.  That way, if
     stuff gets queued again in HandleSelectionRequest, the list
     structure won't change underneath our noses.  */

  queued_transfers.last->next = &temp;
  queued_transfers.next->last = &temp;
  temp.next = queued_transfers.next;
  temp.last = queued_transfers.last;

  queued_transfers.next = &queued_transfers;
  queued_transfers.last = &queued_transfers;

  /* Then, read from the other side of the queue, and handle
     everything.  */
  item = temp.last;

  while (item != &temp)
    {
      last = item;
      item = item->last;

      DebugPrint ("Draining one request with serial: %lu\n",
		  last->event.xselectionrequest.serial);

      HandleSelectionRequest (&last->event);
      XLFree (last);
    }
}

static Bool
HandleSelectionNotify (XEvent *event)
{
  ReadTransfer *transfer;

  /* If event->property is None, then the selection transfer could not
     be made.  Find which transfer couldn't be made by looking in the
     queue for the first transfer matching event->selection,
     event->target and event->time, and unlink it.  */
  if (event->xselection.property == None)
    {
      transfer = FindReadTransfer (event->xselection.selection,
				   event->xselection.target,
				   event->xselection.time);

      if (!transfer)
	return True;

      FinishReadTransfer (transfer, False);
      return True;
    }

  /* Otherwise, find the outstanding selection transfer using the
     property.  */
  transfer = FindReadTransferByProp (event->xselection.property);

  if (!transfer)
    return True;

  /* And start transferring data from it.  */
  StartSelectionRead (transfer);
  return True;
}

static Bool
HandlePropertyDelete (XEvent *event)
{
  WriteTransfer *transfer, *last;

  transfer = write_transfers.last;

  DebugPrint ("Handling property deletion for %lu; window %lu\n",
	      event->xproperty.atom, event->xproperty.window);

  while (transfer != &write_transfers)
    {
      last = transfer->last;

      /* There can be multiple finished transfers with the same
	 property pending deletion if a client not compliant with the
	 ICCCM is in use.  See the large comment in
	 HandleSelectionRequest.  */
      DebugPrint ("Handling transfer %p\n", event);

      if (transfer->state & IsFinished)
	{
	  DebugPrint ("Completing transfer\n");

	  /* The transfer is now complete; finish it by freeing its
	     data, and potentially writing zero-length data.  */
	  FreeTransfer (transfer);
	}
      else if (transfer->state & IsWaitingForIncr)
	{
	  /* If transfer is waiting for the INCR property to be
	     deleted, mark it as started, and flush it again to write
	     the first piece of property data.  */

	  DebugPrint ("Starting transfer in response to INCR property deletion\n");

	  transfer->state |= IsStarted;
	  transfer->state &= ~IsWaitingForIncr;
	  transfer->state &= ~IsWaitingForDelete;

	  /* Flush the transfer again to write the property data.  */
	  FlushTransfer (transfer, False);
	}
      else
	{
	  DebugPrint ("Continuing transfer\n");

	  /* Clear IsWaitingForRelease.  */
	  transfer->state &= ~IsWaitingForDelete;

	  if (transfer->state & IsReadable)
	    {
	      DebugPrint ("Picking read back up from where it was left\n");

	      /* And signal that the transfer is readable again.  */
	      TransferBecameReadable (transfer);
	    }
	}

      transfer = last;
    }

  return True;
}

static Bool
HandlePropertyNotify (XEvent *event)
{
  ReadTransfer *transfer;

  if (event->xproperty.window != DefaultRootWindow (compositor.display))
    /* Xlib selects for PropertyNotifyMask on the root window, which
       results in a lot of noise here.  */
    DebugPrint ("PropertyNotify event:\n"
		"serial:\t%lu\n"
		"window:\t%lu\n"
		"atom:\t%lu\n"
		"time:\t%lu\n"
		"state:\t%s\n",
		event->xproperty.serial,
		event->xproperty.window,
		event->xproperty.atom,
		event->xproperty.time,
		(event->xproperty.state == PropertyNewValue
		 ? "PropertyNewValue" : "PropertyDelete"));

  if (event->xproperty.state != PropertyNewValue)
    return HandlePropertyDelete (event);

  /* Find the transfer corresponding to this property.  */
  transfer = FindReadTransferByProp (event->xproperty.atom);

  if (!transfer)
    return False;

  /* By the time transfer->state & IsIncr, no more out-of-date
     PropertyNotify events from previous selection requests can be
     received.  */

  if (transfer->state & IsIncr
      && transfer->state & IsWaitingForChunk)
    {
      /* Clear the waiting-for-chunk flag, and begin reading from this
	 chunk.  */
      transfer->state &= ~IsWaitingForChunk;
      StartSelectionRead (transfer);
    }

  return True;
}

Bool
HookSelectionEvent (XEvent *event)
{
  if (event->xany.type == SelectionNotify)
    return HandleSelectionNotify (event);

  if (event->xany.type == SelectionRequest)
    return HandleSelectionRequest (event);

  if (event->xany.type == PropertyNotify)
    return HandlePropertyNotify (event);

  return False;
}

void *
GetTransferData (ReadTransfer *transfer)
{
  return transfer->data;
}

Time
GetTransferTime (ReadTransfer *transfer)
{
  return transfer->time;
}

void *
GetWriteTransferData (WriteTransfer *transfer)
{
  return transfer->data;
}

void
SetWriteTransferData (WriteTransfer *transfer, void *data)
{
  transfer->data = data;
}

void
CompleteDelayedTransfer (ReadTransfer *transfer)
{
  /* Right now, we don't have to do any more than this.  */
  XFree (transfer);
}

Bool
OwnSelection (Timestamp time, Atom selection,
	      GetDataFunc (*hook) (WriteTransfer *transfer,
				   Atom, Atom *),
	      Atom *targets, int ntargets)
{
  Window owner;
  SelectionOwnerInfo *info;

  info = XLLookUpAssoc (selection_owner_info, selection);

  if (info && TimestampIs (time, Earlier, info->time))
    /* The timestamp is out of date.  */
    return False;

  XSetSelectionOwner (compositor.display, selection,
		      selection_transfer_window,
		      time.milliseconds);

  /* Check if selection ownership was actually set.  */
  owner = XGetSelectionOwner (compositor.display, selection);

  /* If ownership wasn't successfully set, return.  */
  if (owner != selection_transfer_window)
    return False;

  /* Otherwise, update or allocate the info structure.  */

  if (!info)
    {
      info = XLMalloc (sizeof *info);

      XLMakeAssoc (selection_owner_info, selection,
		   info);
    }
  else
    XLFree (info->targets);

  info->time = time;
  info->targets = XLMalloc (sizeof *info->targets * ntargets);
  info->ntargets = ntargets;
  info->get_transfer_function = hook;
  memcpy (info->targets, targets, sizeof *targets * ntargets);

  return True;
}

void
DisownSelection (Atom selection)
{
  SelectionOwnerInfo *info;

  info = XLLookUpAssoc (selection_owner_info, selection);

  if (info && info->get_transfer_function)
    {
      XSetSelectionOwner (compositor.display, selection,
			  None, info->time.milliseconds);

      /* Also free info->targets.  */
      XLFree (info->targets);
      info->targets = NULL;

      /* And set the selection to disowned.  */
      info->get_transfer_function = NULL;
    }
}

void
StartReading (WriteTransfer *transfer)
{
  TransferBecameReadable (transfer);
}

void
InitSelections (void)
{
  XSetWindowAttributes attrs;
  int flags;

  /* Set up sentinel nodes for various lists.  */
  prop_atoms.next = &prop_atoms;
  prop_atoms.last = &prop_atoms;
  free_list.next = &free_list;
  free_list.last = &free_list;
  read_transfers.next = &read_transfers;
  read_transfers.last = &read_transfers;
  write_transfers.next = &write_transfers;
  write_transfers.last = &write_transfers;
  queued_transfers.next = &queued_transfers;
  queued_transfers.last = &queued_transfers;

  /* Make the window used to transfer selection data.  */
  attrs.override_redirect = True;
  attrs.event_mask = PropertyChangeMask;
  flags = CWEventMask | CWOverrideRedirect;

  selection_transfer_window
    = XCreateWindow (compositor.display,
		     DefaultRootWindow (compositor.display),
		     -1, -1, 1, 1, 0, CopyFromParent, InputOnly,
		     CopyFromParent, flags, &attrs);

  /* Make the assoc tables used to keep track of input masks and
     selection data.  */
  foreign_notify_table = XLCreateAssocTable (32);
  selection_owner_info = XLCreateAssocTable (32);

  DebugPrint ("Selection transfer window is %lu\n",
	      selection_transfer_window);
}
