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

#include <sys/mman.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/errno.h>

#include <stdlib.h>
#include <stdio.h>

#include <math.h>

#include "compositor.h"

struct _RootWindowSelection
{
  /* The next and last event selection records in this chain.  */
  RootWindowSelection *next, *last;

  /* The event mask one piece of code has selected for.  */
  unsigned long event_mask;
};

/* Events that are being selected for on the root window.  */
static RootWindowSelection root_window_events;

void
XLListFree (XLList *list, void (*item_func) (void *))
{
  XLList *tem, *last;

  tem = list;

  while (tem)
    {
      last = tem;
      tem = tem->next;

      if (item_func)
	item_func (last->data);
      XLFree (last);
    }
}

XLList *
XLListRemove (XLList *list, void *data)
{
  XLList *tem, **last;

  last = &list;

  while (*last)
    {
      tem = *last;

      if (tem->data == data)
	{
	  *last = tem->next;
	  XLFree (tem);
	}
      else
	last = &tem->next;
    }

  return list;
}

XLList *
XLListPrepend (XLList *list, void *data)
{
  XLList *tem;

  tem = XLMalloc (sizeof *tem);
  tem->data = data;
  tem->next = list;

  return tem;
}


/* List of XIDs (not pointers).  */

void
XIDListFree (XIDList *list, void (*item_func) (XID))
{
  XIDList *tem, *last;

  tem = list;

  while (tem)
    {
      last = tem;
      tem = tem->next;

      if (item_func)
	item_func (last->data);
      XLFree (last);
    }
}

XIDList *
XIDListRemove (XIDList *list, XID resource)
{
  XIDList *tem, **last;

  last = &list;

  while (*last)
    {
      tem = *last;

      if (tem->data == resource)
	{
	  *last = tem->next;
	  XLFree (tem);
	}
      else
	last = &tem->next;
    }

  return list;
}

XIDList *
XIDListPrepend (XIDList *list, XID resource)
{
  XIDList *tem;

  tem = XLMalloc (sizeof *tem);
  tem->data = resource;
  tem->next = list;

  return tem;
}


/* Hash tables between XIDs and arbitrary data.  */

XLAssocTable *
XLCreateAssocTable (int size)
{
  XLAssocTable *table;
  XLAssoc *buckets;

  table = XLMalloc (sizeof *table);
  buckets = XLCalloc (size, sizeof *buckets);

  table->buckets = buckets;
  table->size = size;

  while (--size >= 0)
    {
      /* Initialize each bucket with the sentinel node. */
      buckets->prev = buckets;
      buckets->next = buckets;
      buckets++;
    }

  return table;
}

static void
Insque (XLAssoc *velem, XLAssoc *vprev)
{
  XLAssoc *elem, *prev, *next;

  elem = velem;
  prev = vprev;
  next = prev->next;

  prev->next = elem;
  if (next)
    next->prev = elem;
  elem->next = next;
  elem->prev = prev;
}

static void
Remque (XLAssoc *velem)
{
  XLAssoc *elem, *prev, *next;

  elem = velem;
  next = elem->next;
  prev = elem->prev;

  if (next)
    next->prev = prev;

  if (prev)
    prev->next = next;
}

void
XLMakeAssoc (XLAssocTable *table, XID x_id, void *data)
{
  int hash;
  XLAssoc *bucket, *entry, *new_entry;

  hash = x_id % table->size;
  bucket = &table->buckets[hash];
  entry = bucket->next;

  if (entry != bucket)
    {
      /* Bucket isn't empty, start searching.  */

      for (; entry != bucket; entry = entry->next)
	{
	  if (entry->x_id == x_id)
	    {
	      entry->data = data;
	      return;
	    }

	  if (entry->x_id > x_id)
	    break;
	}
    }

  /* Insert new_entry immediately before entry.  */

  new_entry = XLMalloc (sizeof *new_entry);
  new_entry->x_id = x_id;
  new_entry->data = data;

  Insque (new_entry, entry->prev);
}

void *
XLLookUpAssoc (XLAssocTable *table, XID x_id)
{
  int hash;
  XLAssoc *bucket, *entry;

  hash = x_id % table->size;
  bucket = &table->buckets[hash];
  entry = bucket->next;

  for (; entry != bucket; entry = entry->next)
    {
      if (entry->x_id == x_id)
	return entry->data;

      if (entry->x_id > x_id)
	return NULL;
    }

  return NULL;
}

void
XLDeleteAssoc (XLAssocTable *table, XID x_id)
{
  int hash;
  XLAssoc *bucket, *entry;

  hash = x_id % table->size;
  bucket = &table->buckets[hash];
  entry = bucket->next;

  for (; entry != bucket; entry = entry->next)
    {
      if (entry->x_id == x_id)
	{
	  Remque (entry);
	  XLFree (entry);
	  return;
	}

      if (entry->x_id > x_id)
	return;
    }
}

void
XLDestroyAssocTable (XLAssocTable *table)
{
  int i;
  XLAssoc *bucket, *entry, *entry_next;

  for (i = 0; i < table->size; i++)
    {
      bucket = &table->buckets[i];

      for (entry = bucket->next; entry != bucket;
	   entry = entry_next)
	{
	  entry_next = entry->next;
	  XLFree (entry);
	}
    }

  XLFree (table->buckets);
  XLFree (table);
}

void
XLAssert (Bool condition)
{
  if (!condition)
    abort ();
}

void
XLScaleRegion (pixman_region32_t *dst, pixman_region32_t *src,
	       float scale_x, float scale_y)
{
  int nrects, i;
  pixman_box32_t *src_rects;
  pixman_box32_t *dst_rects;

  if (scale_x == 1.0f && scale_y == 1.0f)
    {
      pixman_region32_copy (dst, src);
      return;
    }

  src_rects = pixman_region32_rectangles (src, &nrects);

  if (nrects < 128)
    dst_rects = alloca (nrects * sizeof *dst_rects);
  else
    dst_rects = XLMalloc (nrects * sizeof *dst_rects);

  for (i = 0; i < nrects; ++i)
    {
      dst_rects[i].x1 = floor (src_rects[i].x1 * scale_x);
      dst_rects[i].x2 = ceil (src_rects[i].x2 * scale_x);
      dst_rects[i].y1 = floor (src_rects[i].y1 * scale_y);
      dst_rects[i].y2 = ceil (src_rects[i].y2 * scale_y);
    }

  pixman_region32_fini (dst);
  pixman_region32_init_rects (dst, dst_rects, nrects);

  if (nrects >= 128)
    XLFree (dst_rects);
}

int
XLOpenShm (void)
{
  char name[sizeof "SharedBufferXXXXXXXX"];
  int fd;
  unsigned int i;

  i = 0;

  while (i <= 0xffffffff)
    {
      sprintf (name, "SharedBuffer%x", i);
      fd = shm_open (name, O_RDWR | O_CREAT | O_EXCL, 0600);

      if (fd >= 0)
	{
	  shm_unlink (name);
	  return fd;
	}

      if (errno == EEXIST)
	++i;
      else
	{
	  perror ("shm_open");
	  exit (1);
	}
    }

  return -1;
}

static Bool
ServerTimePredicate (Display *display, XEvent *event, XPointer arg)
{
  return (event->type == PropertyNotify
	  && event->xproperty.window == selection_transfer_window
	  && event->xproperty.atom == _XL_SERVER_TIME_ATOM);
}

Time
XLGetServerTimeRoundtrip (void)
{
  XEvent event;

  XChangeProperty (compositor.display, selection_transfer_window,
		   _XL_SERVER_TIME_ATOM, XA_ATOM, 32, PropModeReplace,
		   (unsigned char *) &_XL_SERVER_TIME_ATOM, 1);
  XIfEvent (compositor.display, &event, ServerTimePredicate, NULL);

  return event.xproperty.time;
}

static void
ReselectRootWindowInput (void)
{
  unsigned long effective;
  RootWindowSelection *record;

  effective = NoEventMask;
  record = root_window_events.next;

  if (!record)
    return;

  while (record != &root_window_events)
    {
      effective |= record->event_mask;
      record = record->next;
    }

  XSelectInput (compositor.display,
		DefaultRootWindow (compositor.display),
		effective);
}

RootWindowSelection *
XLSelectInputFromRootWindow (unsigned long event_mask)
{
  RootWindowSelection *selection;

  /* This lets different pieces of code select for input from the root
     window without clobbering eachothers event masks.  */

  selection = XLMalloc (sizeof *selection);

  /* If the global chain has not yet been initialized, initialize it
     now.  */
  if (!root_window_events.next)
    {
      root_window_events.next = &root_window_events;
      root_window_events.last = &root_window_events;
    }

  /* Link this onto the chain of events being selected for on the root
     window.  */
  selection->next = root_window_events.next;
  selection->last = &root_window_events;
  root_window_events.next->last = selection;
  root_window_events.next = selection;

  /* Set the event mask.  */
  selection->event_mask = event_mask;

  /* Actually select for events.  */
  ReselectRootWindowInput ();
  return selection;
}

void
XLDeselectInputFromRootWindow (RootWindowSelection *key)
{
  key->last->next = key->next;
  key->next->last = key->last;

  XLFree (key);
  ReselectRootWindowInput ();
}
