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

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <math.h>

#include "compositor.h"

typedef struct _Busfault Busfault;

struct _RootWindowSelection
{
  /* The next and last event selection records in this chain.  */
  RootWindowSelection *next, *last;

  /* The event mask one piece of code has selected for.  */
  unsigned long event_mask;
};

struct _Busfault
{
  /* Nodes to the left and right.  */
  Busfault *left, *right;

  /* Start of the ignored area.  */
  char *data;

  /* Size of the ignored area.  */
  size_t ignored_area;

  /* Height of this node.  */
  int height;
};

/* Events that are being selected for on the root window.  */
static RootWindowSelection root_window_events;

/* All busfaults.  */
static Busfault *busfault_tree;

/* Whether or not the SIGBUS handler has been installed.  */
static Bool bus_handler_installed;

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

static int
GetHeight (Busfault *busfault)
{
  if (!busfault)
    return 0;

  return busfault->height;
}

static void
FixHeights (Busfault *busfault)
{
  XLAssert (busfault != NULL);

  busfault->height = 1 + MAX (GetHeight (busfault->left),
			      GetHeight (busfault->right));
}

static void
RotateLeft (Busfault **root)
{
  Busfault *old_root, *new_root, *old_middle;

  /* Rotate *root->left to *root.  */
  old_root = *root;
  new_root = old_root->left;
  old_middle = new_root->right;
  old_root->left = old_middle;
  new_root->right = old_root;
  *root = new_root;

  /* Update heights.  */
  FixHeights ((*root)->right);
  FixHeights (*root);
}

static void
RotateRight (Busfault **root)
{
  Busfault *old_root, *new_root, *old_middle;

  /* Rotate *root->right to *root.  */
  old_root = *root;
  new_root = old_root->right;
  old_middle = new_root->left;
  old_root->right = old_middle;
  new_root->left = old_root;
  *root = new_root;

  /* Update heights.  */
  FixHeights ((*root)->left);
  FixHeights (*root);
}

static void
RebalanceBusfault (Busfault **tree)
{
  if (*tree)
    {
      /* Check the left.  It could be too tall.  */
      if (GetHeight ((*tree)->left)
	  > GetHeight ((*tree)->right) + 1)
	{
	  /* The left side is unbalanced.  Look for a taller
	     grandchild of tree->left.  */

	  if (GetHeight ((*tree)->left->left)
	      > GetHeight ((*tree)->left->right))
	    RotateLeft (tree);
	  else
	    {
	      RotateRight (&(*tree)->left);
	      RotateLeft (tree);
	    }

	  return;
	}
      /* Then, check the right.  */
      else if (GetHeight ((*tree)->right)
	       > GetHeight ((*tree)->left) + 1)
	{
	  /* The right side is unbalanced.  Look for a taller
	     grandchild of tree->right.  */

	  if (GetHeight ((*tree)->right->right)
	      > GetHeight ((*tree)->right->left))
	    RotateRight (tree);
	  else
	    {
	      RotateLeft (&(*tree)->right);
	      RotateRight (tree);
	    }

	  return;
	}

      /* Nothing was called, so fix heights.  */
      FixHeights (*tree);
    }
}

static void
RecordBusfault (Busfault **tree, Busfault *node)
{
  if (!*tree)
    {
      /* Tree is empty.  Replace it with node.  */
      *tree = node;
      node->left = NULL;
      node->right = NULL;
      node->height = 1;

      return;
    }

  /* Record busfault into the correct subtree.  */
  if (node->data > (*tree)->data)
    RecordBusfault (&(*tree)->right, node);
  else
    RecordBusfault (&(*tree)->left, node);

  /* Balance the tree.  */
  RebalanceBusfault (tree);
}

static Busfault *
DetectBusfault (Busfault *tree, char *address)
{
  /* This function is reentrant, but the functions that remove and
     insert busfaults are not.  */

  if (!tree)
    return NULL;
  else if (address >= tree->data
	   && address < tree->data + tree->ignored_area)
    return tree;

  /* Continue searching in the tree.  */
  if (address > tree->data)
    /* Search in the right node.  */
    return DetectBusfault (tree->right, address);

  /* Search in the left.  */
  return DetectBusfault (tree->left, address);
}

static void
DeleteMin (Busfault **tree, Busfault *out)
{
  Busfault *old_root;

  /* Delete and return the minimum value of the tree.  */

  XLAssert (*tree != NULL);

  if (!(*tree)->left)
    {
      /* *tree contains the smallest value.  */
      old_root = *tree;
      out->data = old_root->data;
      out->ignored_area = old_root->ignored_area;
      *tree = old_root->right;
      XLFree (old_root);
    }
  else
    /* Keep looking to the left.  */
    DeleteMin (&(*tree)->left, out);

  RebalanceBusfault (tree);
}

static void
RemoveBusfault (Busfault **tree, char *data)
{
  Busfault *old_root;

  if (!*tree)
    /* There should always be a busfault.  */
    abort ();
  else if ((*tree)->data == data)
    {
      if ((*tree)->right)
	/* Replace with min value of right subtree.  */
	DeleteMin (&(*tree)->right, *tree);
      else
	{
	  /* Splice out old root.  */
	  old_root = *tree;
	  *tree = (*tree)->left;
	  XLFree (old_root);
	}
    }
  else if (data > (*tree)->data)
    /* Delete child from the right.  */
    RemoveBusfault (&(*tree)->right, data);
  else
    /* Delete from the left.  */
    RemoveBusfault (&(*tree)->left, data);

  RebalanceBusfault (tree);
}

static void
HandleBusfault (int signal, siginfo_t *siginfo, void *ucontext)
{
  /* SIGBUS received.  If the faulting address is currently part of a
     shared memory buffer, ignore.  Otherwise, print an error and
     return.  Only reentrant functions must be called within this
     signal handler.  */

  if (DetectBusfault (busfault_tree, siginfo->si_addr))
    return;

  write (2, "unexpected bus fault\n",
	 sizeof "unexpected bus fault\n");
  _exit (EXIT_FAILURE);
}

static void
MaybeInstallBusHandler (void)
{
  struct sigaction act;

  /* If the SIGBUS handler is already installed, return.  */
  if (bus_handler_installed)
    return;

  bus_handler_installed = True;
  memset (&act, 0, sizeof act);

  /* Install a SIGBUS handler.  When a client truncates the file
     backing a shared memory buffer without notifying the compositor,
     attempting to access the contents of the mapped memory beyond the
     end of the file will result in SIGBUS being called, which we
     handle here.  */

  act.sa_flags = SA_SIGINFO;
  act.sa_sigaction = HandleBusfault;

  if (sigaction (SIGBUS, &act, NULL))
    {
      perror ("sigaction");
      abort ();
    }
}

static void
BlockSigbus (void)
{
  sigset_t sigset;

  sigemptyset (&sigset);
  sigaddset (&sigset, SIGBUS);

  if (sigprocmask (SIG_BLOCK, &sigset, NULL))
    {
      perror ("sigprocmask");
      abort ();
    }
}

static void
UnblockSigbus (void)
{
  sigset_t sigset;

  sigemptyset (&sigset);

  if (sigprocmask (SIG_BLOCK, &sigset, NULL))
    {
      perror ("sigprocmask");
      abort ();
    }
}

/* These must not overlap.  */

void
XLRecordBusfault (void *data, size_t data_size)
{
  Busfault *node;

  MaybeInstallBusHandler ();

  BlockSigbus ();
  node = XLMalloc (sizeof *node);
  node->data = data;
  node->ignored_area = data_size;

  RecordBusfault (&busfault_tree, node);
  UnblockSigbus ();
}

void
XLRemoveBusfault (void *data)
{
  BlockSigbus ();
  RemoveBusfault (&busfault_tree, data);
  UnblockSigbus ();
}
