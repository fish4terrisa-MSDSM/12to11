/* Tests for the Wayland compositor running on the X server.

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

#include "test_harness.h"

/* imgview -- simple viewer for .dump files.  It opens each file as an
   argument and displays it in a window.  */

/* The display imgview is connected to.  */
static Display *display;

/* The visual being used.  */
static Visual *visual;

/* Various image parameters.  */
static int width, height, depth;

static int
get_depth_for_format (struct image_data_header *header)
{
  switch (header->format)
    {
    case IMAGE_DATA_ARGB8888_LE:
      return 32;

    case IMAGE_DATA_XRGB8888_LE:
      return 24;
    }

  return 0;
}

static Visual *
get_visual_for_format (struct image_data_header *header)
{
  XVisualInfo template, *visuals;
  Visual *visual;
  unsigned long mask;
  int n_visuals;

  /* This isn't right if the pixel layout differs, but the program
     assumes that all 24-depth visuals are XRGB8888 and all 32-bit
     ones are ARGB8888.  */

  template.screen = DefaultScreen (display);
  template.class = TrueColor;

  switch (header->format)
    {
    case IMAGE_DATA_ARGB8888_LE:
      template.depth = 32;
      break;

    case IMAGE_DATA_XRGB8888_LE:
      template.depth = 24;
      break;
    }

  mask = VisualScreenMask | VisualDepthMask | VisualClassMask;
  visuals = XGetVisualInfo (display, mask, &template, &n_visuals);

  if (!n_visuals)
    {
      if (visuals)
	XFree (visuals);

      return NULL;
    }

  visual = visuals[0].visual;
  XFree (visuals);
  return visual;
}

static Pixmap
get_image (const char *filename)
{
  unsigned char *data;
  struct image_data_header header;
  XImage *image;
  Pixmap pixmap;
  GC gc;

  data = load_image_data (filename, &header);

  if (!data)
    return None;

  visual = get_visual_for_format (&header);
  depth = get_depth_for_format (&header);

  if (!depth || !visual)
    {
      free (data);
      return None;
    }

  image = XCreateImage (display, visual, depth, ZPixmap, 0,
			(char *) data, header.width,
			header.height, 8, header.stride);
  image->data = (char *) data;

  pixmap = XCreatePixmap (display, DefaultRootWindow (display),
			  header.width, header.height, depth);
  gc = XCreateGC (display, pixmap, 0, NULL);

  /* Upload the image to the pixmap and free the GC.  */
  XPutImage (display, pixmap, gc, image, 0, 0,
	     0, 0, header.width, header.height);
  XFreeGC (display, gc);

  /* Destroy the XImage data.  */
  free (image->data);
  image->data = NULL;

  /* Destroy the XImage.  */
  XDestroyImage (image);

  /* Set image params.  */
  width = header.width;
  height = header.height;
  depth = depth;

  return pixmap;
}

static void
loop_for_events (void)
{
  XEvent event;

  while (true)
    XNextEvent (display, &event);
}

static void
open_window (Pixmap image)
{
  Window window;
  XSetWindowAttributes attrs;
  unsigned long flags;

  attrs.colormap = XCreateColormap (display,
				    DefaultRootWindow (display),
				    visual, AllocNone);
  /* This should be valid on any TrueColor visual.  */
  attrs.border_pixel = 0;
  attrs.background_pixmap = image;
  attrs.cursor = None;
  flags = (CWColormap | CWBorderPixel | CWBackPixmap | CWCursor);

  window = XCreateWindow (display,
			  DefaultRootWindow (display),
			  0, 0, width, height, 0, depth,
			  InputOutput, visual, flags,
			  &attrs);
  XMapRaised (display, window);
}

int
main (int argc, char **argv)
{
  Pixmap image;
  int i;

  if (argc < 2)
    return 1;

  display = XOpenDisplay (NULL);

  if (!display)
    return 1;

  for (i = 1; i < argc; ++i)
    {
      image = get_image (argv[i]);

      if (!image)
	continue;

      open_window (image);
    }

  loop_for_events ();
}
