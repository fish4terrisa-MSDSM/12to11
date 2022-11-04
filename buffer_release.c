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

/* Simple helper code for managing buffer release in surfaces.  */

typedef struct _ReleaseLaterRecord ReleaseLaterRecord;

struct _ReleaseLaterRecord
{
  /* A monotonically (overflow aside) increasing identifier.  */
  uint64_t id;

  /* The buffer that should be released upon receiving this
     message.  */
  ExtBuffer *buffer;

  /* The idle callback, if any.  */
  IdleCallbackKey key;

  /* The buffer release helper.  */
  BufferReleaseHelper *helper;

  /* The next and last records.  */
  ReleaseLaterRecord *next, *last;
};

struct _BufferReleaseHelper
{
  /* Queue of buffers pending release.  */
  ReleaseLaterRecord records;

  /* Callback run upon all buffers being released.  */
  AllReleasedCallback callback;

  /* Data for that callback.  */
  void *callback_data;
};

BufferReleaseHelper *
MakeBufferReleaseHelper (AllReleasedCallback callback,
			 void *callback_data)
{
  BufferReleaseHelper *helper;

  helper = XLCalloc (1, sizeof *helper);
  helper->records.next = &helper->records;
  helper->records.last = &helper->records;
  helper->callback = callback;
  helper->callback_data = callback_data;

  return helper;
}

void
FreeBufferReleaseHelper (BufferReleaseHelper *helper)
{
  ReleaseLaterRecord *next, *last;

  /* Do an XSync, and then release all the records.  */
  XSync (compositor.display, False);

  next = helper->records.next;
  while (next != &helper->records)
    {
      last = next;
      next = next->next;

      /* Cancel the idle callback if it already exists.  */
      if (last->key)
	RenderCancelIdleCallback (last->key);

      /* Release the buffer now.  */
      XLReleaseBuffer (last->buffer);

      /* Before freeing the record itself.  */
      XLFree (last);
    }

  /* Free the helper.  */
  XLFree (helper);
}

static void
BufferIdleCallback (RenderBuffer buffer, void *data)
{
  ReleaseLaterRecord *record;
  BufferReleaseHelper *helper;

  record = data;
  helper = record->helper;

  /* Release the buffer.  */
  XLReleaseBuffer (record->buffer);

  /* Unlink and free the record.  */
  record->next->last = record->last;
  record->last->next = record->next;
  XLFree (record);

  /* If there are no more records in the helper, run its
     all-released-callback.  */
  if (helper->records.next == &helper->records)
    helper->callback (helper->callback_data);
}

void
ReleaseBufferWithHelper (BufferReleaseHelper *helper, ExtBuffer *buffer,
			 RenderTarget target)
{
  ReleaseLaterRecord *record;
  RenderBuffer render_buffer;

  render_buffer = XLRenderBufferFromBuffer (buffer);

  record = XLCalloc (1, sizeof *record);
  record->next = helper->records.next;
  record->last = &helper->records;
  helper->records.next->last = record;
  helper->records.next = record;

  /* Now, the record is linked into the list.  Record the buffer and
     add an idle callback.  */
  record->buffer = buffer;
  record->key = RenderAddIdleCallback (render_buffer, target,
				       BufferIdleCallback,
				       record);
  record->helper = helper;
}
