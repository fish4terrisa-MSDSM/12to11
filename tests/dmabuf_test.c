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

#include <xf86drm.h>
#include <gbm.h>
#include <drm/drm_fourcc.h>

#include <sys/mman.h>
#include <sys/fcntl.h>

#include "test_harness.h"
#include "linux-dmabuf-unstable-v1.h"

/* N.B. that this test will have to be adjusted once multiple devices
   are fully supported.  */

enum test_kind
  {
    ARGB8888_KIND,
    ARGB8888_LINEAR_KIND,
    XBGR8888_KIND,
  };

static const char *test_names[] =
  {
    "argb8888",
    "argb8888_linear",
    "xbgr8888",
  };

#define LAST_TEST	XBGR8888_KIND

struct test_params_data
{
  /* The buffer.  */
  struct wl_buffer *buffer;

  /* Flag that indicates completion.  */
  bool complete;
};

struct test_feedback_tranche
{
  /* The next tranche (with higher priority).  */
  struct test_feedback_tranche *next;

  /* Array of indices into the format-modifier table.  */
  uint16_t *indices;

  /* Number of format-modifier pairs supported.  */
  int n_indices;
};

struct test_feedback_data
{
  /* The device node of the main device.  */
  dev_t device;

  /* The file descriptor of the format-modifier table.  */
  int fd;

  /* The size of the format-modifier table.  */
  uint32_t format_table_size;

  /* List of tranches.  */
  struct test_feedback_tranche *tranches;

  /* Whether or not a tranche is being recorded.  */
  bool recording_tranche;
};

struct format_modifier_pair
{
  /* See the documentation of
     zwp_linux_dmabuf_feedback_v1::format_table for more details. */
  uint32_t format;
  uint32_t padding;
  uint64_t modifier;
};

/* The display.  */
static struct test_display *display;

/* The dmabuf interface.  */
static struct zwp_linux_dmabuf_v1 *linux_dmabuf_v1;

/* Test interfaces.  */
static struct test_interface test_interfaces[] =
  {
    { "zwp_linux_dmabuf_v1", &linux_dmabuf_v1,
      &zwp_linux_dmabuf_v1_interface, 4, },
  };

/* The test surface window.  */
static Window test_surface_window;

/* The test surface and Wayland surface.  */
static struct test_surface *test_surface;
static struct wl_surface *wayland_surface;

/* The GBM device.  */
static struct gbm_device *gbm_device;

/* The format-modifier table.  */
static struct format_modifier_pair *modifier_table;

/* List of tranches.  */
static struct test_feedback_tranche *feedback_tranches;



/* Forward declarations.  */
static void submit_frame_callback (struct wl_surface *, enum test_kind);
static void submit_surface_damage (struct wl_surface *, int, int, int, int);
static struct wl_buffer *create_rainbow_buffer (uint32_t, uint64_t,
						uint32_t, uint32_t,
						uint32_t);
static bool is_format_supported (uint32_t, uint64_t);



static void
verify_single_step (enum test_kind kind)
{
  switch (kind)
    {
    case ARGB8888_KIND:
      verify_image_data (display, test_surface_window,
			 "argb8888_implicit.dump");
      break;

    case ARGB8888_LINEAR_KIND:
      verify_image_data (display, test_surface_window,
			 "argb8888_linear.dump");
      break;

    case XBGR8888_KIND:
      verify_image_data (display, test_surface_window,
			 "xbgr8888_implicit.dump");
      break;

    default:
      break;
    }

  if (kind == LAST_TEST)
    test_complete ();
}

static void
test_single_step (enum test_kind kind)
{
  struct wl_buffer *buffer;

  test_log ("running test step: %s", test_names[kind]);

  switch (kind)
    {
    case ARGB8888_KIND:
      buffer = create_rainbow_buffer (GBM_FORMAT_ARGB8888,
				      DRM_FORMAT_MOD_INVALID,
				      0xffff0000,
				      0xff00ff00,
				      0xff0000ff);

      if (!buffer)
	report_test_failure ("failed to create ARGB8888 buffer");

      wl_surface_attach (wayland_surface, buffer, 0, 0);
      submit_surface_damage (wayland_surface, 0, 0,
			     INT_MAX, INT_MAX);
      submit_frame_callback (wayland_surface, kind);
      wl_surface_commit (wayland_surface);
      wl_buffer_destroy (buffer);
      break;

    case ARGB8888_LINEAR_KIND:

      if (!is_format_supported (DRM_FORMAT_ARGB8888,
				DRM_FORMAT_MOD_LINEAR))
	{
	  test_log ("skipping ARGB888 with linear modifier as"
		    " that is not supported");
	  test_single_step (XBGR8888_KIND);
	}

      buffer = create_rainbow_buffer (GBM_FORMAT_ARGB8888,
				      DRM_FORMAT_MOD_LINEAR,
				      0xffff0000,
				      0xff00ff00,
				      0xff0000ff);

      if (!buffer)
	report_test_failure ("failed to create ARGB8888 buffer"
			     " with linear storage format");

      wl_surface_attach (wayland_surface, buffer, 0, 0);
      submit_surface_damage (wayland_surface, 0, 0,
			     INT_MAX, INT_MAX);
      submit_frame_callback (wayland_surface, kind);
      wl_surface_commit (wayland_surface);
      wl_buffer_destroy (buffer);
      break;

    case XBGR8888_KIND:

      /* XBGR8888 currently does not work due to a bug in glamor.  */
#if 0
      if (!is_format_supported (DRM_FORMAT_XBGR8888,
				DRM_FORMAT_MOD_INVALID))
#endif
	{
	  test_log ("skipping XBGR8888 with implicit modifier as"
		    " that is not supported");
	  test_complete ();
	}

      buffer = create_rainbow_buffer (GBM_FORMAT_XBGR8888,
				      DRM_FORMAT_MOD_INVALID,
				      0x0000ff,
				      0x00ff00,
				      0xff0000);

      if (!buffer)
	report_test_failure ("failed to create XBGR8888 buffer");

      wl_surface_attach (wayland_surface, buffer, 0, 0);
      submit_surface_damage (wayland_surface, 0, 0,
			     INT_MAX, INT_MAX);
      submit_frame_callback (wayland_surface, kind);
      wl_surface_commit (wayland_surface);
      wl_buffer_destroy (buffer);      
      break;
    }
}

static void
test_next_step (enum test_kind kind)
{
  switch (kind)
    {
    case ARGB8888_KIND:
      test_single_step (ARGB8888_LINEAR_KIND);
      break;

    case ARGB8888_LINEAR_KIND:
      test_single_step (XBGR8888_KIND);
      break;

    default:
      break;
    }
}



static void
handle_test_surface_mapped (void *data, struct test_surface *surface,
			    uint32_t xid, const char *display_string)
{
  /* Sleep for 1 second to ensure that the window is exposed and
     redirected.  */
  sleep (1);

  /* Start the test.  */
  test_surface_window = xid;
}

static const struct test_surface_listener test_surface_listener =
  {
    handle_test_surface_mapped,
  };



static void
handle_wl_callback_done (void *data, struct wl_callback *callback,
			 uint32_t callback_data)
{
  enum test_kind kind;

  /* kind is not a pointer.  It is an enum test_kind stuffed into a
     pointer.  */
  kind = (intptr_t) data;

  wl_callback_destroy (callback);
  verify_single_step (kind);

  /* Now run the next test in this sequence.  */
  test_next_step (kind);
}

static const struct wl_callback_listener wl_callback_listener =
  {
    handle_wl_callback_done,
  };



static void
submit_frame_callback (struct wl_surface *surface, enum test_kind kind)
{
  struct wl_callback *callback;

  callback = wl_surface_frame (surface);
  wl_callback_add_listener (callback, &wl_callback_listener,
			    (void *) (intptr_t) kind);
}

static void
submit_surface_damage (struct wl_surface *surface, int x, int y, int width,
		       int height)
{
  test_log ("damaging surface by %d, %d, %d, %d", x, y, width,
	    height);

  wl_surface_damage (surface, x, y, width, height);
}



static void
handle_feedback_done (void *data,
		      struct zwp_linux_dmabuf_feedback_v1 *feedback)
{
  struct test_feedback_data *test_data;

  test_data = data;

  if (test_data->recording_tranche)
    report_test_failure ("done received while recording tranche");
}

static void
handle_feedback_format_table (void *data,
			      struct zwp_linux_dmabuf_feedback_v1 *feedback,
			      int32_t fd, uint32_t size)
{
  struct test_feedback_data *test_data;

  test_data = data;

  test_data->fd = fd;
  test_data->format_table_size = size;
}

static void
handle_feedback_main_device (void *data,
			     struct zwp_linux_dmabuf_feedback_v1 *feedback,
			     struct wl_array *device)
{
  struct test_feedback_data *test_data;

  test_data = data;

  if (device->size != sizeof test_data->device)
    report_test_failure ("got incorrect array size for dev_t");

  memcpy (&test_data->device, device->data, device->size);
}

static void
handle_feedback_tranche_done (void *data,
			      struct zwp_linux_dmabuf_feedback_v1 *feedback)
{
  struct test_feedback_data *test_data;

  test_data = data;

  if (!test_data->recording_tranche)
    report_test_failure ("tranche_done received but not recording tranche");

  test_data->recording_tranche = false;
}

static void
handle_feedback_tranche_target_device (void *data,
				       struct zwp_linux_dmabuf_feedback_v1 *feedback,
				       struct wl_array *device)
{
  /* Nothing to do here.  */
}

static void
handle_feedback_tranche_formats (void *data,
				 struct zwp_linux_dmabuf_feedback_v1 *feedback,
				 struct wl_array *indices)
{
  struct test_feedback_data *test_data;
  struct test_feedback_tranche *tranche;

  test_data = data;

  if (!test_data->recording_tranche)
    {
      /* Start recording a new tranche.  */
      tranche = calloc (1, sizeof *tranche);

      if (!tranche)
	report_test_failure ("failed to allocate tranche");

      tranche->next = test_data->tranches;
      test_data->tranches = tranche;
      test_data->recording_tranche = true;
    }
  else
    tranche = test_data->tranches;

  if (tranche->indices)
    free (tranche->indices);

  if (indices->size % sizeof (uint16_t))
    report_test_failure ("invalid tranche size: %zu", indices->size);

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif

  tranche->indices = malloc (indices->size);
  tranche->n_indices = indices->size / sizeof (uint16_t);

  if (!tranche->indices)
    report_test_failure ("failed to allocate tranche indices");

  memcpy (tranche->indices, indices->data, indices->size);

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}

static void
handle_feedback_tranche_flags (void *data,
			       struct zwp_linux_dmabuf_feedback_v1 *feedback,
			       uint32_t flags)
{
  /* Nothing to do here.  */
}

static struct zwp_linux_dmabuf_feedback_v1_listener feedback_listener =
  {
    handle_feedback_done,
    handle_feedback_format_table,
    handle_feedback_main_device,
    handle_feedback_tranche_done,
    handle_feedback_tranche_target_device,
    handle_feedback_tranche_formats,
    handle_feedback_tranche_flags,
  };



static int
open_device (dev_t device)
{
  drmDevicePtr device_ptr;
  int fd;

  if (drmGetDeviceFromDevId (device, 0, &device_ptr) < 0)
    return -1;

  fd = -1;

  if (device_ptr->available_nodes & (1 << DRM_NODE_RENDER))
    /* Open the render node if available.  */
    fd = open (device_ptr->nodes[DRM_NODE_RENDER], O_RDWR);

  /* Free the device.  */
  drmFreeDevice (&device_ptr);

  /* Return the file descriptor.  */
  return fd;
}

static void
open_surface (void)
{
  struct zwp_linux_dmabuf_feedback_v1 *feedback;
  struct test_feedback_data data;
  struct test_feedback_tranche *tranche;
  int fd, i;

  feedback
    = zwp_linux_dmabuf_v1_get_default_feedback (linux_dmabuf_v1);

  if (!feedback)
    report_test_failure ("failed to create dmabuf feedback");

  memset (&data, 0, sizeof data);
  data.fd = -1;

  zwp_linux_dmabuf_feedback_v1_add_listener (feedback, &feedback_listener,
					     &data);
  wl_display_roundtrip (display->display);

  /* Now verify that everything required arrived.  */
  if (!data.device || data.fd < 0
      || (data.format_table_size
	  % sizeof (struct format_modifier_pair))
      || !data.tranches)
    report_test_failure ("received invalid parameters from feedback");

  /* Open the provided node.  */
  fd = open_device (data.device);

  if (fd < 0)
    report_test_failure ("failed to open device");

  gbm_device = gbm_create_device (fd);

  if (!gbm_device)
    report_test_failure ("failed to open device");

  /* Now try to map the format-modifier table and verify the validity
     of each tranche.  */
  modifier_table = mmap (NULL, data.format_table_size, PROT_READ,
			 MAP_PRIVATE, data.fd, 0);

  if (modifier_table == (void *) -1)
    report_test_failure ("failed to map modifier table");

  for (tranche = data.tranches; tranche; tranche = tranche->next)
    {
      for (i = 0; i < tranche->n_indices; ++i)
	{
	  if (tranche->indices[i]
	      >= data.format_table_size / sizeof *modifier_table)
	    report_test_failure ("tranche index %"PRIu16" extends outside"
				 " bounds of format modifier table",
				 tranche->indices[i]);
	}
    }

  zwp_linux_dmabuf_feedback_v1_destroy (feedback);
  feedback_tranches = data.tranches;
}

static bool
is_format_supported (uint32_t format, uint64_t modifier)
{
  struct test_feedback_tranche *tranche;
  int i;
  struct format_modifier_pair pair;

  /* Loop through each tranche, looking for a matching entry in the
     targets table.  */
  for (tranche = feedback_tranches; tranche; tranche = tranche->next)
    {
      for (i = 0; i < tranche->n_indices; ++i)
	{
	  pair = modifier_table[i];

	  if (pair.format == format
	      && pair.modifier == modifier)
	    return true;
	}
    }

  return false;
}



static void
handle_params_created (void *data,
		       struct zwp_linux_buffer_params_v1 *params,
		       struct wl_buffer *buffer)
{
  struct test_params_data *params_data;

  params_data = data;
  params_data->complete = true;
  params_data->buffer = buffer;
}

static void
handle_params_failed (void *data,
		      struct zwp_linux_buffer_params_v1 *params)
{
  struct test_params_data *params_data;

  params_data = data;

  if (params_data->buffer)
    report_test_failure ("buffer set but failed sent!");

  params_data->complete = false;
  params_data->buffer = NULL;
}

static const struct zwp_linux_buffer_params_v1_listener params_listener =
  {
    handle_params_created,
    handle_params_failed,
  };



/* Create a 200x200 buffer in some 32 bpp format.  Fill it with three
   colors: red, green, and blue.  */

static struct wl_buffer *
create_rainbow_buffer (uint32_t format, uint64_t modifier,
		       uint32_t red_pixel, uint32_t green_pixel,
		       uint32_t blue_pixel)
{
  struct gbm_bo *buffer_object;
  void *map_data;
  char *buffer_data, *line;
  uint32_t stride;
  int i, fd;
  struct zwp_linux_buffer_params_v1 *params;
  struct test_params_data data;

  /* map_data must be NULL when it is first given to gbm_bo_map.  */
  map_data = NULL;

  if (!is_format_supported (format, modifier))
    report_test_failure ("the specified format %8"PRIx32" with modifier"
			 " 0x%16"PRIx64" is not supported",
			 format, modifier);

  if (modifier != DRM_FORMAT_MOD_INVALID)
    buffer_object
      = gbm_bo_create_with_modifiers2 (gbm_device, 500, 500, format,
				       &modifier, 1,
				       GBM_BO_USE_RENDERING);
  else
    buffer_object = gbm_bo_create (gbm_device, 500, 500, format,
				   GBM_BO_USE_RENDERING);

  if (!buffer_object)
    return NULL;

  buffer_data = gbm_bo_map (buffer_object, 0, 0, 500, 500,
			    GBM_BO_TRANSFER_WRITE, &stride,
			    &map_data);

  if (!buffer_data)
    goto error_1;

  line = malloc (stride);

  if (!line)
    goto error_2;

  /* Fill the line with the red pixel, and then fill the first 166
     rows with it.  buffer_data may not be suitably aligned, so use
     memcpy.  */
  for (i = 0; i < 500; ++i)
    memcpy (line + i * 4, &red_pixel, sizeof red_pixel);

  for (i = 0; i < 166; ++i)
    memcpy (buffer_data + stride * i, line, 4 * 500);

  /* Repeat with the green pixel.  */
  for (i = 0; i < 500; ++i)
    memcpy (line + i * 4, &green_pixel, sizeof green_pixel);

  for (i = 166; i < 332; ++i)
    memcpy (buffer_data + stride * i, line, 4 * 500);

  /* Finally with the blue pixel.  */
  for (i = 0; i < 500; ++i)
    memcpy (line + i * 4, &blue_pixel, sizeof blue_pixel);

  for (i = 332; i < 500; ++i)
    memcpy (buffer_data + stride * i, line, 4 * 500);

  free (line);

  /* Now, export the buffer.  */
  fd = gbm_bo_get_fd (buffer_object);

  if (fd < 1)
    goto error_2;

  params = zwp_linux_dmabuf_v1_create_params (linux_dmabuf_v1);

  if (!params)
    goto error_3;

  zwp_linux_buffer_params_v1_add (params, fd, 0,
				  gbm_bo_get_offset (buffer_object, 0),
				  gbm_bo_get_stride (buffer_object),
				  modifier >> 32,
				  modifier & 0xffffffff);
  zwp_linux_buffer_params_v1_create (params, 500, 500, format, 0);

  /* Now, wait for either success or failure.  */
  zwp_linux_buffer_params_v1_add_listener (params, &params_listener,
					   &data);
  data.complete = false;
  data.buffer = NULL;

  while (!data.complete)
    {
      if (wl_display_dispatch (display->display) == -1)
	die ("wl_display_dispatch");
    }

  if (!data.buffer)
    goto error_4;

  /* Otherwise, the buffer has been created.  Return it now.  */

  zwp_linux_buffer_params_v1_destroy (params);
  close (fd);
  gbm_bo_unmap (buffer_object, map_data);
  gbm_bo_destroy (buffer_object);

  return data.buffer;

 error_4:
  zwp_linux_buffer_params_v1_destroy (params);
 error_3:
  close (fd);
 error_2:
  gbm_bo_unmap (buffer_object, map_data);
 error_1:
  gbm_bo_destroy (buffer_object);
  return NULL;
}



static void
run_test (void)
{
  if (!make_test_surface (display, &wayland_surface,
			  &test_surface))
    report_test_failure ("failed to create test surface");

  /* Open the test surface.  */
  open_surface ();

  test_surface_add_listener (test_surface, &test_surface_listener,
			     NULL);
  test_single_step (ARGB8888_KIND);

  while (true)
    {
      if (wl_display_dispatch (display->display) == -1)
        die ("wl_display_dispatch");
    }
}

int
main (void)
{
  test_init ();
  display = open_test_display (test_interfaces,
			       ARRAYELTS (test_interfaces));

  if (!display)
    report_test_failure ("failed to open display");

  run_test ();
}
