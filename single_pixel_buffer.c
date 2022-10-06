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
#include "single-pixel-buffer-v1.h"
#include "string.h"

typedef struct _Buffer Buffer;

struct _Buffer
{
  /* The ExtBuffer associated with this buffer.  */
  ExtBuffer buffer;

  /* The rendering buffer associated with this buffer.  */
  RenderBuffer render_buffer;

  /* The wl_resource corresponding to this buffer.  */
  struct wl_resource *resource;

  /* The number of references to this buffer.  */
  int refcount;
};

/* The global wp_single_pixel_buffer_manager_v1 resource.  */
static struct wl_global *single_pixel_buffer_global;

static void
DestroyBuffer (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_buffer_interface single_pixel_buffer_impl =
  {
    .destroy = DestroyBuffer,
  };

static void
RetainBuffer (Buffer *buffer)
{
  buffer->refcount++;
}

static void
DereferenceBuffer (Buffer *buffer)
{
  if (--buffer->refcount)
    return;

  RenderFreeSinglePixelBuffer (buffer->render_buffer);
  ExtBufferDestroy (&buffer->buffer);
  XLFree (buffer);
}

static void
ReleaseBufferFunc (ExtBuffer *buffer)
{
  if (((Buffer *) buffer)->resource)
    wl_buffer_send_release (((Buffer *) buffer)->resource);
}

static void
RetainBufferFunc (ExtBuffer *buffer)
{
  RetainBuffer ((Buffer *) buffer);
}

static void
DereferenceBufferFunc (ExtBuffer *buffer)
{
  DereferenceBuffer ((Buffer *) buffer);
}

static RenderBuffer
GetBufferFunc (ExtBuffer *buffer)
{
  return ((Buffer *) buffer)->render_buffer;
}

static unsigned int
WidthFunc (ExtBuffer *buffer)
{
  /* Single pixel buffers are always 1x1.  */
  return 1;
}

static unsigned int
HeightFunc (ExtBuffer *buffer)
{
  /* Single pixel buffers are always 1x1.  */
  return 1;
}

static void
PrintBuffer (Buffer *buffer)
{
  /* Not implemented.  */
}

static void
PrintBufferFunc (ExtBuffer *buffer)
{
  PrintBuffer ((Buffer *) buffer);
}

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  Buffer *buffer;

  buffer = wl_resource_get_user_data (resource);
  buffer->resource = NULL;

  DereferenceBuffer (buffer);
}



static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

void
CreateU32RgbaBuffer (struct wl_client *client, struct wl_resource *resource,
		     uint32_t id, uint32_t r, uint32_t g, uint32_t b,
		     uint32_t a)
{
  Buffer *buffer;
  Bool error;

  buffer = XLSafeMalloc (sizeof *buffer);

  if (!buffer)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (buffer, 0, sizeof *buffer);
  buffer->resource = wl_resource_create (client, &wl_buffer_interface,
					 wl_resource_get_version (resource),
					 id);

  if (!buffer->resource)
    {
    out_of_memory:
      wl_resource_post_no_memory (resource);
      XLFree (buffer);
      return;
    }

  /* Now, create the render target.  */
  error = False;
  buffer->render_buffer = RenderBufferFromSinglePixel (r, g, b, a,
						       &error);

  if (error)
    /* We probably ran out of memory.  */
    goto out_of_memory;

  buffer->refcount = 1;

  /* Initialize function pointers.  */
  buffer->buffer.funcs.retain = RetainBufferFunc;
  buffer->buffer.funcs.dereference = DereferenceBufferFunc;
  buffer->buffer.funcs.get_buffer = GetBufferFunc;
  buffer->buffer.funcs.width = WidthFunc;
  buffer->buffer.funcs.height = HeightFunc;
  buffer->buffer.funcs.release = ReleaseBufferFunc;
  buffer->buffer.funcs.print_buffer = PrintBufferFunc;

  wl_resource_set_implementation (buffer->resource, &single_pixel_buffer_impl,
				  buffer, HandleResourceDestroy);
}

static const struct wp_single_pixel_buffer_manager_v1_interface manager_impl =
  {
    .destroy = Destroy,
    .create_u32_rgba_buffer = CreateU32RgbaBuffer,
  };

static void
HandleBind (struct wl_client *client, void *data, uint32_t version,
	    uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client,
				 &wp_single_pixel_buffer_manager_v1_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &manager_impl,
				  NULL, NULL);
}

void
XLInitSinglePixelBuffer (void)
{
  single_pixel_buffer_global
    = wl_global_create (compositor.wl_display,
			&wp_single_pixel_buffer_manager_v1_interface,
		        1, NULL, HandleBind);
}
