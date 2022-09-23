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

#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>

#include <pthread.h>
#include <iconv.h>

#include "compositor.h"
#include "primary-selection-unstable-v1.h"

#include <X11/extensions/Xfixes.h>

/* X11 data transfer to and from Wayland clients.  */

typedef struct _ReadTargetsData ReadTargetsData;
typedef struct _TargetMapping TargetMapping;
typedef struct _TransferInfo TransferInfo;
typedef struct _ConversionTransferInfo ConversionTransferInfo;
typedef struct _WriteInfo WriteInfo;
typedef struct _ConversionWriteInfo ConversionWriteInfo;
typedef struct _DataConversion DataConversion;
typedef struct _TargetMappingTable TargetMappingTable;

struct _ReadTargetsData
{
  /* Array of atoms read from the selection.  */
  Atom *atoms;

  /* Number of atoms read.  */
  int n_atoms;

  /* What selection is being read from.  */
  Atom selection;
};

struct _TargetMapping
{
  /* The atom of the X target.  The top 3 bits of an XID are
     guaranteed to be 0, so the 31st bit is used to store a flag
     meaning that the next entry is a duplicate of this one, and the
     30th bit is used to store a flag containing state used by
     SendOffers.  */
  Atom atom_flag;

  /* The name of the Wayland MIME type.  */
  const char *mime_type;

  /* Optional translation function.  */
  void (*translation_func) (Time, Atom, Atom, int);
};

#define MappingAtom(mapping)		((mapping)->atom_flag & 0x1fffffff)
#define MappingFlag(mapping)		((mapping)->atom_flag & (1 << 29))
#define MappingIsNextDuplicate(mapping)	((mapping)->atom_flag & (1 << 30))
#define MappingSetFlag(mapping)		((mapping)->atom_flag |= (1 << 29))
#define MappingUnsetFlag(mapping)	((mapping)->atom_flag &= ~(1 << 29))
#define MappingIs(mapping, atom)	(MappingAtom (mapping) == (atom))

struct _DataConversion
{
  /* The MIME type of the Wayland offer.  */
  const char *mime_type;

  /* The atom describing the type of the property data.  */
  Atom type;

  /* The atom of the X target.  */
  Atom atom;

  /* An alternative GetClipboardCallback, if any.  */
  GetDataFunc (*clipboard_callback) (WriteTransfer *, Atom, Atom *,
				     struct wl_resource *,
				     void (*) (struct wl_resource *,
					       const char *, int),
				     Bool);
};

enum
  {
    NeedNewChunk      = 1,
    NeedDelayedFinish = (1 << 2),
  };

struct _TransferInfo
{
  /* The file descriptor being written to.  -1 if it was closed.  */
  int fd;

  /* Some flags.  */
  int flags;

  /* The currently buffered chunk.  */
  unsigned char *chunk;

  /* The size of the currently buffered chunk, the number of bytes
     into the chunk that have been written, and the number of bytes in
     the property after the currently buffered chunk.  */
  ptrdiff_t chunk_size, bytes_into, bytes_after;

  /* Any active file descriptor write callback.  */
  WriteFd *write_callback;
};

struct _ConversionTransferInfo
{
  /* The file descriptor being written to.  -1 if it was closed.  */
  int fd;

  /* Some flags.  */
  int flags;

  /* Conversion buffer.  */
  char *buffer, *position;

  /* The bytes remaining in the conversion buffer.  */
  size_t buffer_size;

  /* The output buffer.  */
  char output_buffer[BUFSIZ];

  /* And the amount of data used in the output buffer.  */
  size_t outsize;

  /* The data format conversion context.  */
  iconv_t cd;

  /* Any active file descriptor write callback.  */
  WriteFd *write_callback;
};

enum
  {
    IsDragAndDrop = (1 << 16),
  };

struct _WriteInfo
{
  /* The file descriptor being read from.  */
  int fd;

  /* Some flags.  */
  int flags;

  /* Any active file descriptor read callback.  */
  ReadFd *read_callback;
};

enum
  {
    ReachedEndOfFile = 1,
  };

struct _ConversionWriteInfo
{
  /* The file descriptor being read from.  -1 if it was closed.  */
  int fd;

  /* Some flags.  */
  int flags;

  /* Any active file descriptor read callback.  */
  ReadFd *read_callback;

  /* Input buffer for iconv.  */
  char inbuf[BUFSIZ];

  /* Number of bytes into the input buffer that have been read.  */
  size_t inread;

  /* Pointer to the current position in the input buffer.  Used by
     iconv.  */
  char *inptr;

  /* Iconv context for this conversion.  */
  iconv_t cd;
};

struct _TargetMappingTable
{
  /* Array of indices into direct_transfer.  */
  unsigned short *buckets[32];

  /* Number of elements in each array.  */
  unsigned short n_elements[32];

  /* Array of indices into direct_transfer.  */
  unsigned short *atom_buckets[16];

  /* Number of elements in each array.  */
  unsigned short n_atom_elements[16];
};

/* Base event code of the Xfixes extension.  */
static int fixes_event_base;

/* This means the next item in the targets mapping table has the same
   MIME type as this one.  */

#define Duplicate (1U << 30)

/* Map of targets that can be transferred from X to Wayland clients
   and vice versa.  */
static TargetMapping direct_transfer[] =
  {
    { XA_STRING,	"text/plain;charset=iso-9889-1"	},
    { Duplicate,	"text/plain;charset=utf-8"	},
    { XA_STRING,	"text/plain;charset=utf-8"	},
    /* These mappings are automatically generated.  */
    DirectTransferMappings
  };

/* Lookup table for such mappings.  */
static TargetMappingTable mapping_table;

/* Map of Wayland offer types to X atoms and data conversion
   functions.  */
static DataConversion data_conversions[] =
  {
    { "text/plain;charset=utf-8", 0, 0, NULL },
    { "text/plain;charset=utf-8", 0, 0,	NULL },
  };

/* The time of the last X selection change.  */
static Time last_x_selection_change;

/* The time ownership was last asserted over CLIPBOARD, and the last
   time any client did that.  */
static Time last_clipboard_time, last_clipboard_change;

/* The last time ownership over PRIMARY changed.  */
static Time last_primary_time;

/* The currently supported selection targets.  */
static Atom *x_selection_targets;

/* The number of targets in that array.  */
static int num_x_selection_targets;

/* The currently supported primary selection targets.  */
static Atom *x_primary_targets;

/* The number of targets in that array.  */
static int num_x_primary_targets;

/* Data source currently being used to provide the X clipboard.  */
static DataSource *selection_data_source;

/* Data source currently being used to provide the X drag-and-drop
   selection.  */
static DataSource *drag_data_source;

/* Data source currently being used to provide the primary
   selection.  */
static PDataSource *primary_data_source;

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

static void
Accept (struct wl_client *client, struct wl_resource *resource,
	uint32_t serial, const char *mime_type)
{
  /* Nothing has to be done here yet.  */
}

static unsigned int
HashMimeString (const char *string)
{
  unsigned int i;

  i = 3323198485ul;
  for (; *string; ++string)
    {
      i ^= *string;
      i *= 0x5bd1e995;
      i ^= i >> 15;
    }
  return i;
}

static void
SetupMappingTable (void)
{
  unsigned int idx, i, nelements;

  /* This is needed for the atoms table, since the atom indices are
     determined by the X server.  */
  XLAssert (ArrayElements (direct_transfer) <= USHRT_MAX);

  for (i = 0; i < ArrayElements (direct_transfer); ++i)
    {
      idx = HashMimeString (direct_transfer[i].mime_type) % 32;

      nelements = ++mapping_table.n_elements[idx];
      mapping_table.buckets[idx]
	= XLRealloc (mapping_table.buckets[idx],
		     nelements * sizeof (unsigned short));
      mapping_table.buckets[idx][nelements - 1] = i;

      /* Now, set up the lookup table indexed by atoms.  It is faster
	 to compare atoms than strings, so the table is smaller.  */

      idx = MappingAtom (&direct_transfer[i]) % 16;

      nelements = ++mapping_table.n_atom_elements[idx];
      mapping_table.atom_buckets[idx]
	= XLRealloc (mapping_table.atom_buckets[idx],
		     nelements * sizeof (unsigned short));
      mapping_table.atom_buckets[idx][nelements - 1] = i;
    }
}

static Bool
HasSelectionTarget (Atom atom, Bool is_primary)
{
  int i;

  if (is_primary)
    {
      /* Do this for the primary selection instead.  */

      for (i = 0; i < num_x_primary_targets; ++i)
	{
	  if (x_primary_targets[i] == atom)
	    return True;
	}

      return False;
    }

  for (i = 0; i < num_x_selection_targets; ++i)
    {
      if (x_selection_targets[i] == atom)
	return True;
    }

  return False;
}

static TargetMapping *
FindTranslationForMimeType (const char *mime_type, Bool is_primary)
{
  unsigned short *buckets, i;
  unsigned int idx;

  idx = HashMimeString (mime_type) % 32;
  buckets = mapping_table.buckets[idx];

  DebugPrint ("Looking for translation of MIME type %s\n",
	      mime_type);

  for (i = 0; i < mapping_table.n_elements[idx]; ++i)
    {
      if (!strcmp (direct_transfer[buckets[i]].mime_type,
		   mime_type)
	  && HasSelectionTarget (MappingAtom (&direct_transfer[buckets[i]]),
				 is_primary))
	{
	  DebugPrint ("Found translation for MIME type %s\n",
		      mime_type);

	  return &direct_transfer[buckets[i]];
	}
    }

  return NULL;
}

static void
FinishTransfer (TransferInfo *info)
{
  if (info->write_callback)
    XLRemoveWriteFd (info->write_callback);

  if (info->fd != -1)
    /* Close the file descriptor, letting the client know that the
       transfer completed.  */
    close (info->fd);

  /* Finally, free the info structure.  */
  XLFree (info);
}

static void
MaybeFinishDelayedTransfer (ReadTransfer *transfer,
			    TransferInfo *info)
{
  if (info->flags & NeedDelayedFinish)
    {
      DebugPrint ("Completing a delayed transfer.\n");
      FinishTransfer (info);
      CompleteDelayedTransfer (transfer);
    }
}

static void
NoticeTransferWritable (int fd, void *data)
{
  ReadTransfer *transfer;
  TransferInfo *info;
  long quantum;
  unsigned char *chunk;
  ptrdiff_t chunk_size, bytes_after;
  ssize_t written;

  DebugPrint ("File descriptor %d became writable\n", fd);

  transfer = data;
  info = GetTransferData (transfer);

  /* Start by reading at most this many bytes from the property.
     TODO: take into account sizeof (long) == 8.  */
  quantum = SelectionQuantum () / 4 * 4;

  if (!info->chunk)
    {
    read_chunk:
      info->flags &= ~NeedNewChunk;
      chunk = ReadChunk (transfer, quantum / 4,
			 &chunk_size, &bytes_after);
      DebugPrint ("Reading a piece of the property of size %ld\n",
		  quantum);

      /* If chunk is NULL, close fd.  The failure callback will also be
	 run soon.  */
      if (!chunk)
	{
	  DebugPrint ("Read failed\n");
	  if (info->fd != -1)
	    close (info->fd);

	  info->fd = -1;

	  MaybeFinishDelayedTransfer (transfer, info);
	  return;
	}

      /* Set info->chunk to the chunk and the chunk size.  */
      info->chunk = chunk;
      info->chunk_size = chunk_size;
      info->bytes_after = bytes_after;
      info->bytes_into = 0;

      DebugPrint ("Read actually got: %td, with %td after\n",
		  chunk_size, bytes_after);
    }

  DebugPrint ("Writing %td bytes of chunk at offset %td\n",
	      info->chunk_size - info->bytes_into,
	      info->bytes_into);

  /* Try to write the chunk into the fd.  */
  written = write (fd, info->chunk + info->bytes_into,
		   info->chunk_size - info->bytes_into);

  DebugPrint ("%zd bytes were really written; offset is now %td\n",
	      written, info->bytes_into + written);

  if (written < 0)
    {
      DebugPrint ("Some bytes could not be written: %s\n",
		  strerror (errno));

      /* Write failed with EAGAIN.  This might cause us to spin.  */
      if (errno == EAGAIN)
	return;

      /* Write failed with EPIPE.  */
      if (errno == EPIPE)
	{
	  /* EPIPE happened; set the fd to -1, skip the chunk if it
	     was not completely read, and return.  */
	  if (!info->bytes_after)
	    SkipChunk (transfer);

	  close (info->fd);
	  info->fd = -1;
	  XFree (info->chunk);
	  info->chunk = NULL;

	  DebugPrint ("EPIPE recieved while reading; cancelling transfer\n");

	  MaybeFinishDelayedTransfer (transfer, info);
	  return;
	}

      perror ("write");
      exit (1);
    }

  /* See how much was written.  */
  info->bytes_into += written;

  if (info->bytes_into == info->chunk_size)
    {
      DebugPrint ("Chunk of %td completely written; bytes left "
		  "in property: %td\n", info->chunk_size,
		  info->bytes_after);

      /* The entire read part of the chunk has been written; read a
	 new chunk, or cancel the write callback if the chunk was
	 completely read.  */

      XFree (info->chunk);
      info->chunk = NULL;

      if (info->bytes_after)
	/* There are still bytes in the property waiting to be
	   read.  */
	goto read_chunk;
      else
	{
	  /* The property has been completely read.  If a new chunk
	     already exists, read and try to write it now.  Otherwise,
	     cancel the write callback and wait for either a new
	     property to be set, or for DirectFinishCallback to be
	     called.  */

	  if (info->flags & NeedNewChunk)
	    goto read_chunk;
	  else
	    {
	      DebugPrint ("Removing write callback\n");

	      XLRemoveWriteFd (info->write_callback);
	      info->write_callback = NULL;
	      MaybeFinishDelayedTransfer (transfer, info);
	    }
	}
    }
}

static void
DirectReadCallback (ReadTransfer *transfer, Atom type, int format,
		    ptrdiff_t size)
{
  TransferInfo *info;

  info = GetTransferData (transfer);

  /* This is the start of a chunk.  If the fd was closed, simply skip
     the chunk.  */
  if (info->fd == -1)
    {
      SkipChunk (transfer);

      DebugPrint ("DirectReadCallback skipped a chunk due to the"
		  " fd being closed\n");
      return;
    }

  if (info->write_callback)
    {
      /* Make sure two chunks aren't read at the same time.  */
      XLAssert (!(info->flags & NeedNewChunk));

      DebugPrint ("DirectReadCallback received a new chunk, but the"
		  " current chunk is still being read from\n");

      info->flags |= NeedNewChunk;
      return;
    }

  /* Ask for a notification to be sent once the file descriptor
     becomes writable.  */
  XLAssert (!info->write_callback);

  DebugPrint ("DirectReadCallback is starting the write callback\n");

  info->write_callback = XLAddWriteFd (info->fd, transfer,
				       NoticeTransferWritable);
}

static Bool
DirectFinishCallback (ReadTransfer *transfer, Bool success)
{
  TransferInfo *info;

  /* The read completed.  Clean up any memory and write callbacks that
     might have been allocated or installed.  */

  info = GetTransferData (transfer);

  if (info->chunk)
    {
      /* The write callback should still exist, since this means the
	 data has not yet been fully written.  */
      XLAssert (info->write_callback != NULL);

      DebugPrint ("The transfer finished, but the chunk was not yet"
		  " completely written; the finish is being delayed.\n");
      info->flags |= NeedDelayedFinish;

      return False;
    }
  else
    DebugPrint ("The transfer finished %s\n",
		success ? "successfully" : "with failure");

  FinishTransfer (info);
  return True;
}

static void
MakeFdNonblocking (int fd)
{
  int rc;

  rc = fcntl (fd, F_GETFL);

  if (rc < 0)
    {
      DebugPrint ("Failed to make selection file descriptor"
		  " %d non-blocking.  Writing to it might hang.\n",
		  fd);
      return;
    }

  rc = fcntl (fd, F_SETFL, rc | O_NONBLOCK);

  if (rc < 0)
    {
      DebugPrint ("Failed to make selection file descriptor"
		  " %d non-blocking.  Writing to it might hang.\n",
		  fd);
      return;
    }
}

static void
PostReceiveDirect (Time time, Atom selection, Atom target, int fd)
{
  TransferInfo *info;

  info = XLCalloc (1, sizeof *info);
  info->fd = fd;

  /* Try to make the file description nonblocking.  Clients seem to
     behave fine this way.  */
  MakeFdNonblocking (fd);

  DebugPrint ("Converting selection at %lu for fd %d\n", time, fd);

  ConvertSelectionFuncs (selection, target, time,
			 info, NULL, DirectReadCallback,
			 DirectFinishCallback);
}

/* Forward declaration.  */

static void PostReceiveConversion (Time, Atom, Atom, int);

#define ReceiveBody(selection, primary)					\
  Time time;								\
  TargetMapping *translation;						\
									\
  DebugPrint ("Receiving %s from X " #selection " \n",			\
	      mime_type);						\
									\
  /* Cast to intptr_t to silence warnings when the pointer type is	\
     larger than long.  */						\
  time = (Time) (intptr_t) wl_resource_get_user_data (resource);	\
									\
  /* Find which selection target corresponds to MIME_TYPE.  */		\
  translation = FindTranslationForMimeType (mime_type, primary);	\
									\
  if (translation)							\
    {									\
      if (!translation->translation_func)				\
	/* If a corresponding target exists, ask to receive it.  */	\
	PostReceiveDirect (time, selection,				\
			   MappingAtom (translation), fd);		\
      else								\
	/* Otherwise, use the translation function.  */			\
	translation->translation_func (time, selection,			\
				       MappingAtom (translation),	\
				       fd);				\
    }									\
  else									\
    close (fd)

static void
Receive (struct wl_client *client, struct wl_resource *resource,
	 const char *mime_type, int fd)
{
  ReceiveBody (CLIPBOARD, False);
}

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
Finish (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
			  "trying to finish foreign data offer");
}

static void
SetActions (struct wl_client *client, struct wl_resource *resource,
	    uint32_t dnd_actions, uint32_t preferred_action)
{
  wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_OFFER,
			  "trying to finish non-drag-and-drop data offer");
}

static const struct wl_data_offer_interface wl_data_offer_impl =
  {
    .accept = Accept,
    .receive = Receive,
    .destroy = Destroy,
    .finish = Finish,
    .set_actions = SetActions,
  };

static struct wl_resource *
CreateOffer (struct wl_client *client, Time time)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_data_offer_interface,
				 3, 0);

  if (!resource)
    /* If allocating the resource failed, return NULL.  */
    return NULL;

  /* Otherwise, set the user_data to the time of the selection
     change.  */
  wl_resource_set_implementation (resource, &wl_data_offer_impl,
				  (void *) time, NULL);
  return resource;
}

static void
ReceivePrimary (struct wl_client *client, struct wl_resource *resource,
		const char *mime_type, int fd)
{
  ReceiveBody (XA_PRIMARY, True);
}

static struct zwp_primary_selection_offer_v1_interface primary_offer_impl =
  {
    .receive = ReceivePrimary,
    .destroy = Destroy,
  };

static struct wl_resource *
CreatePrimaryOffer (struct wl_client *client, Time time)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client,
				 &zwp_primary_selection_offer_v1_interface,
				 1, 0);
  if (!resource)
    /* If allocating the resource failed, return NULL.  */
    return NULL;

  /* Otherwise, set the user_data to the time of the selection
     change.  */
  wl_resource_set_implementation (resource, &primary_offer_impl,
				  (void *) time, NULL);
  return resource;
}

static Bool
CheckDuplicate (unsigned short index, Atom a)
{
  TargetMapping *start;

  start = &direct_transfer[index];

  /* If the flag is already set, then this type has already been
     sent.  */
  if (MappingFlag (start))
    return False;

  /* Set this entry's duplicate flag.  */
  MappingSetFlag (start);

  /* As long as the next index still refers to a duplicate of this
     item, set its duplicate flag.  */

  while (MappingIsNextDuplicate (start))
    MappingSetFlag (++start);

  /* Do the same backwards.  */

  if (index)
    {
      start = &direct_transfer[index - 1];

      while (MappingIsNextDuplicate (start))
	{
	  MappingSetFlag (start);

	  /* If this is now the start of the target mapping table,
	     break.  */
	  if (start == direct_transfer)
	    break;

	  start--;
	}
    }

  return True;
}

static void
SendOffers1 (struct wl_resource *resource, int ntargets, Atom *targets,
	     void (*send_offer_func) (struct wl_resource *, const char *))
{
  int i, j;
  unsigned int idx;
  unsigned short *buckets;

  for (i = 0; i < ntargets; ++i)
    {
      /* Offer each type corresponding to this target.  */
      idx = targets[i] % 16;

      /* N.B. that duplicates do appear in the atom buckets, which
	 is intentional.  */
      buckets = mapping_table.atom_buckets[idx];

      for (j = 0; j < mapping_table.n_atom_elements[idx]; ++j)
	{
	  if (MappingIs (&direct_transfer[buckets[j]],
			 targets[i])
	      && CheckDuplicate (buckets[j], targets[i]))
	    /* If it exists and was not previously offered, offer it
	       to the client.  */
	    send_offer_func (resource,
			     direct_transfer[buckets[j]].mime_type);
	}
    }

  /* Clear the duplicate flag of each item in the targets table that
     was touched.  */
  for (i = 0; i < ArrayElements (direct_transfer); ++i)
    MappingUnsetFlag (&direct_transfer[i]);
}

static void
SendOffers (struct wl_resource *resource, Time time)
{
  if (time < last_x_selection_change)
    /* This offer is out of date.  */
    return;

  SendOffers1 (resource, num_x_selection_targets,
	       x_selection_targets, wl_data_offer_send_offer);
}

static void
SendPrimaryOffers (struct wl_resource *resource, Time time)
{
  if (time < last_primary_time)
    /* This offer is out of date.  */
    return;

  SendOffers1 (resource, num_x_primary_targets,
	       x_primary_targets,
	       zwp_primary_selection_offer_v1_send_offer);
}

static void
HandleNewSelection (Time time, Atom selection, Atom *targets,
		    int ntargets)
{
  CreateOfferFuncs funcs;

  if (selection == XA_PRIMARY)
    {
      /* The primary selection changed, and now has the given
	 targets.  */

      if (time < last_primary_time)
	{
	  XLFree (targets);
	  return;
	}

      /* Set up the new primary targets.  */
      XLFree (x_primary_targets);
      x_primary_targets = targets;
      num_x_primary_targets = ntargets;
      last_primary_time = time;

      /* Add the right functions and set them as the foreign primary
	 selection handler at TIME.  */
      funcs.create_offer = CreatePrimaryOffer;
      funcs.send_offers = SendPrimaryOffers;

      XLSetForeignPrimary (time, funcs);
      return;
    }

  /* Else, the selection that changed is CLIPBOARD.  */
  
  /* Ignore outdated selection changes.  */
  if (time < last_x_selection_change)
    {
      /* We are responsible for deallocating targets.  */
      XLFree (targets);
      return;
    }

  /* Note that our free function explicitly ignores pointers to NULL,
     so this is safe.  */
  XLFree (x_selection_targets);
  x_selection_targets = targets;
  num_x_selection_targets = ntargets;

  last_x_selection_change = time;

  /* Add the right functions and set them as the foreign selection
     handler at TIME.  */

  funcs.create_offer = CreateOffer;
  funcs.send_offers = SendOffers;

  XLSetForeignSelection (time, funcs);
}

static void
TargetsReadCallback (ReadTransfer *transfer, Atom type, int format,
		     ptrdiff_t size)
{
  Atom *atoms;
  ptrdiff_t n_atoms, old, i;
  ReadTargetsData *data;

  if (type != XA_ATOM || format != 32)
    {
      SkipChunk (transfer);
      return;
    }

  /* Since format is 32, size must be a multiple of sizeof (long).  */
  atoms = (Atom *) ReadChunk (transfer, size / sizeof (long), &size,
			      NULL);

  /* Reading the property data failed.  The finish function will be
     run with success set to False soon.  */
  if (!atoms)
    return;

  data = GetTransferData (transfer);
  n_atoms = size / sizeof (long);
  old = data->n_atoms;

  /* Copy the atoms to data->atoms.  */

  if (IntAddWrapv (n_atoms, data->n_atoms,
		   &data->n_atoms))
    {
      /* Overflow.  Something is definitely wrong with the selection
	 data.  */
      data->n_atoms = 0;
      return;
    }

  data->atoms = XLRealloc (data->atoms,
			   data->n_atoms * sizeof *atoms);
  for (i = 0; i < n_atoms; ++i)
    data->atoms[old + i] = atoms[i];

  /* Use XFree, since this is Xlib-allocated memory.  */
  XFree (atoms);
}

static Bool
TargetsFinishCallback (ReadTransfer *transfer, Bool success)
{
  ReadTargetsData *data;

  data = GetTransferData (transfer);

  if (success)
    DebugPrint ("Received targets from %lu\n", data->selection);
  else
    DebugPrint ("Failed to obtain targets\n");

  if (success)
    HandleNewSelection (GetTransferTime (transfer),
			data->selection, data->atoms,
			data->n_atoms);
  else
    /* HandleNewSelection keeps data->atoms around for a while.  */
    XLFree (data->atoms);

  XLFree (data);
  return True;
}

/* Notice that the owner of CLIPBOARD has changed at TIME.  Try to get
   a list of selection targets from it, and if successful, make the
   Wayland data source current.  */

static void
NoticeClipboardChanged (Time time)
{
  ReadTargetsData *data;

  data = XLCalloc (1, sizeof *data);
  data->selection = CLIPBOARD;

  ConvertSelectionFuncs (CLIPBOARD, TARGETS, time, data,
			 NULL, TargetsReadCallback,
			 TargetsFinishCallback);
}

/* The same, but for the primary selection.  */

static void
NoticePrimaryChanged (Time time)
{
  ReadTargetsData *data;

  data = XLCalloc (1, sizeof *data);
  data->selection = XA_PRIMARY;

  ConvertSelectionFuncs (XA_PRIMARY, TARGETS, time, data,
			 NULL, TargetsReadCallback,
			 TargetsFinishCallback);
}

static void
NoticeClipboardCleared (Time time)
{
  /* Ignore outdated events.  */
  if (time < last_x_selection_change)
    return;

  DebugPrint ("CLIPBOARD was cleared at %lu\n", time);

  last_x_selection_change = time;
  XLClearForeignSelection (time);

  /* Free data that is no longer used.  */
  XLFree (x_selection_targets);
  x_selection_targets = NULL;
  num_x_selection_targets = 0;
}

static void
NoticePrimaryCleared (Time time)
{
  /* Ignore outdated events.  */
  if (time < last_primary_time)
    return;

  DebugPrint ("PRIMARY was cleared at %lu\n", time);

  last_primary_time = time;
  XLClearForeignPrimary (time);

  /* Free data that is no longer used.  */
  XLFree (x_primary_targets);
  x_primary_targets = NULL;
  num_x_primary_targets = 0;
}

static void
HandleSelectionNotify (XFixesSelectionNotifyEvent *event)
{
  if (event->owner == selection_transfer_window)
    /* Ignore events sent when we get selection ownership.  */
    return;

  if (event->selection == CLIPBOARD
      && event->selection_timestamp > last_clipboard_change)
    /* This time is used to keep track of whether or not things like
       disowning the selection were successful.  */
    last_clipboard_change = event->selection_timestamp;

  if (event->selection == XA_PRIMARY
      && event->selection_timestamp > last_primary_time)
    last_primary_time = event->selection_timestamp;

  if (event->owner != None
      && event->selection == CLIPBOARD)
    NoticeClipboardChanged (event->timestamp);
  else if (event->selection == CLIPBOARD)
    NoticeClipboardCleared (event->timestamp);
  else if (event->owner != None
	   && event->selection == XA_PRIMARY)
    NoticePrimaryChanged (event->timestamp);
  else if (event->selection == XA_PRIMARY)
    NoticePrimaryCleared (event->timestamp);
}

Bool
XLHandleOneXEventForXData (XEvent *event)
{
  if (event->type == fixes_event_base + XFixesSelectionNotify)
    {
      HandleSelectionNotify ((XFixesSelectionNotifyEvent *) event);

      return True;
    }

  return False;
}

static void
SelectSelectionInput (Atom selection)
{
  int mask;

  /* If the selection already exists, announce it to Wayland clients
     as well.  Use CurrentTime (even though the ICCCM says this is a
     bad idea); any XFixesSelectionNotify event that arrives later
     will clear up our (incorrect) view of the selection change
     time.  */

  if (selection == CLIPBOARD
      && XGetSelectionOwner (compositor.display, CLIPBOARD) != None)
    NoticeClipboardChanged (CurrentTime);

  if (selection == XA_PRIMARY
      && XGetSelectionOwner (compositor.display, XA_PRIMARY) != None)
    NoticePrimaryChanged (CurrentTime);

  mask = XFixesSetSelectionOwnerNotifyMask;
  mask |= XFixesSelectionWindowDestroyNotifyMask;
  mask |= XFixesSelectionClientCloseNotifyMask;

  XFixesSelectSelectionInput (compositor.display,
			      selection_transfer_window,
			      selection, mask);
}

static DataConversion *
GetDataConversion (Atom target)
{
  int i;

  for (i = 0; i < ArrayElements (data_conversions); ++i)
    {
      if (data_conversions[i].atom == target)
	return &data_conversions[i];
    }

  return NULL;
}

static const char *
MimeTypeFromTarget (Atom target, Bool primary)
{
  DataConversion *conversion;
  static char *string;
  Bool missing_type;

  /* TODO: replace XGetAtomName with something more efficient.  */
  if (string)
    XFree (string);
  string = NULL;

  if (!primary)
    missing_type = !XLDataSourceHasAtomTarget (selection_data_source,
					       target);
  else
    missing_type = !XLPDataSourceHasAtomTarget (primary_data_source,
						target);

  if (missing_type)
    {
      /* If the data source does not itself provide the specified
	 target, then a conversion function is in use.  */
      conversion = GetDataConversion (target);

      if (!conversion)
	/* There must be a data conversion here.  */
	abort ();

      DebugPrint ("Converting X type %lu to MIME type %s...\n",
		  conversion->type, conversion->mime_type);

      return conversion->mime_type;
    }

  string = XGetAtomName (compositor.display, target);
  return string;
}

static Atom
TypeFromTarget (Atom target, Bool primary)
{
  DataConversion *conversion;
  Bool missing_type;

  if (!primary)
    missing_type = !XLDataSourceHasAtomTarget (selection_data_source,
					       target);
  else
    missing_type = !XLPDataSourceHasAtomTarget (primary_data_source,
						target);

  if (missing_type)
    {
      /* If the data source does not itself provide the specified
	 target, then a conversion function is in use.  Use the type
	 specified in the conversion table entry.  */
      conversion = GetDataConversion (target);

      if (!conversion)
	/* There must be a data conversion here.  */
	abort ();

      return conversion->type;
    }

  /* Otherwise, assume the data type is the same as the target.  This
     is mostly intended to handle text/uri-list and similar
     formats.  */
  return target;
}

static void
NoticeTransferReadable (int fd, void *data)
{
  WriteTransfer *transfer;
  WriteInfo *info;

  transfer = data;

  /* Retrieve the info and make sure it hasn't been freed.  */
  info = GetWriteTransferData (transfer);
  XLAssert (info != NULL);

  DebugPrint ("Fd %d is now readable...\n", fd);

  /* Now cancel the read callback, and switch from waiting for the
     client to send something to waiting for the requestor to read the
     data.  */
  XLRemoveReadFd (info->read_callback);
  info->read_callback = NULL;

  /* And tell the selection code to start reading.  */
  StartReading (transfer);
}

static ReadStatus
ClipboardReadFunc (WriteTransfer *transfer, unsigned char *buffer,
		   ptrdiff_t buffer_size, ptrdiff_t *nbytes)
{
  WriteInfo *info;
  ssize_t size;

  /* Retrieve the info and make sure it hasn't been freed.  */
  info = GetWriteTransferData (transfer);

  if (buffer_size == -1)
    {
      DebugPrint ("ClipboardReadFunc called to free data for timeout\n");

      if (info)
	{
	  if (info->read_callback)
	    {
	      /* Cancel any read callback in progress.  */
	      XLRemoveReadFd (info->read_callback);
	      info->read_callback = NULL;
	    }

	  close (info->fd);
	  XLFree (info);
	  SetWriteTransferData (transfer, NULL);
	}

      return EndOfFile;
    }

  XLAssert (info != NULL);

  /* By the time this function is entered, the polling callback should
     be NULL.  */
  XLAssert (info->read_callback == NULL);

  DebugPrint ("ClipboardReadFunc called to read %td bytes\n",
	      buffer_size);

  /* Try to read buffer_size bytes into buffer.  */
  size = read (info->fd, buffer, buffer_size);

  /* If EOF, return that, free info, and close the pipe.  */

  if (!size)
    {
      DebugPrint ("EOF; completing transfer\n");

      if (info->read_callback)
	{
	  /* Cancel any read callback in progress.  */
	  XLRemoveReadFd (info->read_callback);
	  info->read_callback = NULL;
	}

      close (info->fd);
      XLFree (info);
      SetWriteTransferData (transfer, NULL);

      *nbytes = 0;
      return EndOfFile;
    }

  /* If an error occured, see what it was.  */

  if (size == -1)
    {
      DebugPrint ("read failed with: %s\n", strerror (errno));

      if (errno == EAGAIN)
	{
	  /* Start the read callback again.  This might make us
	     spin.  */
	  *nbytes = 0;

	  info->read_callback = XLAddReadFd (info->fd, transfer,
					     NoticeTransferReadable);
	  return ReadOk;
	}

      perror ("write");
      exit (1);
    }

  DebugPrint ("Read %zd bytes, starting the read callback again\n",
	      size);

  /* Otherwise, set nbytes to the number of bytes read, and start the
     write callback again.  */
  *nbytes = size;
  info->read_callback = XLAddReadFd (info->fd, transfer,
				     NoticeTransferReadable);
  return ReadOk;
}

static GetDataFunc
GetClipboardCallback (WriteTransfer *transfer, Atom target,
		      Atom *type)
{
  WriteInfo *info;
  int pipe[2], rc;
  DataConversion *conversion;
  struct wl_resource *resource;

  /* If the selection data source is destroyed, the selection will be
     disowned.  */
  XLAssert (selection_data_source != NULL);

  resource = XLResourceFromDataSource (selection_data_source);

  if (!XLDataSourceHasAtomTarget (selection_data_source, target))
    {
      /* If the data source does not itself provide the specified
	 target, then a conversion function is in use.  Call the
	 clipboard callback specified in the conversion table entry,
	 if it exists.  */
      conversion = GetDataConversion (target);

      if (!conversion)
	/* There must be a data conversion here.  */
	abort ();

      if (conversion->clipboard_callback)
	return conversion->clipboard_callback (transfer, target,
					       type, resource,
					       wl_data_source_send_send,
					       False);

      /* Otherwise, use the default callback.  */
      DebugPrint ("Conversion to type %lu specified with default callback\n",
		  target);
    }

  DebugPrint ("Entered GetClipboardCallback; target is %lu\n",
	      target);

  /* First, create a non-blocking pipe.  We will give the write end to
     the client.  */
  rc = pipe2 (pipe, O_NONBLOCK);

  if (rc)
    /* Creating the pipe failed.  */
    return NULL;

  DebugPrint ("Created pipe (%d, %d)\n", pipe[0], pipe[1]);

  /* Send the write end of the pipe to the source.  */
  wl_data_source_send_send (resource,
			    MimeTypeFromTarget (target, False),
			    pipe[1]);
  close (pipe[1]);

  /* And set the prop type appropriately.  */
  *type = TypeFromTarget (target, False);

  /* Create the transfer info structure.  */
  info = XLMalloc (sizeof *info);
  info->fd = pipe[0];
  info->flags = 0;

  DebugPrint ("Adding the read callback\n");

  /* Wait for info to become readable.  */
  info->read_callback = XLAddReadFd (info->fd, transfer,
				     NoticeTransferReadable);

  /* Set info as the transfer data.  */
  SetWriteTransferData (transfer, info);

  return ClipboardReadFunc;
}

/* Conversions between UTF-8 and Latin-1.  */


static void
NoticeConversionTransferReadable (int fd, void *data)
{
  WriteTransfer *transfer;
  ConversionWriteInfo *info;
  size_t nbytes;

  transfer = data;

  /* Retrieve the info and make sure it hasn't been freed.  */
  info = GetWriteTransferData (transfer);
  XLAssert (info != NULL);

  DebugPrint ("Fd %d is now readable...\n", fd);

  /* Now try to fill the conversion buffer.  */
  nbytes = read (info->fd, info->inbuf + info->inread,
		 BUFSIZ - info->inread);

  if (nbytes <= 0)
    {
      /* EOF was reached or an error occured.  Flag the transfer as
	 having reached the EOF.  */
      info->flags |= ReachedEndOfFile;
      DebugPrint ("EOF read from %d\n", fd);

      /* Cancel any read callback in progress.  */
      XLRemoveReadFd (info->read_callback);
      info->read_callback = NULL;

      /* Signal that the selection code should start reading.  */
      StartReading (transfer);
    }
  else
    {
      /* Add the number of bytes read to the number of bytes in the
	 buffer that are filled up.  */
      info->inread += nbytes;

      DebugPrint ("Read %tu bytes\n", info->inread);

      /* If the buffer is full, begin converting from it.  */
      if (info->inread == BUFSIZ)
	{
	  DebugPrint ("Buffer is now full\n");

	  /* Cancel any read callback in progress.  */
	  XLRemoveReadFd (info->read_callback);
	  info->read_callback = NULL;

	  /* Signal that the selection code should start reading.  */
	  StartReading (transfer);
	}
    }
}

static ReadStatus
ConversionReadFunc (WriteTransfer *transfer, unsigned char *buffer,
		    ptrdiff_t buffer_size, ptrdiff_t *nbytes)
{
  ConversionWriteInfo *info;
  size_t nconv, outsize;

  /* Retrieve the info and make sure it hasn't been freed.  */
  info = GetWriteTransferData (transfer);

  DebugPrint ("ClipboardReadFunc called to read %zd bytes\n",
	      buffer_size);

  if (buffer_size == -1)
    {
      DebugPrint ("ConversionReadFunc called to free data for timeout\n");

      if (info)
	{
	  if (info->read_callback)
	    {
	      /* Cancel any read callback in progress.  */
	      XLRemoveReadFd (info->read_callback);
	      info->read_callback = NULL;
	    }

	  close (info->fd);
	  iconv_close (info->cd);
	  XLFree (info);

	  SetWriteTransferData (transfer, NULL);
	}

      return False;
    }

  XLAssert (info != NULL);

  /* By the time this function is entered, the polling callback should
     be NULL.  */
  XLAssert (info->read_callback == NULL);

  /* Convert the appropriate amount of bytes into the output
     buffer.  */
  outsize = buffer_size;
  nconv = iconv (info->cd, &info->inptr, &info->inread,
		 (char **) &buffer, &outsize);

  DebugPrint ("iconv returned: %tu\n", nconv);

  if (nconv == (size_t) -1 && errno != EINVAL)
    {
      if (errno == E2BIG)
	{
	  DebugPrint ("iconv needs a bigger buffer\n");
	  *nbytes = buffer_size - outsize;

	  if (*nbytes < 1)
	    {
	      DebugPrint ("iconv failed with a buffer as large as the"
			  "max-request-size!\n");

	      goto eof;
	    }

	  memmove (info->inbuf, info->inptr, info->inread);
	  info->inptr = info->inbuf;
	  return NeedBiggerBuffer;
	}

      DebugPrint ("iconv failed with: %s; bytes written: %tu\n",
		  strerror (errno), buffer_size - outsize);

    eof:
      if (info->read_callback)
	{
	  /* Cancel any read callback in progress.  */
	  XLRemoveReadFd (info->read_callback);
	  info->read_callback = NULL;
	}

      /* The conversion failed/finished; return EOF.  */
      close (info->fd);
      iconv_close (info->cd);
      XLFree (info);

      SetWriteTransferData (transfer, NULL);
      *nbytes = buffer_size - outsize;
      return EndOfFile;
    }

  /* Otherwise, move the data that has still not been read back to the
     start.  */
  memmove (info->inbuf, info->inptr, info->inread);
  info->inptr = info->inbuf;
  DebugPrint ("iconv wrote: %tu\n", buffer_size - outsize);

  if (info->flags & ReachedEndOfFile)
    goto eof;

  /* Restore the read callback, and return the number of bytes
     read.  */
  *nbytes = buffer_size - outsize;
  info->read_callback = XLAddReadFd (info->fd, transfer,
				     NoticeConversionTransferReadable);

  return ReadOk;
}

static GetDataFunc
GetConversionCallback (WriteTransfer *transfer, Atom target, Atom *type,
		       struct wl_resource *source,
		       void (*send_send) (struct wl_resource *, const char *,
					  int),
		       Bool is_primary)
{
  ConversionWriteInfo *info;
  int pipe[2], rc;
  iconv_t cd;

  DebugPrint ("Performing data conversion of UTF-8 string to %lu\n",
	      target);

  cd = iconv_open ("ISO-8859-1", "UTF-8");

  if (cd == (iconv_t) -1)
    /* The conversion context couldn't be initialized, so fail this
       transfer.  */
    return NULL;

  /* First, create a non-blocking pipe.  We will give the write end to
     the client.  */
  rc = pipe2 (pipe, O_NONBLOCK);

  if (rc)
    return NULL;

  /* Then, send the write end of the pipe to the data source.  */
  send_send (source, "text/plain;charset=utf-8", pipe[1]);
  close (pipe[1]);

  /* Following that, set the property type correctly.  */
  *type = XA_STRING;

  /* Create the transfer info and buffer.  */
  info = XLMalloc (sizeof *info);
  info->fd = pipe[0];
  info->flags = 0;
  info->inptr = info->inbuf;
  info->inread = 0;
  info->cd = cd;

  DebugPrint ("Adding the read callback\n");

  /* Wait for info to become readable.  */
  info->read_callback = XLAddReadFd (info->fd, transfer,
				     NoticeConversionTransferReadable);

  /* Set info as the transfer data.  */
  SetWriteTransferData (transfer, info);

  return ConversionReadFunc;
}

static void
ConversionReadCallback (ReadTransfer *transfer, Atom type, int format,
			ptrdiff_t size)
{
  ConversionTransferInfo *info;
  unsigned char *data;

  /* Read all the data from the chunk now.  */

  data = ReadChunk (transfer, (format == 32 ? size / sizeof (long)
			       : (size + 3) / 4),
		    &size, NULL);

  if (!data)
    return;

  info = GetTransferData (transfer);

  /* We buffer up all the data from the selection and convert it
     later.  */

  info->buffer = XLRealloc (info->buffer, info->buffer_size + size);
  memcpy (info->buffer + info->buffer_size, data, size);
  info->buffer_size += size;
}

static void
FinishConversionTransfer (ReadTransfer *transfer, ConversionTransferInfo *info)
{
  DebugPrint ("Completing conversion transfer...\n");

  XLFree (info->buffer);
  iconv_close (info->cd);

  if (info->write_callback)
    XLRemoveWriteFd (info->write_callback);

  if (info->fd != -1)
    close (info->fd);

  XLFree (info);

  /* Complete the transfer itself.  */
  CompleteDelayedTransfer (transfer);
}

static void
NoticeConversionTransferWritable (int fd, void *data)
{
  ConversionTransferInfo *info;
  ssize_t written;
  size_t outsize, nconv, start;
  char *outbuf;

  /* The pipe is now writable.  First, try to write data from the
     buffer to the file descriptor.  */

  info = GetTransferData (data);

  /* Skip the call to write if there is nothing to do.  */

  if (info->outsize)
    {
      written = write (info->fd, info->output_buffer,
		       info->outsize);

      if (written == -1)
	{
	  DebugPrint ("write: %s\n", strerror (errno));

	  if (errno == EAGAIN)
	    /* Writing the data failed, so return.  This might cause us to
	       spin.  */
	    return;

	  if (errno != EPIPE)
	    {
	      perror ("write");
	      exit (1);
	    }

	  /* The other end of the pipe was closed by the client.  Stop
	     this transfer.  */
	  FinishConversionTransfer (data, info);
	  return;
	}

      /* Writing the data was successful.  Move the written part back to
	 the start.  */
      memmove (info->output_buffer, info->output_buffer + written,
	       info->outsize - written);
      info->outsize -= written;
    }

  /* Now, run iconv if there is still data in the input buffer.  */
  if (info->buffer_size)
    {
      outbuf = info->output_buffer + info->outsize;
      start = outsize = BUFSIZ - info->outsize;
      nconv = iconv (info->cd, &info->position, &info->buffer_size,
		     &outbuf, &outsize);

      if (nconv == (size_t) -1 && errno != EINVAL)
	{
	  /* iconv failed for various reasons.  */

	  if ((errno == E2BIG && outsize == BUFSIZ)
	      || errno != E2BIG)
	    {
	      /* BUFSIZ is either too small to hold the output contents
		 (which shouldn't happen), or some other error
		 occured.  Punt.  */
	      FinishConversionTransfer (data, info);
	      return;
	    }
	}

      /* Increase the output buffer size by the amount that was was
	 written.  */
      info->outsize += start - outsize;
      DebugPrint ("Output buffer is now %tu bytes full\n", info->outsize);
    }
  else if (!info->outsize)
    /* Everything has been written; finish the transfer.  */
    FinishConversionTransfer (data, info);
}

static Bool
ConversionFinishCallback (ReadTransfer *transfer, Bool success)
{
  ConversionTransferInfo *info;

  DebugPrint ("ConversionFinishCallback called; converting data in chunks.\n");

  /* The conversion is complete.  Start the conversion and write the
     correct amount of data into the pipe.  */

  info = GetTransferData (transfer);
  info->position = info->buffer;

  info->write_callback = XLAddWriteFd (info->fd, transfer,
				       NoticeConversionTransferWritable);
  return False;
}

static void
PostReceiveConversion (Time time, Atom selection, Atom target, int fd)
{
  ConversionTransferInfo *info;
  iconv_t cd;

  cd = iconv_open ("UTF-8", "ISO-8859-1");

  if (cd == (iconv_t) -1)
    {
      /* The conversion context couldn't be created.  Close the file
	 descriptor and fail.  */
      close (fd);
      return;
    }

  info = XLCalloc (1, sizeof *info);
  info->fd = fd;
  info->cd = cd;

  /* Try to make the file description nonblocking.  Clients seem to
     behave fine this way.  */
  MakeFdNonblocking (fd);

  DebugPrint ("Converting selection to UTF-8 at %lu for fd %d\n", time, fd);
  ConvertSelectionFuncs (selection, target, time, info, NULL,
			 ConversionReadCallback, ConversionFinishCallback);
}

/* Drag-and-drop support functions.  */


static const char *
DragMimeTypeFromTarget (Atom target)
{
  static char *string;

  /* TODO: replace XGetAtomName with something more efficient.  */
  if (string)
    XFree (string);

  string = XGetAtomName (compositor.display, target);
  return string;
}

static Atom
DragTypeFromTarget (Atom target)
{
  /* Assume the data type is the same as the target.  This is mostly
     intended to handle text/uri-list and similar formats.  */
  return target;
}

static GetDataFunc
GetDragCallback (WriteTransfer *transfer, Atom target,
		 Atom *type)
{
  WriteInfo *info;
  int pipe[2], rc;

  /* If the drag and drop data source is destroyed, the selection will
     be disowned.  */
  XLAssert (drag_data_source != NULL);

  DebugPrint ("Entered GetDragCallback; target is %lu\n",
	      target);

  /* First, create a non-blocking pipe.  We will give the write end to
     the client.  */
  rc = pipe2 (pipe, O_NONBLOCK);

  if (rc)
    /* Creating the pipe failed.  */
    return NULL;

  DebugPrint ("Created pipe (%d, %d)\n", pipe[0], pipe[1]);

  /* Send the write end of the pipe to the source.  */
  wl_data_source_send_send (XLResourceFromDataSource (drag_data_source),
			    DragMimeTypeFromTarget (target), pipe[1]);
  close (pipe[1]);

  /* And set the prop type appropriately.  */
  *type = DragTypeFromTarget (target);

  /* Create the transfer info structure.  */
  info = XLMalloc (sizeof *info);
  info->fd = pipe[0];
  info->flags = IsDragAndDrop;

  DebugPrint ("Adding the read callback\n");

  /* Wait for info to become readable.  */
  info->read_callback = XLAddReadFd (info->fd, transfer,
				     NoticeTransferReadable);

  /* Set info as the transfer data.  */
  SetWriteTransferData (transfer, info);

  return ClipboardReadFunc;
}

Bool
XLOwnDragSelection (Time time, DataSource *source)
{
  Atom *targets;
  int ntargets;
  Bool rc;

  DebugPrint ("Trying to own XdndSelection\n");

  /* Copy targets from the data source.  */
  ntargets = XLDataSourceTargetCount (source);
  targets = XLMalloc (sizeof *targets * ntargets);
  XLDataSourceGetTargets (source, targets);

  /* Own the selection.  */
  drag_data_source = source;

  rc = OwnSelection (time, XdndSelection, GetDragCallback,
		     targets, ntargets);
  XLFree (targets);

  return rc;
}



void
XLNoteSourceDestroyed (DataSource *source)
{
  if (source == selection_data_source)
    {
      DebugPrint ("Disowning CLIPBOARD at %lu (vs. last change %lu)\n"
		  "This is being done in response to the source being destroyed.\n",
		  last_clipboard_time, last_x_selection_change);

      /* Disown the selection.  */
      DisownSelection (CLIPBOARD);
    }

  if (source == drag_data_source)
    {
      DebugPrint ("Disowning XdndSelection\nThis is being done in response"
		  " to the data source being destroyed.\n");

      /* Disown the selection.  */
      DisownSelection (XdndSelection);
    }
}

void
XLNotePrimaryDestroyed (PDataSource *source)
{
  if (source == primary_data_source)
    {
      DebugPrint ("Disowning PRIMARY\nThis is being done in response"
		  " to the data source being destroyed.\n");

      /* Disown the selection.  */
      DisownSelection (XA_PRIMARY);
    }
}

static Bool
FindTargetInArray (Atom *targets, int ntargets, Atom atom)
{
  int i;

  for (i = 0; i < ntargets; ++i)
    {
      if (targets[i] == atom)
	return True;
    }

  return False;
}

/* FIXME: this should really be something other than two giant
   macros...  */

#define NoteLocalSelectionBody(callback, time_1, time_2, atom, field, foreign, n_foreign)	\
  Time time;											\
  Atom *targets;										\
  int ntargets, i, n_data_conversions;								\
  Bool rc;											\
												\
  if (source == NULL)										\
    {												\
      DebugPrint ("Disowning " #atom " at %lu (vs. last change %lu)\n",				\
		  time_1, time_2);								\
												\
      /* Disown the selection.  */								\
      DisownSelection (atom);									\
      field = NULL;										\
												\
      /* Return whether or not the selection was actually					\
	 disowned.  */										\
      return time_1 >= time_2;									\
    }												\
												\
  time = XLSeatGetLastUserTime (seat);								\
												\
  DebugPrint ("Acquiring ownership of " #atom " at %lu\n", time);				\
												\
  if (!time)											\
    /* Nothing has yet happened on the seat.  */						\
    return False;										\
												\
  if (time < time_1 || time < time_2)								\
    /* TIME is out of date.  */									\
    return False;										\
												\
  DebugPrint ("Setting callback function for " #atom "\n");					\
												\
  /* Since the local selection is now set, free foreign selection				\
     data.  */											\
  XLFree (foreign);										\
  n_foreign = 0;										\
  foreign = NULL;										\
												\
  /* Otherwise, set the clipboard change time to TIME.  */					\
  time_1 = time;										\
  time_2 = time;										\
												\

#define NoteLocalSelectionFooter(callback, atom, field, has_target)				\
  /* Now, look to see what standard X targets the client doesn't				\
     offer, and add them to the targets array.							\
												\
     Most functioning Wayland clients will also offer the typical X				\
     selection targets such as STRING and UTF8_STRING in addition to				\
     MIME types; when they do, they perform the data conversion					\
     conversion better than us.  */								\
												\
  for (i = 0; i < ArrayElements (data_conversions); ++i)					\
    {												\
      /* If the source provides MIME typed-data for a format we know				\
	 how to convert, announce the associated selection conversion				\
	 targets.  */										\
												\
      if (has_target (source,									\
		      data_conversions[i].mime_type)						\
	  && !FindTargetInArray (targets, ntargets,						\
				 data_conversions[i].type))					\
	{											\
	  DebugPrint ("Client doesn't provide standard X conversion"				\
		      " target for %s; adding it\n",						\
		      data_conversions[i].mime_type);						\
												\
	  targets[ntargets++] = data_conversions[i].type;					\
	}											\
    }												\
												\
  /* And own the selection.  */									\
  field = source;										\
												\
  rc = OwnSelection (time, atom, callback, targets, ntargets);					\
  XLFree (targets);										\
  return rc											\

Bool
XLNoteLocalSelection (Seat *seat, DataSource *source)
{
  NoteLocalSelectionBody (GetClipboardCallback, last_clipboard_time,
			  last_x_selection_change, CLIPBOARD,
			  selection_data_source, x_selection_targets,
			  num_x_selection_targets);

  /* And copy the targets from the data source.  */
  ntargets = XLDataSourceTargetCount (source);
  n_data_conversions = ArrayElements (data_conversions);
  targets = XLMalloc (sizeof *targets * (ntargets + n_data_conversions));
  XLDataSourceGetTargets (source, targets);

  NoteLocalSelectionFooter (GetClipboardCallback, CLIPBOARD,
			    selection_data_source,
			    XLDataSourceHasTarget);
}

static GetDataFunc
GetPrimaryCallback (WriteTransfer *transfer, Atom target,
		    Atom *type)
{
  WriteInfo *info;
  int pipe[2], rc;
  DataConversion *conversion;
  struct wl_resource *resource;

  /* If the selection data source is destroyed, the selection will be
     disowned.  */
  XLAssert (primary_data_source != NULL);

  resource = XLResourceFromPDataSource (primary_data_source);

  if (!XLPDataSourceHasAtomTarget (primary_data_source, target))
    {
      /* If the data source does not itself provide the specified
	 target, then a conversion function is in use.  Call the
	 clipboard callback specified in the conversion table entry,
	 if it exists.  */
      conversion = GetDataConversion (target);

      if (!conversion)
	/* There must be a data conversion here.  */
	abort ();

      /* This is ugly but the only reasonable way to fit the following
	 function in 80 columns.  */

#define Function zwp_primary_selection_source_v1_send_send

      if (conversion->clipboard_callback)
	return conversion->clipboard_callback (transfer, target,
					       type, resource,
					       Function, True);

#undef Function

      /* Otherwise, use the default callback.  */
      DebugPrint ("Conversion to type %lu specified with default callback\n",
		  target);
    }

  DebugPrint ("Entered GetPrimaryCallback; target is %lu\n",
	      target);

  /* First, create a non-blocking pipe.  We will give the write end to
     the client.  */
  rc = pipe2 (pipe, O_NONBLOCK);

  if (rc)
    /* Creating the pipe failed.  */
    return NULL;

  DebugPrint ("Created pipe (%d, %d)\n", pipe[0], pipe[1]);

  /* Send the write end of the pipe to the source.  */
  zwp_primary_selection_source_v1_send_send (resource,
					     MimeTypeFromTarget (target, True),
					     pipe[1]);
  close (pipe[1]);

  /* And set the prop type appropriately.  */
  *type = TypeFromTarget (target, True);

  /* Create the transfer info structure.  */
  info = XLMalloc (sizeof *info);
  info->fd = pipe[0];
  info->flags = 0;

  DebugPrint ("Adding the read callback\n");

  /* Wait for info to become readable.  */
  info->read_callback = XLAddReadFd (info->fd, transfer,
				     NoticeTransferReadable);

  /* Set info as the transfer data.  */
  SetWriteTransferData (transfer, info);

  return ClipboardReadFunc;
}

Bool
XLNoteLocalPrimary (Seat *seat, PDataSource *source)
{
  /* I forgot why there are two variables for tracking the clipboard
     time, so just use one here.  */
  NoteLocalSelectionBody (GetPrimaryCallback, last_primary_time,
			  last_primary_time, XA_PRIMARY,
			  primary_data_source, x_primary_targets,
			  num_x_primary_targets);

  /* And copy the targets from the data source.  */
  ntargets = XLPDataSourceTargetCount (source);
  n_data_conversions = ArrayElements (data_conversions);
  targets = XLMalloc (sizeof *targets * (ntargets + n_data_conversions));
  XLPDataSourceGetTargets (source, targets);

  NoteLocalSelectionFooter (GetPrimaryCallback, XA_PRIMARY,
			    primary_data_source,
			    XLPDataSourceHasTarget);
}

void
XLInitXData (void)
{
  sigset_t set;
  int rc, fixes_error_base, major, minor;

  rc = XFixesQueryExtension (compositor.display,
			     &fixes_event_base,
			     &fixes_error_base);

  if (!rc)
    {
      fprintf (stderr, "The X server does not support the "
	       "XFixes protocol extension\n");
      exit (1);
    }

  rc = XFixesQueryVersion (compositor.display,
			   &major, &minor);

  if (!rc || major < 1)
    {
      fprintf (stderr, "The X server does not support the "
	       "right version of the XFixes protocol extension\n");
      exit (1);
    }

  SelectSelectionInput (CLIPBOARD);
  SelectSelectionInput (XA_PRIMARY);

  /* Initialize atoms used in the direct transfer table.  */
  direct_transfer[1].atom_flag = UTF8_STRING | Duplicate;
  direct_transfer[2].translation_func = PostReceiveConversion;
  DirectTransferInitializer (direct_transfer, 3);

  /* Set up the direct transfer table.  */
  SetupMappingTable ();

  /* And those used in the data conversions table.  */
  data_conversions[0].atom = UTF8_STRING;
  data_conversions[0].type = UTF8_STRING;
  data_conversions[1].atom = XA_STRING;
  data_conversions[1].type = XA_STRING;
  data_conversions[1].clipboard_callback = GetConversionCallback;

  /* Ignore SIGPIPE, since we could be writing to a pipe whose reading
     end was closed by the client.  */
  sigemptyset (&set);
  sigaddset (&set, SIGPIPE);

  if (pthread_sigmask (SIG_BLOCK, &set, NULL))
    {
      perror ("pthread_sigmask");
      exit (1);
    }
}

void
XLReceiveDataFromSelection (Time time, Atom selection, Atom target,
			    int fd)
{
  PostReceiveDirect (time, selection, target, fd);
}
