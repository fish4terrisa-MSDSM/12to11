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
#include <unistd.h>

#include <sys/mman.h>
#include <sys/fcntl.h>
#include <sys/stat.h>

#include "compositor.h"

typedef struct _Pool
{
  /* The file descriptor corresponding to this pool.  */
  int fd;

  /* The size of this pool.  */
  size_t size;

  /* The number of references to this pool.  */
  int refcount;

  /* Pointer to the raw data in this pool.  */
  void *data;

  /* The wl_resource corresponding to this pool.  */
  struct wl_resource *resource;
} Pool;

typedef struct _Buffer
{
  /* The ExtBuffer associated with this buffer.  */
  ExtBuffer buffer;

  /* The pixmap associated with this buffer.  */
  Pixmap pixmap;

  /* The picture associated with this buffer.  */
  Picture picture;

  /* The width and height of this buffer.  */
  unsigned int width, height;

  /* The stride of this buffer.  */
  int stride;

  /* The offset of this buffer.  */
  int offset;

  /* The format of this buffer.  */
  uint32_t format;

  /* The wl_resource corresponding to this buffer.  */
  struct wl_resource *resource;

  /* The pool corresponding to this buffer.  */
  Pool *pool;

  /* The number of references to this buffer.  */
  int refcount;
} Buffer;

/* List of all resources for our shared memory global.  */
static XLList *all_shms;

/* The shared memory global.  */
static struct wl_global *global_shm;

static void
DereferencePool (Pool *pool)
{
  if (--pool->refcount)
    return;

  munmap (pool->data, pool->size);
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

  XRenderFreePicture (compositor.display, buffer->picture);
  XFreePixmap (compositor.display, buffer->pixmap);
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

static Picture
GetPictureFunc (ExtBuffer *buffer)
{
  return ((Buffer *) buffer)->picture;
}

static Pixmap
GetPixmapFunc (ExtBuffer *buffer)
{
  return ((Buffer *) buffer)->pixmap;
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
  int x, y;
  char *base;
  unsigned int *base32;

  for (y = 0; y < buffer->height; ++y)
    {
      base = buffer->pool->data;
      base += buffer->stride * y;
      base32 = (unsigned int *) base;

      for (x = 0; x < buffer->width; ++x)
	fprintf (stderr, "#x%8x ", base32[x]);
      fprintf (stderr, "\n");
    }
}

static void
PrintBufferFunc (ExtBuffer *buffer)
{
  PrintBuffer ((Buffer *) buffer);
}

static int
DepthForFormat (uint32_t format)
{
  switch (format)
    {
    case WL_SHM_FORMAT_ARGB8888:
      return 32;

    case WL_SHM_FORMAT_XRGB8888:
      return 24;

    default:
      return 0;
    }
}

static XRenderPictFormat *
PictFormatForFormat (uint32_t format)
{
  switch (format)
    {
    case WL_SHM_FORMAT_ARGB8888:
      return compositor.argb_format;

    case WL_SHM_FORMAT_XRGB8888:
      return compositor.xrgb_format;

    default:
      return 0;
    }
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

static void
CreateBuffer (struct wl_client *client, struct wl_resource *resource,
              uint32_t id, int32_t offset, int32_t width, int32_t height,
	      int32_t stride, uint32_t format)
{
  XRenderPictureAttributes picture_attrs;
  Pool *pool;
  Buffer *buffer;
  xcb_shm_seg_t seg;
  Pixmap pixmap;
  Picture picture;
  int fd, depth;

  depth = DepthForFormat (format);

  if (!depth)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FORMAT,
			      "the specified format is not supported");
      return;
    }

  pool = wl_resource_get_user_data (resource);

  if (pool->size < offset || stride != width * 4
      || offset + stride * height > pool->size
      || offset < 0)
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

  /* XCB closes fds after sending them.  */
  fd = fcntl (pool->fd, F_DUPFD_CLOEXEC, 0);

  if (fd < 0)
    {
      wl_resource_destroy (buffer->resource);
      XLFree (buffer);
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
			      "fcntl: %s", strerror (errno));
      return;
    }

  seg = xcb_generate_id (compositor.conn);
  pixmap = xcb_generate_id (compositor.conn);

  xcb_shm_attach_fd (compositor.conn, seg, fd, false);
  xcb_shm_create_pixmap (compositor.conn, pixmap,
			 DefaultRootWindow (compositor.display),
			 width, height, depth, seg, offset);
  xcb_shm_detach (compositor.conn, seg);

  picture = XRenderCreatePicture (compositor.display, pixmap,
				  PictFormatForFormat (format),
				  0, &picture_attrs);

  buffer->pixmap = pixmap;
  buffer->picture = picture;
  buffer->width = width;
  buffer->height = height;
  buffer->stride = stride;
  buffer->offset = offset;
  buffer->format = format;
  buffer->pool = pool;
  buffer->refcount = 1;

  /* Initialize function pointers.  */
  buffer->buffer.funcs.retain = RetainBufferFunc;
  buffer->buffer.funcs.dereference = DereferenceBufferFunc;
  buffer->buffer.funcs.get_picture = GetPictureFunc;
  buffer->buffer.funcs.get_pixmap = GetPixmapFunc;
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

  pool = wl_resource_get_user_data (resource);
  data = mremap (pool->data, pool->size, size, MREMAP_MAYMOVE);

  if (data == MAP_FAILED)
    {
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
			      "mremap: %s", strerror (errno));
      return;
    }

  pool->size = size;
  pool->data = data;
}

static const struct wl_shm_pool_interface wl_shm_pool_impl =
  {
    .destroy = DestroyPool,
    .resize = ResizePool,
    .create_buffer = CreateBuffer,
  };

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  all_shms = XLListRemove (all_shms, resource);
}

static void
CreatePool (struct wl_client *client,
	    struct wl_resource *resource,
	    uint32_t id, int fd, int size)
{
  Pool *pool;

  pool = XLSafeMalloc (sizeof *pool);

  if (!pool)
    wl_resource_post_no_memory (resource);

  memset (pool, 0, sizeof *pool);

  pool->resource = wl_resource_create (client, &wl_shm_pool_interface,
				       wl_resource_get_version (resource),
				       id);

  /* There are no references to this pool yet.  */
  if (!pool->resource)
    {
      XLFree (pool);
      wl_resource_post_no_memory (resource);

      return;
    }

  pool->data = mmap (NULL, size, PROT_READ, MAP_SHARED, fd, 0);

  if (pool->data == (void *) -1)
    {
      wl_resource_destroy (pool->resource);
      XLFree (pool);
      wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
			      "mmap: %s", strerror (errno));

      return;
    }

  wl_resource_set_implementation (pool->resource, &wl_shm_pool_impl,
				  pool, HandlePoolResourceDestroy);

  pool->size = size;
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
  /* TODO: don't hard-code visuals and be slightly more versatile.  */

  wl_shm_send_format (resource, WL_SHM_FORMAT_XRGB8888);
  wl_shm_send_format (resource, WL_SHM_FORMAT_ARGB8888);
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
				  NULL, HandleResourceDestroy);
  all_shms = XLListPrepend (all_shms, resource);

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
  xcb_shm_query_version_reply_t *reply;
  xcb_shm_query_version_cookie_t cookie;

  /* This shouldn't be freed.  */
  const xcb_query_extension_reply_t *ext;

  ext = xcb_get_extension_data (compositor.conn, &xcb_shm_id);

  if (!ext || !ext->present)
    {
      fprintf (stderr, "The MIT-SHM extension is not supported by this X server.\n");
      exit (1);
    }

  cookie = xcb_shm_query_version (compositor.conn);
  reply = xcb_shm_query_version_reply (compositor.conn,
				       cookie, NULL);

  if (!reply)
    {
      fprintf (stderr, "The MIT-SHM extension on this X server is too old.\n");
      exit (1);
    }
  else if (reply->major_version < 1
	   || (reply->major_version == 1
	       && reply->minor_version < 2))
    {
      fprintf (stderr, "The MIT-SHM extension on this X server is too old"
	       " to support POSIX shared memory.\n");
      exit (1);
    }

  free (reply);

  InitRender ();

  global_shm = wl_global_create (compositor.wl_display,
				 &wl_shm_interface, 1,
				 NULL, HandleBind);
}
