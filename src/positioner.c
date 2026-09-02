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

#include <string.h>
#include <stdio.h>

#include "xdg-shell.h"

#include "compositor.h"

#ifdef DEBUG_POSITIONER
#define DebugPrint(format, args...)				\
  fprintf (stderr, "%s: " format "\n", __FUNCTION__, ## args)

static const char *anchor_gravity_names[] =
  {
    "AnchorGravityNone",
    "AnchorGravityTop",
    "AnchorGravityBottom",
    "AnchorGravityLeft",
    "AnchorGravityRight",
    "AnchorGravityTopLeft",
    "AnchorGravityBottomLeft",
    "AnchorGravityTopRight",
    "AnchorGravityBottomRight",
  };

#else
#define DebugPrint(fmt, ...) ((void) 0)
#endif

typedef enum _AnchorGravity Anchor;
typedef enum _AnchorGravity Gravity;

enum _AnchorGravity
  {
    AnchorGravityNone,
    AnchorGravityTop,
    AnchorGravityBottom,
    AnchorGravityLeft,
    AnchorGravityRight,
    AnchorGravityTopLeft,
    AnchorGravityBottomLeft,
    AnchorGravityTopRight,
    AnchorGravityBottomRight,
  };

/* Surface used to handle scaling during constraint adjustment
   calculation.  */
static double scale_adjustment_factor;

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
SetSize (struct wl_client *client, struct wl_resource *resource,
	 int32_t width, int32_t height)
{
  Positioner *positioner;

  if (width < 1 || height < 1)
    {
      wl_resource_post_error (resource, XDG_SURFACE_ERROR_INVALID_SIZE,
			      "invalid size %d %d", width, height);
      return;
    }

  positioner = wl_resource_get_user_data (resource);
  positioner->width = width;
  positioner->height = height;
}

static void
SetAnchorRect (struct wl_client *client, struct wl_resource *resource,
	       int32_t x, int32_t y, int32_t width, int32_t height)
{
  Positioner *positioner;

  if (width < 1 || height < 1)
    {
      wl_resource_post_error (resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
			      "invalid size specified (%d %d)", width, height);
      return;
    }

  positioner = wl_resource_get_user_data (resource);
  positioner->anchor_x = x;
  positioner->anchor_y = y;
  positioner->anchor_width = width;
  positioner->anchor_height = height;
}

static void
SetAnchor (struct wl_client *client, struct wl_resource *resource,
	   uint32_t anchor)
{
  Positioner *positioner;

  if (anchor > XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT)
    {
      wl_resource_post_error (resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
			      "invalid anchor specified (%"PRIu32")", anchor);
      return;
    }

  positioner = wl_resource_get_user_data (resource);
  positioner->anchor = anchor;
}

static void
SetGravity (struct wl_client *client, struct wl_resource *resource,
	    uint32_t gravity)
{
  Positioner *positioner;

  if (gravity > XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT)
    {
      wl_resource_post_error (resource, XDG_POSITIONER_ERROR_INVALID_INPUT,
			      "invalid gravity specified (%"PRIu32")", gravity);
      return;
    }

  positioner = wl_resource_get_user_data (resource);
  positioner->gravity = gravity;
}

static void
SetConstraintAdjustment (struct wl_client *client, struct wl_resource *resource,
			 uint32_t constraint_adjustment)
{
  Positioner *positioner;

  positioner = wl_resource_get_user_data (resource);
  positioner->constraint_adjustment = constraint_adjustment;
}

static void
SetOffset (struct wl_client *client, struct wl_resource *resource,
	   int32_t x, int32_t y)
{
  Positioner *positioner;

  positioner = wl_resource_get_user_data (resource);
  positioner->offset_x = x;
  positioner->offset_y = y;
}

static void
SetReactive (struct wl_client *client, struct wl_resource *resource)
{
  Positioner *positioner;

  positioner = wl_resource_get_user_data (resource);
  positioner->reactive = True;
}

static void
SetParentSize (struct wl_client *client, struct wl_resource *resource,
	       int width, int height)
{
  Positioner *positioner;

  positioner = wl_resource_get_user_data (resource);
  positioner->parent_width = width;
  positioner->parent_height = height;
}

static void
SetParentConfigure (struct wl_client *client, struct wl_resource *resource,
		    uint32_t configure)
{
  /* Unused.  */
  return;
}

static const struct xdg_positioner_interface xdg_positioner_impl =
  {
    .destroy = Destroy,
    .set_size = SetSize,
    .set_anchor_rect = SetAnchorRect,
    .set_anchor = SetAnchor,
    .set_gravity = SetGravity,
    .set_constraint_adjustment = SetConstraintAdjustment,
    .set_offset = SetOffset,
    .set_reactive = SetReactive,
    .set_parent_size = SetParentSize,
    .set_parent_configure = SetParentConfigure,
  };

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  Positioner *positioner;

  positioner = wl_resource_get_user_data (resource);
  XLFree (positioner);
}

static void
CalculatePosition (Positioner *positioner, int *x_out, int *y_out)
{
  int x, y, anchor_x, anchor_y, anchor_width, anchor_height;

  DebugPrint ("anchor: %s, gravity: %s",
	      anchor_gravity_names[positioner->anchor],
	      anchor_gravity_names[positioner->gravity]);

  /* This function calculates an offset from the origin of the parent
     window geometry (in the X coordinate system where the value of
     the Y axis increases as the position grows downwards.)  The
     offset is derived by first computing an anchor point, and then
     placing the surface at that anchor in accordance with the
     gravity.  */

  x = positioner->offset_x;
  y = positioner->offset_y;

  anchor_x = positioner->anchor_x;
  anchor_y = positioner->anchor_y;
  anchor_width = positioner->anchor_width;
  anchor_height = positioner->anchor_height;

  /* First, compute the point at which the surface will be
     anchored.  */

  switch (positioner->anchor)
    {
    case AnchorGravityTop:
    case AnchorGravityTopLeft:
    case AnchorGravityTopRight:
      y += anchor_y;
      break;

    case AnchorGravityBottom:
    case AnchorGravityBottomLeft:
    case AnchorGravityBottomRight:
      y += anchor_y + anchor_height;
      break;

    default:
      y += anchor_y + anchor_height / 2;
    }

  switch (positioner->anchor)
    {
    case AnchorGravityLeft:
    case AnchorGravityTopLeft:
    case AnchorGravityBottomLeft:
      x += anchor_x;
      break;

    case AnchorGravityRight:
    case AnchorGravityTopRight:
    case AnchorGravityBottomRight:
      x += anchor_x + anchor_width;
      break;

    default:
      x += anchor_x + anchor_width / 2;
    }

  /* Next, compute where the surface should be.  positioner->gravity
     specifies the corner opposite to the one that should be touching
     the anchor.  For example:

         (anchor)
            + ------>
            |
            |
	    v

     would be the directions in which the surface rectangle would
     extend if the gravity were to be AnchorGravityBottomRight.  And:

           ^
           |
           |
     <-----+ (anchor)

    would be the those directions were it AnchorGravityTopLeft.  */

  switch (positioner->gravity)
    {
    case AnchorGravityTop:
    case AnchorGravityTopLeft:
    case AnchorGravityTopRight:
      y -= positioner->height;
      break;

    case AnchorGravityBottom:
    case AnchorGravityBottomLeft:
    case AnchorGravityBottomRight:
      y = y;
      break;

    default:
      y -= positioner->height / 2;
    }

  switch (positioner->gravity)
    {
    case AnchorGravityLeft:
    case AnchorGravityTopLeft:
    case AnchorGravityBottomLeft:
      x -= positioner->width;
      break;

    case AnchorGravityRight:
    case AnchorGravityTopRight:
    case AnchorGravityBottomRight:
      x = x;
      break;

    default:
      x -= positioner->width / 2;
    }

  if (x_out)
    *x_out = x;

  if (y_out)
    *y_out = y;
}

static int
TrySlideX (Positioner *positioner, int x, int width, int cx, int cwidth)
{
  int cx1, x1;
  int new_x;

  cx1 = cx + cwidth - 1;
  x1 = x + width - 1;

  DebugPrint ("trying to slide X %d (width %d) according to"
	      " constraint X %d and constraint width %d", x,
	      width, cx, cwidth);

  /* See if the rect is unconstrained on the X axis.  */

  if (x >= cx && x1 <= cx1)
    return x;

  new_x = x;

  /* Which of these conditions to use first depends on the gravity.
     If the gravity is leftwards, then we try to first keep new_x less
     than cx1.  Otherwise, we first try to keep new_x bigger than
     cx.  */

  switch (positioner->gravity)
    {
    case AnchorGravityLeft:
    case AnchorGravityTopLeft:
    case AnchorGravityBottomLeft:
      if (x < cx)
	/* If x is less than cx, move it to cx.  */
	new_x = cx;
      else if (x1 > cx1)
	/* If x1 extends past cx1, move it back.  */
	new_x = x - (x1 - cx1);
      break;

    case AnchorGravityRight:
    case AnchorGravityTopRight:
    case AnchorGravityBottomRight:
      /* There is no X axis gravity.  Choose some arbitrary
	 default.  */
    default:
      if (x1 > cx1)
	/* If x1 extends past cx1, move it back.  */
	new_x = x - (x1 - cx1);
      else if (x < cx)
	/* If x is less than cx, move it to cx.  */
	new_x = cx;
      break;
    }

  DebugPrint ("new X: %d", new_x);

  return new_x;
}

static int
TrySlideY (Positioner *positioner, int y, int height, int cy, int cheight)
{
  int cy1, y1;
  int new_y;

  cy1 = cy + cheight - 1;
  y1 = y + height - 1;

  /* See if the rect is unconstrained on the Y axis.  */

  if (y >= cy && y1 <= cy1)
    return y;

  new_y = y;

  /* Which of these conditions to use first depends on the gravity.
     If the gravity is topwards, then we try to first keep new_y less
     than cy1.  Otherwise, we first try to keep new_y bigger than
     cy.  */

  switch (positioner->gravity)
    {
    case AnchorGravityTop:
    case AnchorGravityTopLeft:
    case AnchorGravityTopRight:
      if (y < cy)
	/* If y is less than cy, move it to cy.  */
	new_y = cy;
      else if (y1 > cy1)
	/* If y1 eytends past cy1, move it back.  */
	new_y = y - (y1 - cy1);
      break;

    case AnchorGravityBottom:
    case AnchorGravityBottomLeft:
    case AnchorGravityBottomRight:
      /* When there is no Y axis gravity, choose some arbitrary
	 default.  */
    default:
      if (y1 > cy1)
	/* If y1 eytends past cy1, move it back.  */
	new_y = y - (y1 - cy1);
      else if (y < cy)
	/* If y is less than cy, move it to cy.  */
	new_y = cy;
      break;
    }

  return new_y;
}

static int
TryFlipX (Positioner *positioner, int x, int width, int cx, int cwidth,
	  int offset)
{
  int cx1, x1;
  int new_x;
  Positioner new;

  cx1 = cx + cwidth - 1;
  x1 = x + width - 1;

  /* If the rect is unconstrained, don't flip anything.  */

  if (x >= cx && x1 <= cx1)
    return x;

  DebugPrint ("x %d width %d found to be constrained by "
	      "constraint x %d constraint width %d", x, width,
	      cx, cwidth);

  /* Otherwise, create a copy of the positioner, but with the X
     gravity and X anchor flipped.  */
  new = *positioner;

  switch (positioner->gravity)
    {
    case AnchorGravityLeft:
      new.gravity = AnchorGravityRight;
      break;

    case AnchorGravityTopLeft:
      new.gravity = AnchorGravityTopRight;
      break;

    case AnchorGravityBottomLeft:
      new.gravity = AnchorGravityBottomRight;
      break;

    case AnchorGravityRight:
      new.gravity = AnchorGravityLeft;
      break;

    case AnchorGravityTopRight:
      new.gravity = AnchorGravityTopLeft;
      break;

    case AnchorGravityBottomRight:
      new.gravity = AnchorGravityBottomLeft;
      break;
    }

  switch (positioner->anchor)
    {
    case AnchorGravityLeft:
      new.anchor = AnchorGravityRight;
      break;

    case AnchorGravityTopLeft:
      new.anchor = AnchorGravityTopRight;
      break;

    case AnchorGravityBottomLeft:
      new.anchor = AnchorGravityBottomRight;
      break;

    case AnchorGravityRight:
      new.anchor = AnchorGravityLeft;
      break;

    case AnchorGravityTopRight:
      new.anchor = AnchorGravityTopLeft;
      break;

    case AnchorGravityBottomRight:
      new.anchor = AnchorGravityBottomLeft;
      break;
    }

  /* If neither the gravity nor the anchor changed, punt, since
     flipping won't help.  */

  if (positioner->gravity == new.gravity
      && positioner->anchor == new.anchor)
    return x;

  DebugPrint ("new anchor: %s, anchor point: %d, %d; gravity: %s",
	      anchor_gravity_names[new.anchor],
	      positioner->anchor_x, positioner->anchor_y,
	      anchor_gravity_names[new.gravity]);

  /* Otherwise, compute a new position using the new positioner.  */
  CalculatePosition (&new, &new_x, NULL);

  /* Scale that position.  */
  new_x *= scale_adjustment_factor;

  DebugPrint ("new x position is %d", new_x + offset);

  /* If new_x is still constrained, use the previous position.  */
  if (new_x + offset < cx
      || new_x + offset + width - 1 > cx1)
    {
      DebugPrint ("position (%d) is still constrained",
		  new_x + offset);
      return x;
    }

  /* Return the new X.  */
  return new_x + offset;
}

static int
TryFlipY (Positioner *positioner, int y, int height, int cy, int cheight,
	  int offset)
{
  int cy1, y1;
  int new_y;
  Positioner new;

  cy1 = cy + cheight - 1;
  y1 = y + height - 1;

  /* If the rect is unconstrained, don't flip anything.  */

  if (y >= cy && y1 <= cy1)
    return y;

  /* Otherwise, create a copy of the positioner, but with the Y
     gravity and Y anchor flipped.  */
  new = *positioner;

  switch (positioner->gravity)
    {
    case AnchorGravityTop:
      new.gravity = AnchorGravityBottom;
      break;

    case AnchorGravityTopLeft:
      new.gravity = AnchorGravityBottomLeft;
      break;

    case AnchorGravityTopRight:
      new.gravity = AnchorGravityBottomRight;
      break;

    case AnchorGravityBottom:
      new.gravity = AnchorGravityTop;
      break;

    case AnchorGravityBottomLeft:
      new.gravity = AnchorGravityTopLeft;
      break;

    case AnchorGravityBottomRight:
      new.gravity = AnchorGravityTopRight;
      break;
    }

  switch (positioner->anchor)
    {
    case AnchorGravityTop:
      new.anchor = AnchorGravityBottom;
      break;

    case AnchorGravityTopLeft:
      new.anchor = AnchorGravityBottomLeft;
      break;

    case AnchorGravityTopRight:
      new.anchor = AnchorGravityBottomRight;
      break;

    case AnchorGravityBottom:
      new.anchor = AnchorGravityTop;
      break;

    case AnchorGravityBottomLeft:
      new.anchor = AnchorGravityTopLeft;
      break;

    case AnchorGravityBottomRight:
      new.anchor = AnchorGravityTopRight;
      break;
    }

  /* If neither the gravity nor the anchor changed, punt, since
     flipping won't help.  */

  if (positioner->gravity == new.gravity
      && positioner->anchor == new.anchor)
    return y;

  /* Otherwise, compute a new position using the new positioner.  */
  CalculatePosition (&new, NULL, &new_y);

  /* Scale that position.  */
  new_y *= scale_adjustment_factor;

  /* If new_y is still constrained, use the previous position.  */
  if (new_y + offset < cy
      || new_y + offset + height - 1 > cy1)
    return y;

  /* Return the new Y.  */
  return new_y + offset;
}

static void
TryResizeX (int x, int width, int cx, int cwidth, int offset,
	    int *new_x, int *new_width)
{
  int x1, cx1, result_width, result_x;

  x1 = x + width - 1;
  cx1 = cx + cwidth - 1;

  if (x >= cx && x1 <= cx1)
    /* The popup is not constrained on the X axis.  */
    return;

  /* Otherwise, resize the popup to fit inside cx, cx1.
     If new_width ends up less than 1, punt.  */

  result_x = MAX (cx, x) - offset;
  result_width = MIN (cx1, x1) - (*new_x + offset) + 1;

  if (result_width <= 0)
    return;

  *new_x = result_x;
  *new_width = result_width;
}

static void
TryResizeY (int y, int height, int cy, int cheight, int offset,
	    int *new_y, int *new_height)
{
  int y1, cy1, result_height, result_y;

  y1 = y + height - 1;
  cy1 = cy + cheight - 1;

  if (y >= cy && y1 <= cy1)
    /* The popup is not constrained on the Y axis.  */
    return;

  /* Otherwise, resize the popup to fit inside cy, cy1.
     If new_height ends up less than 1, punt.  */

  result_y = MAX (cy, y) - offset;
  result_height = MIN (cy1, y1) - (*new_y + offset) + 1;

  if (result_height <= 0)
    return;

  *new_y = result_y;
  *new_height = result_height;
}

static void
GetAdjustmentOffset (Role *parent, int *off_x, int *off_y)
{
  int root_x, root_y, parent_gx, parent_gy;

  XLXdgRoleGetCurrentGeometry (parent, &parent_gx,
			       &parent_gy, NULL, NULL);
  XLXdgRoleCurrentRootPosition (parent, &root_x, &root_y);

  /* Convert the gx and gy to the window coordinate system.  */
  TruncateSurfaceToWindow (parent->surface, parent_gx, parent_gy,
			   &parent_gx, &parent_gy);

  *off_x = root_x + parent_gx;
  *off_y = root_y + parent_gy;
}

static void
ApplyConstraintAdjustment (Positioner *positioner, Role *parent, int x,
			   int y, int *x_out, int *y_out, int *width_out,
			   int *height_out)
{
  int width, height, cx, cy, cwidth, cheight, off_x, off_y;

  width = positioner->width;
  height = positioner->height;

  /* Constraint calculations are simplest if we use scaled
     coordinates, and then unscale them later.  */
  TruncateSurfaceToWindow (parent->surface, x, y, &x, &y);
  TruncateScaleToWindow (parent->surface, width, height, &width,
			 &height);

  /* Set the factor describing how to convert surface coordinates to
     window ones.  */
  scale_adjustment_factor = parent->surface->factor;

  if (positioner->constraint_adjustment
      == XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_NONE)
    /* There is no constraint adjustment.  */
    goto finish;

  /* Compute the current offset.  */
  GetAdjustmentOffset (parent, &off_x, &off_y);

  if (!XLGetOutputRectAt (off_x + x, off_y + y, &cx, &cy,
			  &cwidth, &cheight))
    /* There is no output in which to constrain this popup.  */
    goto finish;

#ifdef DEBUG_POSITIONER

  fputs ("ApplyConstraintAdjustments: constraint adjustments are: ", stderr);

  if (positioner->constraint_adjustment
      & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X)
    fputs ("SLIDE_X ", stderr);

  if (positioner->constraint_adjustment
      & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y)
    fputs ("SLIDE_Y ", stderr);

  if (positioner->constraint_adjustment
      & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X)
    fputs ("FLIP_X ", stderr);

  if (positioner->constraint_adjustment
      & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y)
    fputs ("FLIP_Y ", stderr);

  if (positioner->constraint_adjustment
      & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X)
    fputs ("RESIZE_X ", stderr);

  if (positioner->constraint_adjustment
      & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y)
    fputs ("RESIZE_X ", stderr);

  fprintf (stderr, "\n");

#endif

  if (positioner->constraint_adjustment
      & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X)
    x = TryFlipX (positioner, x + off_x, width,
		  cx, cwidth, off_x) - off_x;

  if (positioner->constraint_adjustment
      & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y)
    y = TryFlipY (positioner, y + off_y, height,
		  cy, cheight, off_y) - off_y;

  if (positioner->constraint_adjustment
      & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X)
    x = TrySlideX (positioner, x + off_x, width,
		   cx, cwidth) - off_x;

  if (positioner->constraint_adjustment
      & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y)
    y = TrySlideY (positioner, y + off_y, height,
		   cy, cheight) - off_y;

  if (positioner->constraint_adjustment
      & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X)
    TryResizeX (x + off_x, width, cx, cwidth,
		off_x, &x, &width);

  if (positioner->constraint_adjustment
      & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y)
    TryResizeY (y + off_y, height, cy, cheight,
		off_y, &y, &height);

 finish:
  /* Now, scale the coordinates back.  */
  TruncateWindowToSurface (parent->surface, x, y, &x, &y);
  TruncateScaleToSurface (parent->surface, width, height, &width, &height);

  *x_out = x;
  *y_out = y;
  *width_out = width;
  *height_out = height;
}

void
XLPositionerCalculateGeometry (Positioner *positioner, Role *parent,
			       int *x_out, int *y_out, int *width_out,
			       int *height_out)
{
  int x, y, width, height;

  CalculatePosition (positioner, &x, &y);

  if (parent->surface)
    ApplyConstraintAdjustment (positioner, parent, x, y,
			       &x, &y, &width, &height);
  else
    width = positioner->width, height = positioner->height;

  *x_out = x;
  *y_out = y;
  *width_out = width;
  *height_out = height;
}

void
XLCreateXdgPositioner (struct wl_client *client, struct wl_resource *resource,
		       uint32_t id)
{
  Positioner *positioner;

  positioner = XLSafeMalloc (sizeof *positioner);

  if (!positioner)
    {
      wl_client_post_no_memory (client);
      return;
    }

  memset (positioner, 0, sizeof *positioner);
  positioner->resource
    = wl_resource_create (client, &xdg_positioner_interface,
			  wl_resource_get_version (resource),
			  id);

  if (!positioner->resource)
    {
      wl_resource_post_no_memory (resource);
      XLFree (positioner);
      return;
    }

  wl_resource_set_implementation (positioner->resource,
				  &xdg_positioner_impl,
				  positioner,
				  HandleResourceDestroy);
}

void
XLCheckPositionerComplete (Positioner *positioner)
{
  if (positioner->anchor_width && positioner->width)
    return;

  wl_resource_post_error (positioner->resource,
			  XDG_WM_BASE_ERROR_INVALID_POSITIONER,
			  "the specified positioner is incomplete");
}
