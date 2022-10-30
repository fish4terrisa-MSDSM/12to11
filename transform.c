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

/* Generic 3x3 matrix transform code.  */

#include <string.h>
#include <stdio.h>

#include "compositor.h"

#include <X11/extensions/Xrender.h>

/* These macros make column major order easier to reason about for C
   folks.  */
#define Index(matrix, row, column)				\
  ((matrix)[(column) * 3 + (row)])

#define MultiplySub(a, b, a_row, a_column, b_row, b_column)	\
  (Index (a, a_row, a_column) * Index (b, b_row, b_column))

#if 0

static void
MatrixPrint (Matrix *matrix)
{
  fprintf (stderr,
	   "%4f %4f %4f\n"
	   "%4f %4f %4f\n"
	   "%4f %4f %4f\n\n",
	   (double) Index (*matrix, 0, 0),
	   (double) Index (*matrix, 0, 1),
	   (double) Index (*matrix, 0, 2),
	   (double) Index (*matrix, 1, 0),
	   (double) Index (*matrix, 1, 1),
	   (double) Index (*matrix, 1, 2),
	   (double) Index (*matrix, 2, 0),
	   (double) Index (*matrix, 2, 1),
	   (double) Index (*matrix, 2, 2));
}

#endif

void
MatrixMultiply (Matrix a, Matrix b, Matrix *product)
{
  Index (*product, 0, 0) = (MultiplySub   (a, b, 0, 0, 0, 0)
			    + MultiplySub (a, b, 0, 1, 1, 0)
			    + MultiplySub (a, b, 0, 2, 2, 0));
  Index (*product, 0, 1) = (MultiplySub   (a, b, 0, 0, 0, 1)
			    + MultiplySub (a, b, 0, 1, 1, 1)
			    + MultiplySub (a, b, 0, 2, 2, 1));
  Index (*product, 0, 2) = (MultiplySub   (a, b, 0, 0, 0, 2)
			    + MultiplySub (a, b, 0, 1, 1, 2)
			    + MultiplySub (a, b, 0, 2, 2, 2));

  Index (*product, 1, 0) = (MultiplySub   (a, b, 1, 0, 0, 0)
			    + MultiplySub (a, b, 1, 1, 1, 0)
			    + MultiplySub (a, b, 1, 2, 2, 0));
  Index (*product, 1, 1) = (MultiplySub   (a, b, 1, 0, 0, 1)
			    + MultiplySub (a, b, 1, 1, 1, 1)
			    + MultiplySub (a, b, 1, 2, 2, 1));
  Index (*product, 1, 2) = (MultiplySub   (a, b, 1, 0, 0, 2)
			    + MultiplySub (a, b, 1, 1, 1, 2)
			    + MultiplySub (a, b, 1, 2, 2, 2));

  Index (*product, 2, 0) = (MultiplySub   (a, b, 2, 0, 0, 0)
			    + MultiplySub (a, b, 2, 1, 1, 0)
			    + MultiplySub (a, b, 2, 2, 2, 0));
  Index (*product, 2, 1) = (MultiplySub   (a, b, 2, 0, 0, 1)
			    + MultiplySub (a, b, 2, 1, 1, 1)
			    + MultiplySub (a, b, 2, 2, 2, 1));
  Index (*product, 2, 2) = (MultiplySub   (a, b, 2, 0, 0, 2)
			    + MultiplySub (a, b, 2, 1, 1, 2)
			    + MultiplySub (a, b, 2, 2, 2, 2));
}

void
MatrixIdentity (Matrix *matrix)
{
  memset (matrix, 0, sizeof *matrix);

  Index (*matrix, 0, 0) = 1.0f;
  Index (*matrix, 1, 1) = 1.0f;
  Index (*matrix, 2, 2) = 1.0f;
}

void
MatrixTranslate (Matrix *transform, float tx, float ty)
{
  Matrix temp, copy;

  MatrixIdentity (&temp);
  memcpy (copy, transform, sizeof copy);

  /* Set the tx and ty.  */
  Index (temp, 0, 2) = tx;
  Index (temp, 1, 2) = ty;

  /* Multiply it with the transform.  */
  MatrixMultiply (copy, temp, transform);
}

void
MatrixScale (Matrix *transform, float sx, float sy)
{
  Matrix temp, copy;

  MatrixIdentity (&temp);
  memcpy (copy, transform, sizeof copy);

  /* Set the scale factors.  */
  Index (temp, 0, 0) = sx;
  Index (temp, 1, 1) = sy;

  /* Multiply it with the transform.  */
  MatrixMultiply (copy, temp, transform);
}

void
MatrixRotate (Matrix *transform, float theta, float x, float y)
{
  Matrix temp, copy;

  /* Translate the matrix to x, y, and then perform rotation by the
     given angle in radians and translate back.  As the transform is
     being performed in the X coordinate system, the given angle
     describes a clockwise rotation.  */

  MatrixIdentity (&temp);
  memcpy (copy, transform, sizeof copy);

  Index (temp, 0, 2) = x;
  Index (temp, 1, 2) = y;

  MatrixMultiply (copy, temp, transform);
  MatrixIdentity (&temp);
  memcpy (copy, transform, sizeof copy);

  Index (temp, 0, 0) = cosf (theta);
  Index (temp, 0, 1) = -sinf (theta);
  Index (temp, 1, 0) = sinf (theta);
  Index (temp, 1, 1) = cosf (theta);

  MatrixMultiply (copy, temp, transform);
  MatrixIdentity (&temp);
  memcpy (copy, transform, sizeof copy);

  Index (temp, 0, 2) = -x;
  Index (temp, 1, 2) = -y;

  MatrixMultiply (copy, temp, transform);
}

void
MatrixMirrorHorizontal (Matrix *transform, float width)
{
  Matrix temp, copy;

  /* Scale the matrix by -1, and then apply a tx of width, in effect
     flipping the image horizontally.  */

  MatrixIdentity (&temp);
  memcpy (copy, transform, sizeof copy);

  Index (temp, 0, 0) = -1.0f;
  Index (temp, 0, 2) = width;

  MatrixMultiply (copy, temp, transform);
}

void
MatrixExport (Matrix *transform, XTransform *xtransform)
{
  /* M1 M2 M3     X
     M4 M5 M6   * Y
     M7 M8 M9     Z

     =

     M1*X + M2*Y + M3*1 = X1
     M4*X + M5*Y + M6*1 = Y1
     M7*X + M8*Y + M9*1 = Z1 (Only on some drivers)

     where

     M1 = matrix[0][0]
     M2 = matrix[0][1]
     M3 = matrix[0][2]
     M4 = matrix[1][0]
     M5 = matrix[1][1]
     M6 = matrix[1][2]
     M7 = matrix[2][0]
     M8 = matrix[2][1]
     M9 = matrix[2][2]  */

#define Export(row, column)						\
  xtransform->matrix[row][column]					\
    = XDoubleToFixed (Index (*transform, row, column))

  Export (0, 0);
  Export (0, 1);
  Export (0, 2);

  Export (1, 0);
  Export (1, 1);
  Export (1, 2);

  Export (2, 0);
  Export (2, 1);
  Export (2, 2);

#undef Export
}

/* Various routines shared between renderers.  */

void
ApplyInverseTransform (int buffer_width, int buffer_height, Matrix *matrix,
		       BufferTransform transform)
{
  float width, height;

  /* Wayland buffer transforms are somewhat confusing.  They are
     actually applied in reverse, so a counterclockwise rotation would
     actually be applied clockwise, and so on.

     The fact that matrix maps from destination coordinates to buffer
     coordinates makes things easier: as the inverse of the inverse of
     a transform is itself, transforms are just applied in that
     order.  */

  width = buffer_width;
  height = buffer_height;

  switch (transform)
    {
    case Normal:
      break;

    case CounterClockwise90:
      /* CounterClockwise90.  Rotate the buffer contents 90 degrees
	 clockwise.  IOW, rotate the destination by 90 degrees
	 counterclockwise, which is 270 degrees clockwise.  */
      MatrixRotate (matrix, M_PI * 1.5, 0, 0);
      MatrixTranslate (matrix, -height, 0);
      break;

    case CounterClockwise180:
      /* CounterClockwise180.  It's 180 degrees.  Apply clockwise 180
	 degree rotation around the center.  */
      MatrixRotate (matrix, M_PI, width / 2.0f, height / 2.0f);
      break;

    case CounterClockwise270:
      /* CounterClockwise270.  Rotate the buffer contents 270 degrees
	 clockwise.  IOW, rotate the destination by 270 degrees
	 counterclockwise, which is 90 degrees clockwise.  */
      MatrixRotate (matrix, M_PI * 0.5, 0, 0);
      MatrixTranslate (matrix, 0, -width);
      break;

    case Flipped:
      /* Flipped.  Apply horizontal flip.  */
      MatrixMirrorHorizontal (matrix, width);
      break;

    case Flipped90:
      /* Flipped90.  Apply a flip but otherwise treat this the same as
	 CounterClockwise90.  */
      MatrixRotate (matrix, M_PI * 1.5, 0, 0);
      MatrixTranslate (matrix, -height, 0);
      MatrixMirrorHorizontal (matrix, height);
      break;

    case Flipped180:
      /* Flipped180.  Apply a flip and treat this the same as
	 CounterClockwise180.  */
      MatrixRotate (matrix, M_PI, width / 2.0f, height / 2.0f);
      MatrixMirrorHorizontal (matrix, width);
      break;

    case Flipped270:
      /* Flipped270.  Apply a flip and treat this the same as
	 CounterClockwise270.  */
      MatrixRotate (matrix, M_PI * 0.5, 0, 0);
      MatrixTranslate (matrix, 0, -width);
      MatrixMirrorHorizontal (matrix, height);
      break;
    }

  return;
}

void
TransformBox (pixman_box32_t *box, BufferTransform transform,
	      int width, int height)
{
  pixman_box32_t work;

  switch (transform)
    {
    case Normal:
    default:
      work = *box;
      break;

    case CounterClockwise90:
      work.x1 = height - box->y2;
      work.y1 = box->x1;
      work.x2 = height - box->y1;
      work.y2 = box->x2;
      break;

    case CounterClockwise180:
      work.x1 = width - box->x2;
      work.y1 = height - box->y2;
      work.x2 = width - box->x1;
      work.y2 = height - box->y1;
      break;

    case CounterClockwise270:
      work.x1 = box->y1;
      work.y1 = width - box->x2;
      work.x2 = box->y2;
      work.y2 = width - box->x1;
      break;

    case Flipped:
      work.x1 = width - box->x2;
      work.y1 = box->y1;
      work.x2 = width - box->x1;
      work.y2 = box->y2;
      break;

    case Flipped90:
      work.x1 = box->y1;
      work.y1 = box->x1;
      work.x2 = box->y2;
      work.y2 = box->x2;
      break;

    case Flipped180:
      work.x1 = box->x1;
      work.y1 = height - box->y2;
      work.x2 = box->x2;
      work.y2 = height - box->y1;
      break;

    case Flipped270:
      work.x1 = height - box->y2;
      work.y1 = width - box->x2;
      work.x2 = height - box->y1;
      work.y2 = width - box->x1;
      break;
    }

  *box = work;
}

BufferTransform
InvertTransform (BufferTransform transform)
{
  switch (transform)
    {
    case CounterClockwise270:
      return CounterClockwise90;

    case CounterClockwise90:
      return CounterClockwise270;

    default:
      return transform;
    }
}
