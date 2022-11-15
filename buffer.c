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

#include "compositor.h"

typedef struct _DestroyListener DestroyListener;

struct _DestroyListener
{
  /* Function to call.  */
  ExtBufferFunc func;

  /* User data.  */
  void *data;
};

void
XLRetainBuffer (ExtBuffer *buffer)
{
  buffer->funcs.retain (buffer);
}

void
XLDereferenceBuffer (ExtBuffer *buffer)
{
  buffer->funcs.dereference (buffer);
}

RenderBuffer
XLRenderBufferFromBuffer (ExtBuffer *buffer)
{
  return buffer->funcs.get_buffer (buffer);
}

unsigned int
XLBufferWidth (ExtBuffer *buffer)
{
  return buffer->funcs.width (buffer);
}

unsigned int
XLBufferHeight (ExtBuffer *buffer)
{
  return buffer->funcs.height (buffer);
}

void
XLReleaseBuffer (ExtBuffer *buffer)
{
  buffer->funcs.release (buffer);
}

void *
XLBufferRunOnFree (ExtBuffer *buffer, ExtBufferFunc func,
		   void *data)
{
  DestroyListener *listener;

  listener = XLMalloc (sizeof *listener);

  listener->func = func;
  listener->data = data;

  buffer->destroy_listeners
    = XLListPrepend (buffer->destroy_listeners,
		     listener);

  return listener;
}

void
XLBufferCancelRunOnFree (ExtBuffer *buffer, void *key)
{
  buffer->destroy_listeners
    = XLListRemove (buffer->destroy_listeners, key);
  XLFree (key);
}

void
XLPrintBuffer (ExtBuffer *buffer)
{
  if (buffer->funcs.print_buffer)
    buffer->funcs.print_buffer (buffer);
}

void
ExtBufferDestroy (ExtBuffer *buffer)
{
  XLList *listener;
  DestroyListener *item;

  /* Now run every destroy listener connected to this buffer.  */
  for (listener = buffer->destroy_listeners;
       listener; listener = listener->next)
    {
      item = listener->data;

      item->func (buffer, item->data);
    }

  /* Free the label if present.  */
  XLFree (buffer->label);

  /* Not very efficient, since the list is followed through twice, but
     destroy listener lists should always be small.  */
  XLListFree (buffer->destroy_listeners, XLFree);
}
