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
  Index (temp, 0, 1) = sinf (theta);
  Index (temp, 1, 0) = -sinf (theta);
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
