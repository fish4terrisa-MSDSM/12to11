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

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "compositor.h"

void *
XLMalloc (size_t size)
{
  void *ptr;

  ptr = malloc (size);

  if (!ptr && size)
    {
      fprintf (stderr, "Allocation of %zu bytes failed\n",
	       size);
      abort ();
    }

  return ptr;
}

void *
XLSafeMalloc (size_t size)
{
  return malloc (size);
}

void *
XLCalloc (size_t nmemb, size_t size)
{
  void *ptr;

  ptr = calloc (nmemb, size);

  if (!ptr && nmemb && size)
    {
      fprintf (stderr, "Allocation of %zu * %zu failed\n",
	       nmemb, size);
      abort ();
    }

  return ptr;
}

void
XLFree (void *ptr)
{
  if (ptr)
    free (ptr);
}

char *
XLStrdup (const char *data)
{
  char *string;

  string = strdup (data);

  if (!string)
    {
      fprintf (stderr, "Allocation of %zu bytes failed\n",
	       strlen (data));
      abort ();
    }

  return string;
}

void *
XLRealloc (void *ptr, size_t size)
{
  if (!ptr)
    return XLMalloc (size);

  ptr = realloc (ptr, size);

  if (size && !ptr)
    {
      fprintf (stderr, "Reallocation of %zu bytes failed\n", size);
      abort ();
    }

  /* Allow realloc to return NULL if size is also NULL.  */

  return ptr;
}
