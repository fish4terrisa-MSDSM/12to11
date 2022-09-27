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
#include <string.h>
#include <stdio.h>

#include "compositor.h"

/* Structure defining a registered renderer.  */

typedef struct _Renderer Renderer;

struct _Renderer
{
  /* The next renderer in the chain of available renderers.  */
  Renderer *next;

  /* The name of this renderer.  */
  const char *name;

  /* Set of buffer manipulation functions.  */
  BufferFuncs *buffer_funcs;

  /* Set of rendering manipulation functions.  */
  RenderFuncs *render_funcs;
};

/* Chain of all renderers.  */
static Renderer *renderers;

/* The selected buffer functions.  */
static BufferFuncs buffer_funcs;

/* The selected render functions.  */
static RenderFuncs render_funcs;

/* Flags of the selected renderer.  */
int renderer_flags;

static Renderer *
AllocateRenderer (void)
{
  static Renderer renderers[2];
  static int used;

  if (used < ArrayElements (renderers))
    return &renderers[used++];

  /* When adding a new renderer, make sure to update the number of
     renderers in the above array.  */
  abort ();
}

RenderTarget
RenderTargetFromWindow (Window window)
{
  return render_funcs.target_from_window (window);
}

RenderTarget
RenderTargetFromPixmap (Pixmap pixmap)
{
  return render_funcs.target_from_pixmap (pixmap);
}

void
RenderNoteTargetSize (RenderTarget target, int width, int height)
{
  if (!render_funcs.note_target_size)
    /* This function can be NULL.  */
    return;

  render_funcs.note_target_size (target, width, height);
}

Picture
RenderPictureFromTarget (RenderTarget target)
{
  return render_funcs.picture_from_target (target);
}

void
RenderFreePictureFromTarget (Picture picture)
{
  render_funcs.free_picture_from_target (picture);
}

void
RenderDestroyRenderTarget (RenderTarget target)
{
  render_funcs.destroy_render_target (target);
}

void
RenderStartRender (RenderTarget target)
{
  if (render_funcs.start_render)
    render_funcs.start_render (target);
}

void
RenderFillBoxesWithTransparency (RenderTarget target, pixman_box32_t *boxes,
				 int nboxes, int min_x, int min_y)
{
  render_funcs.fill_boxes_with_transparency (target, boxes,
					     nboxes, min_x, min_y);
}

void
RenderClearRectangle (RenderTarget target, int x, int y, int width, int height)
{
  render_funcs.clear_rectangle (target, x, y, width, height);
}

void
RenderComposite (RenderBuffer source, RenderTarget target, Operation op,
		 int src_x, int src_y, int x, int y, int width, int height,
		 DrawParams *draw_params)
{
  render_funcs.composite (source, target, op, src_x, src_y, x, y,
			  width, height, draw_params);
}

void
RenderFinishRender (RenderTarget target)
{
  if (render_funcs.finish_render)
    render_funcs.finish_render (target);
}

int
RenderTargetAge (RenderTarget target)
{
  return render_funcs.target_age (target);
}

RenderFence
RenderImportFdFence (int fd, Bool *error)
{
  /* Fence-related functions must be defined if
     SupportExplicitSync is in flags.  */
  return render_funcs.import_fd_fence (fd, error);
}

void
RenderWaitFence (RenderFence fence)
{
  return render_funcs.wait_fence (fence);
}

void
RenderDeleteFence (RenderFence fence)
{
  return render_funcs.delete_fence (fence);
}

int
RenderGetFinishFence (Bool *error)
{
  return render_funcs.get_finish_fence (error);
}

DrmFormat *
RenderGetDrmFormats (int *n_formats)
{
  return buffer_funcs.get_drm_formats (n_formats);
}

dev_t
RenderGetRenderDevice (Bool *error)
{
  return buffer_funcs.get_render_device (error);
}

ShmFormat *
RenderGetShmFormats (int *n_formats)
{
  return buffer_funcs.get_shm_formats (n_formats);
}

RenderBuffer
RenderBufferFromDmaBuf (DmaBufAttributes *attributes, Bool *error)
{
  return buffer_funcs.buffer_from_dma_buf (attributes, error);
}

void
RenderBufferFromDmaBufAsync (DmaBufAttributes *attributes,
			     DmaBufSuccessFunc success_func,
			     DmaBufFailureFunc failure_func,
			     void *callback_data)
{
  return buffer_funcs.buffer_from_dma_buf_async (attributes,
						 success_func,
						 failure_func,
						 callback_data);
}

RenderBuffer
RenderBufferFromShm (SharedMemoryAttributes *attributes, Bool *error)
{
  return buffer_funcs.buffer_from_shm (attributes, error);
}

Bool
RenderValidateShmParams (uint32_t format, uint32_t width, uint32_t height,
			 int32_t offset, int32_t stride, size_t pool_size)
{
  return buffer_funcs.validate_shm_params (format, width, height,
					   offset, stride, pool_size);
}

void
RenderFreeShmBuffer (RenderBuffer buffer)
{
  return buffer_funcs.free_shm_buffer (buffer);
}

void
RenderFreeDmabufBuffer (RenderBuffer buffer)
{
  return buffer_funcs.free_dmabuf_buffer (buffer);
}

void
RenderUpdateBufferForDamage (RenderBuffer buffer, pixman_region32_t *damage,
			     float scale)
{
  if (!buffer_funcs.update_buffer_for_damage)
    return;

  buffer_funcs.update_buffer_for_damage (buffer, damage, scale);
}

Bool
RenderCanReleaseNow (RenderBuffer buffer)
{
  return buffer_funcs.can_release_now (buffer);
}

void
RegisterStaticRenderer (const char *name,
			RenderFuncs *render_funcs,
			BufferFuncs *buffer_funcs)
{
  Renderer *renderer;

  renderer = AllocateRenderer ();
  renderer->next = renderers;
  renderer->buffer_funcs = buffer_funcs;
  renderer->render_funcs = render_funcs;
  renderer->name = name;

  renderers = renderer;
}

static Bool
InstallRenderer (Renderer *renderer)
{
  buffer_funcs = *renderer->buffer_funcs;
  render_funcs = *renderer->render_funcs;

  /* Now, initialize the renderer by calling its init functions.  */

  if (!render_funcs.init_render_funcs ())
    /* If this returns false, then the renderer cannot be used.  */
    return False;

  /* Next, initialize the colormap before init_buffer_funcs.  */
  compositor.colormap
    = XCreateColormap (compositor.display,
		       DefaultRootWindow (compositor.display),
		       compositor.visual, AllocNone);

  buffer_funcs.init_buffer_funcs ();

  /* Finally, set the flags.  The idea is that init_render_funcs
     and/or init_buffer_funcs might change this, so we set them from
     renderer->render_funcs.  */
  renderer_flags = renderer->render_funcs->flags;

  return True;
}

static void
PickRenderer (void)
{
  const char *selected;
  Renderer *renderer;

  /* Install and initialize the first renderer in the list.  */
  XLAssert (renderers != NULL);

  selected = getenv ("RENDERER");

  if (selected)
    {
      /* If selected is "help", print each renderer and exit.  */
      if (!strcmp (selected, "help"))
	{
	  fprintf (stderr, "The following rendering backends can be used:\n");

	  for (renderer = renderers; renderer; renderer = renderer->next)
	    fprintf (stderr, "    %s\n", renderer->name);

	  exit (0);
	}

      /* Otherwise, look for a renderer matching the given name and
	 install it.  */
      for (renderer = renderers; renderer; renderer = renderer->next)
	{
	  if (!strcmp (renderer->name, selected))
	    {
	      if (!InstallRenderer (renderer))
		{
		  fprintf (stderr, "Failed to initialize renderer %s, "
			   "defaulting to %s instead.\n", selected,
			   renderers->name);
		  goto fall_back;
		}

	      return;
	    }
	}

      /* Fall back to the default renderer.  */
      fprintf (stderr, "Defaulting to renderer %s, as %s was not found\n",
	       renderers->name, selected);
    fall_back:
    }

  if (!InstallRenderer (renderers))
    abort ();
}

void
InitRenderers (void)
{
#ifdef HaveEglSupport
  InitEgl ();
#endif
  InitPictureRenderer ();

  PickRenderer ();
}
