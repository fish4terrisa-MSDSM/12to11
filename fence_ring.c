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

#include <sys/fcntl.h>

#include <stdio.h>

#include "compositor.h"

#include <X11/xshmfence.h>
#include <X11/extensions/sync.h>

#include <xcb/dri3.h>

#define FenceFree	(1U << 31)

struct _Fence
{
  /* The next and last fences on this list.  */
  Fence *next;

  /* The xshmfence.  */
  struct xshmfence *fence;

  /* The sync fence.  High bit is a flag meaning that the fence is on
     the free list.  */
  XSyncFence fence_id;

  /* The number of references to this fence.  Incremented by
     FenceRetain, decremented by FenceAwait.  */
  int refcount;
};

/* Chain of all free fences.  */
static Fence *all_fences;

Fence *
GetFence (void)
{
  Fence *fence;
  int fd;
  Window drawable;

  drawable = DefaultRootWindow (compositor.display);

  /* Get one free fence.  */

  for (fence = all_fences; fence; fence = fence->next)
    {
      /* Unlink this fence.  */
      all_fences = fence->next;
      fence->next = NULL;

      /* Mark the fence as used.  */
      fence->fence_id &= ~FenceFree;

      /* Return it.  */
      return fence;
    }

  /* Otherwise, allocate a new fence.  */
  fence = XLCalloc (1, sizeof *fence);
  fd = xshmfence_alloc_shm ();

  if (fd < 0)
    {
      perror ("xshmfence_alloc_shm");
      abort ();
    }

  /* Map it.  */
  fence->fence = xshmfence_map_shm (fd);

  if (!fence->fence)
    {
      perror ("xshmfence_map_shm");
      abort ();
    }

  /* Upload the fence to the X server.  XCB will close the file
     descriptor.  */
  fence->fence_id = xcb_generate_id (compositor.conn);

  /* Make the file descriptor CLOEXEC, since it isn't closed
     immediately.  */
  XLAddFdFlag (fd, FD_CLOEXEC, False);
  xcb_dri3_fence_from_fd (compositor.conn, drawable,
			  fence->fence_id, 0, fd);

  /* Return the fence.  */
  return fence;
}

void
FenceAwait (Fence *fence)
{
  XLAssert (fence->refcount);
  fence->refcount -= 1;

  if (!(fence->fence_id & FenceFree))
    {
      /* Wait for the fence to be triggered.  */
      xshmfence_await (fence->fence);

      /* Reset the fence.  */
      xshmfence_reset (fence->fence);
      fence->fence_id |= FenceFree;
    }

  if (fence->refcount)
    return;

  /* Now that the fence is no longer referenced, it can be put back on
     the list of free fences.  */
  fence->next = all_fences;
  all_fences = fence;
}

void
FenceRetain (Fence *fence)
{
  fence->refcount++;
}

XSyncFence
FenceToXFence (Fence *fence)
{
  return fence->fence_id & ~FenceFree;
}
