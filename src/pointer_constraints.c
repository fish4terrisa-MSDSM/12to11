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
#include <stdlib.h>

#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XInput2.h>

#include "compositor.h"
#include "pointer-constraints-unstable-v1.h"

typedef struct _BarrierLine BarrierLine;
typedef enum _BarrierEdges BarrierEdges;

typedef struct _PointerConfinementDataRecord PointerConfinementDataRecord;
typedef struct _PointerConfinement PointerConfinement;

enum
  {
    IsOneShot		      = 1,
    IsActive		      = (1 << 1),
    IsDead		      = (1 << 2),
    IsLock		      = (1 << 3),
    IsCursorPositionHintSet   = (1 << 4),
    PendingRegion	      = (1 << 10),
    PendingCursorPositionHint = (1 << 11),
  };

struct _PointerConfinement
{
  /* The next and last confinements.  */
  PointerConfinement *next, *last;

  /* The surface associated with the confinement.  */
  Surface *surface;

  /* The seat associated with the confinement.  */
  Seat *seat;

  /* The seat destruction key.  */
  void *seat_key;

  /* The resource of the confinement.  */
  struct wl_resource *resource;

  /* The region to confine the pointer to, or NULL.  */
  pixman_region32_t *region;

  /* Any pending region.  */
  pixman_region32_t *pending_region;

  /* A list of pointer barriers currently applied.  */
  XIDList *applied_barriers;

  /* Any barrier lines currently applied.  */
  BarrierLine *lines;

  /* Any commit callback.  */
  CommitCallback *commit_callback;

  /* The number of such barrier lines.  */
  int nlines;

  /* Various flags, i.e.: whether or not this is a 1-shot confinement,
     and whether or not this is a lock.  */
  int flags;

  /* The root_x and root_y of the last lock applied.  */
  int root_x, root_y;

  /* The last known cursor position relative to the surface.  */
  double last_cursor_x, last_cursor_y;

  /* The cursor position hint.  */
  double cursor_position_x, cursor_position_y;

  /* Pending values.  */
  double pending_x, pending_y;
};

/* Which edges are set.  */
enum _BarrierEdges
  {
    TopEdgeClosed    = 1,
    LeftEdgeClosed   = (1 << 1),
    BottomEdgeClosed = (1 << 2),
    RightEdgeClosed  = (1 << 3),
    AllEdgesClosed   = 0xf,
  };

/* The extents of previously seen rectangles in the pointer
   barrier.  */

struct _BarrierLine
{
  /* The rectangle.  */
  int x1, y1, x2, y2;

  /* Which edges are set.  */
  int edges;
};

struct _PointerConfinementDataRecord
{
  /* List of all pointer confinements.  */
  PointerConfinement confinements;
};

#define Int16Maximum 0x7fff
#define Int16Minimum (-1 - Int16Maximum)

/* The pointer constraints global.  */
static struct wl_global *pointer_constraints_global;

static BarrierLine *
FindLastBandEnd (BarrierLine *lines, BarrierLine *current)
{
  int y1;

  y1 = current->y1;

  while (--current >= lines)
    {
      if (current->y1 != y1)
	return current;
    }

  return NULL;
}

static void
LineMove (BarrierLine *dest, BarrierLine *src, size_t nlines)
{
  memmove (dest, src, nlines * sizeof *dest);
}

/* Close the top edge of the line at *IDX, an index into the line
   buffer LINES.  Open any intersecting edges of lines above.

   Update IDX with the index of the last line in the line buffer.  */

static void
MaybeCloseTopEdge (BarrierLine *lines, int *idx, int max_lines)
{
  BarrierLine original, *last_band_start, *tem;

  /* Maybe close lines[*idx]'s top edge, if it does not overlap with
     any previous line belonging to the last band.  If it does, spit
     the given line if the overlap is partial.  */

  original = lines[*idx];
  last_band_start = FindLastBandEnd (lines, &lines[*idx]);

  if (!last_band_start)
    {
      /* This is the first band, so close the top edge.  */
      lines[*idx].edges |= TopEdgeClosed;
      return;
    }

  if (last_band_start->y2 != original.y1)
    {
      /* The last band does not touch this one; close its bottom
	 edge.  */
      lines[*idx].edges |= TopEdgeClosed;
      return;
    }

  /* Mark the top edge as closed for now.  */
  lines[*idx].edges |= TopEdgeClosed;

  /* Find overlaps between the last band and this one, and split and
     open up the top edge for each overlap.  */
  for (tem = last_band_start;
       tem >= lines && tem->y2 == last_band_start->y2; --tem)
    {
      /* There are two categories of overlaps we care about.  The
	 first is where the current line is entirely inside the line
	 above.

	 +-------------------------------+
	 |                               | <------- line above
	 +-------------------------------+
	    +------------------------+
	    |                        | <----------- current line
	    +------------------------+   */

      if (tem->x1 <= lines[*idx].x1 && tem->x2 >= lines[*idx].x2)
	{
	  /* In that case, it must be transformed to:

	     +--+------------------------+---+
	     |  %(1)                     %(2)|
	     +--+                        +---+
	        +                        +
		|                        |
		+------------------------+

	     Where % denotes the places where tem must be split.  */

	  if (tem->x1 != lines[*idx].x1)
	    {
	      /* Here, tem->x1 is less than lines[*idx]->x1.  That
		 means we must actually do the split at %(1).  */

	      if (*idx + 1 >= max_lines)
		{
		  /* Fail if too many lines would be used.  */
		  *idx = max_lines;
		  return;
		}

	      /* Split tem.  */
	      LineMove (tem + 1, tem, max_lines - (tem - lines) - 1);
	      tem[1].edges &= ~LeftEdgeClosed;
	      tem->edges &= ~RightEdgeClosed;

	      /* Increase idx to compensate for the movement.  */
	      (*idx)++;

	      /* Finish the split.  */
	      tem->x2 = lines[*idx].x1;
	      tem[1].x1 = tem->x2;

	      if (tem[1].x2 != lines[*idx].x2)
		{
		  if (*idx + 1 >= max_lines)
		    {
		      /* Fail if too many lines would be used.  */
		      *idx = max_lines;
		      return;
		    }

		  /* Split tem[1] off at %(2).  */
		  LineMove (tem + 2, tem + 1, max_lines - (tem - lines) - 2);
		  tem[2].edges &= ~LeftEdgeClosed;
		  tem[1].edges &= ~RightEdgeClosed;

		  /* Increase idx to compensate for the movement.  */
		  (*idx)++;

		  /* Finish the split.  */
		  tem[1].x2 = lines[*idx].x2;
		  tem[2].x1 = lines[*idx].x2;
		}

	      /* Open tem[1]'s bottom edge.  */
	      tem[1].edges &= ~BottomEdgeClosed;
	    }
	  else
	    {
	      /* Here, tem->x1 is equal to lines[*idx].x1.  That
		 means the split at %(1) is unnecessary.  */

	      if (tem->x2 != lines[*idx].x2)
		{
		  if (*idx + 1 >= max_lines)
		    {
		      /* Fail if too many lines would be used.  */
		      *idx = max_lines;
		      return;
		    }

		  /* Split tem off at %(2).  */
		  LineMove (tem + 1, tem, max_lines - (tem - lines) - 1);
		  tem[1].edges &= ~LeftEdgeClosed;
		  tem->edges &= ~RightEdgeClosed;

		  /* Increase idx to compensate for the movement.  */
		  (*idx)++;

		  /* Finish the split.  */
		  tem->x2 = lines[*idx].x2;
		  tem[1].x1 = lines[*idx].x2;
		}

	      /* Open tem's bottom edge.  */
	      tem->edges &= ~BottomEdgeClosed;
	    }

	  /* Open lines[*idx]'s top edge.  */
	  lines[*idx].edges &= ~TopEdgeClosed;

	  return;
	}

      /* The second category is where the current line overlaps with
	 the line above.

	   +-----------------------------------------------+
   tem->x1 |       			                   | tem->x2
	   +-----------------------------------------------+
	        +------------------------------------------------+
lines[*idx].x1  |                                                | lines[*idx].x2
		+------------------------------------------------+

	   (In other words,    tem->x1 < lines[*idx].x1
	                    && tem->x2 < lines[*idx].x2
			    && tem->x1 < lines[*idx].x2
			    && tem->x2 > lines[*idx].x1)

         The case shown above is particularly nasty, because it
         requires splitting both tem and lines[*idx].  This is case
         A.

					              +-------------------+
					      tem->x1 |                   | tem->x2
						      +-------------------+
	        +------------------------------------------------+
lines[*idx].x1  |                                                | lines[*idx].x2
		+------------------------------------------------+

	   (In other words,    tem->x1 > lines[*idx].x1
	                    && tem->x2 > lines[*idx].x2
			    && tem->x1 < lines[*idx].x2
			    && tem->x2 > lines[*idx].x1)

	 The case shown above also requires splitting both lines.
	 This is case B.

		 +------------------------------------------------+
		 |                                                |
		 +------------------------------------------------+
	     +--------------------------------------------------------+
	     |                                                        |
	     +--------------------------------------------------------+

	   (In other words,    tem->x1 >= lines[*idx].x1
	                    && tem->x2 <= lines[*idx].x2)

	 This case only requires splitting the current line.  It is
	 case C.  */

      if (tem->x1 < lines[*idx].x1
	  && tem->x2 < lines[*idx].x2
	  && tem->x1 < lines[*idx].x2
	  && tem->x2 > lines[*idx].x1)
	{
	  /* Case A.  Perform the following splits and transforms:

	     +----%(1)---------------------------------------+
	     |                                               |
	     +----%(1)                                       +
		  +                                          %(2)--+
		  |                                                |
		  +------------------------------------------%(2)--+  */

	  if (*idx + 2 >= max_lines)
	    {
	      /* Fail if too many lines would be used.  */
	      *idx = max_lines;
	      return;
	    }

	  /* Do the first split.  */
	  LineMove (tem + 1, tem, max_lines - (tem - lines) - 1);
	  tem[1].edges &= ~LeftEdgeClosed;
	  tem->edges &= ~RightEdgeClosed;

	  /* Increase idx to compensate for the movement.  */
	  (*idx)++;

	  /* Finish the split.  */
	  tem->x2 = lines[*idx].x1;
	  tem[1].x1 = lines[*idx].x1;

	  /* Do the second split.  Leave idx intact.  */
	  LineMove (&lines[*idx + 1], &lines[*idx], max_lines - *idx - 1);
	  lines[*idx + 1].edges &= ~LeftEdgeClosed;
	  lines[*idx].edges &= ~RightEdgeClosed;

	  /* Finish the second split.  */
	  lines[*idx].x2 = tem[1].x2;
	  lines[*idx + 1].x1 = tem[1].x2;

	  /* Open lines[*idx]'s top edge and tem[1]'s bottom edge.  */
	  lines[*idx].edges &= ~TopEdgeClosed;
	  tem[1].edges &= ~BottomEdgeClosed;

	  /* Increment idx.  */
	  (*idx)++;

	  return;
	}
      else if (tem->x1 > lines[*idx].x1
	       && tem->x2 > lines[*idx].x2
	       && tem->x1 < lines[*idx].x2
	       && tem->x2 > lines[*idx].x1)
	{
	  /* Case B.  Perform the following splits and transforms:

						  +----------%(1)-----+
						  |                   |
						  +          %(1)-----+
	    +-------------------------------------%(2)       +
	    |                                                |
	    +-------------------------------------%(2)-------+  */

	  if (*idx + 2 >= max_lines)
	    {
	      /* Fail if too many lines would be used.  */
	      *idx = max_lines;
	      return;
	    }

	  /* Do the first split.  */
	  LineMove (tem + 1, tem, max_lines - (tem - lines) - 1);
	  tem[1].edges &= ~LeftEdgeClosed;
	  tem->edges &= ~RightEdgeClosed;

	  /* Increase idx to compensate for movement.  */
	  (*idx)++;

	  /* Finish the split.  */
	  tem->x2 = lines[*idx].x2;
	  tem[1].x1 = lines[*idx].x2;

	  /* Do the second split.  Do not increase idx.  */
	  LineMove (&lines[*idx + 1], &lines[*idx], max_lines - *idx - 1);
	  lines[*idx + 1].edges &= ~LeftEdgeClosed;
	  lines[*idx].edges &= ~RightEdgeClosed;

	  /* Finish the second split.  */
	  lines[*idx].x2 = tem->x1;
	  lines[*idx + 1].x1 = tem->x1;

	  /* Now, open lines[*idx + 1]'s top edge and tem's bottom
	     edge.  */
	  lines[*idx + 1].edges &= ~TopEdgeClosed;
	  tem->edges &= ~BottomEdgeClosed;

	  /* Finally, resolve edges for line[*idx], which may still
	     have overlaps to the top.  */
	  MaybeCloseTopEdge (lines, idx, max_lines);

	  /* And increment *idx.  */
	  (*idx)++;

	  return;
	}
      else if (tem->x1 >= lines[*idx].x1
	       && tem->x2 <= lines[*idx].x2)
	{
	  /* Case C.  Do the following splits and transforms:

		 +------------------------------------------------+
		 |                                                |
		 +                                                +
	     +---%(1)                                             %(2)+
	     |                                                        |
	     +---%(1)---------------------------------------------%(2)+  */

	  if (tem->x1 != lines[*idx].x1)
	    {
	      if (*idx + 1 >= max_lines)
		{
		  /* Fail if too many lines would be used.  */
		  *idx = max_lines;
		  return;
		}

	      /* Do the first split.  Do not increase idx yet.  */
	      LineMove (&lines[*idx + 1], &lines[*idx],
			max_lines - *idx - 1);
	      lines[*idx + 1].edges &= ~LeftEdgeClosed;
	      lines[*idx].edges &= ~RightEdgeClosed;

	      /* Finish the split.  */
	      lines[*idx].x2 = tem->x1;
	      lines[*idx + 1].x1 = tem->x1;

	      if (tem->x2 != lines[*idx + 1].x2)
		{
		  if (*idx + 1 >= max_lines)
		    {
		      /* Fail if too many lines would be used.  */
		      *idx = max_lines;
		      return;
		    }

		  /* Do the second split.  */
		  LineMove (&lines[*idx + 2], &lines[*idx + 1],
			   max_lines - *idx - 2);
		  lines[*idx + 2].edges &= ~LeftEdgeClosed;
		  lines[*idx + 1].edges &= ~RightEdgeClosed;

		  /* Finish the split.  */
		  lines[*idx + 1].x2 = tem->x2;
		  lines[*idx + 2].x1 = tem->x2;

		  /* Now, we are in a very complicated state of
		     affairs.  idx is the index of a line that must be
		     processed again.  idx + 1 is the index of the
		     line we are really processing, and idx + 2 is a
		     rectangle that does not overlap with anything
		     above (or otherwise case B or the first category
		     would have run first).  First, open tem's bottom
		     corner and lines[idx + 1]'s top corner.  */
		  tem->edges &= ~BottomEdgeClosed;
		  lines[*idx + 1].edges &= ~TopEdgeClosed;

		  /* Next, process splits for lines[*idx].  */
		  MaybeCloseTopEdge (lines, idx, max_lines);

		  /* Finally, increment idx by 2.  */
		  *idx += 2;
		}
	      else
		{
		  /* The second split is not required.  Simply open
		     tem's bottom edge, and lines[*idx + 1]'s top
		     edge.  */
		  tem->edges &= ~BottomEdgeClosed;
		  lines[*idx + 1].edges &= ~TopEdgeClosed;

		  /* And process splits for lines[*idx].  */
		  MaybeCloseTopEdge (lines, idx, max_lines);

		  /* Increment idx by 1.  */
		  (*idx)++;
		}
	    }
	  else
	    {
	      if (*idx + 1 >= max_lines)
		{
		  /* Fail if too many lines would be used.  */
		  *idx = max_lines;
		  return;
		}

	      /* Do the second split.  The first split is not
		 required, as it would generate an empty line.  The
		 second split is always required here, because
		 otherwise this would be a first category overlap.  */
	      LineMove (&lines[*idx + 1], &lines[*idx],
			max_lines - *idx - 1);
	      lines[*idx + 1].edges &= ~LeftEdgeClosed;
	      lines[*idx].edges &= ~RightEdgeClosed;

	      /* Finish the split.  */
	      lines[*idx].x2 = tem->x2;
	      lines[*idx + 1].x1 = tem->x2;

	      /* idx is the index of the overlapping line.  */
	      tem->edges &= ~BottomEdgeClosed;
	      lines[*idx].edges &= ~TopEdgeClosed;

	      /* Increment idx.  */
	      (*idx)++;
	    }

	  return;
	}
    }
}

static BarrierLine *
ComputeBarrier (pixman_region32_t *region, int *nlines)
{
  BarrierLine *lines;
  pixman_box32_t *boxes;
  int i, nrects, l, max_lines;

  /* Compute a list of rectangles along with edges that compose a
     pointer barrier confining the pointer within the region specified
     by REGION.  The rectangles in the region must be in the XY banded
     order used by the X server.  */

  boxes = pixman_region32_rectangles (region, &nrects);
  l = 0;

  /* Each rectangle can be split into 3 rectangles, potentially more
     taking into account left splitting.  Assume there are 6 lines for
     each rectangle; should it take any more, just error.  */
  max_lines = nrects * 6;
  lines = XLMalloc (max_lines * sizeof *lines);

  for (i = 0; i < nrects; ++i)
    {
      /* Compute which sides of each rectangle are "open", i.e. face
	 towards another rectangle.  */

      if (!l)
	{
	  /* Initialize the first line.  The rectangle is definitely
	     closed on the top and the left, and may be open on the
	     right and bottom.  */
	  lines[l].x1 = boxes[i].x1;
	  lines[l].y1 = boxes[i].y1;
	  lines[l].x2 = boxes[i].x2;
	  lines[l].y2 = boxes[i].y2;
	  lines[l].edges = (TopEdgeClosed | LeftEdgeClosed
			    | RightEdgeClosed | BottomEdgeClosed);

	  l++;

	  if (l >= max_lines)
	    /* L is too big.  */
	    goto failure;
	}
      else
	{
	  if (lines[l - 1].y1 == boxes[i].y1)
	    {
	      /* The rect is in the same band as the line above.  This
		 means that only its x1 and x2 are different.  If x1
		 == last.x2, extend last to x2.  */
	      if (boxes[i].x1 == lines[l - 1].x2)
		{
		  l -= 1;
		  lines[l].x2 = boxes[i].x2;
		}
	      else
		{
		  lines[l].x1 = boxes[i].x1;
		  lines[l].y1 = boxes[i].y1;
		  lines[l].x2 = boxes[i].x2;
		  lines[l].y2 = boxes[i].y2;

		  /* The left and right edges are definitely closed.
		     The bottom edge won't be opened until the next
		     line starts being processed.  */
		  lines[l].edges = (LeftEdgeClosed
				    | BottomEdgeClosed
				    | RightEdgeClosed);
		}

	      /* Compute whether or not the top edge is closed.  */
	      MaybeCloseTopEdge (lines, &l, max_lines);

	      /* l is now the index of the last used line.  Increase
		 it.  */
	      l++;

	      if (l >= max_lines)
		/* L is too big.  */
		goto failure;
	    }
	  else
	    {
	      /* Initialize the first line of this new band.  The
		 rectangle in this band closed on the left, and may be
		 open on the right, top and bottom.  */
	      lines[l].x1 = boxes[i].x1;
	      lines[l].y1 = boxes[i].y1;
	      lines[l].x2 = boxes[i].x2;
	      lines[l].y2 = boxes[i].y2;
	      lines[l].edges = (LeftEdgeClosed
				| BottomEdgeClosed
				| RightEdgeClosed);

	      /* Compute whether or not the top edge is closed.  */
	      MaybeCloseTopEdge (lines, &l, max_lines);

	      /* l is now the index of the last used line.  Increase
		 it.  */
	      l++;

	      if (l >= max_lines)
		/* L is too big.  */
		goto failure;
	    }
	}
    }

  /* Close the right edge of the last line.  */
  if (l)
    lines[l - 1].edges |= RightEdgeClosed;

  *nlines = l;
  return lines;

 failure:
  XLFree (lines);
  return NULL;
}

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}



static void
FreePointerConfinementDataRecord (void *pointer)
{
  PointerConfinementDataRecord *data;
  PointerConfinement *confinement;

  data = pointer;

  /* Assert that the record has been initialized correctly.  */
  XLAssert (data->confinements.next != NULL);

  /* Detach each confinement.  */
  confinement = data->confinements.next;

  while (confinement != &data->confinements)
    {
      confinement->surface = NULL;
      confinement->commit_callback = NULL;
      confinement = confinement->next;
    }
}

static void
InitConfinementData (PointerConfinementDataRecord *data)
{
  /* This record has already been initialized.  */
  if (data->confinements.next)
    return;

  /* Initialize the pointer confinement data.  */
  data->confinements.next = &data->confinements;
  data->confinements.last = &data->confinements;
}

static void
DestroyConfinedPointer (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
DestroyLockedPointer (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

/* Forward declaration.  */
static void RecheckPointerConfinement (Seat *, PointerConfinement *);

static void
HandleSurfaceCommit (Surface *surface, void *data)
{
  PointerConfinement *confinement;

  confinement = data;

  if (confinement->flags & PendingRegion)
    {
      /* Free any existing region.  */
      if (confinement->region)
	{
	  pixman_region32_fini (confinement->region);
	  XLFree (confinement->region);
	}

      /* Apply the new region.  */
      confinement->region = confinement->pending_region;
      confinement->pending_region = NULL;

      if (confinement->seat)
	/* If the seat is set, recheck pointer confinement.  If this
	   region change would cause the confinement to deactivate,
	   deactivate it and vice versa.  */
	RecheckPointerConfinement (confinement->seat, confinement);

      /* Clear the pending region flag.  */
      confinement->flags &= ~PendingRegion;
    }

  if (confinement->flags & PendingCursorPositionHint)
    {
      confinement->cursor_position_x = confinement->pending_x;
      confinement->cursor_position_y = confinement->pending_y;
      confinement->flags &= ~PendingCursorPositionHint;
      confinement->flags |= IsCursorPositionHintSet;
    }
}

static void
SetCursorPositionHint (struct wl_client *client, struct wl_resource *resource,
		       wl_fixed_t surface_x, wl_fixed_t surface_y)
{
  PointerConfinement *confinement;

  confinement = wl_resource_get_user_data (resource);

  if (!confinement)
    /* This is a resource created for an inert seat.  */
    return;

  confinement->pending_x = wl_fixed_to_double (surface_x);
  confinement->pending_y = wl_fixed_to_double (surface_y);
  confinement->flags |= PendingCursorPositionHint;

  if (!confinement->commit_callback && confinement->surface)
    /* Attach a commit callback so we know when to apply the
       region.  */
    confinement->commit_callback
      = XLSurfaceRunAtCommit (confinement->surface, HandleSurfaceCommit,
			      confinement);
}

static void
SetRegion (struct wl_client *client, struct wl_resource *resource,
	   struct wl_resource *region_resource)
{
  PointerConfinement *confinement;
  pixman_region32_t *new_region;

  confinement = wl_resource_get_user_data (resource);

  if (!confinement)
    /* This is a resource created for an inert seat.  */
    return;

  /* The region changed.  First, copy the new region to the old
     one.  */
  if (!region_resource)
    {
      if (confinement->pending_region)
	pixman_region32_fini (confinement->pending_region);
      XLFree (confinement->pending_region);
      confinement->pending_region = NULL;
    }
  else
    {
      if (!confinement->pending_region)
	{
	  confinement->pending_region
	    = XLMalloc (sizeof *confinement->pending_region);
	  pixman_region32_init (confinement->pending_region);
	}

      /* Copy the new region over.  */
      new_region = wl_resource_get_user_data (region_resource);
      pixman_region32_copy (confinement->pending_region, new_region);
    }

  confinement->flags |= PendingRegion;

  if (!confinement->commit_callback && confinement->surface)
    /* Attach a commit callback so we know when to apply the
       region.  */
    confinement->commit_callback
      = XLSurfaceRunAtCommit (confinement->surface, HandleSurfaceCommit,
			      confinement);
}

static const struct zwp_confined_pointer_v1_interface confined_pointer_impl =
  {
    .destroy = DestroyConfinedPointer,
    .set_region = SetRegion,
  };

static const struct zwp_locked_pointer_v1_interface locked_pointer_impl =
  {
    .destroy = DestroyLockedPointer,
    .set_cursor_position_hint = SetCursorPositionHint,
    /* SetRegion can be shared between both types of resources.  */
    .set_region = SetRegion,
  };

static void
FreeSingleBarrier (XID xid)
{
  XFixesDestroyPointerBarrier (compositor.display, xid);
}

/* Forward declaration.   */
static void DeactivateConfinement (PointerConfinement *);

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  PointerConfinement *confinement;

  confinement = wl_resource_get_user_data (resource);

  /* Deactivate the confinement if it is active.  */
  if (confinement->flags & IsActive)
    DeactivateConfinement (confinement);

  if (confinement->surface)
    {
      /* Detach the confinement.  */
      confinement->next->last = confinement->last;
      confinement->last->next = confinement->next;

      /* Clear the surface field in case some code later on relies on
	 that.  */
      confinement->surface = NULL;

      /* Remove the commit callback.  */
      if (confinement->commit_callback)
	XLSurfaceCancelCommitCallback (confinement->commit_callback);
      confinement->commit_callback = NULL;
    }

  /* Free all pointer barriers activated.  */
  XIDListFree (confinement->applied_barriers,
	       FreeSingleBarrier);
  confinement->applied_barriers = NULL;

  /* Free lines if they are set.  */
  XLFree (confinement->lines);
  confinement->lines = NULL;

  /* Free the seat key.  */
  if (confinement->seat_key)
    XLSeatCancelDestroyListener (confinement->seat_key);

  /* Free the confinement.  */
  if (confinement->region)
    pixman_region32_fini (confinement->region);
  XLFree (confinement->region);

  /* And any pending region.  */
  if (confinement->pending_region)
    pixman_region32_fini (confinement->pending_region);
  XLFree (confinement->pending_region);

  /* Free the resource data.  */
  XLFree (confinement);
}

static void
HandleSeatDestroyed (void *data)
{
  PointerConfinement *confinement;

  /* Since the seat is gone, it no longer makes any sense to keep the
     confinement around any longer.  Clear the surface field and
     remove the confinement from the surface.  */

  confinement = data;

  /* Deactivate the confinement.  */
  if (confinement->flags & IsActive)
    DeactivateConfinement (confinement);

  if (confinement->surface)
    {
      /* Detach the confinement.  */
      confinement->next->last = confinement->last;
      confinement->last->next = confinement->next;

      /* Clear the confinement surface field.  */
      confinement->surface = NULL;

      /* Cancel the commit callback.  */
      if (confinement->commit_callback)
	XLSurfaceCancelCommitCallback (confinement->commit_callback);
      confinement->commit_callback = NULL;
    }

  confinement->seat = NULL;
  confinement->seat_key = NULL;

  /* Free all pointer barriers previously activated.  */
  XIDListFree (confinement->applied_barriers,
	       FreeSingleBarrier);
  confinement->applied_barriers = NULL;
}



static void
RecheckPointerConfinement (Seat *seat, PointerConfinement *confinement)
{
  Surface *surface;
  double x, y, root_x, root_y;

  XLSeatGetMouseData (seat, &surface, &x, &y, &root_x, &root_y);

  if (surface == confinement->surface)
    /* Check if the surface contains the pointer barrier.  */
    XLPointerBarrierCheck (seat, surface, x, y, root_x, root_y);
  else if (!surface && confinement->flags & IsActive)
    /* The pointer is not in that surface anymore, and it is
       active.  */
    DeactivateConfinement (confinement);
}

static PointerConfinement *
FindConfinement (PointerConfinementDataRecord *data, Seat *seat)
{
  PointerConfinement *confinement;

  confinement = data->confinements.next;
  while (confinement != &data->confinements)
    {
      if (confinement->seat == seat)
	return confinement;
    }

  return NULL;
}

static void
LockPointer (struct wl_client *client, struct wl_resource *resource,
	     uint32_t id, struct wl_resource *surface_resource,
	     struct wl_resource *pointer_resource,
	     struct wl_resource *region_resource, uint32_t lifetime)
{
  PointerConfinement *confinement;
  PointerConfinementDataRecord *data;
  Surface *surface;
  Pointer *pointer;
  Seat *seat;
  struct wl_resource *dummy_resource;

  /* Pointer locking is implemented very similarly to pointer
     constraints.  The major difference is that reporting pointer
     events is not allowed when the lock is in effect, and the
     constraint rectangle is always 1x1 around the pointer.  */

  surface = wl_resource_get_user_data (surface_resource);
  pointer = wl_resource_get_user_data (pointer_resource);
  seat = XLPointerGetSeat (pointer);

  /* If seat is inert, then the confinement will never trigger.
     Simply create a blank zwp_locked_pointer_v1 resource that never
     does anything.  */
  if (XLSeatIsInert (seat))
    {
      dummy_resource = wl_resource_create (client,
					   &zwp_locked_pointer_v1_interface,
					   wl_resource_get_version (resource),
					   id);

      if (!dummy_resource)
	wl_resource_post_no_memory (resource);
      else
	wl_resource_set_implementation (dummy_resource, &locked_pointer_impl,
					NULL, NULL);

      return;
    }

  data = XLSurfaceGetClientData (surface, PointerConfinementData,
				 sizeof *data,
				 FreePointerConfinementDataRecord);

  /* Potentially initialize confinement data.  */
  InitConfinementData (data);

#define AlreadyConstrained				\
  ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED

  if (FindConfinement (data, seat))
    {
      wl_resource_post_error (resource, AlreadyConstrained,
			      "pointer constraint already requested on"
			      " the given surface");
      return;
    }

#undef AlreadyConstrained

  if (lifetime != ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT
      && lifetime != ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT)
    {
      /* The lifetime specified is invalid.  */
      wl_resource_post_error (resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			      "invalid constraint lifetime");
      return;
    }

  confinement = XLCalloc (1, sizeof *confinement);

  /* Try to create the locked pointer resource.  */
  confinement->resource
    = wl_resource_create (client, &zwp_locked_pointer_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!confinement->resource)
    {
      wl_resource_post_no_memory (resource);
      XLFree (confinement);
      return;
    }

  if (region_resource)
    {
      /* Copy over the confinement region.  */
      confinement->region = XLMalloc (sizeof *confinement->region);
      pixman_region32_init (confinement->region);
      pixman_region32_copy (confinement->region,
			    wl_resource_get_user_data (region_resource));
    }

  /* Mark this as a lock.  */
  confinement->flags |= IsLock;

  if (lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT)
    confinement->flags |= IsOneShot;

  /* Attach the surface.  */
  confinement->surface = surface;

  /* Attach the seat.  */
  confinement->seat = seat;
  confinement->seat_key
    = XLSeatRunOnDestroy (seat, HandleSeatDestroyed, confinement);

  /* Link the confinement onto the list.  */
  confinement->next = data->confinements.next;
  confinement->last = &data->confinements;
  data->confinements.next->last = confinement;
  data->confinements.next = confinement;

  /* Attach the resource implementation.  */
  wl_resource_set_implementation (confinement->resource,
				  &locked_pointer_impl,
				  confinement,
				  HandleResourceDestroy);

  /* Check if the pointer can be confined.  */
  RecheckPointerConfinement (seat, confinement);
}

static void
ConfinePointer (struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *surface_resource,
		struct wl_resource *pointer_resource,
		struct wl_resource *region_resource, uint32_t lifetime)
{
  PointerConfinement *confinement;
  PointerConfinementDataRecord *data;
  Surface *surface;
  Pointer *pointer;
  Seat *seat;
  struct wl_resource *dummy_resource;

  surface = wl_resource_get_user_data (surface_resource);
  pointer = wl_resource_get_user_data (pointer_resource);
  seat = XLPointerGetSeat (pointer);

  /* If seat is inert, then the confinement will never trigger.
     Simply create a blank zwp_confined_pointer_v1 resource that never
     does anything.  */
  if (XLSeatIsInert (seat))
    {
      dummy_resource = wl_resource_create (client,
					   &zwp_confined_pointer_v1_interface,
					   wl_resource_get_version (resource),
					   id);

      if (!dummy_resource)
	wl_resource_post_no_memory (resource);
      else
	wl_resource_set_implementation (dummy_resource, &confined_pointer_impl,
					NULL, NULL);

      return;
    }

  data = XLSurfaceGetClientData (surface, PointerConfinementData,
				 sizeof *data,
				 FreePointerConfinementDataRecord);

  /* Potentially initialize the confinement data.  */
  InitConfinementData (data);

#define AlreadyConstrained			\
  ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED

  if (FindConfinement (data, seat))
    {
      wl_resource_post_error (resource, AlreadyConstrained,
			      "pointer constraint already requested on"
			      " the given surface");
      return;
    }

#undef AlreadyConstrained

  if (lifetime != ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT
      && lifetime != ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT)
    {
      /* The lifetime specified is invalid.  */
      wl_resource_post_error (resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			      "invalid constraint lifetime");
      return;
    }

  confinement = XLCalloc (1, sizeof *confinement);

  /* Try to create the confined pointer resource.  */
  confinement->resource
    = wl_resource_create (client, &zwp_confined_pointer_v1_interface,
			  wl_resource_get_version (resource), id);

  if (!confinement->resource)
    {
      wl_resource_post_no_memory (resource);
      XLFree (confinement);
      return;
    }

  if (region_resource)
    {
      /* Copy over the confinement region.  */
      confinement->region = XLMalloc (sizeof *confinement->region);
      pixman_region32_init (confinement->region);
      pixman_region32_copy (confinement->region,
			    wl_resource_get_user_data (region_resource));
    }

  if (lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT)
    confinement->flags |= IsOneShot;

  /* Attach the surface.  */
  confinement->surface = surface;

  /* Attach the seat.  */
  confinement->seat = seat;
  confinement->seat_key
    = XLSeatRunOnDestroy (seat, HandleSeatDestroyed, confinement);

  /* Link the confinement onto the list.  */
  confinement->next = data->confinements.next;
  confinement->last = &data->confinements;
  data->confinements.next->last = confinement;
  data->confinements.next = confinement;

  /* Attach the resource implementation.  */
  wl_resource_set_implementation (confinement->resource,
				  &confined_pointer_impl,
				  confinement,
				  HandleResourceDestroy);

  /* Check if the pointer can be confined.  */
  RecheckPointerConfinement (seat, confinement);
}

static struct zwp_pointer_constraints_v1_interface pointer_constraints_impl =
  {
    .destroy = Destroy,
    .lock_pointer = LockPointer,
    .confine_pointer = ConfinePointer,
  };

static void
HandleBind (struct wl_client *client, void *data, uint32_t version,
	    uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &zwp_pointer_constraints_v1_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &pointer_constraints_impl,
				  NULL, NULL);
}

#ifdef DEBUG

static GC
GetDebugGC (Window window)
{
  static GC gc;
  XGCValues gcvalues;
  XColor color;

  if (gc)
    return gc;

  color.red = 0;
  color.green = 0;
  color.blue = 0;

  if (!XAllocColor (compositor.display, compositor.colormap,
		    &color))
    abort ();

  gcvalues.foreground = color.pixel;
  gcvalues.line_width = 1;
  gcvalues.subwindow_mode = IncludeInferiors;

  gc = XCreateGC (compositor.display, window,
		  GCForeground | GCLineWidth | GCSubwindowMode,
		  &gcvalues);
  return gc;
}

#endif

static void
ApplyLines (Window window, PointerConfinement *confinement,
	    BarrierLine *lines, int nlines, int root_x, int root_y)
{
  int i, device_id;
  PointerBarrier barrier;
#ifdef DEBUG
  GC gc;

  gc = GetDebugGC (window);
#endif

  /* Free all pointer barriers previously activated.  */
  XIDListFree (confinement->applied_barriers,
	       FreeSingleBarrier);
  confinement->applied_barriers = NULL;

  /* Set the pointer device.  */
  device_id = XLSeatGetPointerDevice (confinement->seat);

  if (nlines == 1 && lines[0].edges == AllEdgesClosed)
    {
      /* lines[0] is a perfect rectangle, and also the only rectangle.
	 The X server has trouble keeping the mouse confined to really
	 accurate barriers, so in this case create 4 intersecting
	 barriers where each barrier extends along its blocking axis
	 to the edges of the screen.  */

      /* Top.  */
      barrier
	= XFixesCreatePointerBarrier (compositor.display, window,
				      Int16Minimum, root_y + lines[0].y1,
				      Int16Maximum, root_y + lines[0].y1,
				      BarrierPositiveY, 1, &device_id);

      confinement->applied_barriers
	= XIDListPrepend (confinement->applied_barriers,
			  barrier);

      /* Bottom.  */
      barrier
	= XFixesCreatePointerBarrier (compositor.display, window,
				      Int16Minimum, root_y + lines[0].y2 - 1,
				      Int16Maximum, root_y + lines[0].y2 - 1,
				      BarrierNegativeY, 1, &device_id);

      confinement->applied_barriers
	= XIDListPrepend (confinement->applied_barriers,
			  barrier);

      /* Left.  */
      barrier
	= XFixesCreatePointerBarrier (compositor.display, window,
				      root_x + lines[0].x1, Int16Minimum,
				      root_x + lines[0].x1, Int16Maximum,
				      BarrierPositiveX, 1, &device_id);

      confinement->applied_barriers
	= XIDListPrepend (confinement->applied_barriers,
			  barrier);

      /* Right.  */
      barrier
	= XFixesCreatePointerBarrier (compositor.display, window,
				      root_x + lines[0].x2 - 1, Int16Minimum,
				      root_x + lines[0].x2 - 1, Int16Maximum,
				      BarrierNegativeX, 1, &device_id);

      confinement->applied_barriers
	= XIDListPrepend (confinement->applied_barriers,
			  barrier);

      return;
    }

  /* For each rectangle, draw the lines that are set.  */
  for (i = 0; i < nlines; ++i)
    {
      /* Due to X server bugs, this fails to constrain the pointer
	 reliably.  */

      if (lines[i].edges & TopEdgeClosed)
	{
	  barrier = XFixesCreatePointerBarrier (compositor.display,
						window,
						root_x + lines[i].x1,
						root_y + lines[i].y1,
						root_x + lines[i].x2 - 1,
						root_y + lines[i].y1,
						BarrierPositiveY,
						1, &device_id);

#ifdef DEBUG
	  XDrawLine (compositor.display, window, gc, lines[i].x1,
		     lines[i].y1, lines[i].x2 - 1, lines[i].y1);
#endif

	  confinement->applied_barriers
	    = XIDListPrepend (confinement->applied_barriers,
			      barrier);
	}

      if (lines[i].edges & LeftEdgeClosed)
	{
	  barrier = XFixesCreatePointerBarrier (compositor.display,
						window,
						root_x + lines[i].x1,
						root_y + lines[i].y1,
						root_x + lines[i].x1,
						root_y + lines[i].y2 - 1,
						BarrierPositiveX,
						1, &device_id);
#ifdef DEBUG
	  XDrawLine (compositor.display, window, gc, lines[i].x1,
		     lines[i].y1, lines[i].x1, lines[i].y2 - 1);
#endif

	  confinement->applied_barriers
	    = XIDListPrepend (confinement->applied_barriers,
			      barrier);
	}

      if (lines[i].edges & RightEdgeClosed)
	{
	  barrier = XFixesCreatePointerBarrier (compositor.display,
						window,
						root_x + lines[i].x2 - 1,
						root_y + lines[i].y1,
						root_x + lines[i].x2 - 1,
						root_y + lines[i].y2 - 1,
						BarrierNegativeX,
						1, &device_id);
#ifdef DEBUG
	  XDrawLine (compositor.display, window, gc, lines[i].x2 - 1,
		     lines[i].y1, lines[i].x2 - 1, lines[i].y2 - 1);
#endif

	  confinement->applied_barriers
	    = XIDListPrepend (confinement->applied_barriers,
			      barrier);
	}

      if (lines[i].edges & BottomEdgeClosed)
	{
	  barrier = XFixesCreatePointerBarrier (compositor.display,
						window,
						root_x + lines[i].x1,
						root_y + lines[i].y2 - 1,
						root_x + lines[i].x2 - 1,
						root_y + lines[i].y2 - 1,
						BarrierNegativeY,
						1, &device_id);
#ifdef DEBUG
	  XDrawLine (compositor.display, window, gc, lines[i].x1,
		     lines[i].y2 - 1, lines[i].x2 - 1, lines[i].y2 - 1);
#endif

	  confinement->applied_barriers
	    = XIDListPrepend (confinement->applied_barriers,
			      barrier);
	}
    }
}

static Bool
DrawPointerBarriers (PointerConfinement *confinement,
		     pixman_region32_t *region,
		     int *root_x_return, int *root_y_return)
{
  BarrierLine *lines;
  int nlines, root_x, root_y;
  Window window, child;

  XLFree (confinement->lines);
  confinement->lines = NULL;
  confinement->nlines = 0;

  if (!confinement->surface)
    return False;

  window = XLWindowFromSurface (confinement->surface);

  /* Decompose the region into rectangles that can contain up to 4
     lines.  */
  lines = ComputeBarrier (region, &nlines);

  if (!lines)
    return False;

  if (root_x_return && root_y_return
      && *root_x_return != INT_MIN
      && *root_y_return != INT_MIN)
    {
      /* This mechanism is simply used to avoid redundant syncs when
	 recursively processing subsurfaces.  */
      root_x = *root_x_return;
      root_y = *root_y_return;
    }
  else
    {
      /* Obtain the root-window relative coordinates of the window.  */
      XTranslateCoordinates (compositor.display, window,
			     DefaultRootWindow (compositor.display),
			     0, 0, &root_x, &root_y, &child);

      if (root_x_return)
	*root_x_return = root_x;

      if (root_y_return)
	*root_y_return = root_y;
    }

  /* Apply the lines.  */
  ApplyLines (window, confinement, lines, nlines, root_x, root_y);

  /* Set the lines.  */
  confinement->lines = lines;
  confinement->nlines = nlines;

  return True;
}

static void
DrawLock (PointerConfinement *confinement, double root_x_subpixel,
	  double root_y_subpixel)
{
  Window window;
  int device_id, root_x, root_y;
  PointerBarrier barrier;

  root_x = lrint (root_x_subpixel);
  root_y = lrint (root_y_subpixel);

  /* Draw a lock, meaning a 1x1 rectangle around root_x and root_y.
     Truncate root_x and root_y, like the X server.  This serves to
     prevent the cursor from moving onscreen.  */

  window = XLWindowFromSurface (confinement->surface);

  /* Free all pointer barriers previously activated.  */
  XIDListFree (confinement->applied_barriers,
	       FreeSingleBarrier);
  confinement->applied_barriers = NULL;

  /* Set the pointer device.  */
  device_id = XLSeatGetPointerDevice (confinement->seat);

  /* Top.  */
  barrier = XFixesCreatePointerBarrier (compositor.display, window,
					Int16Minimum, root_y,
					Int16Maximum, root_y,
					BarrierPositiveY, 1, &device_id);

  confinement->applied_barriers
    = XIDListPrepend (confinement->applied_barriers,
		      barrier);

  /* Bottom.  */
  barrier = XFixesCreatePointerBarrier (compositor.display, window,
					Int16Minimum, root_y + 1,
					Int16Maximum, root_y + 1,
					BarrierNegativeY, 1, &device_id);

  confinement->applied_barriers
    = XIDListPrepend (confinement->applied_barriers,
		      barrier);

  /* Left.  */
  barrier = XFixesCreatePointerBarrier (compositor.display, window,
				        root_x, Int16Minimum,
					root_x, Int16Maximum,
					BarrierPositiveX, 1, &device_id);
  confinement->applied_barriers
    = XIDListPrepend (confinement->applied_barriers,
		      barrier);

  /* Right.  */
  barrier = XFixesCreatePointerBarrier (compositor.display, window,
				        root_x + 1, Int16Minimum,
					root_x + 1, Int16Maximum,
					BarrierNegativeX, 1, &device_id);
  confinement->applied_barriers
    = XIDListPrepend (confinement->applied_barriers,
		      barrier);

  /* Set the last root_x and root_y.  */
  confinement->root_x = root_x;
  confinement->root_y = root_y;

  /* Warp the pointer to root_x by root_y, after rounding it.  */
  XIWarpPointer (compositor.display, device_id, None,
		 DefaultRootWindow (compositor.display),
		 0.0, 0.0, 0.0, 0.0, root_x, root_y);
}

static void
WarpToHint (PointerConfinement *confinement)
{
  int offset_x, offset_y;
  Window window, child;
  int root_x, root_y, device_id;

  if (!confinement->surface || !confinement->seat)
    return;

  window = XLWindowFromSurface (confinement->surface);
  device_id = XLSeatGetPointerDevice (confinement->seat);

  if (!window)
    return;

  ViewTranslate (confinement->surface->view, 0, 0, &offset_x,
		 &offset_y);
  XTranslateCoordinates (compositor.display, window,
			 DefaultRootWindow (compositor.display),
			 0, 0, &root_x, &root_y, &child);

  /* Warp the pointer to the right position.  */
  XIWarpPointer (compositor.display, device_id, None,
		 window, 0.0, 0.0, 0.0, 0.0,
		 confinement->cursor_position_x - offset_x,
		 confinement->cursor_position_y - offset_y);
}

static void
DeactivateConfinement (PointerConfinement *confinement)
{
  confinement->flags &= ~IsActive;
  XIDListFree (confinement->applied_barriers,
	       FreeSingleBarrier);
  confinement->applied_barriers = NULL;

  /* Free lines if they are set.  */
  XLFree (confinement->lines);
  confinement->lines = NULL;
  confinement->nlines = 0;

  /* Unlock the pointer on the seat.  */
  if (confinement->seat)
    XLSeatUnlockPointer (confinement->seat);

  /* Send unconfined.  */
  if (confinement->flags & IsLock)
    {
      zwp_locked_pointer_v1_send_unlocked (confinement->resource);

      /* Apply the cursor position hint if it is set.  */
      if (confinement->flags & IsCursorPositionHintSet)
	WarpToHint (confinement);
    }
  else
    zwp_confined_pointer_v1_send_unconfined (confinement->resource);

  /* If the confinement is one shot, mark it as dead.  */
  if (confinement->flags & IsOneShot)
    confinement->flags |= IsDead;
}

static void
RecomputeConfinement (PointerConfinement *confinement, int *root_x,
		      int *root_y)
{
  Surface *surface;
  pixman_region32_t intersection;
  int offset_x, offset_y;

  /* This should not be called for locks, which must not require
     reconfinement.  */

  surface = confinement->surface;
  pixman_region32_init (&intersection);

  if (confinement->region)
    pixman_region32_intersect (&intersection, confinement->region,
			       &surface->current_state.input);
  else
    pixman_region32_copy (&intersection, &surface->current_state.input);

  /* Scale the intersection to window coordinates.  */
  XLScaleRegion (&intersection, &intersection, surface->factor,
		 surface->factor);

  /* Intersect with the view bounds.  */
  pixman_region32_intersect_rect (&intersection, &intersection,
				  0, 0, ViewWidth (surface->view),
				  ViewHeight (surface->view));

  /* Translate the region by the offset of the view into the
     subcompositor.  */
  ViewTranslate (surface->view, 0, 0, &offset_x, &offset_y);
  pixman_region32_translate (&intersection, -offset_x, -offset_y);

  /* Send the confined message.  */
  zwp_confined_pointer_v1_send_confined (confinement->resource);

  /* Draw each pointer barrier for confinement.  */
  if (!DrawPointerBarriers (confinement, &intersection,
			    root_x, root_y))
    /* Rendering the confinement failed.  This is one of the
       oddball regions that require too much memory to
       process.  */
    DeactivateConfinement (confinement);

  pixman_region32_fini (&intersection);
}

static void
RewarpPointer (PointerConfinement *confinement, int *root_x_return,
	       int *root_y_return)
{
  int offset_x, offset_y;
  Window window, child;
  int root_x, root_y;

  /* Warp the pointer back to its position in the surface, to keep it
     locked.  */
  XLAssert (confinement->surface != NULL);

  /* Get the subcompositor window.  */
  window = XLWindowFromSurface (confinement->surface);

  if (!window)
    return;

  /* First, get the offset of the view into the subcompositor.  */
  ViewTranslate (confinement->surface->view, 0, 0, &offset_x,
		 &offset_y);

  if (root_x_return && root_y_return
      && *root_x_return != INT_MIN
      && *root_y_return != INT_MIN)
    {
      /* This mechanism is simply used to avoid redundant syncs when
	 recursively processing subsurfaces.  */
      root_x = *root_x_return;
      root_y = *root_y_return;
    }
  else
    {
      /* Obtain the root-window relative coordinates of the window.  */
      XTranslateCoordinates (compositor.display, window,
			     DefaultRootWindow (compositor.display),
			     0, 0, &root_x, &root_y, &child);

      if (root_x_return)
	*root_x_return = root_x;

      if (root_y_return)
	*root_y_return = root_y;
    }

  /* And lock the pointer to the right spot.  */
  DrawLock (confinement,
	    confinement->last_cursor_x - offset_x + root_x,
	    confinement->last_cursor_y - offset_y + root_y);
}

static void
Reconfine (Surface *surface, int *root_x, int *root_y,
	   Bool process_subsurfaces)
{
  PointerConfinementDataRecord *record;
  PointerConfinement *confinement;
  XLList *tem;

  /* If the subsurface is attached to some surface without a window,
     return.  */
  if (!XLWindowFromSurface (surface))
    return;

  record = XLSurfaceFindClientData (surface, PointerConfinementData);

  if (!record)
    return;

  confinement = record->confinements.next;

  while (confinement != &record->confinements)
    {
      if (confinement->flags & IsActive)
	{
	  if (!(confinement->flags & IsLock))
	    RecomputeConfinement (confinement, root_x, root_y);
	  else
	    RewarpPointer (confinement, root_x, root_y);
	}

      confinement = confinement->next;
    }

  if (!process_subsurfaces)
    return;

  /* Process subsurfaces recursively as well.  */
  for (tem = surface->subsurfaces; tem; tem = tem->next)
    Reconfine (surface, root_x, root_y, True);
}

void
XLPointerBarrierLeft (Seat *seat, Surface *surface)
{
  PointerConfinementDataRecord *record;
  PointerConfinement *confinement;

  /* The pointer has now left the given surface.  If there is an
     active confinement for that surface and seat, disable it.  */

  record = XLSurfaceFindClientData (surface, PointerConfinementData);

  if (!record)
    return;

  confinement = FindConfinement (record, seat);

  if (confinement && confinement->flags & IsActive)
    DeactivateConfinement (confinement);
}

void
XLPointerBarrierCheck (Seat *seat, Surface *dispatch, double x, double y,
		       double root_x, double root_y)
{
  PointerConfinement *confinement;
  PointerConfinementDataRecord *record;
  pixman_region32_t intersection;
  pixman_box32_t box;
  int offset_x, offset_y;

  record = XLSurfaceFindClientData (dispatch, PointerConfinementData);

  if (!record)
    return;

  confinement = FindConfinement (record, seat);

  if (!confinement)
    return;

  if (confinement->flags & IsDead)
    /* The confinement is a 1 shot confinement that has been used
       up.  */
    return;

  /* Initialize the confinement region.  */
  pixman_region32_init (&intersection);

  if (confinement->region)
    pixman_region32_intersect (&intersection, confinement->region,
			       &dispatch->current_state.input);
  else
    pixman_region32_copy (&intersection, &dispatch->current_state.input);

  /* Scale the intersection to window coordinates.  */
  XLScaleRegion (&intersection, &intersection, dispatch->factor,
		 dispatch->factor);

  /* If X and Y are in the pointer confinement area, then activate the
     confinement.  */
  if (pixman_region32_contains_point (&intersection, x, y, &box))
    {
      if (!(confinement->flags & IsActive))
	{
	  /* Intersect with the view bounds.  This is done after
	     pixman_region32_contains_point because X and Y must be
	     within the correct bounds for this function to be called
	     in the first place.  */
	  pixman_region32_intersect_rect (&intersection, &intersection,
					  0, 0, ViewWidth (dispatch->view),
					  ViewHeight (dispatch->view));

	  /* Translate the region by the offset of the view into the
	     subcompositor.  */
	  ViewTranslate (dispatch->view, 0, 0, &offset_x, &offset_y);
	  pixman_region32_translate (&intersection, -offset_x, -offset_y);

	  /* Activate the confinement.  Set the IsActive flag.  */
	  confinement->flags |= IsActive;

	  /* Send the confined message.  */
	  if (confinement->flags & IsLock)
	    {
	      zwp_locked_pointer_v1_send_locked (confinement->resource);

	      /* Lock the seat on the pointer.  */
	      XLSeatLockPointer (confinement->seat);

	      /* Draw the lock.  */
	      DrawLock (confinement, root_x, root_y);
	      confinement->last_cursor_x = x;
	      confinement->last_cursor_y = y;
	    }
	  else
	    {
	      zwp_confined_pointer_v1_send_confined (confinement->resource);

	      /* Draw each pointer barrier for confinement.  */
	      if (!DrawPointerBarriers (confinement, &intersection,
					NULL, NULL))
		/* Rendering the confinement failed.  This is one of the
		   oddball regions that require too much memory to
		   process.  */
		DeactivateConfinement (confinement);
	    }
	}
      else if (confinement->flags & IsLock)
	{
	  /* This is a mess, but so are the interactions between
	     Xfixes and subpixel pointer movement... */
	  if ((confinement->root_x - root_x) < 1.0
	      && (confinement->root_x - root_x) > -1.0
	      && (confinement->root_y - root_y) < 1.0
	      && (confinement->root_y - root_y) > -1.0
	      && confinement->applied_barriers)
	    goto finish;

	  /* The pointer moved while locked; redraw all the locks.  */
	  DrawLock (confinement, root_x, root_y);
	  confinement->last_cursor_x = x;
	  confinement->last_cursor_y = y;
	}
    }
  else if (confinement->flags & IsActive)
    /* The pointer moved out of the active confinement.  Deactivate
       it.  */
    DeactivateConfinement (confinement);

 finish:
  pixman_region32_fini (&intersection);
}

/* Motion functions.  These functions notice that the position of a
   surface has changed.  One is called when a surface with an X window
   that accepts input changes its position relative to the root window.

   The other is called when a subsurface changes its position relative
   to the parent.

   Both functions then look up any active constraints and move them
   accordingly.  */

void
XLPointerConstraintsSurfaceMovedTo (Surface *surface, int root_x,
				    int root_y)
{
  PointerConfinementDataRecord *record;
  PointerConfinement *confinement;
  Window window;
  XLList *tem;

  /* Since root_x and root_y are already known, there is no need to
     query for the position manually.  Simply move the lines for the
     surface's window and each of its subsurfaces.  */

  record = XLSurfaceFindClientData (surface, PointerConfinementData);

  if (!record)
    return;

  window = XLWindowFromSurface (surface);

  confinement = record->confinements.next;
  while (confinement != &record->confinements)
    {
      /* Reapply the lines with the new root window coordinates.  */

      if (confinement->lines)
	ApplyLines (window, confinement, confinement->lines,
		    confinement->nlines, root_x, root_y);
      else if (confinement->flags & IsActive
	       && confinement->flags & IsLock)
	/* Warp the pointer back to where it originally was relative
	   to the surface.  */
	RewarpPointer (confinement, &root_x, &root_y);

      /* Move to the next confinement.  */
      confinement = confinement->next;
    }

  /* Do the same for each subsurface.  */
  for (tem = surface->subsurfaces; tem; tem = tem->next)
    XLPointerConstraintsSurfaceMovedTo (surface, root_x, root_y);
}

void
XLPointerConstraintsSubsurfaceMoved (Surface *surface)
{
  int root_x, root_y;

  root_x = INT_MIN;
  root_y = INT_MIN;

  Reconfine (surface, &root_x, &root_y, True);
}

void
XLPointerConstraintsReconfineSurface (Surface *surface)
{
  int root_x, root_y;

  root_x = INT_MIN;
  root_y = INT_MIN;

  Reconfine (surface, &root_x, &root_y, False);
}

void
XLInitPointerConstraints (void)
{
  pointer_constraints_global
    = wl_global_create (compositor.wl_display,
			&zwp_pointer_constraints_v1_interface,
			1, NULL, HandleBind);
}
