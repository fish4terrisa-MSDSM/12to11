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
    /* This means that the subcompositor is frozen and updates should
       do nothing.  */
    SubcompositorIsFrozen	   = (1 << 5),
    /* This means that the subcompositor has a target attached.  */
    SubcompositorIsTargetAttached  = (1 << 6),
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

#define SetFrozen(subcompositor)				\
  ((subcompositor)->state |= SubcompositorIsFrozen)
#define IsFrozen(subcompositor)					\
  ((subcompositor)->state & SubcompositorIsFrozen)

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
  /* The buffer associated with this view, or None if nothing is
     attached.  */
  ExtBuffer *buffer;

  /* Function called upon the view potentially being resized.  */
  void (*maybe_resized) (View *);

  /* Some data associated with this view.  Can be a surface or
     something else.  */
  void *data;

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

  /* The damage region of previous updates.  last_damage is what the
     damage region was 1 update ago, and before_damage is what the
     damage region was 2 updates ago.  */
  pixman_region32_t *last_damage, *before_damage;

  /* The last attached presentation callback, if any.  */
  PresentCompletionKey present_key;

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

  /* Updates were optimized out.  */
  if (!doflags)
    return;

  list = subcompositor->inferiors->next;
  min_x = max_x = min_y = max_y = 0;

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
    subcompositor->min_x = view->abs_x;

  if (view->abs_x < view->subcompositor->min_y)
    subcompositor->min_y = view->abs_y;

  if (view->subcompositor->max_x < ViewMaxX (view))
    subcompositor->max_x = ViewMaxX (view);

  if (view->subcompositor->max_y < ViewMaxY (view))
    subcompositor->max_y = ViewMaxY (view);
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

TEST_STATIC void
SubcompositorInsert (Subcompositor *compositor, View *view)
{
  /* Link view into the list of children.  */
  ListInsertBefore (compositor->last_children, view->self);

  /* Make view's inferiors part of the compositor.  */
  ListRelinkBefore (view->link, view->inferior,
		    compositor->last);

  /* Now that the view hierarchy has been changed, garbage the
     subcompositor.  */
  SetGarbaged (compositor);

#ifndef TEST
  /* And update bounds.  */
  SubcompositorUpdateBoundsForInsert (compositor, view);
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

  /* Now that the view hierarchy has been changed, garbage the
     subcompositor.  */
  SetGarbaged (compositor);

#ifndef TEST
  /* And update bounds.  */
  SubcompositorUpdateBoundsForInsert (compositor, view);
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

  /* Now that the view hierarchy has been changed, garbage the
     subcompositor.  */
  SetGarbaged (compositor);

#ifndef TEST
  /* And update bounds.  */
  SubcompositorUpdateBoundsForInsert (compositor, view);
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

  if (view->subcompositor)
    SetGarbaged (view->subcompositor);

#ifndef TEST
  /* Also update the absolute positions of the child.  */
  child->abs_x = view->abs_x + child->x;
  child->abs_y = view->abs_y + child->y;
  ViewRecomputeChildren (child, NULL);

  /* And update bounds.  */
  ViewUpdateBoundsForInsert (view);
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

  /* Now that the view hierarchy has been changed, garbage the
     subcompositor.  */

  if (view->subcompositor)
    SetGarbaged (view->subcompositor);

#ifndef TEST
  /* Also update the absolute positions of the child.  */
  child->abs_x = view->abs_x + child->x;
  child->abs_y = view->abs_y + child->y;
  ViewRecomputeChildren (child, NULL);

  /* And update bounds.  */
  ViewUpdateBoundsForInsert (view);
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

  /* Now that the view hierarchy has been changed, garbage the
     subcompositor.  */

  if (view->subcompositor)
    SetGarbaged (view->subcompositor);

#ifndef TEST
  /* Also update the absolute positions of the child.  */
  child->abs_x = view->abs_x + child->x;
  child->abs_y = view->abs_y + child->y;
  ViewRecomputeChildren (child, NULL);

  /* Update subcompositor bounds.  Inserting a view cannot shrink
     anything.  */
  ViewUpdateBoundsForInsert (view);
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
#endif

      /* Then, garbage the subcompositor.  */
      SetGarbaged (child->subcompositor);
    }
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


/* The subcompositor composites its inferior views to a drawable,
   normally a window, each time the SubcompositorUpdate function is
   called.  Since it is not very efficient to draw every view every
   time an update occurs, the subcompositor keeps track of which parts
   of the inferiors have changed, and uses that information to only
   composite a reasonable minimum set of inferiors and screen areas on
   each update (reasonable meaning whatever can be computed quickly
   while keeping graphics updates fast).  The subcompositor also keeps
   track of which areas of an inferior are opaque, and uses that
   information to avoid compositing in response to damage on inferiors
   that are obscured from above.

   The subcompositor normally assumes that the contents of the target
   drawable are what was drawn by the subcompositor during previous
   updates.  With that in mind, the subcompositor tries to calculate a
   "global damage region" consisting of the areas of the target that
   have to be updated, and a "update inferior", the first inferior
   that will be composited onto the target drawable, by unioning up
   damage and opaque regions of each inferior until the first
   unobscured inferior is found.  Then, the contents of all inferiors
   that intersect with the global damage region are composited onto
   the target drawable.  Afterwards, the damage region of each
   inferior is cleared, and the process can begin again.

   However, under some situations, the contents of the target drawable
   may reflect what was drawn two or three invocations of
   SubcompositorUpdate ago.  To enable efficient updates when that is
   the case, the subcompositor will keep track of the global damage
   regions of the past two updates, and intersect the resulting global
   damage region of each invocation with the appropriate number of
   previous regions.

   For simplicity's sake, the update inferior is reset to the first
   view in the subcompositor's inferior list whenever the global
   damage region is intersected with the damage region of a previous
   update.

   Such computation is not reliable, however, if the size or position
   of a view changes.  In the interest of keeping thing simple, every
   inferior is composited onto the target drawable whenever a view
   change is detected.  These changes are marked by calls to the macro
   SetGarbaged.

   Further more, the X server can sometimes erase the contents of an
   area of the target window, in response to it being obscured.  When
   that happens, that area is entirely composited to the target
   window.  See SubcompositorExpose for more details.  */

#ifndef TEST

/* Notice that VIEW's size has changed, while VIEW itself has not
   moved.  Recompute the max_x, min_x, min_y, and max_y of its
   subcompositor.  In addition, run the view's resize function, if
   any.  */

static void
ViewAfterSizeUpdate (View *view)
{
  int doflags;
  Bool mapped;

  if (view->maybe_resized)
    view->maybe_resized (view);

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

      /* We don't have to update max_x anymore.  */
      doflags &= ~DoMaxX;
    }

  if (view->subcompositor->max_y < ViewMaxY (view))
    {
      view->subcompositor->max_y = ViewMaxY (view);

      /* We don't have to update max_x anymore.  */
      doflags &= ~DoMaxY;
    }

  /* Finally, update the bounds.  */
  SubcompositorUpdateBounds (view->subcompositor, doflags);
}

void
ViewAttachBuffer (View *view, ExtBuffer *buffer)
{
  ExtBuffer *old;

  old = view->buffer;
  view->buffer = buffer;

  if (!old != !buffer)
    {
      /* TODO: just damage intersecting views before view->link if the
	 buffer was removed.  */
      if (view->subcompositor)
	SetGarbaged (view->subcompositor);
    }

  if ((buffer && !old)
      || (old && !buffer)
      || (buffer && old
	  && (XLBufferWidth (buffer) != XLBufferWidth (old)
	      || XLBufferHeight (buffer) != XLBufferHeight (old))))
    {
      if (view->subcompositor
	  /* If a viewport is specified, then the view width and
	     height are determined independently from the buffer
	     size.  */
	  && !IsViewported (view))
	{
	  /* A new buffer was attached, so garbage the subcompositor
	     as well.  */
	  SetGarbaged (view->subcompositor);

	  /* Recompute view and subcompositor bounds.  */
	  ViewAfterSizeUpdate (view);
	}
    }

  if (buffer && IsViewUnmapped (view))
    {
      /* A buffer is now attached.  Automatically map the view, should
	 it be unmapped.  */
      ClearUnmapped (view);

      if (view->subcompositor)
	{
	  /* Garbage the subcompositor and recompute bounds.  */
	  SetGarbaged (view->subcompositor);
	  SubcompositorUpdateBounds (view->subcompositor, DoAll);
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
  int doflags;
  Bool mapped;

  doflags = 0;

  if (x != view->x || y != view->y)
    {
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
	    }

	  if (view->abs_y < view->subcompositor->min_x)
	    {
	      view->subcompositor->min_y = view->abs_y;

	      /* min_y has already been updated so there is no need to
		 recompute it later.  */
	      doflags &= ~DoMinY;
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
	    }

	  if (view->subcompositor->max_y < ViewMaxX (view))
	    {
	      view->subcompositor->max_y = ViewMaxX (view);

	      /* max_y has been updated so there is no need to
		 recompute it later.  If a child is bigger, then
		 ViewRecomputeChildren will handle it as well.  */
	      doflags &= ~DoMaxY;
	    }

	  /* Also garbage the subcompositor since those values
	     changed.  TODO: just damage intersecting views before
	     view->link.  */
	  SetGarbaged (view->subcompositor);
	}

      /* Now calculate the absolute position for this view and all of
	 its children.  N.B. that this operation can also update
	 subcompositor.min_x or subcompositor.min_y.  */
      ViewRecomputeChildren (view, &doflags);

      /* Update subcompositor bounds.  */
      if (view->subcompositor)
	SubcompositorUpdateBounds (view->subcompositor, doflags);
    }
}

void
ViewMoveFractional (View *view, double x, double y)
{
  XLAssert (x < 1.0 && y < 1.0);

  /* This does not necessitate adjustments to the view size, but does
     require that the subcompositor be garbaged.  */
  view->fract_x = x;
  view->fract_y = y;

  if (view->subcompositor)
    SetGarbaged (view->subcompositor);
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
    {
      /* Garbage the subcompositor and recompute bounds, if something
	 is attached to the view.  */
      SetGarbaged (view->subcompositor);
      SubcompositorUpdateBounds (view->subcompositor, DoAll);
    }
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

void
ViewDamage (View *view, pixman_region32_t *damage)
{
  /* This damage must be transformed by the viewport and scale, but
     must NOT be transformed by the subpixel (fractional) offset.  */
  pixman_region32_union (&view->damage, &view->damage,
			 damage);
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
  BufferTransform inverse;

  /* Invert the transform.  */
  inverse = InvertTransform (view->transform);

  /* Calculate the width and height of the buffer after the
     transform.  */
  width = XLBufferWidth (view->buffer);
  height = XLBufferHeight (view->buffer);

  /* Transform the damage.  */
  XLTransformRegion (damage, source, inverse, width, height);
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

  /* Garbage the subcompositor as damage can no longer be trusted.  */
  if (view->subcompositor)
    SubcompositorGarbage (view->subcompositor);
}

void
ViewClearViewport (View *view)
{
  ClearViewported (view);

  /* Update min_x and min_y.  */
  ViewAfterSizeUpdate (view);

  /* Garbage the subcompositor as damage can no longer be trusted.  */
  if (view->subcompositor)
    SubcompositorGarbage (view->subcompositor);
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

static void
FillBoxesWithTransparency (Subcompositor *subcompositor,
			   pixman_box32_t *boxes, int nboxes)
{
  RenderFillBoxesWithTransparency (subcompositor->target, boxes,
				   nboxes, subcompositor->min_x,
				   subcompositor->min_y);
}

static Bool
ViewContainsExtents (View *view, pixman_box32_t *box)
{
  int x, y, width, height;

  x = view->abs_x;
  y = view->abs_y;
  width = ViewWidth (view);
  height = ViewHeight (view);

  return (box->x1 >= x && box->y1 >= y
	  && box->x2 <= x + width
	  && box->y2 <= x + height);
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

static Bool
IntersectBoxes (pixman_box32_t *in, pixman_box32_t *other,
		pixman_box32_t *out)
{
  pixman_box32_t a, b;

  /* Take copies of all the boxes, since one of them might be out.  */
  a = *in;
  b = *other;

  out->x1 = MAX (a.x1, b.x1);
  out->y1 = MAX (a.y1, b.y1);

  out->x2 = MIN (a.x2, b.x2);
  out->y2 = MIN (a.y2, b.y2);

  /* If the intersection is empty, return False.  */
  if (out->x2 - out->x1 < 0 || out->y2 - out->y1 < 0)
    return False;

  return True;
}

static Bool
NoViewsAfter (View *first_view)
{
  List *list;
  View *view;

  list = first_view->link->next;

  while (list != first_view->link)
    {
      view = list->view;

      if (!view)
	goto next_1;

      if (IsViewUnmapped (view))
	{
	  /* Skip the unmapped view.  */
	  list = view->inferior;
	  goto next_1;
	}

      if (IsSkipped (view))
	{
	  /* We must skip this view, as it represents (for
	     instance) a subsurface that has been added, but not
	     committed.  */
	  goto next_1;
	}

      if (!view->buffer)
	goto next_1;

      /* There is view in front of view that potentially obscures it.
	 Bail out! */
      return False;

    next_1:
      list = list->next;
    }

  return True;
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

void
SubcompositorUpdate (Subcompositor *subcompositor)
{
  pixman_region32_t update_region, temp, start_opaque;
  pixman_region32_t total_opaque, total_input;
  View *start, *original_start, *view, *first;
  List *list;
  pixman_box32_t *boxes, *extents, temp_boxes;
  int nboxes, i, tx, ty, view_width, view_height;
  Operation op;
  RenderBuffer buffer;
  int min_x, min_y, age, n_seen;
  DrawParams draw_params;
  Bool presented;
  PresentCompletionKey key;

  /* Just return if no target was specified.  */
  if (!IsTargetAttached (subcompositor))
    return;

  /* Likewise if the subcompositor is "frozen".  */
  if (IsFrozen (subcompositor))
    return;

  if (subcompositor->present_key)
    /* Cancel the presentation callback.  The next presentation will
       either be to an unmapped window, cancel the presentation, or
       start a new one.  */
    RenderCancelPresentationCallback (subcompositor->present_key);
  subcompositor->present_key = NULL;

  list = subcompositor->inferiors;
  min_x = subcompositor->min_x;
  min_y = subcompositor->min_y;
  tx = subcompositor->tx;
  ty = subcompositor->ty;
  start = NULL;
  original_start = NULL;
  pixman_region32_init (&temp);
  pixman_region32_init (&update_region);
  n_seen = 0;
  presented = False;

  start = subcompositor->inferiors->next->view;
  age = RenderTargetAge (subcompositor->target);

  /* If there is not enough prior damage available to satisfy age, set
     it to -1.  */

  if (age > 0 && !subcompositor->last_damage)
    age = -1;

  if (age > 2 && !subcompositor->before_damage)
    age = -1;

  /* If the subcompositor is garbaged, clear all prior damage.  */
  if (IsGarbaged (subcompositor))
    {
      if (subcompositor->last_damage)
	pixman_region32_clear (subcompositor->last_damage);

      if (subcompositor->before_damage)
	pixman_region32_clear (subcompositor->before_damage);

      /* Reset these fields to NULL, so we do not try to use them
	 later on.  */
      subcompositor->last_damage = NULL;
      subcompositor->before_damage = NULL;
    }

  /* Clear the "is partially mapped" flag.  It will be set later on if
     there is actually a partially mapped view.  */
  subcompositor->state &= ~SubcompositorIsPartiallyMapped;

  if (subcompositor->note_bounds)
    subcompositor->note_bounds (subcompositor->note_bounds_data,
				min_x, min_y, subcompositor->max_x,
				subcompositor->max_y);

  /* Note the size of this subcompositor, so the viewport can be set
     accordingly.  */
  RenderNoteTargetSize (subcompositor->target,
			subcompositor->max_x - min_x + 1,
		        subcompositor->max_y - min_y + 1);

  if (!IsGarbaged (subcompositor)
      /* If the target contents are too old or invalid, we go down the
	 usual IsGarbaged code path, except we do not recompute the
	 input or opaque regions unless they are dirty.  */
      && (age >= 0 && age < 3))
    {
      start = NULL;
      original_start = NULL;

      if (IsOpaqueDirty (subcompositor))
	pixman_region32_init (&total_opaque);

      if (IsInputDirty (subcompositor))
	pixman_region32_init (&total_input);

      pixman_region32_init (&start_opaque);

      do
	{
	  view = list->view;

	  if (!view)
	    goto next;

	  if (IsViewUnmapped (view))
	    {
	      /* The view is unmapped.  Skip past it and all its
		 children.  */
	      list = view->inferior;

	      /* Set the "is partially mapped" flag.  This is an
		 optimization used to make inserting views in deeply
		 nested hierarchies faster.  */
	      SetPartiallyMapped (subcompositor);
	      goto next;
	    }

	  if (IsSkipped (view))
	    {
	      /* We must skip this view, as it represents (for
		 instance) a subsurface that has been added, but not
		 committed.  */
	      SetPartiallyMapped (subcompositor);
	      goto next;
	    }

	  if (!view->buffer)
	    goto next;

	  /* Increase the number of views seen count.  */
	  n_seen++;

	  /* Obtain the view width and height here.  */
	  view_width = ViewWidth (view);
	  view_height = ViewHeight (view);

	  if (!start)
	    {
	      start = view;
	      original_start = view;
	    }

	  if (pixman_region32_not_empty (&list->view->opaque))
	    {
	      /* Translate the region into the subcompositor
		 coordinate space.  */
	      pixman_region32_translate (&list->view->opaque,
					 list->view->abs_x,
					 list->view->abs_y);

	      /* Only use the intersection between the opaque region
		 and the rectangle of the view, since the opaque areas
		 cannot extend outside it.  */

	      pixman_region32_intersect_rect (&temp, &view->opaque,
					      view->abs_x, view->abs_y,
					      view_width, view_height);

	      if (IsOpaqueDirty (subcompositor))
		pixman_region32_union (&total_opaque, &total_opaque, &temp);

	      pixman_region32_subtract (&update_region,
					&update_region, &temp);

	      /* This view will obscure all preceding damaged areas,
		 so make start here.  This optimization is disabled if
		 the target contents are too old, as prior damage
		 could reveal contents below.  */
	      if (!pixman_region32_not_empty (&update_region) && !age)
		{
		  start = list->view;

		  /* Now that start changed, record the opaque region.
		     That way, if some damage happens outside the
		     opaque region in the future, this operation can
		     be undone.  */
		  pixman_region32_copy (&start_opaque, &view->opaque);
		}

	      pixman_region32_translate (&list->view->opaque,
					 -list->view->abs_x,
					 -list->view->abs_y);
	    }

	  if (pixman_region32_not_empty (&list->view->input)
	      && IsInputDirty (subcompositor))
	    {
	      /* Translate the region into the subcompositor
		 coordinate space.  */
	      pixman_region32_translate (&list->view->input,
					 list->view->abs_x,
					 list->view->abs_y);

	      pixman_region32_intersect_rect (&temp, &view->input,
					      view->abs_x, view->abs_y,
					      view_width, view_height);

	      pixman_region32_union (&total_input, &total_input, &temp);

	      /* Restore the original input region.  */
	      pixman_region32_translate (&list->view->input,
					 -list->view->abs_x,
					 -list->view->abs_y);
	    }

	  /* Update the attached buffer from the damage.  This is only
	     required on some backends, where we have to upload data
	     from a shared memory buffer to the graphics hardware.

	     The update is performed even when there is no damage,
	     because the initial data might need to be uploaded.
	     However, the function does not perform partial updates
	     when the damage region is empty.  */

	  /* Compute the transform and put it in draw_params, so TRT
	     can be done in the rendering backend.  */
	  ViewComputeTransform (view, &draw_params, False);

	  buffer = XLRenderBufferFromBuffer (view->buffer);
	  RenderUpdateBufferForDamage (buffer, &list->view->damage,
				       &draw_params);

	  if (pixman_region32_not_empty (&list->view->damage))
	    {
	      /* Translate the region into the subcompositor
		 coordinate space.  */
	      pixman_region32_translate (&list->view->damage,
					 list->view->abs_x,
					 list->view->abs_y);

	      /* Similarly intersect the damage region with the
		 clipping.  */
	      pixman_region32_intersect_rect (&temp, &list->view->damage,
					      view->abs_x, view->abs_y,
					      view_width, view_height);

	      /* If a fractional offset is set, extend the damage by 1
		 pixel to cover the offset.  */
	      if (view->fract_x != 0.0 && view->fract_y != 0.0)
		{
		  XLExtendRegion (&temp, &temp, 1, 1);

		  /* Intersect the region again.  */
		  pixman_region32_intersect_rect (&temp, &temp, view->abs_x,
						  view->abs_y, view_width,
						  view_height);
		}

	      /* Union the region with the update region.  */
	      pixman_region32_union (&update_region, &temp, &update_region);

	      /* If the damage extends outside the area known to be
		 obscured by the current start, reset start back to
		 the original starting point.  */
	      if (start != original_start && original_start)
		{
		  pixman_region32_subtract (&temp, &list->view->damage,
					    &start_opaque);

		  if (pixman_region32_not_empty (&temp))
		    start = original_start;
		}

	      /* Clear the damaged area, since it will either be drawn
		 or be obscured.  */
	      pixman_region32_clear (&list->view->damage);
	    }

	next:
	  list = list->next;
	}
      while (list != subcompositor->inferiors);

      if (IsOpaqueDirty (subcompositor))
	{
	  /* The opaque region changed, so run any callbacks.  */
	  if (subcompositor->opaque_change)
	    {
	      /* Translate this to appear in the "virtual" coordinate
		 space.  */
	      pixman_region32_translate (&total_opaque, -min_x, -min_y);

	      subcompositor->opaque_change (subcompositor,
					    subcompositor->opaque_change_data,
					    &total_opaque);
	    }

	  pixman_region32_fini (&total_opaque);
	}

      if (IsInputDirty (subcompositor))
	{
	  /* The input region changed, so run any callbacks.  */
	  if (subcompositor->input_change)
	    {
	      /* Translate this to appear in the "virtual" coordinate
		 space.  */
	      pixman_region32_translate (&total_input, -min_x, -min_y);

	      subcompositor->input_change (subcompositor,
					   subcompositor->input_change_data,
					   &total_input);
	    }

	  pixman_region32_fini (&total_input);
	}

      pixman_region32_fini (&start_opaque);

      /* First store previous damage.  */
      StorePreviousDamage (subcompositor, &update_region);

      /* Now, apply any prior damage that might be required.  */
      if (age > 0)
	pixman_region32_union (&update_region, &update_region,
			       /* This is checked to exist upon
				  entering this code path.  */
			       subcompositor->last_damage);

      if (age > 1)
	pixman_region32_union (&update_region, &update_region,
			       /* This is checked to exist upon
				  entering this code path.  */
			       subcompositor->before_damage);
    }
  else
    {
      /* To save from iterating over all the views twice, perform the
	 input and opaque region updates in the draw loop instead.  */

      if (IsGarbaged (subcompositor))
	{
	  pixman_region32_init (&total_opaque);
	  pixman_region32_init (&total_input);
	}
      else
	{
	  /* Otherwise, we are in the IsGarbaged code because the
	     target contents are too old.  Only initialize the opaque
	     and input regions if they are dirty.  */

	  if (IsOpaqueDirty (subcompositor))
	    pixman_region32_init (&total_opaque);

	  if (IsInputDirty (subcompositor))
	    pixman_region32_init (&total_input);
	}

      /* Either way, put something in the prior damage ring.  */
      StorePreviousDamage (subcompositor, NULL);
    }

  /* Increase the frame count and announce the new frame number.  This
     must be done even if no graphics changes were committed.  */
  if (subcompositor->note_frame)
    subcompositor->note_frame (ModeStarted,
			       ++subcompositor->frame_counter,
			       subcompositor->note_frame_data);

  /* If there's nothing to do, return.  */

  if (!start)
    /* There is no starting view.  Presentation is not cancelled in
       this case, because the surface should now be unmapped.  */
    goto complete;

  /* Now update all views from start onwards.  */

  list = start->link;
  first = NULL;

  /* Begin rendering.  This is unnecessary on XRender, but required on
     EGL to make the surface current and set the viewport.  */
  RenderStartRender (subcompositor->target);

  do
    {
      view = list->view;

      if (!view)
	goto next_1;

      if (IsViewUnmapped (view))
	{
	  /* Skip the unmapped view.  */
	  list = view->inferior;

	  /* Set the "is partially mapped" flag.  This is an
	     optimization used to make inserting views in deeply
	     nested hierarchies faster.  */
	  SetPartiallyMapped (subcompositor);
	  goto next_1;
	}

      if (IsSkipped (view))
	{
	  /* We must skip this view, as it represents (for
	     instance) a subsurface that has been added, but not
	     committed.  */
	  SetPartiallyMapped (subcompositor);
	  goto next_1;
	}

      if (!view->buffer)
	goto next_1;

      /* Get the view width and height here.  */
      view_width = ViewWidth (view);
      view_height = ViewHeight (view);

      /* And the buffer.  */
      buffer = XLRenderBufferFromBuffer (view->buffer);

      if (IsGarbaged (subcompositor))
	/* Update the attached buffer from the damage.  This is only
	   required on some backends, where we have to upload data
	   from a shared memory buffer to the graphics hardware.

	   As the damage cannot be trusted while the subcompositor is
	   update, pass NULL; this tells the renderer to update the
	   entire buffer.

	   Note that if the subcompositor is not garbaged, then this
	   has already been done.  */
	RenderUpdateBufferForDamage (buffer, NULL, NULL);
      else if (age < 0 || age >= 3)
	{
	  /* Compute the transform and put it in draw_params, so TRT
	     can be done in the rendering backend.  */
	  ViewComputeTransform (view, &draw_params, False);

	  /* The target contents are too old, but the damage can be
	     trusted.  */
	  RenderUpdateBufferForDamage (buffer, &view->damage,
				       &draw_params);
	}

      /* Compute the transform and put it in draw_params.  */
      ViewComputeTransform (view, &draw_params, True);

      if (!first)
	{
	  /* See if the first mapped and visible view after start is eligible
	     for direct presentation.  It is considered eligible if:

	     - its bounds match that of the subcompositor.
	     - its depth and masks match that of the subcompositor.
	     - it is not occluded by any other view, above or below.
	     - it has no transform whatsoever.

	     Also, presentation is done asynchronously, so we only
	     consider the view as eligible for presentation if
	     completion callbacks are attached.  */

	  if (!draw_params.flags
	      && view->abs_x == subcompositor->min_x
	      && view->abs_y == subcompositor->min_y
	      && view_width == SubcompositorWidth (subcompositor)
	      && view_height == SubcompositorHeight (subcompositor)
	      /* N.B. that n_seen is not set (0) if the view is
		 garbaged.  */
	      && (n_seen == 1 || (!n_seen && NoViewsAfter (view)))
	      && subcompositor->note_frame)
	    {
	      /* Direct presentation is okay.  Present the pixmap to
		 the drawable.  */
	      if (IsGarbaged (subcompositor))
		key = RenderPresentToWindow (subcompositor->target, buffer,
					     NULL, PresentCompletedCallback,
					     subcompositor);
	      else
		key = RenderPresentToWindow (subcompositor->target, buffer,
					     &update_region,
					     PresentCompletedCallback,
					     subcompositor);

	      /* Now set presented to whether or not key is non-NULL.  */
	      presented = key != NULL;

	      /* And set the presentation callback.  */
	      subcompositor->present_key = key;

	      if (presented)
		{
		  /* If presentation succeeded, don't composite.
		     Instead, just continue looping to set the input
		     region if garbaged.  */

		  if (!IsGarbaged (subcompositor) && (age >= 0 && age < 3))
		    /* And if not garbaged, skip everything.  */
		    goto present_success;
		  else
		    goto present_success_garbaged;
		}
	    }
	  else
	    {
	      RenderCancelPresentation (subcompositor->target);

	      /* Tell the surface to make the compositor redirect the
		 window again.  */
	      if (subcompositor->note_frame)
		subcompositor->note_frame (ModeNotifyDisablePresent,
					   subcompositor->frame_counter,
					   subcompositor->note_frame_data);
	    }

	  /* The first view with an attached buffer should be drawn
	     with PictOpSrc so that transparency is applied correctly,
	     if it contains the entire update region.  */

	  if (IsGarbaged (subcompositor) || age < 0 || age >= 3)
	    {
	      extents = &temp_boxes;

	      /* Make extents the entire region, since that's what is
		 being updated.  */
	      temp_boxes.x1 = min_x;
	      temp_boxes.y1 = min_y;
	      temp_boxes.x2 = subcompositor->max_x + 1;
	      temp_boxes.y2 = subcompositor->max_y + 1;
	    }
	  else
	    extents = pixman_region32_extents (&update_region);

	  if (ViewContainsExtents (view, extents))
	    /* The update region is contained by the entire view, so
	       use source.  */
	    op = OperationSource;
	  else
	    {
	      /* Otherwise, fill the whole update region with
		 transparency.  */

	      if (IsGarbaged (subcompositor) || age < 0 || age >= 3)
		{
		  /* Use the entire subcompositor bounds if
		     garbaged.  */
		  boxes = &temp_boxes;
		  nboxes = 1;
		}
	      else
		boxes = pixman_region32_rectangles (&update_region,
						    &nboxes);

	      /* Fill with transparency.  */
	      FillBoxesWithTransparency (subcompositor,
					 boxes, nboxes);

	      /* And use over as usual.  */
	      op = OperationOver;
	    }
	}
      else
	op = OperationOver;

      if (presented && (IsGarbaged (subcompositor)
			|| age < 0 || age >= 3))
	goto present_success_garbaged;

      first = view;

      if (!IsGarbaged (subcompositor) && (age >= 0 && age < 3))
	{
	  /* Next, composite every rectangle in the update region
	     intersecting with the target.  */
	  boxes = pixman_region32_rectangles (&update_region, &nboxes);

	  for (i = 0; i < nboxes; ++i)
	    {
	      /* Check if the rectangle is completely inside the
		 region.  We used to take the intersection of the
		 region, but that proved to be too slow.  */

	      temp_boxes.x1 = view->abs_x;
	      temp_boxes.y1 = view->abs_y;
	      temp_boxes.x2 = view->abs_x + view_width;
	      temp_boxes.y2 = view->abs_y + view_height;

	      if (IntersectBoxes (&boxes[i], &temp_boxes, &temp_boxes))
		RenderComposite (buffer, subcompositor->target, op,
				 /* src-x.  */
				 BoxStartX (temp_boxes) - view->abs_x,
				 /* src-y.  */
				 BoxStartY (temp_boxes) - view->abs_y,
				 /* dst-x.  */
				 BoxStartX (temp_boxes) - min_x + tx,
				 /* dst-y.  */
				 BoxStartY (temp_boxes) - min_y + ty,
				 /* width.  */
				 BoxWidth (temp_boxes),
				 /* height, draw-params.  */
				 BoxHeight (temp_boxes), &draw_params);
	    }
	}
      else
	{
	  /* If the subcompositor is garbaged, composite the entire
	     view to the right location.  */
	  RenderComposite (buffer, subcompositor->target, op,
			   /* src-x, src-y.  */
			   0, 0,
			   /* dst-x.  */
			   view->abs_x - min_x + tx,
			   /* dst-y.  */
			   view->abs_y - min_y + ty,
			   /* width.  */
			   view_width,
			   /* height, draw-params.  */
			   view_height, &draw_params);

	present_success_garbaged:
	  /* Clear the damaged area, since it will either be drawn or
	     be obscured.  We didn't get a chance to clear the damage
	     earlier, since the compositor was garbaged.  */
	  pixman_region32_clear (&view->damage);

	  /* Also adjust the opaque and input regions here.  */

	  if (pixman_region32_not_empty (&view->opaque)
	      /* If the subcompositor is garbaged, the opaque region
		 must always be updated.  But if we are here because
		 the target is too old, it must only be updated if the
		 opaque region is also dirty.  */
	      && (IsGarbaged (subcompositor)
		  || IsOpaqueDirty (subcompositor)))
	    {
	      /* Translate the region into the global coordinate
		 space.  */
	      pixman_region32_translate (&list->view->opaque,
					 list->view->abs_x,
					 list->view->abs_y);

	      pixman_region32_intersect_rect (&temp, &view->opaque,
					      view->abs_x, view->abs_y,
					      view_width, view_height);
	      pixman_region32_union (&total_opaque, &temp, &total_opaque);

	      /* Translate it back.  */
	      pixman_region32_translate (&list->view->opaque,
					 -list->view->abs_x,
					 -list->view->abs_y);
	    }

	  if (pixman_region32_not_empty (&view->input)
	      /* Ditto for the input region.  */
	      && (IsGarbaged (subcompositor)
		  || IsInputDirty (subcompositor)))
	    {
	      /* Translate the region into the global coordinate
		 space.  */
	      pixman_region32_translate (&list->view->input,
					 list->view->abs_x,
					 list->view->abs_y);
	      pixman_region32_intersect_rect (&temp, &view->input,
					      view->abs_x, view->abs_y,
					      view_width, view_height);
	      pixman_region32_union (&total_input, &temp, &total_input);

	      /* Translate it back.  */
	      pixman_region32_translate (&list->view->input,
					 -list->view->abs_x,
					 -list->view->abs_y);
	    }
	}

    next_1:
      list = list->next;
    }
  while (list != subcompositor->inferiors);

 present_success:

  /* Swap changes to display.  */

  if (IsGarbaged (subcompositor) || age < 0 || age >= 3)
    RenderFinishRender (subcompositor->target, NULL);
  else
    /* Swap changes to display based on the update region.  */
    RenderFinishRender (subcompositor->target, &update_region);

 complete:

  if (IsGarbaged (subcompositor)
      || ((age < 0 || age >= 3)
	  && (IsInputDirty (subcompositor)
	      || IsOpaqueDirty (subcompositor))))
    {
      if (IsGarbaged (subcompositor)
	  || IsOpaqueDirty (subcompositor))
	{
	  /* The opaque region changed, so run any callbacks.  */
	  if (subcompositor->opaque_change)
	    {
	      /* Translate this to appear in the "virtual" coordinate
		 space.  */
	      pixman_region32_translate (&total_opaque, -min_x, -min_y);

	      subcompositor->opaque_change (subcompositor,
					    subcompositor->opaque_change_data,
					    &total_opaque);
	    }

	  pixman_region32_fini (&total_opaque);
	}

      if (IsGarbaged (subcompositor)
	  || IsInputDirty (subcompositor))
	{
	  /* The input region changed, so run any callbacks.  */
	  if (subcompositor->input_change)
	    {
	      /* Translate this to appear in the "virtual" coordinate
		 space.  */
	      pixman_region32_translate (&total_input, -min_x, -min_y);

	      subcompositor->input_change (subcompositor,
					   subcompositor->input_change_data,
					   &total_input);
	    }

	  pixman_region32_fini (&total_input);
	}
    }

  pixman_region32_fini (&temp);
  pixman_region32_fini (&update_region);

  /* The update has completed, so the compositor is no longer
     garbaged.  */
  subcompositor->state &= ~SubcompositorIsGarbaged;
  subcompositor->state &= ~SubcompositorIsOpaqueDirty;
  subcompositor->state &= ~SubcompositorIsInputDirty;

  /* Call the frame complete function if presentation did not happen.  */
  if (subcompositor->note_frame && !presented)
    subcompositor->note_frame (ModeComplete,
			       subcompositor->frame_counter,
			       subcompositor->note_frame_data);
}

void
SubcompositorExpose (Subcompositor *subcompositor, XEvent *event)
{
  List *list;
  View *view;
  int x, y, width, height, nboxes, min_x, min_y, tx, ty;
  pixman_box32_t extents, *boxes;
  int i;
  Operation op;
  pixman_region32_t temp;
  RenderBuffer buffer;
  DrawParams draw_params;

  /* Graphics exposures are not yet handled.  */
  if (event->type == GraphicsExpose)
    return;

  /* No target?  No update.  */
  if (!IsTargetAttached (subcompositor))
    return;

  x = event->xexpose.x + subcompositor->min_x;
  y = event->xexpose.y + subcompositor->min_y;
  width = event->xexpose.width;
  height = event->xexpose.height;

  min_x = subcompositor->min_x;
  min_y = subcompositor->min_y;
  tx = subcompositor->tx;
  ty = subcompositor->ty;

  extents.x1 = x;
  extents.y1 = y;
  extents.x2 = x + width;
  extents.y2 = y + height;

  view = NULL;

  /* Draw every subsurface overlapping the exposure region from the
     subcompositor onto the target.  Most importantly, do NOT update
     the bounds of the target, in case the exposure is in response to
     a resize.  */

  list = subcompositor->inferiors;

  /* Begin rendering.  This is unnecessary on XRender, but required on
     EGL to make the surface current and set the viewport.  */
  RenderStartRender (subcompositor->target);

  do
    {
      if (!list->view)
	goto next;

      if (IsViewUnmapped (list->view))
	{
	  list = list->view->inferior;
	  goto next;
	}

      if (IsSkipped (list->view))
	{
	  /* We must skip this view, as it represents (for
	     instance) a subsurface that has been added, but not
	     committed.  */
	  SetPartiallyMapped (subcompositor);
	  goto next;
	}

      if (!list->view->buffer)
	goto next;

      /* If the first mapped view contains everything, draw it with
	 PictOpSrc.  */
      if (!view && ViewContainsExtents (list->view, &extents))
	op = OperationSource;
      else
	{
	  /* Otherwise, fill the region with transparency for the
	     first update, and then use PictOpOver.  */

	  if (!view)
	    FillBoxesWithTransparency (subcompositor,
				       &extents, 1);

	  op = OperationOver;
	}

      view = list->view;

      /* Now, get the intersection of the rectangle with the view
	 bounds.  */
      pixman_region32_init_rect (&temp, x, y, width, height);
      pixman_region32_intersect_rect (&temp, &temp, view->abs_x,
				      view->abs_y, ViewWidth (view),
				      ViewHeight (view));

      /* Composite the contents according to OP.  */
      buffer = XLRenderBufferFromBuffer (view->buffer);
      boxes = pixman_region32_rectangles (&temp, &nboxes);

      /* Compute the transform.  */
      ViewComputeTransform (view, &draw_params, False);

      /* Update the attached buffer from any damage.  */
      RenderUpdateBufferForDamage (buffer, &list->view->damage,
				   &draw_params);

      /* If a fractional offset is set, recompute the transform again,
	 this time for drawing.  */
      if (list->view->fract_x != 0.0
	  || list->view->fract_y != 0.0)
	ViewComputeTransform (view, &draw_params, True);

      for (i = 0; i < nboxes; ++i)
	RenderComposite (buffer, subcompositor->target, op,
			 /* src-x.  */
			 BoxStartX (boxes[i]) - view->abs_x,
			 /* src-y.  */
			 BoxStartY (boxes[i]) - view->abs_y,
			 /* dst-x.  */
			 BoxStartX (boxes[i]) - min_x + tx,
			 /* dst-y.  */
			 BoxStartY (boxes[i]) - min_y + ty,
			 /* width, height.  */
			 BoxWidth (boxes[i]), BoxHeight (boxes[i]),
			 /* draw-params.  */
			 &draw_params);

      /* Free the scratch region used to compute the intersection.  */
      pixman_region32_fini (&temp);

    next:
      /* Move onto the next view.  */
      list = list->next;
    }
  while (list != subcompositor->inferiors);

  /* Swap changes to display.  */
  RenderFinishRender (subcompositor->target, NULL);
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

  /* Remove the presentation key.  */
  if (subcompositor->present_key)
    RenderCancelPresentationCallback (subcompositor->present_key);

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
	  || temp_x >= ViewWidth (list->view)
	  || temp_y >= ViewHeight (list->view))
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

void
SubcompositorFreeze (Subcompositor *subcompositor)
{
  SetFrozen (subcompositor);
}

void
SubcompositorUnfreeze (Subcompositor *subcompositor)
{
  subcompositor->state &= ~SubcompositorIsFrozen;
}

#endif
