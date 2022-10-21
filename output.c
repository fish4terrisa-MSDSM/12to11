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
#include <stdlib.h>
#include <string.h>

#include "compositor.h"

typedef struct _Mode Mode;
typedef struct _Output Output;
typedef struct _ScaleChangeCallback ScaleChangeCallback;

struct _Mode
{
  int flags;
  unsigned int width, height;
  unsigned long refresh;
};

struct _Output
{
  /* The output ID of this output.  */
  RROutput output;

  /* Physical height of this output.  */
  unsigned int mm_width, mm_height;

  /* List of display modes.  */
  XLList *modes;

  /* The global associated with this output.  */
  struct wl_global *global;

  /* A list of resources associated with this output.  */
  XLList *resources;

  /* The X and Y position of this output.  */
  int x, y;

  /* The width and height of this output.  */
  int width, height;

  /* The name of the output.  */
  char *name;

  /* The transform and subpixel layout of this output.  */
  uint32_t transform, subpixel;

  /* The scale of this output.  */
  int scale;
};

struct _ScaleChangeCallback
{
  /* The next and last callbacks in this list.  */
  ScaleChangeCallback *next, *last;

  /* Callback to run when the display scale changes.  */
  void (*scale_change) (void *, int);

  /* Data for the callback.  */
  void *data;
};

enum
  {
    ModesChanged    = 1,
    GeometryChanged = (1 << 2),
    /* N.B. that this isn't currently checked during comparisons,
       since the rest of the code only supports a single global
       scale.  */
    ScaleChanged    = (1 << 3),
  };

/* List of all outputs registered.  */
static XLList *all_outputs;

/* List of all scale factor change callbacks.  */
static ScaleChangeCallback scale_callbacks;

/* The scale factor currently applied on a global basis.  */
int global_scale_factor;

/* Function run upon any kind of XRandR notify event.  */
static void (*change_hook) (Time);

static Bool
ApplyEnvironment (const char *name, int *variable)
{
  char *env;

  env = getenv (name);

  if (!env || !atoi (env))
    return False;

  *variable = atoi (env);
  return True;
}

static void
FreeSingleOutputResource (void *data)
{
  struct wl_resource *resource;

  /* Set this to NULL so HandleResourceDestroy doesn't try to mutate
     the list inside XLListFree.  */

  resource = data;
  wl_resource_set_user_data (resource, NULL);
  wl_resource_destroy (resource);
}

static void
FreeOutput (Output *output)
{
  /* Free all resources.  */
  XLListFree (output->resources, FreeSingleOutputResource);

  /* Free all modes.  */
  XLListFree (output->modes, XLFree);

  /* Free the global if it exists.  */
  if (output->global)
    wl_global_destroy (output->global);

  /* Free the output name.  */
  XLFree (output->name);

  /* Finally, free the output itself.  */
  XLFree (output);
}

static void
FreeSingleOutput (void *data)
{
  FreeOutput (data);
}

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  Output *output;

  output = wl_resource_get_user_data (resource);

  /* If output still exists, remove this resource.  */

  if (output)
    output->resources = XLListRemove (output->resources,
				      resource);
}

static void
HandleRelease (struct wl_client *client,
	       struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_output_interface wl_output_impl =
  {
    .release = HandleRelease,
  };

static void
SendGeometry (Output *output, struct wl_resource *resource)
{
  wl_output_send_geometry (resource, output->x, output->y,
			   output->mm_width, output->mm_height,
			   output->subpixel,
			   ServerVendor (compositor.display),
			   output->name, output->transform);
}

static void
SendScale (Output *output, struct wl_resource *resource)
{
  wl_output_send_scale (resource, output->scale);
}

static void
SendMode (Mode *mode, struct wl_resource *resource)
{
  wl_output_send_mode (resource, mode->flags, mode->width,
		       mode->height, mode->refresh);
}

static Bool
FindOutputId (RROutput *outputs, int noutputs, RROutput id)
{
  int i;

  for (i = 0; i < noutputs; ++i)
    {
      if (outputs[i] == id)
	return True;
    }

  return False;
}

static void
HandleOutputBound (struct wl_client *client, Output *output,
		   struct wl_resource *resource)
{
  Surface *surface;

  /* When a new output is bound, see if a surface owned by the client
     is inside.  If it is, send the enter event to the surface.  */

  surface = all_surfaces.next;

  while (surface != &all_surfaces)
    {
      if (client == wl_resource_get_client (surface->resource)
	  && FindOutputId (surface->outputs, surface->n_outputs,
			   output->output))
	wl_surface_send_enter (surface->resource, resource);

      surface = surface->next;
    }
}

static void
HandleBind (struct wl_client *client, void *data,
	    uint32_t version, uint32_t id)
{
  struct wl_resource *resource;
  Output *output;
  XLList *tem;

  output = data;

  resource = wl_resource_create (client, &wl_output_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &wl_output_impl, data,
				  HandleResourceDestroy);
  output->resources = XLListPrepend (output->resources, resource);

  SendGeometry (output, resource);
  SendScale (output, resource);

  for (tem = output->modes; tem; tem = tem->next)
    SendMode (tem->data, resource);

  if (wl_resource_get_version (resource) >= 2)
    wl_output_send_done (resource);

  HandleOutputBound (client, output, resource);
}

static Output *
AddOutput (void)
{
  Output *output;

  output = XLCalloc (1, sizeof *output);

  return output;
}

static Mode *
AddMode (Output *output)
{
  Mode *mode;

  mode = XLCalloc (1, sizeof *mode);
  output->modes = XLListPrepend (output->modes, mode);

  return mode;
}

static double
GetModeRefresh (XRRModeInfo *info)
{
  double rate, vscan;

  vscan = info->vTotal;

  if (info->modeFlags & RR_DoubleScan)
    vscan *= 2;

  if (info->modeFlags & RR_Interlace)
    vscan /= 2;

  if (info->hTotal && vscan)
    rate = (info->dotClock
	    / (info->hTotal * vscan));
  else
    rate = 0;

  return rate;
}

static void
AppendRRMode (Output *output, RRMode id, XRRScreenResources *res,
	      XRRCrtcInfo *info, Bool preferred)
{
  Mode *mode;
  int i;

  for (i = 0; i < res->nmode; ++i)
    {
      if (res->modes[i].id == id)
	{
	  mode = AddMode (output);

	  mode->width = res->modes[i].width;
	  mode->height = res->modes[i].height;
	  mode->refresh
	    = GetModeRefresh (&res->modes[i]) * 1000;

	  if (info->mode == id)
	    mode->flags |= WL_OUTPUT_MODE_CURRENT;

	  if (preferred)
	    mode->flags |= WL_OUTPUT_MODE_PREFERRED;
	}
    }
}

static uint32_t
ComputeSubpixel (XRROutputInfo *info)
{
  switch (info->subpixel_order)
    {
    case SubPixelHorizontalRGB:
      return WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB;

    case SubPixelHorizontalBGR:
      return WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR;

    case SubPixelVerticalRGB:
      return WL_OUTPUT_SUBPIXEL_VERTICAL_RGB;

    case SubPixelVerticalBGR:
      return WL_OUTPUT_SUBPIXEL_VERTICAL_BGR;

    case SubPixelNone:
      return WL_OUTPUT_SUBPIXEL_NONE;

    default:
      return WL_OUTPUT_SUBPIXEL_UNKNOWN;
    }
}

static uint32_t
ComputeTransform (XRRCrtcInfo *info)
{
  /* TODO: support surface transforms.  */
  return WL_OUTPUT_TRANSFORM_NORMAL;
}

static XLList *
BuildOutputTree (void)
{
  XRRScreenResources *resources;
  XRROutputInfo *info;
  XRRCrtcInfo *crtc;
  int i, j;
  Output *output;
  XLList *all_outputs;

  all_outputs = NULL;

  resources = XRRGetScreenResources (compositor.display,
				     DefaultRootWindow (compositor.display));

  if (!resources || !resources->noutput)
    {
      if (resources)
	XRRFreeScreenResources (resources);

      return NULL;
    }

  for (i = 0; i < resources->noutput; ++i)
    {
      /* Catch errors in case the output disappears.  */
      CatchXErrors ();
      info = XRRGetOutputInfo (compositor.display, resources,
			       resources->outputs[i]);
      UncatchXErrors (NULL);

      if (!info)
	continue;

      if (info->connection != RR_Disconnected
	  && info->crtc != None)
	{
	  /* Catch errors in case the CRT controller disappears.  */
	  CatchXErrors ();
	  crtc = XRRGetCrtcInfo (compositor.display, resources,
				 info->crtc);
	  UncatchXErrors (NULL);

	  if (!crtc)
	    {
	      XRRFreeOutputInfo (info);
	      continue;
	    }

	  output = AddOutput ();
	  output->output = resources->outputs[i];
	  output->scale = global_scale_factor;

	  ApplyEnvironment ("OUTPUT_SCALE", &output->scale);

	  output->mm_width = info->mm_width;
	  output->mm_height = info->mm_height;
	  output->x = crtc->x;
	  output->y = crtc->y;
	  output->width = crtc->width;
	  output->height = crtc->height;
	  output->transform = ComputeTransform (crtc);
	  output->subpixel = ComputeSubpixel (info);

	  all_outputs = XLListPrepend (all_outputs, output);

	  output->name = XLStrdup (info->name);

	  for (j = info->nmode - 1; j >= 0; --j)
	    {
	      if (info->npreferred != j)
		AppendRRMode (output, info->modes[j],
			      resources, crtc, False);
	    }

	  AppendRRMode (output, info->modes[info->npreferred],
			resources, crtc, True);

	  XRRFreeCrtcInfo (crtc);
	}

      XRRFreeOutputInfo (info);
    }

  XRRFreeScreenResources (resources);
  return all_outputs;
}

static Bool
AreModesIdentical (XLList *first, XLList *second)
{
  XLList *tem1, *tem2;

  /* This explicitly also checks the order in which the modes
     appear.  */
  for (tem1 = first, tem2 = second; tem1 && tem2;
       tem1 = tem1->next, tem2 = tem2->next)
    {
      if (memcmp (tem1->data, tem2->data, sizeof (Mode)))
	return False;
    }

  /* If either tem1 or tem2 are not NULL, then one list ended early,
     so the lists are not identical.  */

  return !tem1 && !tem2;
}

static void
CompareOutputs (Output *output, Output *other, int *flags)
{
  int difference;

  difference = 0;

  if (!AreModesIdentical (output->modes, other->modes))
    difference |= ModesChanged;

  if (output->mm_width != other->mm_width
      || output->mm_height != other->mm_height
      || output->x != other->x
      || output->y != other->y
      || output->subpixel != other->subpixel
      || output->transform != other->transform
      || !strcmp (output->name, other->name))
    difference |= GeometryChanged;

  *flags = difference;
}

static Output *
FindOutputById (RROutput output)
{
  XLList *tem;
  Output *item;

  for (tem = all_outputs; tem; tem = tem->next)
    {
      item = tem->data;

      if (item->output == output)
	return item;
    }

  return NULL;
}

static void
MakeGlobal (Output *output)
{
  XLAssert (!output->global);

  output->global = wl_global_create (compositor.wl_display,
				     &wl_output_interface, 2,
				     output, HandleBind);

  if (!output->global)
    {
      fprintf (stderr, "Failed to allocate global output\n");
      exit (1);
    }
}

static void
MakeGlobalsForOutputTree (XLList *list)
{
  for (; list; list = list->next)
    MakeGlobal (list->data);
}

static void
SendUpdates (Output *output, int difference)
{
  XLList *tem, *tem1;

  if (!difference)
    return;

  for (tem = output->resources; tem; tem = tem->next)
    {
      if (difference & GeometryChanged)
	SendGeometry (output, tem->data);

      if (difference & ModesChanged)
	{
	  for (tem1 = output->modes; tem1; tem1 = tem->next)
	    SendMode (tem1->data, tem->data);
	}

      if (difference & ScaleChanged)
	SendScale (output, tem->data);

      if (wl_resource_get_version (tem->data) >= 2)
	wl_output_send_done (tem->data);
    }
}

static void
UpdateResourceUserData (Output *output)
{
  XLList *tem;

  for (tem = output->resources; tem; tem = tem->next)
    wl_resource_set_user_data (tem->data, output);
}

static void
NoticeOutputsMaybeChanged (void)
{
  XLList *new_list, *tem;
  Output *new, *current;
  int change_flags;
  Bool any_change;
  Surface *surface;

  new_list = BuildOutputTree ();
  any_change = False;

  /* Since it's hard and race-prone to figure out what changed from
     the RandR notification events themselves, simply compare the
     outputs before and after.  */

  for (tem = new_list; tem; tem = tem->next)
    {
      new = tem->data;
      current = FindOutputById (new->output);

      if (!current)
	{
	  /* This is an entirely new output.  Create a new global.  */
	  MakeGlobal (new);

	  /* Mark the list of outputs as having changed.  */
	  any_change = True;
	  continue;
	}

      /* Otherwise, this output already exists.  Tell clients about
	 any changes that happened, and transfer the existing global
	 and resources to the new output.  */
      new->global = current->global;
      new->resources = current->resources;

      /* Update the user data of the globals and resources.  */
      wl_global_set_user_data (new->global, new);
      UpdateResourceUserData (new);

      /* Clear the current output's globals.  Otherwise, they will get
	 freed later on.  */
      current->global = NULL;
      current->resources = NULL;

      /* Compare the two outputs to determine what updates must be
	 made.  */
      CompareOutputs (new, current, &change_flags);

      /* Send updates.  */
      SendUpdates (new, change_flags);

      /* Mark the list of outputs as having changed if some attribute
	 of the output did change.  */
      if (change_flags)
	any_change = True;
    }

  /* Free the current output list and make the new list current.  */
  XLListFree (all_outputs, FreeSingleOutput);
  all_outputs = new_list;

  if (any_change)
    {
      /* If something changed, clear each surface's output region.  We
         rely on the window manager to send a ConfigureNotify event
         and move the windows around appropriately.  */

      surface = all_surfaces.next;
      for (; surface != &all_surfaces; surface = surface->next)
	pixman_region32_clear (&surface->output_region);
    }
}

static double
GetCurrentRefresh (Output *output)
{
  Mode *mode;
  XLList *list;

  for (list = output->modes; list; list = list->next)
    {
      mode = list->data;

      if (mode->flags & WL_OUTPUT_MODE_CURRENT)
	return (double) mode->refresh / 1000;
    }

  /* No current mode! */
  return 0.0;
}

static Output *
GetOutputAt (int x, int y)
{
  Output *output;
  XLList *tem;
  int x1, y1, x2, y2;

  for (tem = all_outputs; tem; tem = tem->next)
    {
      output = tem->data;

      x1 = output->x;
      y1 = output->y;
      x2 = output->x + output->width - 1;
      y2 = output->y + output->height - 1;

      if (x >= x1 && x <= x2
	  && y >= y1 && y <= y2)
	return output;
    }

  return NULL;
}

static Bool
AnyIntersectionBetween (int x, int y, int x1, int y1,
			int x2, int y2, int x3, int y3)
{
  return (x < x3 && x1 > x2 && y < y3 && y1 > y2);
}

static int
ComputeSurfaceOutputs (int x, int y, int width, int height,
		       int noutputs, Output **outputs)
{
  XLList *tem;
  Output *output;
  int i;

  i = 0;

  for (tem = all_outputs; tem; tem = tem->next)
    {
      output = tem->data;

      /* Test all four corners in case some extend outside the screen
	 or output.  */
      if (AnyIntersectionBetween (x, y, x + width - 1, y + height - 1,
				  output->x, output->y,
				  output->x + output->width - 1,
				  output->y + output->height - 1)
	  && i + 1 < noutputs)
	outputs[i++] = output;
    }

  return i;
}

static Bool
FindOutput (Output **outputs, int noutputs, RROutput id)
{
  int i;

  for (i = 0; i < noutputs; ++i)
    {
      if (outputs[i]->output == id)
	return True;
    }

  return False;
}

static struct wl_resource *
FindOutputResource (Output *output, Surface *client_surface)
{
  struct wl_client *client;
  XLList *tem;

  client = wl_resource_get_client (client_surface->resource);

  for (tem = output->resources; tem; tem = tem->next)
    {
      if (wl_resource_get_client (tem->data) == client)
	return tem->data;
    }

  return NULL;
}

static Bool
BoxContains (pixman_box32_t *box, int x, int y, int width,
	     int height)
{
  return (x >= box->x1 && y >= box->y1
	  && x + width <= box->x2
	  && y + height <= box->y2);
}

void
XLUpdateSurfaceOutputs (Surface *surface, int x, int y, int width,
			int height)
{
  Output *outputs[256], *output;
  int n, i;
  struct wl_resource *resource;

  if (width == -1)
    width = ViewWidth (surface->view);

  if (height == -1)
    height = ViewHeight (surface->view);

  if (BoxContains (pixman_region32_extents (&surface->output_region),
		   x, y, width, height))
    /* The surface didn't move past the output region.  */
    return;

  /* TODO: store the number of outputs somewhere instead of hardcoding
     256.  */
  n = ComputeSurfaceOutputs (x, y, width, height,
			     ArrayElements (outputs),
			     outputs);

  /* First, find and leave all the outputs that the surface is no
     longer inside.  */
  for (i = 0; i < surface->n_outputs; ++i)
    {
      if (!FindOutput (outputs, n, surface->outputs[i]))
	{
	  output = FindOutputById (surface->outputs[i]);

	  if (!output)
	    continue;

	  resource = FindOutputResource (output, surface);

	  if (resource)
	    wl_surface_send_leave (surface->resource, resource);
	}
    }

  /* Then, send enter events for all the outputs that the surface has
     not previously entered.  Also calculate a rectangle defining an
     area in which output recomputation need not take place.  */

  if (!n)
    pixman_region32_init (&surface->output_region);
  else
    pixman_region32_clear (&surface->output_region);

  for (i = 0; i < n; ++i)
    {
      if (!FindOutputId (surface->outputs, surface->n_outputs,
			 outputs[i]->output))
	{
	  resource = FindOutputResource (outputs[i], surface);

	  if (resource)
	    wl_surface_send_enter (surface->resource, resource);
	}

      if (!i)
	pixman_region32_init_rect (&surface->output_region,
				   outputs[i]->x, outputs[i]->y,
				   outputs[i]->width,
				   outputs[i]->height);
      else
	pixman_region32_intersect_rect (&surface->output_region,
					&surface->output_region,
					outputs[i]->x,
					outputs[i]->y,
					outputs[i]->width,
					outputs[i]->height);
    }

  /* Copy the list of outputs to the surface as well.  */
  surface->n_outputs = n;

  if (surface->n_outputs)
    surface->outputs = XLRealloc (surface->outputs,
				  sizeof *surface->outputs
				  * surface->n_outputs);
  else
    {
      XLFree (surface->outputs);
      surface->outputs = NULL;
    }

  for (i = 0; i < n; ++i)
    surface->outputs[i] = outputs[i]->output;

  /* At the same time, also update outputs for attached
     subsurfaces.  */
  XLUpdateOutputsForChildren (surface, x, y);

  /* And make a record of the coordinates that were used to compute
     outputs for this surface.  */
  surface->output_x = x;
  surface->output_y = y;
}

Bool
XLGetOutputRectAt (int x, int y, int *x_out, int *y_out,
		   int *width_out, int *height_out)
{
  Output *output;

  output = GetOutputAt (x, y);

  /* There is no output at this position.  */
  if (!output)
    return False;

  if (x_out)
    *x_out = output->x;

  if (y_out)
    *y_out = output->y;

  if (width_out)
    *width_out = output->width;

  if (height_out)
    *height_out = output->height;

  return True;
}

Bool
XLHandleOneXEventForOutputs (XEvent *event)
{
  XRRNotifyEvent *notify;
  XRROutputPropertyNotifyEvent *property;
  XRRResourceChangeNotifyEvent *resource;
  Time time;

  notify = (XRRNotifyEvent *) event;

  if (event->type == compositor.rr_event_base + RRNotify)
    {
      NoticeOutputsMaybeChanged ();

      if (change_hook)
	{
	  time = CurrentTime;

	  /* See if a timestamp of some sort can be extracted.  */
	  switch (notify->subtype)
	    {
	    case RRNotify_OutputProperty:
	      property = (XRROutputPropertyNotifyEvent *) notify;
	      time = property->timestamp;
	      break;

	    case RRNotify_ResourceChange:
	      resource = (XRRResourceChangeNotifyEvent *) notify;
	      time = resource->timestamp;
	      break;
	    }

	  change_hook (time);
	}

      return True;
    }

  if (event->type == compositor.rr_event_base + RRScreenChangeNotify)
    {
      XRRUpdateConfiguration (event);
      return True;
    }

  return False;
}

void
XLOutputGetMinRefresh (struct timespec *timespec)
{
  Output *output;
  XLList *list;
  double min_refresh, t, between;

  timespec->tv_sec = 0;
  timespec->tv_nsec = 16000000 * 2;
  min_refresh = 0.0;

  /* Find the output with the lowest current refresh rate, and use
     that.  */
  for (list = all_outputs; list; list = list->next)
    {
      output = list->data;
      t = GetCurrentRefresh (output);

      if (t < min_refresh || min_refresh == 0.0)
	min_refresh = t;
    }

  /* min_refresh can be 0.0 if there are no valid modes.  In that
     case, fall back to 30 fps.  */
  if (min_refresh == 0.0)
    {
      fprintf (stderr, "Warning: defaulting to 30fps refresh rate\n");
      return;
    }

  /* This is vblank + the time it takes to scan a frame, in
     seconds.  */
  between = 1.0 / min_refresh;

  /* Now populate the timespec with it.  */
  timespec->tv_nsec = MIN (1000000000, between * 1000000000);
  timespec->tv_sec = (between - timespec->tv_nsec) / 1000000000;
}

static void
RunScaleChangeCallbacks (void)
{
  ScaleChangeCallback *callback;

  callback = scale_callbacks.next;

  while (callback != &scale_callbacks)
    {
      callback->scale_change (callback->data,
			      global_scale_factor);
      callback = callback->next;
    }
}

static void
HandleScaleChange (int scale)
{
  Output *output;
  XLList *list;

  if (scale == global_scale_factor)
    return;

  /* Update the scale factor.  */
  global_scale_factor = scale;

  /* Update the scale for each output and send changes.  */
  for (list = all_outputs; list; list = list->next)
    {
      output = list->data;
      output->scale = scale;

      SendUpdates (output, ScaleChanged);
    }

  /* Now, run every scale change function.  */
  RunScaleChangeCallbacks ();
}

void *
XLAddScaleChangeCallback (void *data, void (*func) (void *, int))
{
  ScaleChangeCallback *callback;

  callback = XLMalloc (sizeof *callback);
  callback->next = scale_callbacks.next;
  callback->last = &scale_callbacks;
  scale_callbacks.next->last = callback;
  scale_callbacks.next = callback;

  callback->scale_change = func;
  callback->data = data;

  return callback;
}

void
XLRemoveScaleChangeCallback (void *key)
{
  ScaleChangeCallback *callback;

  callback = key;
  callback->last->next = callback->next;
  callback->next->last = callback->last;

  XLFree (callback);
}

void
XLClearOutputs (Surface *surface)
{
  Output *output;
  int i;
  struct wl_resource *resource;

  for (i = 0; i < surface->n_outputs; ++i)
    {
      output = FindOutputById (surface->outputs[i]);

      if (!output)
	continue;

      resource = FindOutputResource (output, surface);

      if (!resource)
	continue;

      wl_surface_send_leave (surface->resource, resource);
    }

  XLFree (surface->outputs);
  surface->outputs = NULL;
  surface->n_outputs = 0;
}

void
XLOutputSetChangeFunction (void (*change_func) (Time))
{
  change_hook = change_func;
}

void
XLInitRROutputs (void)
{
  Bool extension;

  extension = XRRQueryExtension (compositor.display,
				 &compositor.rr_event_base,
				 &compositor.rr_error_base);

  if (!extension)
    {
      fprintf (stderr, "Display '%s' does not support the RandR extension\n",
	       DisplayString (compositor.display));
      abort ();
    }

  XRRQueryVersion (compositor.display,
		   &compositor.rr_major,
		   &compositor.rr_minor);

  if (compositor.rr_major < 1
      || (compositor.rr_major == 1
	  && compositor.rr_minor < 3))
    {
      fprintf (stderr, "Display '%s' does not support a"
	       " sufficiently new version of the RandR extension\n",
	       DisplayString (compositor.display));
      abort ();
    }

  /* Set the initial scale.  */
  global_scale_factor = 1;

  /* Start listening to changes to the scale factor, unless a scale
     factor was specified for debugging.  */
  if (!ApplyEnvironment ("GLOBAL_SCALE", &global_scale_factor))
    XLListenToIntegerSetting ("Gdk/WindowScalingFactor",
			      HandleScaleChange);

  /* Initialize the scale change callback list sentinel node.  */
  scale_callbacks.next = &scale_callbacks;
  scale_callbacks.last = &scale_callbacks;

  if (compositor.rr_major > 1
      && (compositor.rr_major == 1
	  && compositor.rr_minor >= 4))
    XRRSelectInput (compositor.display,
		    DefaultRootWindow (compositor.display),
		    (RRCrtcChangeNotifyMask
		     | RROutputChangeNotifyMask
		     | RROutputPropertyNotifyMask
		     | RRResourceChangeNotifyMask));
  else
    XRRSelectInput (compositor.display,
		    DefaultRootWindow (compositor.display),
		    (RRCrtcChangeNotifyMask
		     | RROutputChangeNotifyMask
		     | RROutputPropertyNotifyMask));

  all_outputs = BuildOutputTree ();
  MakeGlobalsForOutputTree (all_outputs);
}
