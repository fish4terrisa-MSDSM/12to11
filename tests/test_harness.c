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
#include <string.h>
#include <stdio.h>

#include <setjmp.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/errno.h>
#include <sys/uio.h>

#include <png.h>

#include "test_harness.h"

#define BIG_ENDIAN_BYTE_ORDER (1 << 8)

#if PNG_LIBPNG_VER < 10500
#define PNG_JMPBUF(ptr) ((ptr)->jmpbuf)
# else
#define PNG_JMPBUF(ptr)						\
  (*png_set_longjmp_fn (ptr, longjmp, sizeof (jmp_buf)))
#endif

/* Whether or not to write image data instead of verifying it.  */
static bool write_image_data_instead;

/* The test display.  */
static struct test_display *display;

static void
handle_test_manager_display_string (void *data, struct test_manager *manager,
				    const char *display_string)
{
  struct test_display *display;

  display = data;
  display->x_display = XOpenDisplay (display_string);
  display->pixmap_formats
    = XListPixmapFormats (display->x_display,
			  &display->num_pixmap_formats);
}

static void
handle_test_manager_serial (void *data, struct test_manager *manager,
			    uint32_t serial)
{
  struct test_display *display;

  display = data;
  display->serial = serial;
}

static const struct test_manager_listener test_manager_listener =
  {
    handle_test_manager_display_string,
    handle_test_manager_serial,
  };

static bool
test_manager_check (struct test_display *display)
{
  return (display->x_display != NULL
	  && display->num_pixmap_formats
	  && display->pixmap_formats);
}



static void
handle_registry_global (void *data, struct wl_registry *registry,
			uint32_t id, const char *interface,
			uint32_t version)
{
  struct test_display *display;
  int i;

  display = data;

  if (!strcmp (interface, "wl_compositor") && version >= 5)
    display->compositor
      = wl_registry_bind (registry, id, &wl_compositor_interface,
			  5);
  if (!strcmp (interface, "wl_shm") && version >= 1)
    display->shm = wl_registry_bind (registry, id, &wl_shm_interface, 1);
  else if (!strcmp (interface, "test_manager"))
    display->test_manager
      = wl_registry_bind (registry, id, &test_manager_interface, 1);
  else
    {
      /* Look through the user specified list of interfaces.  */
      for (i = 0; i < display->num_test_interfaces; ++i)
	{
	  if (!strcmp (interface, display->interfaces[i].interface)
	      && display->interfaces[i].version >= version)
	    /* Bind to it.  */
	    *((void **) display->interfaces[i].data)
	      = wl_registry_bind (registry, id,
				  display->interfaces[i].c_interface,
				  display->interfaces[i].version);
	}
    }
}

static void
handle_registry_global_remove (void *data, struct wl_registry *registry,
			       uint32_t name)
{
  return;
}

static const struct wl_registry_listener registry_listener =
  {
    handle_registry_global,
    handle_registry_global_remove,
  };

/* Check whether or not all required globals were found and bound
   to.  */

static bool
registry_listener_check (struct test_display *display)
{
  int i;

  if (!display->compositor)
    return false;

  if (!display->shm)
    return false;

  if (!display->test_manager)
    return false;

  for (i = 0; i < display->num_test_interfaces; ++i)
    {
      if (!*((void **) display->interfaces[i].interface))
	return false;
    }

  return true;
}

void __attribute__ ((noreturn))
die (const char *reason)
{
  perror (reason);
  exit (1);
}

/* This should be called when a test failed due to a non-IO error.  It
   destroys the scale lock and synchronizes with the compositor so
   that by the time the next test is run, the current test's scale
   lock will have been released.  */

static void __attribute__ ((noreturn))
exit_with_code (int code)
{
  if (display)
    {
      test_scale_lock_destroy (display->scale_lock);
      wl_display_roundtrip (display->display);
    }

  exit (code);
}

struct test_display *
open_test_display (struct test_interface *interfaces, int num_interfaces)
{
  if (display)
    /* The display was already opened.  */
    return display;

  display = malloc (sizeof *display);

  if (!display)
    return NULL;

  display->display = wl_display_connect (NULL);

  if (!display->display)
    goto error_1;

  display->registry = wl_display_get_registry (display->display);

  if (!display->registry)
    goto error_2;

  display->interfaces = interfaces;
  display->num_test_interfaces = num_interfaces;
  wl_registry_add_listener (display->registry, &registry_listener,
			    display);
  wl_display_roundtrip (display->display);

  if (!registry_listener_check (display))
    goto error_2;

  /* Now establish the connection to the X display.  */
  test_manager_add_listener (display->test_manager,
			     &test_manager_listener, display);
  wl_display_roundtrip (display->display);

  if (!test_manager_check (display))
    goto error_3;

  /* And try to set up the scale lock.  */
  display->scale_lock
    = test_manager_get_scale_lock (display->test_manager, 1);

  if (!display->scale_lock)
    goto error_4;

  display->seat = NULL;
  return display;

 error_4:
  XFree (display->pixmap_formats);
 error_3:
  if (display->x_display)
    XCloseDisplay (display->x_display);
 error_2:
  wl_display_disconnect (display->display);
 error_1:
  free (display);
  display = NULL;
  return NULL;
}

int
get_shm_file_descriptor (void)
{
  char name[sizeof "test_driver_buffer_XXXXXXXX"];
  int fd;
  uint32_t i;

  i = 0;

  while (i <= 0xffffffff)
    {
      sprintf (name, "test_driver_buffer_%"PRIu32, i);
      fd = shm_open (name, O_RDWR | O_CREAT | O_EXCL, 0600);

      if (fd >= 0)
	{
	  shm_unlink (name);
	  return fd;
	}

      if (errno == EEXIST)
	++i;
      else
	return -1;
    }

  return -1;
}

#define IMAGE_PAD(nbytes, pad) ((((nbytes) + ((pad) - 1)) / (pad)) * ((pad) >> 3))

size_t
get_image_stride (struct test_display *display, int depth, int width)
{
  int bpp, scanline_pad, i;
  size_t stride;

  for (i = 0; i < display->num_pixmap_formats; ++i)
    {
      if (display->pixmap_formats[i].depth == depth)
	{
	  bpp = display->pixmap_formats[i].bits_per_pixel;
	  scanline_pad = display->pixmap_formats[i].bits_per_pixel;
	  stride = IMAGE_PAD (width * bpp, scanline_pad);

	  return stride;
	}
    }

  return 0;
}

struct wl_buffer *
upload_image_data (struct test_display *display, const char *data,
		   int width, int height, int depth)
{
  size_t stride;
  int fd;
  void *mapping;
  struct wl_shm_pool *pool;
  struct wl_buffer *buffer;

  if (depth != 32 && depth != 24)
    return NULL;

  stride = get_image_stride (display, depth, width);

  if (!stride)
    return NULL;

  fd = get_shm_file_descriptor ();

  if (fd < 0)
    return NULL;

  if (ftruncate (fd, stride * height) < 0)
    return NULL;

  mapping = mmap (NULL, stride * height, PROT_WRITE, MAP_SHARED,
		  fd, 0);

  if (mapping == (void *) -1)
    {
      perror ("mmap");
      close (fd);
      return NULL;
    }

  memcpy (mapping, data, stride * height);

  if (munmap (mapping, stride * height) < 0)
    die ("munmap");

  pool = wl_shm_create_pool (display->shm, fd, stride * height);

  if (!pool)
    {
      close (fd);
      return NULL;
    }

  close (fd);
  buffer = wl_shm_pool_create_buffer (pool, 0, width, height, stride,
				      (depth == 32
				       ? WL_SHM_FORMAT_ARGB8888
				       : WL_SHM_FORMAT_XRGB8888));
  wl_shm_pool_destroy (pool);

  return buffer;
}

void __attribute__ ((noreturn, format (gnu_printf, 1, 2)))
report_test_failure (const char *format, ...)
{
  va_list ap;

  va_start (ap, format);
  fputs ("failure: ", stderr);
  vfprintf (stderr, format, ap);
  fputs ("\n", stderr);
  va_end (ap);

  exit_with_code (1);
}

void __attribute__ ((format (gnu_printf, 1, 2)))
test_log (const char *format, ...)
{
  va_list ap;

  va_start (ap, format);
  fputs ("note: ", stderr);
  vfprintf (stderr, format, ap);
  fputs ("\n", stderr);
  va_end (ap);
}

static void __attribute__ ((noreturn, format (gnu_printf, 1, 2)))
report_test_internal_error (const char *format, ...)
{
  va_list ap;

  va_start (ap, format);
  fputs ("internal error: ", stderr);
  vfprintf (stderr, format, ap);
  fputs ("\n", stderr);
  va_end (ap);

  abort ();
}

bool
make_test_surface (struct test_display *display,
		   struct wl_surface **surface_return,
		   struct test_surface **test_surface_return)
{
  struct wl_surface *surface;
  struct test_surface *test_surface;

  surface = wl_compositor_create_surface (display->compositor);

  if (!surface)
    return false;

  test_surface
    = test_manager_get_test_surface (display->test_manager,
				     surface);

  if (!test_surface)
    {
      wl_surface_destroy (surface);
      return false;
    }

  *surface_return = surface;
  *test_surface_return = test_surface;
  return true;
}

/* Swizzle the big-endian data in ROW_DATA to the little-endian format
   Wayland mandates.  */

static void
swizzle_png_row (unsigned char *row_data, int width)
{
  int i;
  unsigned char byte_1, byte_2, byte_3, byte_4;

  for (i = 0; i < width; ++i)
    {
      byte_1 = row_data[i * 4 + 0];
      byte_2 = row_data[i * 4 + 1];
      byte_3 = row_data[i * 4 + 2];
      byte_4 = row_data[i * 4 + 3];

      row_data[i * 4 + 0] = byte_3;
      row_data[i * 4 + 1] = byte_2;
      row_data[i * 4 + 2] = byte_1;
      row_data[i * 4 + 3] = byte_4;
    }
}

/* Do the same, but also premultiply the individual color components
   with the alpha.  */

static void
swizzle_png_row_premultiply (unsigned char *row_data, int width)
{
  int i;
  unsigned char byte_1, byte_2, byte_3, byte_4;

  for (i = 0; i < width; ++i)
    {
      byte_1 = row_data[i * 4 + 0];
      byte_2 = row_data[i * 4 + 1];
      byte_3 = row_data[i * 4 + 2];
      byte_4 = row_data[i * 4 + 3];

      row_data[i * 4 + 0] = (byte_3 * 1u * byte_4) / 255u;
      row_data[i * 4 + 1] = (byte_2 * 1u * byte_4) / 255u;
      row_data[i * 4 + 2] = (byte_1 * 1u * byte_4) / 255u;
      row_data[i * 4 + 3] = byte_4;
    }
}

/* Load a PNG image into a wl_buffer.  The image must be either
   PNG_COLOR_TYPE_RGB or PNG_COLOR_TYPE_RGB_ALPHA.  The image
   background is ignored.  */

struct wl_buffer *
load_png_image (struct test_display *display, const char *filename)
{
  FILE *file;
  unsigned char signature[8];
  png_structp png_ptr;
  png_infop info_ptr;
  png_uint_32 width, height;
  int bit_depth, color_type, depth;
  png_uint_32 i, rowbytes;
  png_bytep *row_pointers;
  unsigned char *image_data;
  struct wl_buffer *buffer;

  image_data = NULL;
  file = fopen (filename, "r");

  if (!file)
    return NULL;

  if (fread (signature, 1, 8, file) != 8)
    goto error_1;

  if (!png_check_sig (signature, 8))
    goto error_1;

  png_ptr = png_create_read_struct (PNG_LIBPNG_VER_STRING, NULL, NULL,
				    NULL);

  if (!png_ptr)
    goto error_1;

  info_ptr = png_create_info_struct (png_ptr);

  if (!info_ptr)
    goto error_2;

  if (setjmp (PNG_JMPBUF (png_ptr)))
    goto error_3;

  png_init_io (png_ptr, file);
  png_set_sig_bytes (png_ptr, 8);
  png_read_info (png_ptr, info_ptr);
  png_get_IHDR (png_ptr, info_ptr, &width, &height, &bit_depth,
		&color_type, NULL, NULL, NULL);

  if (color_type != PNG_COLOR_TYPE_RGB
      && color_type != PNG_COLOR_TYPE_RGB_ALPHA)
    goto error_3;

  /* Get data as ARGB.  */
  if (color_type == PNG_COLOR_TYPE_RGB)
    png_set_filler (png_ptr, 0, PNG_FILLER_AFTER);

  /* Start reading the PNG data.  Get the stride and depth.  */
  depth = (color_type == PNG_COLOR_TYPE_RGB_ALPHA
	   ? 32 : 24);

  png_read_update_info (png_ptr, info_ptr);
  rowbytes = get_image_stride (display, depth, width);

  /* Allocate a buffer for the image data.  */
  image_data = malloc (rowbytes * height);

  if (!image_data)
    goto error_3;

  /* Allocate the array of row pointers.  */
  row_pointers = alloca (sizeof *row_pointers * height);

  for (i = 0; i < height; ++i)
    row_pointers[i] = image_data + i * rowbytes;

  /* Read the data.  */
  png_read_image (png_ptr, row_pointers);
  png_read_end (png_ptr, NULL);

  for (i = 0; i < height; ++i)
    {
      /* Swizzle the big-endian data.  */

      if (color_type == PNG_COLOR_TYPE_RGB_ALPHA)
	swizzle_png_row_premultiply (row_pointers[i], width);
      else
	swizzle_png_row (row_pointers[i], width);
    }

  /* Now, destroy the read struct and close the file.  */
  png_destroy_read_struct (&png_ptr, &info_ptr, NULL);
  fclose (file);

  /* Upload the image data.  */
  buffer = upload_image_data (display, (const char *) image_data,
			      width, height, depth);
  test_manager_set_buffer_label (display->test_manager, buffer,
				 filename);

  /* Free the image data.  */
  free (image_data);

  return buffer;

 error_3:
  if (image_data)
    free (image_data);

  /* This looks silly... */
  if (true)
    png_destroy_read_struct (&png_ptr, &info_ptr, NULL);
  else
 error_2:
    png_destroy_read_struct (&png_ptr, NULL, NULL);
 error_1:
  fclose (file);
  return NULL;
}

static unsigned short
bytes_per_pixel_for_format (enum image_data_format format)
{
  switch (format)
    {
    case IMAGE_DATA_ARGB8888_LE:
    case IMAGE_DATA_XRGB8888_LE:
      return 4;

    default:
      return 0;
    }
}

static int
byte_order_for_format (enum image_data_format format)
{
  switch (format)
    {
    case IMAGE_DATA_ARGB8888_LE:
    case IMAGE_DATA_XRGB8888_LE:
      return LSBFirst;

    default:
      return 0;
    }
}

/* Load image data from a file.  */

unsigned char *
load_image_data (const char *filename, struct image_data_header *header)
{
  int fd;
  struct iovec iov;
  unsigned char *buffer;
  unsigned short bpp;

  fd = open (filename, O_RDONLY);

  if (fd < 0)
    return NULL;

  iov.iov_base = header;
  iov.iov_len = sizeof *header;

  if (readv (fd, &iov, 1) != iov.iov_len)
    goto error_1;

#ifdef __BIG_ENDIAN__
  if (!(header->version & BIG_ENDIAN_BYTE_ORDER))
    goto error_1;
#endif

  if ((header->version & ~BIG_ENDIAN_BYTE_ORDER) != 1)
    goto error_1;

  bpp = bytes_per_pixel_for_format (header->format);

  if (!bpp || header->stride < header->width * bpp)
    goto error_1;

  buffer = malloc (header->stride * header->height);

  if (!buffer)
    goto error_1;

  iov.iov_base = buffer;
  iov.iov_len = header->stride * header->height;

  if (readv (fd, &iov, 1) != iov.iov_len)
    goto error_2;

  close (fd);
  return buffer;

 error_2:
  free (buffer);
 error_1:
  close (fd);
  return NULL;
}

static void
write_image_data_1 (XImage *image, const char *filename)
{
  struct image_data_header header;
  struct iovec iovecs[2];
  int fd;

  memset (&header, 0, sizeof header);
  header.version = 1;
#ifdef __BIG_ENDIAN__
  header.version |= BIG_ENDIAN_BYTE_ORDER;
#endif

  if ((image->depth != 24 && image->depth != 32)
      || image->bits_per_pixel != 32)
    report_test_failure ("don't know how to save image of depth %d (bpp %d)",
			 image->depth, image->bits_per_pixel);

  if (image->byte_order != LSBFirst)
    report_test_failure ("don't know how to save big-endian image");

  /* TODO: determine the image format based on the visual of the
     window.  */
  header.format = (image->depth == 24
		   ? IMAGE_DATA_XRGB8888_LE
		   : IMAGE_DATA_ARGB8888_LE);
  header.width = image->width;
  header.height = image->height;
  header.stride = image->bytes_per_line;

  /* Open the output file and write the data.  */
  fd = open (filename, O_TRUNC | O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);

  if (fd < 0)
    report_test_failure ("failed to open output file %s", filename);

  iovecs[0].iov_base = &header;
  iovecs[0].iov_len = sizeof header;
  iovecs[1].iov_base = image->data;
  iovecs[1].iov_len = image->bytes_per_line * image->height;

  if (writev (fd, iovecs, 2) != iovecs[0].iov_len + iovecs[1].iov_len)
    report_test_failure ("failed to write to output file %s", filename);

  close (fd);
}

static void
compare_single_row (unsigned char *data, int row_no,
		    struct image_data_header *header, XImage *image)
{
  char *xdata;
  unsigned short bytes_per_pixel;

  bytes_per_pixel = bytes_per_pixel_for_format (header->format);
  data = data + header->stride * row_no;
  xdata = image->data + image->bytes_per_line * row_no;

  if (memcmp (data, xdata, bytes_per_pixel * header->width))
    {
      /* Write the reject to a file.  */
      test_log ("writing reject to reject.dump");
      write_image_data_1 (image, "reject.dump");

      report_test_failure ("row %d differs", row_no);
    }
}

static void
write_image_data (struct test_display *display, Window window,
		  const char *filename)
{
  XImage *image;
  XWindowAttributes attrs;

  test_log ("writing contents of drawable to reference %s", filename);

  XGetWindowAttributes (display->x_display, window, &attrs);
  image = XGetImage (display->x_display, window, 0, 0, attrs.width,
		     attrs.height, ~0, ZPixmap);

  if (!image)
    report_test_failure ("failed to load from drawable 0x%lx", window);

  write_image_data_1 (image, filename);
  XDestroyImage (image);

  test_log ("image data written to %s", filename);
}

/* Verify the image data against a file.  */

void
verify_image_data (struct test_display *display, Window window,
		   const char *filename)
{
  XImage *image;
  XWindowAttributes attrs;
  unsigned char *data;
  struct image_data_header header;
  unsigned short data_bpp, i;
  int byte_order;

  if (write_image_data_instead)
    write_image_data (display, window, filename);

  data = load_image_data (filename, &header);

  if (!data)
    report_test_failure ("failed to load input file: %s", filename);

  XGetWindowAttributes (display->x_display, window, &attrs);
  image = XGetImage (display->x_display, window, 0, 0, attrs.width,
		     attrs.height, ~0, ZPixmap);

  test_log ("verifying image data from: %s", filename);

  if (!image)
    report_test_failure ("failed to load from drawable 0x%lx", window);

  /* Check if the image data is compatible.  */
  data_bpp = bytes_per_pixel_for_format (header.format);
  byte_order = byte_order_for_format (header.format);

  if (byte_order != image->byte_order)
    report_test_failure ("image data has wrong byte order");

  if (data_bpp * 8 != image->bits_per_pixel)
    report_test_failure ("image data has %d bits per pixel, but reference"
			 " data has %hd * 8", image->bits_per_pixel, data_bpp);

  if (image->width != header.width
      || image->height != header.height)
    report_test_failure ("image data is %d by %d, but reference data is"
			 " %hd by %hd", image->width, image->height,
			 header.width, header.height);

  /* Now compare the actual image data.  Make sure this is done with
     the same visual as the reference data was saved in!  */

  for (i = 0; i < header.height; ++i)
    compare_single_row (data, i, &header, image);

  /* Destroy the images.  */
  free (data);
  XDestroyImage (image);

  test_log ("verified image data");
}

void
test_set_scale (struct test_display *display, int scale)
{
  test_scale_lock_set_scale (display->scale_lock, scale);
}

void
test_init (void)
{
  write_image_data_instead
    = getenv ("TEST_WRITE_REFERENCE") != NULL;
}



static void
handle_seat_controller_device_id (void *data,
				  struct test_seat_controller *controller,
				  uint32_t device_id)
{
  struct test_display *display;

  display = data;
  display->seat->device_id = device_id;
}

static const struct test_seat_controller_listener seat_controller_listener =
  {
    handle_seat_controller_device_id,
  };



void
test_init_seat (struct test_display *display)
{
  if (display->seat)
    report_test_internal_error ("tried to initialize seat twice");

  display->seat = malloc (sizeof *display->seat);

  if (!display->seat)
    report_test_failure ("failed to allocate seat");

  display->seat->controller
    = test_manager_get_test_seat (display->test_manager);

  if (!display->seat->controller)
    report_test_failure ("failed to obtain seat controller");

  display->seat->device_controller
    = test_seat_controller_get_device_controller (display->seat->controller);

  if (!display->seat->device_controller)
    report_test_failure ("failed to obtain device controller");

  /* Fetch the device ID of the seat.  */
  display->seat->device_id = 0;
  test_seat_controller_add_listener (display->seat->controller,
				     &seat_controller_listener,
				     display);
  wl_display_roundtrip (display->display);

  if (!display->seat->device_id)
    report_test_failure ("failed to obtain device ID");

  /* The protocol translator currently supports version 8 of wl_seat,
     so bind to that.  */

  display->seat->seat
    = test_seat_controller_bind_seat (display->seat->controller,
				      8);

  if (!display->seat->seat)
    report_test_failure ("failed to bind to test seat");

  display->seat->pointer
    = wl_seat_get_pointer (display->seat->seat);

  if (!display->seat->pointer)
    report_test_failure ("failed to bind to test pointer");

  display->seat->keyboard
    = wl_seat_get_keyboard (display->seat->seat);

  if (!display->seat->keyboard)
    report_test_failure ("failed to bind to test keyboard");
}

void __attribute__ ((noreturn))
test_complete (void)
{
  test_log ("test ran successfully");
  exit_with_code (0);
}

uint32_t
test_get_serial (struct test_display *display)
{
  test_manager_get_serial (display->test_manager);
  wl_display_roundtrip (display->display);

  return display->serial;
}
