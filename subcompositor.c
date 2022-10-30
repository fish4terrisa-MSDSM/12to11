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

#ifndef TEST

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <alloca.h>

#include "compositor.h"

#define TEST_STATIC
#else

typedef int Bool;

#define True	1
#define False	0

typedef struct _View View;
typedef struct _List List;
typedef struct _Subcompositor Subcompositor;

#define TEST_STATIC static

#endif

/* This module implements a "subcompositor" that composites together
   the contents of hierarchies of "views", each of which have attached
   ExtBuffers and other assorted state.

   Each view has a parent (which can be the subcompositor itself), and
   a list of children, which is sorted according to Z order.  In
   addition to the list of children of the subcompositor itself, every
   view in the subcompositor is stored in a single doubly-linked list,
   ordered implicitly according to the in which every inferior (direct
   or indirect children of the subcompositor) will be composited.
   This list is updated whenever a new view is inserted or the Z order
   or parent of one of the views change.

   For example, assume the subcompositor has the following children:

			  [A]      [B]     [C]
			   |        |       |
			[D] [E]  [F] [G] [H] [I]

   Then, the contents of the list will be:

              [A], [D], [E], [B], [F], [G], [C], [H], [I]

   To aid in updating the linked list, each view maintains a pointer
   to the link in the list containing the view itself, and the link
   containing the last inferior (direct or indirect children of the
   view) of the view.  So, in the above example, the view "A" will
   also point to:

               + = link pointer of "A"
                         + = last inferior pointer of "A"
              [A], [D], [E], [B], [F], [G], [C], [H], [I]

   To add view to another view, the view is first appended to the end
   of the other view's list of children, and the links between its
   link and its last inferior link are linked after its last inferior
   link.  Finally, the other view and each of its parents is iterated
   through, and the last inferior pointer is updated to the last
   inferior link of the view that was inserted if it is equal to the
   other view's original last inferior pointer.

   If a view named "J" with no children were to be inserted at the end
   of "A", then "J" would first be added to the end of "A"'s list of
   children, creating such a hierarchy:

				   [A]
				    |
			       [D] [E] [J]

   Then, "J"'s link and inferior pointers would be inserted after "E"
   (where + represents the current location of "A"'s last inferior
   pointer), resulting in the subcompositor's list of inferiors
   looking like this:

            +              * = link pointer of "J"
                      +    * = last inferior pointer of "J"
           [A], [D], [E], [J], [B], [F], [G], [C], [H], [I]

   Finally, the inferior pointer of each of "E"'s parents that
   previously pointed to "E" is updated, like so:

            +               *
                           +*
           [A], [D], [E], [J], [B], [F], [G], [C], [H], [I]

   A similar procedure applies to adding a view to the subcompositor
   itself.

   Unparenting a view (thereby removing it from the view hierarchy) is
   is done by unlinking the implicitly-formed list between the view's
   link pointer and the view's last inferior pointer from its
   surroundings, and removing it from its parent's list of children.
   This in turn creates a separate, implicitly-formed list, that
   allows for view hierarchy operations to be performed on a detached
   view.  Unlinking "A" from the above hierarchy would produce two
   separate lists:

            +               *
                           +*
           [A], [D], [E], [J] = the implicit sub-list of "A"
                               [B], [F], [G], [C], [H], [I] = the
			       subcompositor inferior list

   Finally, the inferior pointer of all parents pointing to the
   unparented view's inferior pointer are updated to the
   next-bottom-most sibling view's inferior pointer.  This cannot be
   demonstrated using the chart above, since "A" is a toplevel.

   Unlike the Wayland protocol itself, this does not support placing
   children of a view before the view itself.  That is implemented
   manually by moving such children to a separate sibling of the
   parent that is always stacked below that view.  */

enum
  {
    /* This means that the view hierarchy has changed, and all
       subcompositing optimisations should be skipped.  */
    SubcompositorIsGarbaged	   = 1,
    /* This means that the opaque region of one of the views
       changed.  */
    SubcompositorIsOpaqueDirty	   = (1 << 2),
    /* This means that the input region of one of the views
       changed.  */
    SubcompositorIsInputDirty	   = (1 << 3),
    /* This means that there is at least one unmapped view in this
       subcompositor.  */
    SubcompositorIsPartiallyMapped = (1 << 4),
    /* This means that the subcompositor has a target attached.  */
    SubcompositorIsTargetAttached  = (1 << 5),
  };

#define IsGarbaged(subcompositor)				\
  ((subcompositor)->state & SubcompositorIsGarbaged)
#define SetGarbaged(subcompositor)				\
  ((subcompositor)->state |= SubcompositorIsGarbaged)

#define SetOpaqueDirty(subcompositor)				\
  ((subcompositor)->state |= SubcompositorIsOpaqueDirty)
#define IsOpaqueDirty(subcompositor)				\
  ((subcompositor)->state & SubcompositorIsOpaqueDirty)

#define SetInputDirty(subcompositor)				\
  ((subcompositor)->state |= SubcompositorIsInputDirty)
#define IsInputDirty(subcompositor)				\
  ((subcompositor)->state & SubcompositorIsInputDirty)

#define SetPartiallyMapped(subcompositor)			\
  ((subcompositor)->state |= SubcompositorIsPartiallyMapped)
#define IsPartiallyMapped(subcompositor)			\
  ((subcompositor)->state & SubcompositorIsPartiallyMapped)

#define SetTargetAttached(subcompositor)			\
  ((subcompositor)->state |= SubcompositorIsTargetAttached)
#define IsTargetAttached(subcompositor)				\
  ((subcompositor)->state & SubcompositorIsTargetAttached)

#ifndef TEST

enum
  {
    /* This means that the view and all its inferiors should be
       skipped in bounds computation, input tracking, et cetera.  */
    ViewIsUnmapped   = 1,
    /* This means that the view itself (not including its inferiors)
       should be skipped for bounds computation and input
       tracking, etc.  */
    ViewIsSkipped    = 1 << 2,
    /* This means that the view has a viewport specifying its size,
       effectively decoupling its relation to the buffer width	and
       height.  */
    ViewIsViewported = 1 << 3,
  };

#define IsViewUnmapped(view)			\
  ((view)->flags & ViewIsUnmapped)
#define SetUnmapped(view)			\
  ((view)->flags |= ViewIsUnmapped)
#define ClearUnmapped(view)			\
  ((view)->flags &= ~ViewIsUnmapped)

#define IsSkipped(view)				\
  ((view)->flags & ViewIsSkipped)
#define SetSkipped(view)			\
  ((view)->flags |= ViewIsSkipped)
#define ClearSkipped(view)			\
  ((view)->flags &= ~ViewIsSkipped)

#define IsViewported(view)			\
  ((view)->flags & ViewIsViewported)
#define SetViewported(view)			\
  ((view)->flags |= ViewIsViewported)
#define ClearViewported(view)			\
  ((view)->flags &= ~ViewIsViewported)

#endif

struct _List
{
  /* Pointer to the next element of this list.
     This list itself if this is the sentinel link.  */
  List *next;

  /* Pointer to the last element of this list.
     This list itself if this is the sentinel link.  */
  List *last;

  /* The view of this list.  */
  View *view;
};

struct _View
{
  /* Subcompositor this view belongs to.  NULL at first; callers are
     supposed to call ViewSetSubcompositor before inserting a view
     into a compositor.  */
  Subcompositor *subcompositor;

  /* Pointer to the parent view.  NULL if the parent is the
     subcompositor itself.  */
  View *parent;

  /* Pointer to the link containing the view itself.  */
  List *link;

  /* Pointer to another such link used in the view hierarchy.  */
  List *self;

  /* Pointer to the link containing the view's last inferior.  */
  List *inferior;

  /* List of children.  */
  List *children;

  /* The end of that list.  */
  List *children_last;

  /* Buffer data.  */

#ifndef TEST
  /* Width and height of the view.  Not valid until
     ViewAfterSizeUpdate!  */
  int width, height;

  /* The buffer associated with this view, or None if nothing is
     attached.  */
  ExtBuffer *buffer;

  /* Function called upon the view potentially being resized.  */
  void (*maybe_resized) (View *);

  /* Some data associated with this view.  Can be a surface or
     something else.  */
  void *data;

  /* Culling data; this is not valid after drawing completes.  */
  pixman_region32_t *cull_region;

  /* The damaged and opaque regions.  */
  pixman_region32_t damage, opaque;

  /* The input region.  */
  pixman_region32_t input;

  /* The position of this view relative to its parent.  */
  int x, y;

  /* The absolute position of this view relative to the subcompositor
     (or topmost parent if the view hierarchy is detached).  */
  int abs_x, abs_y;

  /* The scale of this view.  */
  int scale;

  /* Flags; whether or not this view is unmapped, etc.  */
  int flags;

  /* Any transform associated with this view.  */
  BufferTransform transform;

  /* The viewport data.  */
  double src_x, src_y, crop_width, crop_height, dest_width, dest_height;

  /* Fractional offset applied to the view contents and damage during
     compositing.  */
  double fract_x, fract_y;
#else
  /* Label used during tests.  */
  const char *label;
#endif
};

struct _Subcompositor
{
  /* List of all inferiors in compositing order.  */
  List *inferiors, *last;

  /* Toplevel children of this subcompositor.  */
  List *children, *last_children;

#ifndef TEST
  /* Target this subcompositor draws to.  */
  RenderTarget target;

  /* Function called when the opaque region changes.  */
  void (*opaque_change) (Subcompositor *, void *,
			 pixman_region32_t *);

  /* Function called when the input region changes.  */
  void (*input_change) (Subcompositor *, void *,
			pixman_region32_t *);

  /* Function called with the bounds before each update.  */
  void (*note_bounds) (void *, int, int, int, int);

  /* Function called with the frame counter on each update.  */
  void (*note_frame) (FrameMode, uint64_t, void *);

  /* The current frame counter, incremented with each frame.  */
  uint64_t frame_counter;

  /* Data for those three functions.  */
  void *opaque_change_data, *input_change_data, *note_bounds_data;

  /* Data for the fourth.  */
  void *note_frame_data;

  /* Buffers used to store that damage.  */
  pixman_region32_t prior_damage[2];

  /* Any additional damage to be applied to the subcompositor.  */
  pixman_region32_t additional_damage;

  /* The damage region of previous updates.  last_damage is what the
     damage region was 1 update ago, and before_damage is what the
     damage region was 2 updates ago.  */
  pixman_region32_t *last_damage, *before_damage;

  /* The last attached presentation callback, if any.  */
  PresentCompletionKey present_key;

  /* The last attached render completion callback, if any.  */
  RenderCompletionKey render_key;

  /* The minimum origin of any surface in this subcompositor.  Used to
     compute the actual size of the subcompositor.  */
  int min_x, min_y;

  /* The maximum position of any surface in this subcompositor.  Used
     to compute the actual size of the subcompositor.  */
  int max_x, max_y;

  /* An additional offset to apply when drawing to the target.  */
  int tx, ty;
#endif

  /* Various flags describing the state of this subcompositor.  */
  int state;
};

#ifndef TEST

enum
  {
    DoMinX = 1,
    DoMinY = (1 << 1),
    DoMaxX = (1 << 2),
    DoMaxY = (1 << 3),
    DoAll  = 0xf,
  };

#endif


/* Circular doubly linked list of views.  These lists work unusually:
   for example, only some lists have a "sentinel" node at the
   beginning with the value NULL.  This is so that sub-lists can be
   extracted from them without consing.  */

static List *
ListInit (View *value)
{
  List *link;

  link = XLCalloc (1, sizeof *link);
  link->next = link;
  link->last = link;
  link->view = value;

  return link;
}

static void
ListRelinkAfter (List *start, List *end, List *dest)
{
  end->next = dest->next;
  start->last = dest;

  dest->next->last = end;
  dest->next = start;
}

static void
ListInsertAfter (List *after, List *item)
{
  ListRelinkAfter (item, item, after);
}

static void
ListInsertBefore (List *before, List *item)
{
  ListRelinkAfter (item, item, before->last);
}

static void
ListRelinkBefore (List *start, List *end, List *dest)
{
  ListRelinkAfter (start, end, dest->last);
}

/* Unlink the list between START and END from their surroundings.
   Then, turn START and END into a proper list.  This requires that
   START is not the sentinel node.  */

static void
ListUnlink (List *start, List *end)
{
  /* First, make the list skip past END.  */
  start->last->next = end->next;
  end->next->last = start->last;

  /* Then, unlink the list.  */
  start->last = end;
  end->next = start;
}

TEST_STATIC Subcompositor *
MakeSubcompositor (void)
{
  Subcompositor *subcompositor;

  subcompositor = XLCalloc (1, sizeof *subcompositor);
  subcompositor->inferiors = ListInit (NULL);
  subcompositor->children = ListInit (NULL);

  subcompositor->last = subcompositor->inferiors;
  subcompositor->last_children = subcompositor->children;

  /* Initialize the buffers used to store previous damage.  */
  pixman_region32_init (&subcompositor->prior_damage[0]);
  pixman_region32_init (&subcompositor->prior_damage[1]);

  /* And the buffer used to store additional damage.  */
  pixman_region32_init (&subcompositor->additional_damage);

  return subcompositor;
}

TEST_STATIC View *
MakeView (void)
{
  View *view;

  view = XLCalloc (1, sizeof *view);
  view->subcompositor = NULL;
  view->parent = NULL;

  /* Note that view->link is not supposed to have a sentinel; it can
     only be part of a larger list.  */
  view->link = ListInit (view);
  view->inferior = view->link;

  /* Likewise for view->self.  */
  view->self = ListInit (view);

  /* But view->children is a complete list by itself.  */
  view->children = ListInit (NULL);
  view->children_last = view->children;

#ifndef TEST
  view->buffer = NULL;

  pixman_region32_init (&view->damage);
  pixman_region32_init (&view->opaque);
  pixman_region32_init (&view->input);

  view->transform = Normal;
#endif

  return view;
}

#ifndef TEST

static int
ViewMaxX (View *view)
{
  return view->abs_x + ViewWidth (view) - 1;
}

static int
ViewMaxY (View *view)
{
  return view->abs_y + ViewHeight (view) - 1;
}

static Bool
ViewIsMapped (View *view)
{
  if (view->subcompositor
      && !IsPartiallyMapped (view->subcompositor))
    return True;

  if (IsViewUnmapped (view))
    return False;

  if (view->parent)
    return ViewIsMapped (view->parent);

  return True;
}

static void
SubcompositorUpdateBounds (Subcompositor *subcompositor, int doflags)
{
  List *list;
  int min_x, min_y, max_x, max_y;
  int old_min_x, old_min_y, old_max_x, old_max_y;

  /* Updates were optimized out.  */
  if (!doflags)
    return;

  list = subcompositor->inferiors->next;
  min_x = max_x = min_y = max_y = 0;
  old_min_x = subcompositor->min_x;
  old_min_y = subcompositor->min_y;
  old_max_x = subcompositor->max_x;
  old_max_y = subcompositor->max_y;

  while (list != subcompositor->inferiors)
    {
      if (list->view)
	{
	  /* If the view is unmapped, skip past its children.  */
	  if (IsViewUnmapped (list->view))
	    {
	      list = list->view->inferior;
	      goto next;
	    }

	  if (IsSkipped (list->view))
	    /* Skip past the view itself should it be skipped.  */
	    goto next;

	  if ((doflags & DoMinX) && min_x > list->view->abs_x)
	    min_x = list->view->abs_x;

	  if ((doflags & DoMinY) && min_x > list->view->abs_y)
	    min_y = list->view->abs_y;

	  if ((doflags & DoMaxX) && max_x < ViewMaxX (list->view))
	    max_x = ViewMaxX (list->view);

	  if ((doflags & DoMaxY) && max_y < ViewMaxY (list->view))
	    max_y = ViewMaxY (list->view);
	}

    next:
      list = list->next;
    }

  if (doflags & DoMinX)
    subcompositor->min_x = min_x;

  if (doflags & DoMinY)
    subcompositor->min_y = min_y;

  if (doflags & DoMaxX)
    subcompositor->max_x = max_x;

  if (doflags & DoMaxY)
    subcompositor->max_y = max_y;

  if (subcompositor->min_x != old_min_x
      || subcompositor->min_y != old_min_y
      || subcompositor->max_x != old_max_x
      || subcompositor->max_y != old_max_y)
    SetGarbaged (subcompositor);
}

static void
SubcompositorUpdateBoundsForInsert (Subcompositor *subcompositor,
				    View *view)
{
  XLAssert (view->subcompositor == subcompositor);

  if (!ViewIsMapped (view) || IsSkipped (view))
    /* If the view is unmapped, do nothing.  */
    return;

  /* Inserting a view cannot shrink the subcompositor.  */

  if (view->abs_x < subcompositor->min_x)
    {
      subcompositor->min_x = view->abs_x;

      /* Garbage the subcompositor for this change.  */
      SetGarbaged (subcompositor);
    }

  if (view->abs_x < view->subcompositor->min_y)
    {
      subcompositor->min_y = view->abs_y;

      /* Garbage the subcompositor for this change.  */
      SetGarbaged (subcompositor);
    }

  if (view->subcompositor->max_x < ViewMaxX (view))
    {
      subcompositor->max_x = ViewMaxX (view);

      /* Garbage the subcompositor for this change.  */
      SetGarbaged (subcompositor);
    }

  if (view->subcompositor->max_y < ViewMaxY (view))
    {
      subcompositor->max_y = ViewMaxY (view);

      /* Garbage the subcompositor for this change.  */
      SetGarbaged (subcompositor);
    }
}

#endif

#ifndef TEST

void
SubcompositorSetTarget (Subcompositor *compositor,
			RenderTarget *target_in)
{
  if (target_in)
    {
      compositor->target = *target_in;
      SetTargetAttached (compositor);
    }
  else
    compositor->state &= SubcompositorIsTargetAttached;

  /* We don't know if the new picture has the previous state left
     over.  */
  SetGarbaged (compositor);
}

#endif

#define SkipSlug(list, view, next)				\
  {								\
    if (!list->view)						\
      goto next;						\
								\
    if (IsViewUnmapped (list->view))				\
      {								\
	/* Skip the unmapped view.  */				\
	list = list->view->inferior;				\
	SetPartiallyMapped (subcompositor);			\
	goto next;						\
      }								\
								\
    if (IsSkipped (list->view))					\
      {								\
	/* We must skip this view, as it represents (for	\
	   instance) a subsurface that has been added, but not	\
	   committed.  */					\
	SetPartiallyMapped (subcompositor);			\
	goto next;						\
      }								\
								\
    if (!list->view->buffer)					\
      goto next;						\
								\
    view = list->view;						\
  }								\

static void
ViewUnionInferiorBounds (View *parent, pixman_region32_t *region)
{
  List *list;
  View *view;
  Subcompositor *subcompositor;

  /* Return the bounds of each of VIEW's inferiors in REGION.  */
  list = parent->link;
  subcompositor = parent->subcompositor;

  while (True)
    {
      SkipSlug (list, view, next);

      /* Union the view bounds with the given region.  */
      pixman_region32_union_rect (region, region, view->abs_x,
				  view->abs_y, view->width,
				  view->height);

    next:

      if (list == parent->inferior)
	/* Break if we are at the end of the list.  */
	break;

      list = list->next;
    }
}

static void
DamageIncludingInferiors (View *parent)
{
  List *list;
  View *view;
  Subcompositor *subcompositor;

  if (parent->subcompositor)
    /* No subcompositor is attached... */
    return;

  pixman_region32_union_rect (&parent->damage, &parent->damage,
			      0, 0, parent->width, parent->height);

  /* Now, damage each inferior.  */
  list = parent->link;
  subcompositor = parent->subcompositor;

  while (True)
    {
      SkipSlug (list, view, next);

      /* Union the view damage with its bounds.  */
      pixman_region32_union_rect (&view->damage, &view->damage,
				  view->abs_x, view->abs_y,
				  view->width, view->height);

    next:

      if (list == parent->inferior)
	/* Break if we are at the end of the list.  */
	break;

      list = list->next;
    }
}

TEST_STATIC void
SubcompositorInsert (Subcompositor *compositor, View *view)
{
  /* Link view into the list of children.  */
  ListInsertBefore (compositor->last_children, view->self);

  /* Make view's inferiors part of the compositor.  */
  ListRelinkBefore (view->link, view->inferior,
		    compositor->last);

#ifndef TEST
  /* And update bounds.  */
  SubcompositorUpdateBoundsForInsert (compositor, view);

  /* Now, if the subcompositor is still not garbaged, damage each
     inferior of the view.  */
  if (!IsGarbaged (compositor))
    DamageIncludingInferiors (view);
#endif
}

TEST_STATIC void
SubcompositorInsertBefore (Subcompositor *compositor, View *view,
			   View *sibling)
{
  /* Link view into the list of children, before the given
     sibling.  */
  ListInsertBefore (sibling->self, view->self);

  /* Make view's inferiors part of the compositor.  */
  ListRelinkBefore (view->link, view->inferior, sibling->link);

#ifndef TEST
  /* And update bounds.  */
  SubcompositorUpdateBoundsForInsert (compositor, view);

  /* Now, if the subcompositor is still not garbaged, damage each
     inferior of the view.  */
  if (!IsGarbaged (compositor))
    DamageIncludingInferiors (view);
#endif
}

TEST_STATIC void
SubcompositorInsertAfter (Subcompositor *compositor, View *view,
			  View *sibling)
{
  /* Link view into the list of children, after the given sibling.  */
  ListInsertAfter (sibling->self, view->self);

  /* Make view's inferiors part of the compositor.  */
  ListRelinkAfter (view->link, view->inferior, sibling->inferior);

#ifndef TEST
  /* And update bounds.  */
  SubcompositorUpdateBoundsForInsert (compositor, view);

  /* Now, if the subcompositor is still not garbaged, damage each
     inferior of the view.  */
  if (!IsGarbaged (compositor))
    DamageIncludingInferiors (view);
#endif
}

#ifndef TEST

static Bool
ViewVisibilityState (View *view, Bool *mapped)
{
  if (IsViewUnmapped (view) && mapped)
    {
      *mapped = False;

      /* Clear mapped, so it will never be set again.  */
      mapped = NULL;
    }

  if (view->parent)
    return ViewVisibilityState (view->parent, mapped);

  if (mapped)
    *mapped = !IsViewUnmapped (view);

  return view->link->next != view->link;
}

Bool
ViewIsVisible (View *view)
{
  Bool mapped;

  if (!ViewVisibilityState (view, &mapped))
    return False;

  if (IsSkipped (view))
    return False;

  return mapped;
}

static void
ViewRecomputeChildren (View *view, int *doflags)
{
  List *list;
  View *child;
  Bool attached, mapped;

  list = view->children;
  attached = ViewVisibilityState (view, &mapped);

  do
    {
      list = list->next;

      if (list->view)
	{
	  child = list->view;

	  child->abs_x = view->abs_x + child->x;
	  child->abs_y = view->abs_y + child->y;

	  if (view->subcompositor
	      /* Don't operate on the subcompositor should the view be
		 detached.  */
	      && attached
	      /* Or if it isn't mapped, or none of its parents are
		 mapped.  */
	      && mapped
	      /* Or if it is skipped.  */
	      && !IsSkipped (view))
	    {
	      if (child->abs_x < view->subcompositor->min_x)
		{
		  view->subcompositor->min_x = child->abs_x;

		  if (doflags)
		    *doflags &= ~DoMinX;
		}

	      if (child->abs_x < view->subcompositor->min_y)
		{
		  view->subcompositor->min_y = child->abs_y;

		  if (doflags)
		    *doflags &= ~DoMinY;
		}

	      if (view->subcompositor->max_x < ViewMaxX (child))
		{
		  view->subcompositor->max_x = ViewMaxX (child);

		  if (doflags)
		    *doflags &= ~DoMaxX;
		}

	      if (view->subcompositor->max_y < ViewMaxY (child))
		{
		  view->subcompositor->max_y = ViewMaxY (child);

		  if (doflags)
		    *doflags &= ~DoMaxY;
		}
	    }

	  ViewRecomputeChildren (child, doflags);
	}
    }
  while (list != view->children);
}

static void
ViewUpdateBoundsForInsert (View *view)
{
  if (view->subcompositor)
    SubcompositorUpdateBoundsForInsert (view->subcompositor,
					view);
}

#endif

TEST_STATIC void
ViewInsert (View *view, View *child)
{
  View *parent;
  List *prior;

  /* Make child's parent view.  */
  child->parent = view;

  /* Insert child into the hierarchy list.  */
  ListInsertBefore (view->children_last, child->self);

  /* Insert child's inferior list.  */
  ListRelinkAfter (child->link, child->inferior, view->inferior);

  /* Note what the previous last inferior pointer of view was.  */
  prior = view->inferior;

  /* Update the entire view hierarchy's inferior pointers, starting
     from view.  */
  for (parent = view; parent; parent = parent->parent)
    {
      /* The last inferior of this view has been changed already;
	 update it.  */
      if (parent->inferior == prior)
	parent->inferior = child->inferior;
    }

  /* Now that the view hierarchy has been changed, garbage the
     subcompositor.  */

#ifndef TEST
  /* Also update the absolute positions of the child.  */
  child->abs_x = view->abs_x + child->x;
  child->abs_y = view->abs_y + child->y;
  ViewRecomputeChildren (child, NULL);

  /* And update bounds.  */
  ViewUpdateBoundsForInsert (view);

  /* Now, if the subcompositor is still not garbaged, damage each
     inferior of the view.  */
  if (view->subcompositor
      && !IsGarbaged (view->subcompositor))
    DamageIncludingInferiors (view);
#endif
}

TEST_STATIC void
ViewInsertAfter (View *view, View *child, View *sibling)
{
  View *parent;
  List *prior;

  /* Make child's parent view.  */
  child->parent = view;

  /* Insert child into the hierarchy list.  */
  ListInsertAfter (sibling->self, child->self);

  /* Insert child's inferior list.  */
  ListRelinkAfter (child->link, child->inferior,
		   sibling->inferior);

  /* Change the inferior pointers if sibling->inferior was the old
     one.  */

  if (sibling->inferior == view->inferior)
    {
      /* Note what the previous last inferior pointer of view was.  */
      prior = sibling->inferior;

      /* Update the entire view hierarchy's inferior pointers, starting
	 from view.  */
      for (parent = view; parent; parent = parent->parent)
	{
	  /* The last inferior of this view has been changed already;
	     update it.  */
	  if (parent->inferior == prior)
	    parent->inferior = child->inferior;
	}
    }

#ifndef TEST
  /* Also update the absolute positions of the child.  */
  child->abs_x = view->abs_x + child->x;
  child->abs_y = view->abs_y + child->y;
  ViewRecomputeChildren (child, NULL);

  /* And update bounds.  */
  ViewUpdateBoundsForInsert (view);

  /* Now, if the subcompositor is still not garbaged, damage each
     inferior of the view.  */
  if (view->subcompositor
      && !IsGarbaged (view->subcompositor))
    DamageIncludingInferiors (view);
#endif
}

TEST_STATIC void
ViewInsertBefore (View *view, View *child, View *sibling)
{
  /* Make child's parent view.  */
  child->parent = view;

  /* Insert child into the hierarchy list.  */
  ListInsertBefore (sibling->self, child->self);

  /* Insert child's inferior list.  */
  ListRelinkBefore (child->link, child->inferior,
		    sibling->link);

#ifndef TEST
  /* Also update the absolute positions of the child.  */
  child->abs_x = view->abs_x + child->x;
  child->abs_y = view->abs_y + child->y;
  ViewRecomputeChildren (child, NULL);

  /* Update subcompositor bounds.  Inserting a view cannot shrink
     anything.  */
  ViewUpdateBoundsForInsert (view);

  /* Now, if the subcompositor is still not garbaged, damage each
     inferior of the view.  */
  if (view->subcompositor
      && !IsGarbaged (view->subcompositor))
    DamageIncludingInferiors (view);
#endif

  /* Inserting inferiors before a sibling can never bump the inferior
     pointer.  */
}

TEST_STATIC void
ViewInsertStart (View *view, View *child)
{
  /* If view has no children, just call ViewInsert.  Note that
     view->children is a sentinel node whose value is NULL.  */
  if (view->children->next == view->children)
    ViewInsert (view, child);
  else
    /* Otherwise, insert child before the first child.  */
    ViewInsertBefore (view, child,
		      view->children->next->view);
}

TEST_STATIC void
ViewUnparent (View *child)
{
  View *parent;
  Bool mapped, attached;
  pixman_region32_t damage;

  /* See if the view is attached or not.  */
  attached = (ViewVisibilityState (child, &mapped)
	      && mapped);

  if (attached && child->subcompositor)
    {
      /* Init the damage region.  */
      pixman_region32_init (&damage);

      /* And store what additional damage should be applied for this
	 unparent.  */
      ViewUnionInferiorBounds (child, &damage);
    }

  /* Parent is either the subcompositor or another view.  */
  ListUnlink (child->self, child->self);

  if (child->parent)
    {
      /* Now update the inferior pointer of each parent currently
	 pointing to child->inferior to the inferior of its leftmost
	 sibling, or its parent itself.  */

      for (parent = child->parent; parent; parent = parent->parent)
	{
	  if (parent->inferior == child->inferior)
	    /* If this is the bottom-most child, then
	       child->link->last will be the parent itself.  */
	    parent->inferior = child->link->last;
	}

      /* And reset the pointer to the parent.  */
      child->parent = NULL;
    }

  /* Unlink the sub-list between the link pointer and the last
     inferior pointer from that of the parent.  */
  ListUnlink (child->link, child->inferior);

  /* Reset the absolute positions of child, and recompute that of its
     children.  This is done after unlinking, because
     ViewRecomputeChildren will otherwise try to operate on the
     subcompositor.  */
#ifndef TEST
  child->abs_x = child->x;
  child->abs_y = child->y;

  ViewRecomputeChildren (child, NULL);
#endif

  /* Now that the view hierarchy has been changed, garbage the
     subcompositor.  TODO: an optimization for removing views would be
     to damage each intersecting view before child->link instead, if
     view bounds did not change.  */
  if (child->subcompositor)
    {
#ifndef TEST
      /* Update the bounds of the subcompositor.  */
      SubcompositorUpdateBounds (child->subcompositor, DoAll);

      /* If the subcompositor is not garbaged, then apply additional
	 damage.  */
      if (attached && !IsGarbaged (child->subcompositor))
	pixman_region32_union (&child->subcompositor->additional_damage,
			       &child->subcompositor->additional_damage,
			       &damage);
#endif
    }

  if (attached && child->subcompositor)
    /* Finalize the damage region.  */
    pixman_region32_fini (&damage);
}

TEST_STATIC void
ViewSetSubcompositor (View *view, Subcompositor *subcompositor)
{
  List *list;

  list = view->link;

  /* Attach the subcompositor recursively for all of view's
     inferiors.  */

  do
    {
      if (list->view)
	list->view->subcompositor = subcompositor;

      list = list->next;
    }
  while (list != view->link);
}

#ifdef TEST

/* The depth of the current view being printed.  */

static int print_level;

static void
PrintView (View *view)
{
  List *list;

  printf ("%*c%s\n", print_level * 2, ' ',
	  view->label);

  print_level++;
  list = view->children;
  do
    {
      if (list->view)
	PrintView (list->view);
      list = list->next;
    }
  while (list != view->children);
  print_level--;
}

static void
PrintSubcompositor (Subcompositor *compositor)
{
  List *list;

  list = compositor->children;
  do
    {
      if (list->view)
	PrintView (list->view);
      list = list->next;
    }
  while (list != compositor->children);

  list = compositor->inferiors;
  do
    {
      if (list->view)
	printf ("[%s], ", list->view->label);
      list = list->next;
      fflush (stdout);
    }
  while (list != compositor->inferiors);

  fflush (stdout);
  printf ("\n");
  fflush (stdout);

  for (list = compositor->last->last;
       list != compositor->last; list = list->last)
    {
      if (list->view)
	printf ("(%s), ", list->view->label);
      fflush (stdout);
    }
  printf ("\n");
  fflush (stdout);
}

static View *
TestView (Subcompositor *compositor, const char *label)
{
  View *view;

  view = MakeView ();
  view->label = label;

  ViewSetSubcompositor (view, compositor);

  return view;
}

static void
TestSubcompositor (void)
{
  Subcompositor *compositor;
  View *a, *b, *c, *d, *e, *f, *g, *h, *i, *j;
  View *k, *l, *m, *n, *o, *p;

  compositor = MakeSubcompositor ();
  a = TestView (compositor, "A");
  b = TestView (compositor, "B");
  c = TestView (compositor, "C");
  d = TestView (compositor, "D");
  e = TestView (compositor, "E");
  f = TestView (compositor, "F");
  g = TestView (compositor, "G");
  h = TestView (compositor, "H");
  i = TestView (compositor, "I");
  j = TestView (compositor, "J");
  k = TestView (compositor, "K");
  l = TestView (compositor, "L");
  m = TestView (compositor, "M");
  n = TestView (compositor, "N");
  o = TestView (compositor, "O");
  p = TestView (compositor, "P");

  printf ("SubcompositorInsert (COMPOSITOR, A)\n");
  SubcompositorInsert (compositor, a);
  PrintSubcompositor (compositor);
  printf ("ViewInsert (A, D)\n");
  ViewInsert (a, d);
  PrintSubcompositor (compositor);
  printf ("ViewInsert (A, E)\n");
  ViewInsert (a, e);
  PrintSubcompositor (compositor);
  printf ("ViewInsert (B, F)\n");
  ViewInsert (b, f);
  printf ("ViewInsert (B, G)\n");
  ViewInsert (b, g);
  printf ("SubcompositorInsert (COMPOSITOR, B)\n");
  SubcompositorInsert (compositor, b);
  PrintSubcompositor (compositor);
  printf ("ViewInsert (C, H)\n");
  ViewInsert (c, h);
  printf ("SubcompositorInsert (COMPOSITOR, C)\n");
  SubcompositorInsert (compositor, c);
  PrintSubcompositor (compositor);
  printf ("ViewInsert (C, I)\n");
  ViewInsert (c, i);
  PrintSubcompositor (compositor);

  printf ("ViewInsert (A, J)\n");
  ViewInsert (a, j);
  PrintSubcompositor (compositor);

  printf ("ViewUnparent (A)\n");
  ViewUnparent (a);
  PrintSubcompositor (compositor);

  printf ("ViewUnparent (C)\n");
  ViewUnparent (c);
  PrintSubcompositor (compositor);

  printf ("ViewUnparent (G)\n");
  ViewUnparent (g);
  printf ("ViewUnparent (J)\n");
  ViewUnparent (j);
  printf ("ViewInsert (G, J)\n");
  ViewInsert (g, j);
  printf ("SubcompositorInsert (COMPOSITOR, G)\n");
  SubcompositorInsert (compositor, g);
  PrintSubcompositor (compositor);

  printf ("ViewInsertBefore (G, C, J)\n");
  ViewInsertBefore (g, c, j);
  PrintSubcompositor (compositor);

  printf ("ViewInsertAfter (C, A, H)\n");
  ViewInsertAfter (c, a, h);
  PrintSubcompositor (compositor);

  printf ("ViewInsert (K, L)\n");
  ViewInsert (k, l);

  printf ("SubcompositorInsertBefore (COMPOSITOR, K, G)\n");
  SubcompositorInsertBefore (compositor, k, g);
  PrintSubcompositor (compositor);

  printf ("SubcompositorInsertAfter (COMPOSITOR, M, B)\n");
  SubcompositorInsertAfter (compositor, m, b);
  PrintSubcompositor (compositor);

  printf ("ViewInsert (M, N)\n");
  ViewInsert (m, n);
  PrintSubcompositor (compositor);

  printf ("ViewInsertStart (M, O)\n");
  ViewInsertStart (m, o);
  PrintSubcompositor (compositor);

  printf ("ViewInsertStart (L, P)\n");
  ViewInsertStart (l, p);
  PrintSubcompositor (compositor);
}

int
main (int argc, char **argv)
{
  TestSubcompositor ();
}

#endif



#ifndef TEST

/* Notice that VIEW's size has changed, while VIEW itself has not
   moved.  Recompute the max_x, min_x, min_y, and max_y of its
   subcompositor.  In addition, run the view's resize function, if
   any.  */

static void
ViewAfterSizeUpdate (View *view)
{
  int doflags, old_width, old_height;
  Bool mapped;

  if (view->maybe_resized)
    view->maybe_resized (view);

  /* These are used to decide how to damage the subcompositor.  */
  old_width = view->width;
  old_height = view->height;

  /* Calculate view->width and view->height again.  */
  view->width = ViewWidth (view);
  view->height = ViewHeight (view);

  if (!view->subcompositor || !ViewVisibilityState (view, &mapped)
      || !mapped || IsSkipped (view))
    return;

  /* First, assume we will have to compute both max_x and max_y.  */
  doflags = DoMaxX | DoMaxY;

  /* If the view is now wider than max_x and/or max_y, update those
     now.  */

  if (view->subcompositor->max_x < ViewMaxX (view))
    {
      view->subcompositor->max_x = ViewMaxX (view);
      SetGarbaged (view->subcompositor);

      /* We don't have to update max_x anymore.  */
      doflags &= ~DoMaxX;
    }

  if (view->subcompositor->max_y < ViewMaxY (view))
    {
      view->subcompositor->max_y = ViewMaxY (view);
      SetGarbaged (view->subcompositor);

      /* We don't have to update max_x anymore.  */
      doflags &= ~DoMaxY;
    }

  /* Finally, update the bounds.  */
  SubcompositorUpdateBounds (view->subcompositor, doflags);

  /* If the subcompositor is not garbaged and the view shrunk, damage
     the subcompositor accordingly.  */
  if (!IsGarbaged (view->subcompositor)
      && (view->width < old_width
	  || view->height < old_height))
    pixman_region32_union_rect (&view->subcompositor->additional_damage,
				&view->subcompositor->additional_damage,
				view->abs_x, view->abs_y, old_width,
				old_height);
}

void
ViewAttachBuffer (View *view, ExtBuffer *buffer)
{
  ExtBuffer *old;

  old = view->buffer;
  view->buffer = buffer;

  if (!view->buffer && old && view->subcompositor)
    /* The view needs a size update, as it is now 0 by 0.  */
    ViewAfterSizeUpdate (view);
  else if (((buffer && !old)
	    || (old && !buffer)
	    || (buffer && old
		&& (XLBufferWidth (buffer) != XLBufferWidth (old)
		    || XLBufferHeight (buffer) != XLBufferHeight (old))))
	   && !IsViewported (view))
    /* Recompute view and subcompositor bounds.  */
    ViewAfterSizeUpdate (view);

  if (buffer && IsViewUnmapped (view))
    {
      /* A buffer is now attached.  Automatically map the view, should
	 it be unmapped.  */
      ClearUnmapped (view);

      if (view->subcompositor)
	{
	  /* Recompute subcompositor bounds.  */
	  SubcompositorUpdateBounds (view->subcompositor, DoAll);

	  /* Garbage the subcompositor.  */
	  SetGarbaged (view->subcompositor);
	}
    }

  if (old)
    XLDereferenceBuffer (old);

  if (view->buffer)
    XLRetainBuffer (buffer);
}

void
ViewMove (View *view, int x, int y)
{
  pixman_region32_t damage;
  int doflags;
  Bool mapped;

  doflags = 0;

  if (x != view->x || y != view->y)
    {
      pixman_region32_init (&damage);

      view->x = x;
      view->y = y;

      if (view->parent)
	{
	  view->abs_x = view->parent->abs_x + x;
	  view->abs_y = view->parent->abs_y + y;
	}
      else
	{
	  view->abs_x = x;
	  view->abs_y = x;
	}

      if (view->subcompositor && ViewVisibilityState (view, &mapped)
	  /* If this view isn't mapped or is skipped, then do nothing.
	     The bounds will be recomputed later.  */
	  && mapped && !IsSkipped (view))
	{
	  /* First assume everything will have to be updated.  */
	  doflags |= DoMaxX | DoMaxY | DoMinY | DoMinX;

	  /* If this view was moved before subcompositor.min_x and/or
	     subcompositor.min_y, don't recompute those values
	     unnecessarily.  */

	  if (view->abs_x < view->subcompositor->min_x)
	    {
	      view->subcompositor->min_x = view->abs_x;

	      /* min_x has already been updated so there is no need to
		 recompute it later.  */
	      doflags &= ~DoMinX;

	      /* Also garbage the subcompositor since the bounds
		 changed.  */
	      SetGarbaged (view->subcompositor);
	    }

	  if (view->abs_y < view->subcompositor->min_x)
	    {
	      view->subcompositor->min_y = view->abs_y;

	      /* min_y has already been updated so there is no need to
		 recompute it later.  */
	      doflags &= ~DoMinY;

	      /* Also garbage the subcompositor since the bounds
		 changed.  */
	      SetGarbaged (view->subcompositor);
	    }

	  /* If moving this biew bumps subcompositor.max_x and/or
	     subcompositor.max_y, don't recompute either.  */

	  if (view->subcompositor->max_x < ViewMaxX (view))
	    {
	      view->subcompositor->max_x = ViewMaxX (view);

	      /* max_x has been updated so there is no need to
		 recompute it later.  If a child is bigger, then
		 ViewRecomputeChildren will handle it as well.  */
	      doflags &= ~DoMaxX;

	      /* Also garbage the subcompositor since the bounds
		 changed.  */
	      SetGarbaged (view->subcompositor);
	    }

	  if (view->subcompositor->max_y < ViewMaxX (view))
	    {
	      view->subcompositor->max_y = ViewMaxX (view);

	      /* max_y has been updated so there is no need to
		 recompute it later.  If a child is bigger, then
		 ViewRecomputeChildren will handle it as well.  */
	      doflags &= ~DoMaxY;

	      /* Also garbage the subcompositor since the bounds
		 changed.  */
	      SetGarbaged (view->subcompositor);
	    }
	}

      /* If the subcompositor is not garbaged, then damage the union
	 of the previous view bounds and the current view bounds.  */
      if (view->subcompositor)
	{
	  if (!IsGarbaged (view->subcompositor))
	    ViewUnionInferiorBounds (view, &damage);

	  /* Update the subcompositor bounds.  */
	  SubcompositorUpdateBounds (view->subcompositor, doflags);

	  /* Now calculate the absolute position for this view and all of
	     its children.  N.B. that this operation can also update
	     subcompositor.min_x or subcompositor.min_y.  */
	  ViewRecomputeChildren (view, &doflags);

	  /* If the subcompositor is still not garbaged, union damage
	     the rest of the way and apply it.  */
	  if (!IsGarbaged (view->subcompositor))
	    {
	      ViewUnionInferiorBounds (view, &damage);

	      pixman_region32_union (&view->subcompositor->additional_damage,
				     &view->subcompositor->additional_damage,
				     &damage);
	    }
	}
      else
	/* Now calculate the absolute position for this view and all
	   of its children.  */
	ViewRecomputeChildren (view, &doflags);

      pixman_region32_fini (&damage);
    }
}

void
ViewMoveFractional (View *view, double x, double y)
{
  XLAssert (x < 1.0 && y < 1.0);

  if (view->fract_x == x || view->fract_y == y)
    return;

  /* This does not necessitate adjustments to the view size, but does
     require that the view be redrawn.  */
  view->fract_x = x;
  view->fract_y = y;

  if (view->subcompositor)
    /* Damage the entire view.  */
    pixman_region32_union_rect (&view->damage, &view->damage,
				0, 0, view->width, view->height);
}

void
ViewDetach (View *view)
{
  ViewAttachBuffer (view, NULL);
}

void
ViewMap (View *view)
{
  if (!IsViewUnmapped (view))
    return;

  ClearUnmapped (view);

  if (view->subcompositor
      && (view->link != view->inferior || view->buffer))
    {
      /* Garbage the subcompositor and recompute bounds, if something
	 is attached to the view or it is not empty.  */
      SetGarbaged (view->subcompositor);
      SubcompositorUpdateBounds (view->subcompositor, DoAll);
    }
}

void
ViewUnmap (View *view)
{
  if (IsViewUnmapped (view))
    return;

  /* Mark the view as unmapped.  */
  SetUnmapped (view);

  if (view->subcompositor)
    {
      /* Mark the subcompositor as having unmapped views.  */
      SetPartiallyMapped (view->subcompositor);

      /* If the link pointer is the inferior pointer and there is no
	 buffer attached to the view, it is empty.  There is no need
	 to do anything other than marking the subcompositor as
	 partially mapped.  */
      if (view->link != view->inferior || view->buffer)
	{
	  /* Recompute the bounds of the subcompositor.  */
	  SubcompositorUpdateBounds (view->subcompositor,
				     DoAll);

	  /* Garbage the view's subcompositor.  */
	  SetGarbaged (view->subcompositor);
	}
    }
}

void
ViewUnskip (View *view)
{
  if (!IsSkipped (view))
    return;

  ClearSkipped (view);

  if (view->subcompositor && view->buffer)
    /* Damage the whole view bounds.  */
    pixman_region32_union_rect (&view->damage, &view->damage,
				view->abs_x, view->abs_y,
				view->width, view->height);
}

void
ViewSkip (View *view)
{
  if (IsSkipped (view))
    return;

  /* Mark the view as skipped.  */
  SetSkipped (view);

  if (view->subcompositor)
    {
      /* Mark the subcompositor as having unmapped or skipped
	 views.  */
      SetPartiallyMapped (view->subcompositor);

      /* If nothing is attached, the subcompositor need not be
	 garbaged.  */
      if (view->buffer)
	{
	  /* Recompute the bounds of the subcompositor.  */
	  SubcompositorUpdateBounds (view->subcompositor,
				     DoAll);

	  /* Garbage the view's subcompositor.  */
	  SetGarbaged (view->subcompositor);
	}
    }
}

void
ViewFree (View *view)
{
  /* It's not valid to call this function on a view with children or a
     parent.  */
  XLAssert (view->link == view->inferior);
  XLAssert (view->link->last == view->link);

  if (view->buffer)
    ViewDetach (view);

  XLFree (view->link);
  XLFree (view->self);
  XLFree (view->children);

  pixman_region32_fini (&view->damage);
  pixman_region32_fini (&view->opaque);
  pixman_region32_fini (&view->input);

  XLFree (view);
}

/* Forward declarations.  */

static void ApplyBufferDamage (View *, pixman_region32_t *);
static void ApplyUntransformedDamage (View *, pixman_region32_t *);

void
ViewDamage (View *view, pixman_region32_t *damage)
{
  /* This damage must be transformed by the viewport and scale, but
     must NOT be transformed by the subpixel (fractional) offset.  */
  pixman_region32_union (&view->damage, &view->damage, damage);

  /* Update any attached buffer with the given damage.  */
  if (view->buffer)
    ApplyBufferDamage (view, damage);
}

static double
GetContentScale (int scale)
{
  if (scale > 0)
    return 1.0 / (scale + 1);

  return -scale + 1;
}

static int
BufferWidthAfterTransform (View *view)
{
  if (RotatesDimensions (view->transform))
    return XLBufferHeight (view->buffer);

  return XLBufferWidth (view->buffer);
}

static int
BufferHeightAfterTransform (View *view)
{
  if (RotatesDimensions (view->transform))
    return XLBufferWidth (view->buffer);

  return XLBufferHeight (view->buffer);
}

static void
TransformBufferDamage (pixman_region32_t *damage,
		       pixman_region32_t *source,
		       View *view)
{
  int width, height;

  /* Calculate the width and height of the buffer after the
     transform.  */
  width = XLBufferWidth (view->buffer);
  height = XLBufferHeight (view->buffer);

  /* Transform the damage.  */
  XLTransformRegion (damage, source, view->transform,
		     width, height);
}

void
ViewDamageBuffer (View *view, pixman_region32_t *damage)
{
  pixman_region32_t temp;
  double x_factor, y_factor;
  double crop_width, stretch_width;
  double crop_height, stretch_height;

  if (!view->buffer)
    return;

  if (view->transform == Normal
      && !view->scale && !IsViewported (view))
    /* There is no scale, transform, nor viewport.  Just damage the
       view directly.  */
    ViewDamage (view, damage);
  else
    {
      /* Otherwise, apply the transform to the view.  */
      pixman_region32_init (&temp);

      /* First, apply the content scale.  */
      x_factor = GetContentScale (view->scale);
      y_factor = GetContentScale (view->scale);

      if (view->transform != Normal)
	{
	  /* Transform the given buffer damage if need be.  */
	  TransformBufferDamage (&temp, damage, view);

	  /* Scale the region.  */
	  XLScaleRegion (&temp, &temp, x_factor, y_factor);
	}
      else
	/* Scale the region.  */
	XLScaleRegion (&temp, damage, x_factor, y_factor);

      /* Next, apply the viewport.  */
      if (IsViewported (view))
	{
	  crop_width = view->crop_width;
	  crop_height = view->crop_height;
	  stretch_width = view->dest_width;
	  stretch_height = view->dest_height;

	  /* Offset the region.  */
	  if (view->src_x != 1.0 || view->src_y != 1.0)
	    pixman_region32_translate (&temp, -view->src_x,
				       -view->src_y);

	  /* If the crop width or height were not specified, use the
	     current buffer width/height.  */
	  if (crop_width == -1)
	    {
	      crop_width = (BufferWidthAfterTransform (view)
			    * GetContentScale (view->scale));
	      crop_height = (BufferHeightAfterTransform (view)
			     * GetContentScale (view->scale));
	    }

	  x_factor = stretch_width / crop_width;
	  y_factor = stretch_height / crop_height;

	  /* Scale the region again.  */
	  XLScaleRegion (&temp, &temp, x_factor, y_factor);
	}

      /* Damage the view.  */
      pixman_region32_union (&view->damage, &view->damage, &temp);
      pixman_region32_fini (&temp);

      /* Apply the untransformed damage directly.  */
      ApplyUntransformedDamage (view, damage);
    }
}

void
ViewSetOpaque (View *view, pixman_region32_t *opaque)
{
  pixman_region32_copy (&view->opaque, opaque);

  if (view->subcompositor)
    SetOpaqueDirty (view->subcompositor);
}

void
ViewSetInput (View *view, pixman_region32_t *input)
{
  if (pixman_region32_equal (input, &view->input))
    return;

  pixman_region32_copy (&view->input, input);

  if (view->subcompositor)
    SetInputDirty (view->subcompositor);
}

Subcompositor *
ViewGetSubcompositor (View *view)
{
  return view->subcompositor;
}

double
ViewGetContentScale (View *view)
{
  return GetContentScale (view->scale);
}

int
ViewWidth (View *view)
{
  int width;

  if (!view->buffer)
    return 0;

  if (IsViewported (view))
    /* The view has a viewport specified.  view->dest_width and
       view->dest_height can be fractional values.  When that happens,
       we simply use the ceiling and rely on the renderer to DTRT with
       scaling.  */
    return ceil (view->dest_width);

  width = BufferWidthAfterTransform (view);

  if (view->scale < 0)
    return ceil (width * (abs (view->scale) + 1));
  else
    return ceil (width / (view->scale + 1));
}

int
ViewHeight (View *view)
{
  int height;

  if (!view->buffer)
    return 0;

  if (IsViewported (view))
    /* The view has a viewport specified.  view->dest_width and
       view->dest_height can be fractional values.  When that happens,
       we simply use the ceiling and rely on the renderer to DTRT with
       scaling.  */
    return ceil (view->dest_height);

  height = BufferHeightAfterTransform (view);

  if (view->scale < 0)
    return ceil (height * (abs (view->scale) + 1));
  else
    return ceil (height / (view->scale + 1));
}

void
ViewSetScale (View *view, int scale)
{
  if (view->scale == scale)
    return;

  view->scale = scale;

  /* Recompute subcompositor bounds; they could've changed.  */
  ViewAfterSizeUpdate (view);

  /* The scale of the view changed, so prior damage cannot be trusted
     any longer.  */
  pixman_region32_union_rect (&view->damage, &view->damage,
			      0, 0, view->width, view->height);
}

void
ViewSetTransform (View *view, BufferTransform transform)
{
  BufferTransform old_transform;

  if (view->transform == transform)
    return;

  old_transform = view->transform;
  view->transform = transform;

  if (RotatesDimensions (transform)
      != RotatesDimensions (old_transform))
    /* Subcompositor bounds may have changed.  */
    ViewAfterSizeUpdate (view);

  /* The transform of the view changed, so prior damage cannot be
     trusted any longer.  */
  pixman_region32_union_rect (&view->damage, &view->damage,
			      0, 0, view->width, view->height);
}

void
ViewSetViewport (View *view, double src_x, double src_y,
		 double crop_width, double crop_height,
		 double dest_width, double dest_height)
{
  SetViewported (view);

  view->src_x = src_x;
  view->src_y = src_y;
  view->crop_width = crop_width;
  view->crop_height = crop_height;
  view->dest_width = dest_width;
  view->dest_height = dest_height;

  /* Update min_x and min_y.  */
  ViewAfterSizeUpdate (view);

  /* The transform of the view changed, so prior damage cannot be
     trusted any longer.  */
  pixman_region32_union_rect (&view->damage, &view->damage,
			      0, 0, view->width, view->height);
}

void
ViewClearViewport (View *view)
{
  ClearViewported (view);

  /* Update min_x and min_y.  */
  ViewAfterSizeUpdate (view);

  /* The transform of the view changed, so prior damage cannot be
     trusted any longer.  */
  pixman_region32_union_rect (&view->damage, &view->damage,
			      0, 0, view->width, view->height);
}

static void
ViewComputeTransform (View *view, DrawParams *params, Bool draw)
{
  /* Compute the effective transform of VIEW, then put it in PARAMS.
     DRAW means whether or not the transform is intended for drawing;
     when not set, the parameters are being used for damage tracking
     instead.  */

  /* First, there is no transform.  */
  params->flags = 0;
  params->off_x = 0.0;
  params->off_y = 0.0;

  if (view->transform != Normal)
    {
      params->flags |= TransformSet;
      params->transform = view->transform;
    }

  if (view->scale)
    {
      /* There is a scale, so set it.  */
      params->flags |= ScaleSet;
      params->scale = GetContentScale (view->scale);
    }

  if (IsViewported (view))
    {
      /* Set the viewport (a.k.a "stretch" and "offset" in the
	 rendering code).  */

      params->flags |= StretchSet;
      params->flags |= OffsetSet;

      params->off_x = view->src_x;
      params->off_y = view->src_y;
      params->crop_width = view->crop_width;
      params->stretch_width = view->dest_width;
      params->crop_height = view->crop_height;
      params->stretch_height = view->dest_height;

      /* If the crop width/height were not specified, use the current
	 buffer width/height.  */

      if (params->crop_width == -1)
	{
	  params->crop_width = (BufferWidthAfterTransform (view)
				* GetContentScale (view->scale));
	  params->crop_height = (BufferHeightAfterTransform (view)
				 * GetContentScale (view->scale));
	}
    }

  if ((view->fract_x != 0.0 || view->fract_y != 0.0)
      && draw)
    {
      params->flags |= OffsetSet;

      /* This is not entirely right.  When applying a negative offset,
	 contents to the left of where the picture actually is can
	 appear to "shine through".  */
      params->off_x -= view->fract_x;
      params->off_y -= view->fract_y;
    }
}

static void
ApplyBufferDamage (View *view, pixman_region32_t *damage)
{
  DrawParams params;
  RenderBuffer buffer;

  /* Compute the transform.  */
  ViewComputeTransform (view, &params, False);
  buffer = XLRenderBufferFromBuffer (view->buffer);

  /* Upload the buffer contents.  */
  RenderUpdateBufferForDamage (buffer, damage, &params);
}

static void
ApplyUntransformedDamage (View *view, pixman_region32_t *buffer_damage)
{
  RenderBuffer buffer;
  DrawParams params;

  buffer = XLRenderBufferFromBuffer (view->buffer);
  params.flags = 0;

  /* Upload the buffer contents.  */
  RenderUpdateBufferForDamage (buffer, buffer_damage, &params);
}

void
SubcompositorSetOpaqueCallback (Subcompositor *subcompositor,
				void (*opaque_changed) (Subcompositor *,
							void *,
							pixman_region32_t *),
				void *data)
{
  subcompositor->opaque_change = opaque_changed;
  subcompositor->opaque_change_data = data;
}

void
SubcompositorSetInputCallback (Subcompositor *subcompositor,
			       void (*input_changed) (Subcompositor *,
						      void *,
						      pixman_region32_t *),
			       void *data)
{
  subcompositor->input_change = input_changed;
  subcompositor->input_change_data = data;
}

void
SubcompositorSetBoundsCallback (Subcompositor *subcompositor,
				void (*note_bounds) (void *, int, int,
						     int, int),
				void *data)
{
  subcompositor->note_bounds = note_bounds;
  subcompositor->note_bounds_data = data;
}

void
SubcompositorSetNoteFrameCallback (Subcompositor *subcompositor,
				   void (*note_frame) (FrameMode, uint64_t,
						       void *),
				   void *data)
{
  subcompositor->note_frame = note_frame;
  subcompositor->note_frame_data = data;
}

void
SubcompositorBounds (Subcompositor *subcompositor,
		     int *min_x, int *min_y, int *max_x, int *max_y)
{
  *min_x = subcompositor->min_x;
  *min_y = subcompositor->min_y;
  *max_x = subcompositor->max_x;
  *max_y = subcompositor->max_y;
}

Bool
SubcompositorIsEmpty (Subcompositor *subcompositor)
{
  return (subcompositor->min_x == subcompositor->max_x
	  && subcompositor->min_y == subcompositor->max_y);
}

static void
StorePreviousDamage (Subcompositor *subcompositor,
		     pixman_region32_t *update_region)
{
  pixman_region32_t *prior;

  if (renderer_flags & NeverAges)
    /* Aging never happens, so recording prior damage is
       unnecessary.  */
    return;

  /* Move last_damage to prior_damage if it already exists, and find
     something to hold more damage and set it as last_damage.  There
     is no need to do this if the render target age never exceeds
     0.  */

  if (!subcompositor->last_damage)
    subcompositor->last_damage = &subcompositor->prior_damage[0];
  else if (!subcompositor->before_damage)
    {
      subcompositor->before_damage = subcompositor->last_damage;
      subcompositor->last_damage = &subcompositor->prior_damage[1];
    }
  else
    {
      prior = subcompositor->before_damage;
      subcompositor->before_damage = subcompositor->last_damage;
      subcompositor->last_damage = prior;
    }

  /* NULL means use the bounds of the subcompositor.  */
  if (!update_region)
    {
      pixman_region32_fini (subcompositor->last_damage);
      pixman_region32_init_rect (subcompositor->last_damage,
				 subcompositor->min_x,
				 subcompositor->min_y,
				 subcompositor->max_x,
				 subcompositor->max_y);
    }
  else
    /* Copy the update region to last_damage.  */
    pixman_region32_copy (subcompositor->last_damage,
			  update_region);
}

static void
PresentCompletedCallback (void *data)
{
  Subcompositor *subcompositor;

  subcompositor = data;

  /* The presentation callback should still be set here.  */
  XLAssert (subcompositor->present_key != NULL);
  subcompositor->present_key = NULL;

  /* Call the presentation callback if it is still set.  */
  if (subcompositor->note_frame)
    subcompositor->note_frame (ModePresented,
			       subcompositor->frame_counter,
			       subcompositor->note_frame_data);
}

static void
RenderCompletedCallback (void *data)
{
  Subcompositor *subcompositor;

  subcompositor = data;

  /* The render completion callback must still be set here.  */
  XLAssert (subcompositor->render_key != NULL);
  subcompositor->render_key = NULL;

  /* Call the frame function if it s still set.  */
  if (subcompositor->note_frame)
    subcompositor->note_frame (ModeComplete,
			       subcompositor->frame_counter,
			       subcompositor->note_frame_data);
}

/* Update ancillary data upon commit.  This includes the input and
   opaque regions.  */

static void
SubcompositorUpdateAncillary (Subcompositor *subcompositor)
{
  Bool update_input, update_opaque;
  List *list;
  View *view;
  pixman_region32_t input, opaque, temp;

  if (IsGarbaged (subcompositor))
    {
      update_opaque
	= subcompositor->opaque_change != NULL;
      update_input
	= subcompositor->input_change != NULL;

      pixman_region32_init (&input);
      pixman_region32_init (&opaque);
    }
  else
    {
      update_opaque = (IsOpaqueDirty (subcompositor)
		       && subcompositor->opaque_change);
      update_input = (IsInputDirty (subcompositor)
		      && subcompositor->input_change);

      if (update_input)
	pixman_region32_init (&input);

      if (update_opaque)
	pixman_region32_init (&opaque);
    }

  if (!update_input && !update_opaque)
    /* There is nothing to update.  */
    return;

  /* This is a temporary region used for some operations.  */
  pixman_region32_init (&temp);

  list = subcompositor->inferiors->next;

  while (list != subcompositor->inferiors)
    {
      SkipSlug (list, view, next);

      if (update_input)
	{
	  /* Add this view's input region to the total.  */
	  pixman_region32_intersect_rect (&temp, &view->input, 0, 0,
					  view->width, view->height);
	  pixman_region32_translate (&temp, view->abs_x, view->abs_y);
	  pixman_region32_union (&input, &input, &temp);
	}

      if (update_opaque)
	{
	  /* Add this view's opaque region to the total.  */
	  pixman_region32_intersect_rect (&temp, &view->opaque, 0, 0,
					  view->width, view->height);
	  pixman_region32_translate (&temp, view->abs_x, view->abs_y);
	  pixman_region32_union (&opaque, &opaque, &temp);
	}

    next:
      list = list->next;
    }

  /* Now, notify the client of any changes.  */

  if (update_input)
    subcompositor->input_change (subcompositor,
				 subcompositor->input_change_data,
				 &input);

  if (update_opaque)
    subcompositor->opaque_change (subcompositor,
				  subcompositor->opaque_change_data,
				  &opaque);

  /* And free the temp regions.  */

  pixman_region32_fini (&temp);

  if (update_input)
    pixman_region32_fini (&input);

  if (update_opaque)
    pixman_region32_fini (&opaque);

  subcompositor->state &= ~SubcompositorIsOpaqueDirty;
  subcompositor->state &= ~SubcompositorIsInputDirty;
}

static pixman_region32_t *
CopyRegion (pixman_region32_t *source)
{
  pixman_region32_t *region;

  region = XLMalloc (sizeof *region);
  pixman_region32_init (region);
  pixman_region32_copy (region, source);

  return region;
}

static void
FreeRegion (pixman_region32_t *region)
{
  pixman_region32_fini (region);
  XLFree (region);
}

static Bool
AnyParentUnmapped (View *view, List **link)
{
  View *unmapped;

  if (!IsPartiallyMapped (view->subcompositor))
    return False;

  /* Find the topmost unmapped parent of VIEW, or VIEW itself, if any,
     and set *link to its link pointer.  */
  unmapped = NULL;

  while (view)
    {
      if (IsViewUnmapped (view))
	unmapped = view;

      view = view->parent;
    }

  if (unmapped)
    {
      *link = unmapped->link;
      return True;
    }

  return False;
}

static void
DoCull (Subcompositor *subcompositor, pixman_region32_t *damage,
	pixman_region32_t *background)
{
  List *list;
  View *view;
  pixman_region32_t temp;
  RenderBuffer buffer;

  view = NULL;

  /* Process the background region.  The background must at most be
     drawn beneath the damage; anywhere else, it will be obscured by
     the opaque parts of views above or the bottommost view.  */
  pixman_region32_intersect (background, background, damage);

  /* Perform culling.  Walk the inferior list from top to bottom.
     Each time a view is encountered and has an opaque region, set
     damage as its "clip region", and then subtract its opaque region
     from damage.  */

  pixman_region32_init (&temp);
  list = subcompositor->inferiors->last;
  while (list != subcompositor->inferiors)
    {
      if (!list->view)
	goto last;

      if (AnyParentUnmapped (list->view, &list))
	/* Skip the unmapped view.  */
	goto last;

      if (IsSkipped (list->view))
	/* We must skip this view, as it represents (for instance) a
	   subsurface that has been added, but not committed.  */
	goto last;

      if (!list->view->buffer)
	goto last;

      view = list->view;
      buffer = XLRenderBufferFromBuffer (list->view->buffer);

      /* Set view's cull region to the intersection of the current
	 region and its bounds.  */
      pixman_region32_intersect_rect (&temp, damage,
				      view->abs_x,
				      view->abs_y,
				      view->width,
				      view->height);

      /* Don't set the cull region if it is empty.  */
      if (pixman_region32_not_empty (&temp))
	view->cull_region = CopyRegion (&temp);

      /* Subtract the damage region by the view's opaque region.  */

      if (!pixman_region32_not_empty (&view->opaque))
	goto last;

      if (RenderIsBufferOpaque (buffer))
	/* If the buffer is opaque, we can just ignore its opaque
	   region.  */
	pixman_region32_init_rect (&temp, view->abs_x, view->abs_y,
				   view->width, view->height);
      else
	{
	  pixman_region32_intersect_rect (&temp, &view->opaque, 0, 0,
					  view->width, view->height);
	  pixman_region32_translate (&temp, view->abs_x, view->abs_y);
	}

      pixman_region32_subtract (damage, damage, &temp);

      /* Also subtract the opaque region from the background.  */
      pixman_region32_subtract (background, background, &temp);

      /* If damage is already empty, finish early.  */
      if (!pixman_region32_not_empty (damage))
	break;

    last:
      list = list->last;
    }

  if (view && view->cull_region)
    /* Also subtract the region of the bottommost view that will be
       drawn from the background, as it will use PictOpCopy.  */
    pixman_region32_subtract (background, background, view->cull_region);

  pixman_region32_fini (&temp);
}

static void
DrawBackground (Subcompositor *subcompositor, pixman_region32_t *damage)
{
  pixman_box32_t *boxes;
  int nboxes;

  boxes = pixman_region32_rectangles (damage, &nboxes);

  if (nboxes)
    RenderFillBoxesWithTransparency (subcompositor->target, boxes,
				     nboxes, subcompositor->min_x,
				     subcompositor->min_y);
}

static void
CompositeSingleView (View *view, pixman_region32_t *region,
		     Operation op, DrawParams *transform)
{
  pixman_box32_t *boxes;
  int nboxes, i;
  RenderBuffer buffer;
  int min_x, min_y, tx, ty;
  Subcompositor *subcompositor;

  subcompositor = view->subcompositor;
  min_x = subcompositor->min_x;
  min_y = subcompositor->min_y;
  tx = subcompositor->tx;
  ty = subcompositor->ty;

  boxes = pixman_region32_rectangles (region, &nboxes);
  buffer = XLRenderBufferFromBuffer (view->buffer);

  for (i = 0; i < nboxes; ++i)
    RenderComposite (buffer, view->subcompositor->target, op,
		     /* src-x.  */
		     boxes[i].x1 - view->abs_x,
		     /* src-y.  */
		     boxes[i].y1 - view->abs_y,
		     /* dst-x.  */
		     boxes[i].x1 - min_x + tx,
		     /* dst-y.  */
		     boxes[i].y1 - min_y + ty,
		     /* width.  */
		     boxes[i].x2 - boxes[i].x1,
		     /* height.  */
		     boxes[i].y2 - boxes[i].y1,
		     /* draw-params.  */
		     transform);
}

static void
InitBackground (Subcompositor *subcompositor,
		pixman_region32_t *region)
{
  int min_x, min_y, max_x, max_y;

  min_x = subcompositor->min_x;
  min_y = subcompositor->min_y;
  max_x = subcompositor->max_x;
  max_y = subcompositor->max_y;

  pixman_region32_init_rect (region, min_x, min_y,
			     max_x - min_x + 1,
			     max_y - min_y + 1);
}

static Bool
TryPresent (View *view, pixman_region32_t *damage, DrawParams *transform)
{
  PresentCompletionKey key, existing_key;
  RenderBuffer buffer;

  if (view->abs_x == view->subcompositor->min_x
      && view->abs_y == view->subcompositor->min_y
      && view->width == (view->subcompositor->max_x
			 - view->subcompositor->min_x
			 + 1)
      && view->height == (view->subcompositor->max_y
			  - view->subcompositor->min_y
			  + 1)
      && view->subcompositor->note_frame
      && !transform->flags)
    {
      buffer = XLRenderBufferFromBuffer (view->buffer);

      /* Now, we know that the view overlaps the entire subcompositor
	 and has no transforms, and can thus be presented.  Translate
	 the damage into the window coordinate space.  */
      pixman_region32_translate (damage, -view->subcompositor->min_x,
				 -view->subcompositor->min_y);

      /* Present the buffer with the given damage.  */
      key = RenderPresentToWindow (view->subcompositor->target, buffer,
				   damage, PresentCompletedCallback,
				   view->subcompositor);

      /* Translate the damage back.  */
      pixman_region32_translate (damage, view->subcompositor->min_x,
				 view->subcompositor->min_y);

      if (key)
	{
	  /* BeginFrame should have canceled the presentation.
	     However, a present key may still exist if this
	     presentation is being done in response to an
	     exposure.  */
	  existing_key = view->subcompositor->present_key;

	  if (existing_key)
	    RenderCancelPresentationCallback (existing_key);

	  /* Do the same for the render completion callback, if
	     any.  */
	  if (view->subcompositor->render_key)
	    RenderCancelCompletionCallback (view->subcompositor->render_key);
	  view->subcompositor->render_key = NULL;

	  /* Presentation was successful.  Attach the presentation key
	     to the subcompositor.  */
	  view->subcompositor->present_key = key;
	  return True;
	}
    }

  /* Presentation failed.  */
  return False;
}

static void
ClearDamage (Subcompositor *subcompositor)
{
  List *list;
  View *view;

  list = subcompositor->inferiors->next;

  while (list != subcompositor->inferiors)
    {
      SkipSlug (list, view, next);

      /* Clear the damage.  */
      pixman_region32_clear (&view->damage);

    next:
      list = list->next;
    }
}

static void
ClearCull (Subcompositor *subcompositor)
{
  List *list;
  View *view;

  /* Free the cull region of every view.  */
  list = subcompositor->inferiors->next;
  view = NULL;

  while (list != subcompositor->inferiors)
    {
      SkipSlug (list, view, next);

      if (view->cull_region)
	FreeRegion (view->cull_region);
      view->cull_region = NULL;

    next:
      list = list->next;
    }
}

static Bool
CheckBailOnDraw (Subcompositor *subcompositor)
{
  List *list;
  View *view;

  list = subcompositor->inferiors->next;
  view = NULL;

  while (list != subcompositor->inferiors)
    {
      if (view && view->cull_region)
	/* This view will be drawn beneath some other view, so
	   presentation is not possible.  */
	goto need_bail;

      SkipSlug (list, view, next);

    next:
      list = list->next;
    }

  /* This only means that views prior to the last view will not be
     drawn.  We won't know if the topmost view can be presented until
     we actually try.  */
  return True;

 need_bail:
  ClearCull (subcompositor);

  /* And bail out.  */
  return False;
}

static Bool
SubcompositorComposite1 (Subcompositor *subcompositor,
			 pixman_region32_t *damage,
			 Bool bail_on_draw)
{
  List *list;
  View *view;
  pixman_region32_t background;
  Operation op;
  pixman_region32_t copy;
  DrawParams transform;
  Bool success, presented;
  RenderCompletionKey key;

  /* Draw the first view by copying.  */
  op = OperationSource;
  pixman_region32_init (&copy);
  pixman_region32_copy (&copy, damage);

  /* Initialize the background region.  */
  InitBackground (subcompositor, &background);

  /* Cull out parts of the damage that are obscured by opaque portions
     of views.  */
  DoCull (subcompositor, damage, &background);

  if (pixman_region32_not_empty (&background))
    {
      /* The background has to be drawn below the bottommost view, so
	 presentation is not possible.  Return if bail_on_draw.  */
      if (bail_on_draw)
	{
	  /* Free the cull regions and temp regions.  */
	  pixman_region32_fini (&background);
	  pixman_region32_fini (&copy);
	  ClearCull (subcompositor);

	  return False;
	}

      /* Now draw the background.  */
      DrawBackground (subcompositor, &background);
    }

  /* Free the background region.  */
  pixman_region32_fini (&background);

  /* bail_on_draw means that this function should return and let
     SubcompositorUpdate draw again upon encountering a view that
     cannot be presented.  */

  if (bail_on_draw && !CheckBailOnDraw (subcompositor))
    {
      /* Free the temp region.  */
      pixman_region32_fini (&copy);

      return False;
    }

  list = subcompositor->inferiors->next;

  /* Also recalculate whether or not the subcompositor is partially
     mapped while at this.  */
  subcompositor->state &= ~SubcompositorIsPartiallyMapped;

  /* Start rendering.  */
  RenderStartRender (subcompositor->target);
  view = NULL;
  success = True;
  presented = False;

  while (list != subcompositor->inferiors)
    {
      /* Update the views at the start of the loop.  Thus, if there is
	 only a single view, we can present it instead.  */

      if (view && view->cull_region)
	{
	  /* Compute the transform.  */
	  ViewComputeTransform (view, &transform, True);

	  /* Copy or composite the view contents.  */
	  CompositeSingleView (view, view->cull_region, op,
			       &transform);

	  /* And free the cull region.  */
	  FreeRegion (view->cull_region);
	  view->cull_region = NULL;

	  /* Subsequent views should be composited.  */
	  op = OperationOver;
	}

      SkipSlug (list, view, next);

    next:
      list = list->next;
    }

  /* Finally, update the last view.  */
  if (view && view->cull_region)
    {
      /* Compute the transform.  */
      ViewComputeTransform (view, &transform, True);

      /* This is the topmost view.  If there are no preceeding
	 views, present it.  */
      if (op != OperationSource
	  || !TryPresent (view, view->cull_region, &transform))
	{
	  if (bail_on_draw)
	    /* CompositeSingleView will be called, bail! */
	    success = False;
	  else
	    /* Copy or composite the view contents.  */
	    CompositeSingleView (view, view->cull_region, op,
				 &transform);
	}
      else
	/* Set this flag to true so the code below doesn't scribble
	   over the presentation callback.  */
	presented = True;

      /* And free the cull region.  */
      FreeRegion (view->cull_region);
      view->cull_region = NULL;
    }

  /* If a note_frame callback is attached, then this function can pass
     a RenderCompletedCallback to the picture renderer and have it
     present the back buffer to the window.  If not, however, it must
     use XCopyArea so that the buffer swap is done in order wrt to
     other requests.  */

  if (subcompositor->note_frame && !presented)
    {
      /* This goes down the XPresentPixmap code path.  N.B. that no
	 buffer swap must happen if presentation happened.  */
      if (subcompositor->render_key)
	RenderCancelCompletionCallback (subcompositor->render_key);
      if (subcompositor->present_key)
	RenderCancelPresentationCallback (subcompositor->present_key);
      subcompositor->present_key = NULL;

      pixman_region32_translate (damage, -subcompositor->min_x,
				 -subcompositor->min_y);
      key = RenderFinishRender (subcompositor->target, &copy,
				RenderCompletedCallback, subcompositor);
      pixman_region32_fini (&copy);

      subcompositor->render_key = key;
    }
  else
    {
      if (subcompositor->render_key)
	RenderCancelCompletionCallback (subcompositor->render_key);
      subcompositor->render_key = NULL;

      /* We must spare the presentation key if presentation
	 happened.  */
      if (!presented && subcompositor->present_key)
	{
	  RenderCancelPresentationCallback (subcompositor->present_key);
	  subcompositor->present_key = NULL;
	}

      /* This goes down the XCopyArea code path, unless presentation
	 happened, in which case it does nothing.  */
      pixman_region32_translate (damage, -subcompositor->min_x,
				 -subcompositor->min_y);
      key = RenderFinishRender (subcompositor->target, &copy, NULL,
				NULL);
      pixman_region32_fini (&copy);
    }

  if (success)
    /* Proceeed to clear the damage region of each view.  */
    ClearDamage (subcompositor);

  return success;
}

static Bool
SubcompositorComposite (Subcompositor *subcompositor)
{
  pixman_region32_t damage, temp;
  List *list;
  View *view;
  int age;
  Bool rc;

  age = RenderTargetAge (subcompositor->target);

  /* First, calculate a global damage region.  */

  pixman_region32_init (&damage);
  pixman_region32_init (&temp);
  list = subcompositor->inferiors->next;

  while (list != subcompositor->inferiors)
    {
      SkipSlug (list, view, next);

      /* Subtract the view's opaque region from the output damage
	 region.  */

      if (pixman_region32_not_empty (&view->opaque))
	{
	  /* Avoid reporting damage that will be covered up by views
	     above.  */
	  pixman_region32_intersect_rect (&temp, &view->opaque,
					  0, 0, view->width,
					  view->height);
	  pixman_region32_translate (&temp, view->abs_x, view->abs_y);
	  pixman_region32_subtract (&damage, &damage, &temp);
	}

      /* Add the view's damage region to the output damage region.  */
      pixman_region32_intersect_rect (&temp, &view->damage, 0, 0,
				      view->width, view->height);
      pixman_region32_translate (&temp, view->abs_x, view->abs_y);
      pixman_region32_union (&damage, &damage, &temp);

    next:
      list = list->next;
    }

  /* Add damage caused by i.e. movement.  */
  pixman_region32_union (&damage, &damage,
			 &subcompositor->additional_damage);

  /* If there is no damage, just return without drawing anything.  */
  if (!pixman_region32_not_empty (&damage))
    {
      pixman_region32_fini (&damage);
      pixman_region32_fini (&temp);
      return True;
    }

  if (age == -1 || age > 2)
    {
      /* The target is too old.  */
      pixman_region32_fini (&damage);
      pixman_region32_fini (&temp);
      return False;
    }

  if ((age > 0 && !subcompositor->last_damage)
      || (age > 1 && !subcompositor->before_damage))
    {
      /* Damage required for incremental update is missing.  */
      pixman_region32_fini (&damage);
      pixman_region32_fini (&temp);
      return False;
    }

  /* Copy the damage so StorePreviousDamage gets the damage before it
     was unioned.  */
  pixman_region32_copy (&temp, &damage);

  /* Now, damage contains the current damage of each view.  Add any
     previous damage if required.  */

  if (age > 0)
    pixman_region32_union (&damage, &damage,
			   subcompositor->last_damage);

  if (age > 1)
    pixman_region32_union (&damage, &damage,
			   subcompositor->before_damage);

  /* Add this damage onto the damage ring.  */
  StorePreviousDamage (subcompositor, &temp);
  pixman_region32_fini (&temp);

  /* Finally, paint.  If age is -2, then we must bail if the
     background could be drawn or the view is not presentable.  */
  rc = SubcompositorComposite1 (subcompositor, &damage, age == -2);

  pixman_region32_fini (&damage);

  if (rc)
    /* Clear any additional damage applied.  */
    pixman_region32_clear (&subcompositor->additional_damage);

  return rc;
}

static void
SubcompositorRedraw (Subcompositor *subcompositor)
{
  pixman_region32_t damage;

  /* Damage the entire subcompositor and render it.  */
  pixman_region32_init_rect (&damage, subcompositor->min_x,
			     subcompositor->min_y,
			     SubcompositorWidth (subcompositor),
			     SubcompositorHeight (subcompositor));
  SubcompositorComposite1 (subcompositor, &damage, False);
  pixman_region32_fini (&damage);

  /* Clear any additional damage applied.  */
  pixman_region32_clear (&subcompositor->additional_damage);
}

static void
BeginFrame (Subcompositor *subcompositor)
{
  if (!subcompositor->note_frame)
    return;

  subcompositor->note_frame (ModeStarted,
			     ++subcompositor->frame_counter,
			     subcompositor->note_frame_data);

  /* Cancel any presentation callback that is currently in
     progress.  */
  if (subcompositor->present_key)
    RenderCancelPresentationCallback (subcompositor->present_key);
  subcompositor->present_key = NULL;

  /* Cancel any render callback that is currently in progress.  */
  if (subcompositor->render_key)
    RenderCancelCompletionCallback (subcompositor->render_key);
  subcompositor->render_key = NULL;
}

static void
EndFrame (Subcompositor *subcompositor)
{
  if (!subcompositor->note_frame)
    return;

  /* Make sure that we wait for the presentation callback or render
     callback if they are attached.  */

  if (!subcompositor->present_key && !subcompositor->render_key)
    subcompositor->note_frame (ModeComplete,
			       subcompositor->frame_counter,
			       subcompositor->note_frame_data);
}

void
SubcompositorUpdate (Subcompositor *subcompositor)
{
  Bool could_composite;

  if (!IsTargetAttached (subcompositor))
    return;

  if (subcompositor->note_bounds)
    subcompositor->note_bounds (subcompositor->note_bounds_data,
				subcompositor->min_x,
				subcompositor->min_y,
				subcompositor->max_x,
				subcompositor->max_y);

  RenderNoteTargetSize (subcompositor->target,
			SubcompositorWidth (subcompositor),
			SubcompositorHeight (subcompositor));

  if (IsGarbaged (subcompositor))
    {
      BeginFrame (subcompositor);

      /* Update ancillary regions.  */
      SubcompositorUpdateAncillary (subcompositor);

      /* The subcompositor is garbaged.  Simply draw everything.  */
      SubcompositorRedraw (subcompositor);

      EndFrame (subcompositor);

      /* Clear the garbaged flag.  */
      subcompositor->state &= ~SubcompositorIsGarbaged;

      return;
    }

  /* Perform an update.  If ancillary regions are dirty, update
     them.  */

  BeginFrame (subcompositor);

  /* Now try to composite.  */
  could_composite = SubcompositorComposite (subcompositor);

  if (!could_composite)
    SubcompositorRedraw (subcompositor);

  if (IsInputDirty (subcompositor) || IsOpaqueDirty (subcompositor))
    SubcompositorUpdateAncillary (subcompositor);

  EndFrame (subcompositor);
}

void
SubcompositorExpose (Subcompositor *subcompositor, XEvent *event)
{
  pixman_region32_t damage;

  if (event->type == Expose)
    pixman_region32_init_rect (&damage, event->xexpose.x,
			       event->xexpose.y,
			       event->xexpose.width,
			       event->xexpose.height);
  else
    pixman_region32_init_rect (&damage, event->xgraphicsexpose.x,
			       event->xgraphicsexpose.y,
			       event->xgraphicsexpose.width,
			       event->xgraphicsexpose.height);
  SubcompositorComposite1 (subcompositor, &damage, False);
  pixman_region32_fini (&damage);
}

void
SubcompositorGarbage (Subcompositor *subcompositor)
{
  SetGarbaged (subcompositor);
}

void
SubcompositorSetProjectiveTransform (Subcompositor *subcompositor,
				     int tx, int ty)
{
  subcompositor->tx = tx;
  subcompositor->ty = ty;
}

void
SubcompositorFree (Subcompositor *subcompositor)
{
  /* It isn't valid to call this function with children attached.  */
  XLAssert (subcompositor->children->next
	    == subcompositor->children);
  XLAssert (subcompositor->inferiors->next
	    == subcompositor->inferiors);

  XLFree (subcompositor->children);
  XLFree (subcompositor->inferiors);

  /* Finalize the buffers used to store previous damage.  */
  pixman_region32_fini (&subcompositor->prior_damage[0]);
  pixman_region32_fini (&subcompositor->prior_damage[1]);

  /* Finalize the region used to store additional damage.  */
  pixman_region32_fini (&subcompositor->additional_damage);

  /* Remove the presentation key.  */
  if (subcompositor->present_key)
    RenderCancelPresentationCallback (subcompositor->present_key);

  /* And the render completion key.  */
  if (subcompositor->render_key)
    RenderCancelCompletionCallback (subcompositor->render_key);

  XLFree (subcompositor);
}

View *
SubcompositorLookupView (Subcompositor *subcompositor, int x, int y,
			 int *view_x, int *view_y)
{
  List *list;
  int temp_x, temp_y;
  pixman_box32_t box;

  x += subcompositor->min_x;
  y += subcompositor->min_y;

  for (list = subcompositor->inferiors->last;
       list != subcompositor->inferiors;
       list = list->last)
    {
      if (!list->view)
	continue;

      if (IsViewUnmapped (list->view))
	{
	  list = list->view->inferior;
	  continue;
	}

      if (IsSkipped (list->view))
	{
	  /* We must skip this view, as it represents (for instance) a
	     subsurface that has been added, but not committed.  */
	  SetPartiallyMapped (subcompositor);
	  continue;
	}

      if (!list->view->buffer)
	continue;

      temp_x = x - list->view->abs_x;
      temp_y = y - list->view->abs_y;

      /* If the coordinates don't fit in the view bounds, skip the
	 view.  This test is the equivalent to intersecting the view's
	 input region with the bounds of the view.  */
      if (temp_x < 0 || temp_y < 0
	  || temp_x >= list->view->width
	  || temp_y >= list->view->height)
	continue;

      /* Now see if the input region contains the given
	 coordinates.  If it does, return the view.  */
      if (pixman_region32_contains_point (&list->view->input, temp_x,
					  temp_y, &box))
	{
	  *view_x = list->view->abs_x - subcompositor->min_x;
	  *view_y = list->view->abs_y - subcompositor->min_y;

	  return list->view;
	}
    }

  return NULL;
}

void *
ViewGetData (View *view)
{
  return view->data;
}

void
ViewSetData (View *view, void *data)
{
  view->data = data;
}

void
ViewSetMaybeResizedFunction (View *view, void (*func) (View *))
{
  view->maybe_resized = func;
}

void
ViewTranslate (View *view, int x, int y, int *x_out, int *y_out)
{
  if (view->subcompositor)
    {
      /* X and Y are assumed to be in the "virtual" coordinate
	 space.  */
      x += view->subcompositor->min_x;
      y += view->subcompositor->min_y;
    }

  *x_out = x - view->abs_x;
  *y_out = y - view->abs_y;
}

View *
ViewGetParent (View *view)
{
  return view->parent;
}

void
SubcompositorInit (void)
{
  /* Nothing to do here... */
}

int
SubcompositorWidth (Subcompositor *subcompositor)
{
  return subcompositor->max_x - subcompositor->min_x + 1;
}

int
SubcompositorHeight (Subcompositor *subcompositor)
{
  return subcompositor->max_y - subcompositor->min_y + 1;
}

#endif
