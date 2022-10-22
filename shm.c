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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/mman.h>

#include "compositor.h"

enum
  {
    PoolCannotSigbus = 1,
  };

typedef struct _Pool
{
  /* The file descriptor corresponding to this pool.  */
  int fd;

  /* The size of this pool.  */
  size_t size;

  /* The number of references to this pool.  */
  int refcount;

  /* Various flags.  */
  int flags;

  /* Pointer to the raw data in this pool.  */
  void *data;

  /* The wl_resource corresponding to this pool.  */
  struct wl_resource *resource;
} Pool;

typedef struct _Buffer
{
  /* The ExtBuffer associated with this buffer.  */
  ExtBuffer buffer;

  /* The rendering buffer associated with this buffer.  */
  RenderBuffer render_buffer;

  /* The width and height of this buffer.  */
  unsigned int width, height;

  /* The wl_resource corresponding to this buffer.  */
  struct wl_resource *resource;

  /* The pool corresponding to this buffer.  */
  Pool *pool;

  /* The number of references to this buffer.  */
  int refcount;
} Buffer;

/* The shared memory global.  */
static struct wl_global *global_shm;

static void
DereferencePool (Pool *pool)
{
  if (--pool->refcount)
    return;

  munmap (pool->data, pool->size);

  /* Cancel the busfault trap.  */

  if (pool->data != (void *) -1
      /* If reading from the pool cannot possibly cause SIGBUS, then
	 no bus fault trap was installed.  */
      && !(pool->flags & PoolCannotSigbus))
    XLRemoveBusfault (pool->data);

  close (pool->fd);
  XLFree (pool);
}

static void
RetainPool (Pool *pool)
{
  pool->refcount++;
}

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

  RenderFreeShmBuffer (buffer->render_buffer);
  DereferencePool (buffer->pool);

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
  return ((Buffer *) buffer)->width;
}

static unsigned int
HeightFunc (ExtBuffer *buffer)
{
  return ((Buffer *) buffer)->height;
}

static void
DestroyBuffer (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
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
HandleBufferResourceDestroy (struct wl_resource *resource)
{
  Buffer *buffer;

  buffer = wl_resource_get_user_data (resource);
  buffer->resource = NULL;

  DereferenceBuffer (buffer);
}

static const struct wl_buffer_interface wl_shm_buffer_impl =
  {
    .destroy = DestroyBuffer,
  };

static Bool
IsFormatSupported (uint32_t format)
{
  ShmFormat *formats;
  int nformats, i;

  formats = RenderGetShmFormats (&nformats);

  for (i = 0; i < nformats; ++i)
    {
      if (formats[i].format == format)
	return True;
    }

  return False;
}

static void
CreateBuffer (struct wl_client *client, struct wl_resource *resource,
              uint32_t id, int32_t offset, int32_t width, int32_t height,
	      int32_t stride, uint32_t format)
{
  Pool *pool;
  Buffer *buffer;
  RenderBuffer render_buffer;
  SharedMemoryAttributes attrs;
  Bool failure;

  if (!IsFormatSupported (format))
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FORMAT,
			      "the specified format is not supported");
      return;
    }

  pool = wl_resource_get_user_data (resource);

  if (!RenderValidateShmParams (format, width, height,
				offset, stride, pool->size))
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_STRIDE,
			      "invalid offset or stride, or pool too small");
      return;
    }

  if (width > 32768 || height > 32768)
    {
      /* X doesn't support larger drawables.  */
      wl_resource_post_no_memory (resource);
      return;
    }

  if (width < 1 || height < 1)
    {
      /* X doesn't support smaller drawables.  */
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_STRIDE,
			      "invalid size, this server does not support"
			      " zero-width drawables");
      return;
    }

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
      wl_resource_post_no_memory (resource);
      XLFree (buffer);
      return;
    }

  attrs.format = format;
  attrs.offset = offset;
  attrs.width = width;
  attrs.height = height;
  attrs.stride = stride;
  attrs.fd = pool->fd;

  /* Pass a reference instead of the pointer itself.  The pool will
     stay valid as long as the buffer is still alive, and the data
     pointer can change if the client resizes the pool.  */
  attrs.data = &pool->data;
  attrs.pool_size = pool->size;

  /* Now, create the renderer buffer.  */
  failure = False;
  render_buffer = RenderBufferFromShm (&attrs, &failure);

  /* If a platform specific error happened, fail now.  */
  if (failure)
    {
      wl_resource_destroy (buffer->resource);
      XLFree (buffer);
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
			      "unknown error creating buffer");
      return;
    }

  buffer->render_buffer = render_buffer;
  buffer->width = width;
  buffer->height = height;
  buffer->pool = pool;
  buffer->refcount = 1;

  /* Initialize function pointers.  */
  buffer->buffer.funcs.retain = RetainBufferFunc;
  buffer->buffer.funcs.dereference = DereferenceBufferFunc;
  buffer->buffer.funcs.get_buffer = GetBufferFunc;
  buffer->buffer.funcs.width = WidthFunc;
  buffer->buffer.funcs.height = HeightFunc;
  buffer->buffer.funcs.release = ReleaseBufferFunc;
  buffer->buffer.funcs.print_buffer = PrintBufferFunc;

  RetainPool (pool);

  wl_resource_set_implementation (buffer->resource,
				  &wl_shm_buffer_impl,
				  buffer,
				  HandleBufferResourceDestroy);
}

static void
HandlePoolResourceDestroy (struct wl_resource *resource)
{
  Pool *pool;

  pool = wl_resource_get_user_data (resource);
  DereferencePool (pool);
}

static void
DestroyPool (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
ResizePool (struct wl_client *client, struct wl_resource *resource,
	    int32_t size)
{
  Pool *pool;
  void *data;
#ifdef F_GET_SEALS
  int seals;
  struct stat statb;
#endif

  pool = wl_resource_get_user_data (resource);

  if (size == pool->size)
    /* There is no need to do anything, since the pool is still the
       same size.  */
    return;

  if (size < pool->size)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
			      "shared memory pools cannot be shrunk");
      return;
    }

  data = mremap (pool->data, pool->size, size, MREMAP_MAYMOVE);

  if (data == MAP_FAILED)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
			      "mremap: %s", strerror (errno));
      return;
    }

  /* Now cancel the existing bus fault handler, should it have been
     installed.  */
  if (pool->size && !(pool->flags & PoolCannotSigbus))
    XLRemoveBusfault (pool->data);

  pool->flags = 0;

  /* Recheck whether or not reading from the pool can cause
     SIGBUS.  */
#ifdef F_GET_SEALS
  seals = fcntl (pool->fd, F_GET_SEALS);

  if (seals != -1 && seals & F_SEAL_SHRINK
      && fstat (pool->fd, &statb) >= 0
      && statb.st_size >= size)
    pool->flags |= PoolCannotSigbus;
#endif

  pool->size = size;
  pool->data = data;

  /* And add a new handler.  */
  if (pool->size && !(pool->flags & PoolCannotSigbus))
    XLRecordBusfault (pool->data, pool->size);
}

static const struct wl_shm_pool_interface wl_shm_pool_impl =
  {
    .destroy = DestroyPool,
    .resize = ResizePool,
    .create_buffer = CreateBuffer,
  };

static void
CreatePool (struct wl_client *client, struct wl_resource *resource,
	    uint32_t id, int32_t fd, int32_t size)
{
  Pool *pool;
#ifdef F_GET_SEALS
  int seals;
  struct stat statb;
#endif

  if (size <= 0)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_STRIDE,
			      "invalid size given to create_pool");
      close (fd);
      return;
    }

  pool = XLSafeMalloc (sizeof *pool);

  if (!pool)
    {
      wl_resource_post_no_memory (resource);
      close (fd);
      return;
    }

  memset (pool, 0, sizeof *pool);

  pool->resource = wl_resource_create (client, &wl_shm_pool_interface,
				       wl_resource_get_version (resource),
				       id);

  /* There are no references to this pool yet.  */
  if (!pool->resource)
    {
      XLFree (pool);
      wl_resource_post_no_memory (resource);
      close (fd);

      return;
    }

  pool->data = mmap (NULL, size, PROT_READ, MAP_SHARED, fd, 0);

  if (pool->data == (void *) -1)
    {
      wl_resource_destroy (pool->resource);
      XLFree (pool);
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
			      "mmap: %s", strerror (errno));
      close (fd);

      return;
    }

  wl_resource_set_implementation (pool->resource, &wl_shm_pool_impl,
				  pool, HandlePoolResourceDestroy);

  pool->size = size;

  /* Try to determine whether or not the accessing the pool data
     cannot result in SIGBUS, as the file is already larger (or equal
     in size) to the pool and the size is sealed.  */
  pool->flags = 0;
#ifdef F_GET_SEALS
  seals = fcntl (fd, F_GET_SEALS);

  if (seals != -1 && seals & F_SEAL_SHRINK
      && fstat (fd, &statb) >= 0
      && statb.st_size >= size)
    pool->flags |= PoolCannotSigbus;
#endif

  /* Begin trapping SIGBUS from this pool.  The client may truncate
     the file without telling us, in which case accessing its contents
     will cause crashes.  */
  if (!(pool->flags & PoolCannotSigbus))
    XLRecordBusfault (pool->data, pool->size);

  pool->fd = fd;
  pool->refcount = 1;

  return;
}

static const struct wl_shm_interface wl_shm_impl =
  {
    .create_pool = CreatePool,
  };

static void
PostFormats (struct wl_resource *resource)
{
  ShmFormat *formats;
  int nformats, i;

  formats = RenderGetShmFormats (&nformats);

  for (i = 0; i < nformats; ++i)
    wl_shm_send_format (resource, formats[i].format);
}

static void
HandleBind (struct wl_client *client, void *data,
	    uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_shm_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &wl_shm_impl,
				  NULL, NULL);

  PostFormats (resource);
}

static void
InitRender (void)
{
  int major, minor, base, dummy;

  if (!XRenderQueryExtension (compositor.display,
			      &base, &dummy))
    {
      fprintf (stderr, "XRender is not supported by this X server\n");
      exit (1);
    }

  if (!XRenderQueryVersion (compositor.display,
			    &major, &minor)
      || (!major && minor < 2))
    {
      fprintf (stderr, "XRender is not supported by this X server\n");
      exit (1);
    }

  compositor.argb_format
    = XRenderFindStandardFormat (compositor.display,
				 PictStandardARGB32);
  compositor.xrgb_format
    = XRenderFindStandardFormat (compositor.display,
				 PictStandardRGB24);

  if (!compositor.argb_format)
    {
      fprintf (stderr, "Failed to find standard format PictStandardARGB32\n");
      exit (1);
    }

  if (!compositor.xrgb_format)
    {
      fprintf (stderr, "Failed to find standard format PictStandardRGB24\n");
      exit (1);
    }
}

void
XLInitShm (void)
{
  InitRender ();

  global_shm = wl_global_create (compositor.wl_display,
				 &wl_shm_interface, 1,
				 NULL, HandleBind);
}
