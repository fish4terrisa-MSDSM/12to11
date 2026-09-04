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

/* Copyright (C) 2001-2022 Free Software Foundation, Inc.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as
published by the Free Software Foundation; either version 2.1 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this program.

If not, see <https://www.gnu.org/licenses/>.  */

#ifndef IntAddWrapv

#define _GL_INT_CONVERT(e, v)		((1 ? 0 : (e)) + (v))
#define _GL_INT_NEGATE_CONVERT(e, v)	((1 ? 0 : (e)) - (v))
#define _GL_TYPE_SIGNED(t)		(! ((t) 0 < (t) -1))
#define _GL_EXPR_SIGNED(e)		(_GL_INT_NEGATE_CONVERT (e, 1) < 0)
#define _GL_TYPE_WIDTH(t)		(sizeof (t) * CHAR_BIT)

#define _GL_INT_OP_WRAPV_VIA_UNSIGNED(a, b, op, ut, t)					\
  ((t) ((ut) (a) op (ut) (b)))

#define _GL_INT_NEGATE_RANGE_OVERFLOW(a, min, max) 					\
  ((min) < 0 ? (a) < - (max) : 0 < (a))

#define _GL_INT_MINIMUM(e)								\
  (_GL_EXPR_SIGNED (e)									\
   ? ~ _GL_SIGNED_INT_MAXIMUM (e)							\
   : _GL_INT_CONVERT (e, 0))

#define _GL_INT_MAXIMUM(e)								\
  (_GL_EXPR_SIGNED (e)									\
   ? _GL_SIGNED_INT_MAXIMUM (e)								\
   : _GL_INT_NEGATE_CONVERT (e, 1))

#define _GL_SIGNED_INT_MAXIMUM(e)							\
  (((_GL_INT_CONVERT (e, 1) << (_GL_TYPE_WIDTH (+ (e)) - 2)) - 1) * 2 + 1)

#define _GL_INT_OP_CALC(a, b, r, op, overflow, ut, t, tmin, tmax)			\
  (overflow (a, b, tmin, tmax)								\
   ? (*(r) = _GL_INT_OP_WRAPV_VIA_UNSIGNED (a, b, op, ut, t), 1)			\
   : (*(r) = _GL_INT_OP_WRAPV_VIA_UNSIGNED (a, b, op, ut, t), 0))

# define _GL_INT_NEGATE_OVERFLOW(a)							\
   _GL_INT_NEGATE_RANGE_OVERFLOW (a, _GL_INT_MINIMUM (a), _GL_INT_MAXIMUM (a))

#define _GL_INT_ADD_RANGE_OVERFLOW(a, b, tmin, tmax)					\
  ((b) < 0										\
   ? (((tmin)										\
       ? ((_GL_EXPR_SIGNED (_GL_INT_CONVERT (a, (tmin) - (b))) || (b) < (tmin))		\
          && (a) < (tmin) - (b))							\
       : (a) <= -1 - (b))								\
      || ((_GL_EXPR_SIGNED (a) ? 0 <= (a) : (tmax) < (a)) && (tmax) < (a) + (b)))	\
   : (a) < 0										\
   ? (((tmin)										\
       ? ((_GL_EXPR_SIGNED (_GL_INT_CONVERT (b, (tmin) - (a))) || (a) < (tmin))		\
          && (b) < (tmin) - (a))							\
       : (b) <= -1 - (a))								\
      || ((_GL_EXPR_SIGNED (_GL_INT_CONVERT (a, b)) || (tmax) < (b))			\
          && (tmax) < (a) + (b)))							\
   : (tmax) < (b) || (tmax) - (b) < (a))
#define _GL_INT_SUBTRACT_RANGE_OVERFLOW(a, b, tmin, tmax)				\
  (((a) < 0) == ((b) < 0)								\
   ? ((a) < (b)										\
      ? !(tmin) || -1 - (tmin) < (b) - (a) - 1						\
      : (tmax) < (a) - (b))								\
   : (a) < 0										\
   ? ((!_GL_EXPR_SIGNED (_GL_INT_CONVERT ((a) - (tmin), b)) && (a) - (tmin) < 0)	\
      || (a) - (tmin) < (b))								\
   : ((! (_GL_EXPR_SIGNED (_GL_INT_CONVERT (tmax, b))					\
          && _GL_EXPR_SIGNED (_GL_INT_CONVERT ((tmax) + (b), a)))			\
       && (tmax) <= -1 - (b))								\
      || (tmax) + (b) < (a)))
#define _GL_INT_MULTIPLY_RANGE_OVERFLOW(a, b, tmin, tmax)				\
  ((b) < 0										\
   ? ((a) < 0										\
      ? (_GL_EXPR_SIGNED (_GL_INT_CONVERT (tmax, b))					\
         ? (a) < (tmax) / (b)								\
         : ((_GL_INT_NEGATE_OVERFLOW (b)						\
             ? _GL_INT_CONVERT (b, tmax) >> (_GL_TYPE_WIDTH (+ (b)) - 1)		\
             : (tmax) / -(b))								\
            <= -1 - (a)))								\
      : _GL_INT_NEGATE_OVERFLOW (_GL_INT_CONVERT (b, tmin)) && (b) == -1		\
      ? (_GL_EXPR_SIGNED (a)								\
         ? 0 < (a) + (tmin)								\
         : 0 < (a) && -1 - (tmin) < (a) - 1)						\
      : (tmin) / (b) < (a))								\
   : (b) == 0										\
   ? 0											\
   : ((a) < 0										\
      ? (_GL_INT_NEGATE_OVERFLOW (_GL_INT_CONVERT (a, tmin)) && (a) == -1		\
         ? (_GL_EXPR_SIGNED (b) ? 0 < (b) + (tmin) : -1 - (tmin) < (b) - 1)		\
         : (tmin) / (a) < (b))								\
      : (tmax) / (b) < (a)))

#define _GL_INT_OP_WRAPV(a, b, r, op, overflow)						\
  (_Generic										\
   (*(r),										\
    signed char:									\
    _GL_INT_OP_CALC (a, b, r, op, overflow, unsigned int,				\
		     signed char, SCHAR_MIN, SCHAR_MAX),				\
    unsigned char:									\
    _GL_INT_OP_CALC (a, b, r, op, overflow, unsigned int,				\
		     unsigned char, 0, UCHAR_MAX),					\
    short int:										\
    _GL_INT_OP_CALC (a, b, r, op, overflow, unsigned int,				\
		     short int, SHRT_MIN, SHRT_MAX),					\
    unsigned short int:									\
    _GL_INT_OP_CALC (a, b, r, op, overflow, unsigned int,				\
		     unsigned short int, 0, USHRT_MAX),					\
    int:										\
    _GL_INT_OP_CALC (a, b, r, op, overflow, unsigned int,				\
		     int, INT_MIN, INT_MAX),						\
    unsigned int:									\
    _GL_INT_OP_CALC (a, b, r, op, overflow, unsigned int,				\
		     unsigned int, 0, UINT_MAX),					\
    long int:										\
    _GL_INT_OP_CALC (a, b, r, op, overflow, unsigned long int,				\
		     long int, LONG_MIN, LONG_MAX),					\
    unsigned long int:									\
    _GL_INT_OP_CALC (a, b, r, op, overflow, unsigned long int,				\
		     unsigned long int, 0, ULONG_MAX),					\
    long long int:									\
    _GL_INT_OP_CALC (a, b, r, op, overflow, unsigned long long int,			\
		     long long int, LLONG_MIN, LLONG_MAX),				\
    unsigned long long int:								\
    _GL_INT_OP_CALC (a, b, r, op, overflow, unsigned long long int,			\
		     unsigned long long int, 0, ULLONG_MAX)))

#define _GL_INT_ADD_WRAPV(a, b, r)							\
  _GL_INT_OP_WRAPV (a, b, r, +, _GL_INT_ADD_RANGE_OVERFLOW)
#define _GL_INT_SUBTRACT_WRAPV(a, b, r)							\
  _GL_INT_OP_WRAPV (a, b, r, -, _GL_INT_SUBTRACT_RANGE_OVERFLOW)
#define _GL_INT_MULTIPLY_WRAPV(a, b, r)							\
  _GL_INT_OP_WRAPV (a, b, r, *, _GL_INT_MULTIPLY_RANGE_OVERFLOW)

#define IntAddWrapv(a, b, r) _GL_INT_ADD_WRAPV (a, b, r)
#define IntSubtractWrapv(a, b, r) _GL_INT_SUBTRACT_WRAPV (a, b, r)
#define IntMultiplyWrapv(a, b, r) _GL_INT_MULTIPLY_WRAPV (a, b, r)

#endif

#ifndef Popcount
#define NeedPortPopcount

extern int PortPopcount (unsigned long long int);

#define Popcount(number)	(PortPopcount (number))

#endif
