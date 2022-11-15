/* Tests for the Wayland compositor running on the X server.

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

#include "test_harness.h"
#include "viewporter.h"

/* Tests for subsurfaces.  */

enum test_kind
  {
    MAP_WINDOW_KIND,
    SUBSURFACE_UNDER_KIND,
    SUBSURFACE_OVER_KIND,
    SUBSURFACE_MOVE_KIND,
    SUBSURFACE_STACKING_KIND,
    SUBSURFACE_TREE_KIND,
    SUBSURFACE_GROW_SHRINK_KIND,
    SUBSURFACE_STACKING_1_KIND,
    SUBSURFACE_DESYNC_KIND,
    SUBSURFACE_COMPLEX_DAMAGE_KIND,
    SUBSURFACE_SCALE_KIND,
    SUBSURFACE_REPARENT_KIND,
  };

static const char *test_names[] =
  {
    "map_window",
    "subsurface_under",
    "subsurface_over",
    "subsurface_move",
    "subsurface_stacking",
    "subsurface_tree",
    "subsurface_grow_shrink",
    "subsurface_stacking_1",
    "subsurface_desync",
    "subsurface_complex_damage",
    "subsurface_scale",
    "subsurface_reparent",
  };

struct test_subsurface
{
  /* The subsurface itself.  */
  struct wl_subsurface *subsurface;

  /* The associated surface.  */
  struct wl_surface *surface;
};

#define LAST_TEST       SUBSURFACE_REPARENT_KIND

/* The display.  */
static struct test_display *display;

/* The subcompositor interface.  */
static struct wl_subcompositor *subcompositor;

/* Test interfaces.  */
static struct test_interface test_interfaces[] =
  {
    { "wl_subcompositor", &subcompositor, &wl_subcompositor_interface, 1, },
  };

/* The test surface window.  */
static Window test_surface_window;

/* The test surface and Wayland surface.  */
static struct test_surface *test_surface;
static struct wl_surface *wayland_surface;

/* Various subsurfaces.  */
static struct test_subsurface *subsurfaces[11];

/* Various buffers.  */
static struct wl_buffer *tiny_png;
static struct wl_buffer *subsurface_base_png;
static struct wl_buffer *subsurface_damage_png;
static struct wl_buffer *subsurface_1_png;
static struct wl_buffer *subsurface_1_damaged_png;
static struct wl_buffer *subsurface_transparency_png;
static struct wl_buffer *cow_transparent_png;
static struct wl_buffer *gradient_png;
static struct wl_buffer *big_png;
static struct wl_buffer *small_png;
static struct wl_buffer *subsurface_1_complex_png;
static struct wl_buffer *subsurface_transparency_damage_png;
static struct wl_buffer *subsurface_stack_1_png;
static struct wl_buffer *subsurface_stack_2_png;

/* The test image ID.  */
static uint32_t current_test_image;



/* Forward declarations.  */
static void wait_frame_callback (struct wl_surface *);



static void
sleep_or_verify (void)
{
#ifndef INSPECTION
  char *dump_name;
#endif

#ifdef INSPECTION
  /* Sleep for 1 second so the human can see what is happening.  */
  sleep (1);
#else
  if (asprintf (&dump_name, "subsurface_test_%"PRIu32".dump",
		++current_test_image) < 0)
    die ("asprintf");

  verify_image_data (display, test_surface_window, dump_name);
  free (dump_name);
#endif
}

static void
submit_surface_opaque_region (struct wl_surface *surface,
			      int x, int y, int width, int height)
{
  struct wl_region *region;

  region = wl_compositor_create_region (display->compositor);
  wl_region_add (region, x, y, width, height);
  wl_surface_set_opaque_region (surface, region);
  wl_region_destroy (region);
}

static struct test_subsurface *
make_test_subsurface (void)
{
  struct test_subsurface *subsurface;

  subsurface = malloc (sizeof *subsurface);

  if (!subsurface)
    goto error_1;

  subsurface->surface
    = wl_compositor_create_surface (display->compositor);

  if (!subsurface->surface)
    goto error_2;

  subsurface->subsurface
    = wl_subcompositor_get_subsurface (subcompositor,
				       subsurface->surface,
				       wayland_surface);

  if (!subsurface->subsurface)
    goto error_3;

  return subsurface;

 error_3:
  wl_surface_destroy (subsurface->surface);
 error_2:
  free (subsurface);
 error_1:
  return NULL;
}

static struct test_subsurface *
make_test_subsurface_with_parent (struct test_subsurface *parent)
{
  struct test_subsurface *subsurface;

  subsurface = malloc (sizeof *subsurface);

  if (!subsurface)
    goto error_1;

  subsurface->surface
    = wl_compositor_create_surface (display->compositor);

  if (!subsurface->surface)
    goto error_2;

  subsurface->subsurface
    = wl_subcompositor_get_subsurface (subcompositor,
				       subsurface->surface,
				       parent->surface);

  if (!subsurface->subsurface)
    goto error_3;

  return subsurface;

 error_3:
  wl_surface_destroy (subsurface->surface);
 error_2:
  free (subsurface);
 error_1:
  return NULL;
}

static void
delete_subsurface_role (struct test_subsurface *subsurface)
{
  wl_subsurface_destroy (subsurface->subsurface);
  subsurface->subsurface = NULL;
}

static void
recreate_subsurface (struct test_subsurface *subsurface,
		     struct wl_surface *parent)
{
  subsurface->subsurface
    = wl_subcompositor_get_subsurface (subcompositor,
				       subsurface->surface,
				       parent);

  if (!subsurface->subsurface)
    report_test_failure ("failed to recreate subsurface");
}

static void
test_single_step (enum test_kind kind)
{
  struct wl_region *region;

  test_log ("running test step: %s", test_names[kind]);

  switch (kind)
    {
    case MAP_WINDOW_KIND:
      tiny_png = load_png_image (display, "tiny.png");

      if (!tiny_png)
	report_test_failure ("failed to load tiny.png");

      wl_surface_attach (wayland_surface, tiny_png, 0, 0);
      wl_surface_damage (wayland_surface, 0, 0, INT_MAX, INT_MAX);
      submit_surface_opaque_region (wayland_surface, 0, 0, 4, 4);
      wl_surface_commit (wayland_surface);
      break;

    case SUBSURFACE_UNDER_KIND:
      /* Make a subsurface, and then place it *under* the parent.
	 Wait for a frame callback, damage it, and check the
	 results.  */
      subsurfaces[0] = make_test_subsurface ();

      if (!subsurfaces[0])
	report_test_failure ("failed to create subsurface");

      subsurface_base_png
	= load_png_image (display, "subsurface_base.png");

      if (!subsurface_base_png)
	report_test_failure ("failed to load subsurface_base.png");

      subsurface_damage_png
	= load_png_image (display, "subsurface_damage.png");

      if (!subsurface_damage_png)
	report_test_failure ("failed to load subsurface_damage.png");

      wl_subsurface_place_below (subsurfaces[0]->subsurface,
				 wayland_surface);
      wl_surface_attach (subsurfaces[0]->surface, subsurface_base_png,
			 0, 0);
      wl_surface_damage (subsurfaces[0]->surface, 0, 0, 1024, 1024);
      submit_surface_opaque_region (subsurfaces[0]->surface,
				    0, 0, 1024, 1024);
      wl_surface_commit (subsurfaces[0]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      wl_surface_attach (subsurfaces[0]->surface, subsurface_damage_png,
			 0, 0);
      wl_surface_damage (subsurfaces[0]->surface, 256, 256, 512, 512);
      wl_surface_commit (subsurfaces[0]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      test_single_step (SUBSURFACE_OVER_KIND);
      break;

    case SUBSURFACE_OVER_KIND:
      /* Make a subsurface, and then place it over the parent.  Set an
	 opaque region and attach a buffer.  Move the subsurface. */
      subsurfaces[1] = make_test_subsurface ();

      if (!subsurfaces[1])
	report_test_failure ("failed to create subsurface");

      subsurface_1_png
	= load_png_image (display, "subsurface_1.png");

      if (!subsurface_1_png)
	report_test_failure ("failed to load subsurface_1.png");

      subsurface_1_damaged_png
	= load_png_image (display, "subsurface_1_damaged.png");

      if (!subsurface_1_damaged_png)
	report_test_failure ("failed to load subsurface_1_damaged.png");

      /* Now, move the surface a little.  */
      wl_subsurface_set_position (subsurfaces[1]->subsurface, 40, 40);

      /* Set an opaque region.  */
      submit_surface_opaque_region (subsurfaces[1]->surface,
				    0, 0, 256, 256);

      /* Damage the subsurface.  */
      wl_surface_attach (subsurfaces[1]->surface, subsurface_1_png,
			 0, 0);
      wl_surface_damage (subsurfaces[1]->surface, 0, 0, 256, 256);

      /* Commit the subsurface and its parent.  */
      wl_surface_commit (subsurfaces[1]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Change the opaque region.  */
      region = wl_compositor_create_region (display->compositor);
      wl_region_add (region, 0, 0, 256, 256);
      wl_region_subtract (region, 128, 128, 70, 70);
      wl_surface_set_opaque_region (subsurfaces[1]->surface, region);
      wl_region_destroy (region);

      /* Attach a buffer with transparency.  */
      wl_surface_attach (subsurfaces[1]->surface, subsurface_1_damaged_png,
			 0, 0);
      wl_surface_damage (subsurfaces[1]->surface, 128, 128, 70, 70);

      /* Commit the subsurface and its parent.  */
      wl_surface_commit (subsurfaces[1]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();
      test_single_step (SUBSURFACE_MOVE_KIND);
      break;

    case SUBSURFACE_MOVE_KIND:
      /* Move the subsurface to the right a little and check that the
	 correct area is exposed.  */
      wl_subsurface_set_position (subsurfaces[1]->subsurface, 50, 50);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Move the subsurface to the right a little more.  */
      wl_subsurface_set_position (subsurfaces[1]->subsurface, 100, 100);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();
      test_single_step (SUBSURFACE_STACKING_KIND);
      break;

    case SUBSURFACE_STACKING_KIND:
      /* Stack the first test subsurface above the other test
	 subsurface.  Then, load a transparent pattern into the first
	 subsurface to verify the stacking order.  */

      subsurface_transparency_png
	= load_png_image (display, "subsurface_transparency.png");

      if (!subsurface_transparency_png)
	report_test_failure ("failed to load subsurface_transparency.png");

      wl_subsurface_place_above (subsurfaces[0]->subsurface,
				 subsurfaces[1]->surface);
      wl_surface_attach (subsurfaces[0]->surface,
			 subsurface_transparency_png, 0, 0);

      /* Also update the opaque region.  */
      submit_surface_opaque_region (subsurfaces[0]->surface,
				    640, 640, 128, 128);
      wl_surface_damage (subsurfaces[0]->surface, 0, 0, 1024, 1024);
      wl_surface_commit (subsurfaces[0]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Afterwards, damage the other test subsurface and see if the
	 damage is composited correctly.  */
      submit_surface_opaque_region (subsurfaces[1]->surface,
				    0, 0, 256, 256);
      wl_surface_attach (subsurfaces[1]->surface, subsurface_1_png,
			 0, 0);
      wl_surface_damage (subsurfaces[1]->surface, 128, 128, 70, 70);

      /* Commit the subsurface and its parent.  */
      wl_surface_commit (subsurfaces[1]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      test_single_step (SUBSURFACE_TREE_KIND);
      break;

    case SUBSURFACE_TREE_KIND:
      /* Create two subsurfaces attached to the other test subsurface;
	 1 below, 1 over.  */
      subsurfaces[2]
	= make_test_subsurface_with_parent (subsurfaces[1]);
      subsurfaces[3]
	= make_test_subsurface_with_parent (subsurfaces[1]);

      if (!subsurfaces[2] || !subsurfaces[3])
	report_test_failure ("failed to create subsurfaces");

      gradient_png = load_png_image (display, "gradient.png");

      if (!gradient_png)
	report_test_failure ("failed to load gradient.png");

      cow_transparent_png = load_png_image (display, "cow_transparent.png");

      if (!cow_transparent_png)
	report_test_failure ("failed to load cow_transparent.png");

      wl_subsurface_place_below (subsurfaces[2]->subsurface,
				 subsurfaces[1]->surface);
      wl_subsurface_place_above (subsurfaces[3]->subsurface,
				 subsurfaces[1]->surface);

      /* Attach buffers to the various subsurfaces and commit
	 them.  */
      wl_surface_attach (subsurfaces[2]->surface, gradient_png,
			 0, 0);
      wl_surface_attach (subsurfaces[3]->surface, cow_transparent_png,
			 0, 0);
      submit_surface_opaque_region (subsurfaces[2]->surface, 0, 0,
				    100, 300);
      wl_surface_damage (subsurfaces[2]->surface, 0, 0, INT_MAX, INT_MAX);
      wl_surface_damage (subsurfaces[3]->surface, 0, 0, INT_MAX, INT_MAX);
      wl_surface_commit (subsurfaces[2]->surface);
      wl_surface_commit (subsurfaces[3]->surface);

      /* Nothing should happen to the surface here.  */
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Nor here.  */
      wl_surface_commit (subsurfaces[1]->surface);

      /* Until here.  */
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Now, unmap the surface.  Subsurfaces 2, 3, 4 should all
	 disappear!  */
      wl_surface_attach (subsurfaces[1]->surface, NULL, 0, 0);
      wl_surface_commit (subsurfaces[1]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Map it again.  All subsurfaces should appear again, this time
	 with the opaque circle in subsurface 2 transparent.  */
      wl_surface_attach (subsurfaces[1]->surface, subsurface_1_damaged_png,
			 0, 0);

      region = wl_compositor_create_region (display->compositor);
      wl_region_add (region, 0, 0, 256, 256);
      wl_region_subtract (region, 128, 128, 70, 70);
      wl_surface_set_opaque_region (subsurfaces[1]->surface, region);
      wl_region_destroy (region);

      wl_surface_damage (subsurfaces[1]->surface, 128, 128, 70, 70);
      wl_surface_commit (subsurfaces[1]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Move subsurface 1 to where it gets obscured by the opaque
	 region of the view above.  Verify that the views all move
	 accordingly.  */
      wl_subsurface_set_position (subsurfaces[1]->subsurface, 640, 640);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Move one of the views further to the right.  */
      wl_subsurface_set_position (subsurfaces[2]->subsurface, 100, 100);
      wl_surface_commit (subsurfaces[1]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      test_single_step (SUBSURFACE_GROW_SHRINK_KIND);
      break;

    case SUBSURFACE_GROW_SHRINK_KIND:
      /* This test attaches a big buffer to the second subsurface.
	 Then, it attaches a smaller buffer to the second subsurface.
	 But first, it moves the first subsurface back to 40, 40, and
	 unmaps the second and third subsurfaces.  */

      big_png = load_png_image (display, "big.png");

      if (!big_png)
	report_test_failure ("failed to load big.png");

      small_png = load_png_image (display, "small.png");

      if (!small_png)
	report_test_failure ("failed to load small.png");

      wl_subsurface_set_position (subsurfaces[1]->subsurface, 40, 40);

      wl_surface_attach (subsurfaces[3]->surface, NULL, 0, 0);
      wl_surface_attach (subsurfaces[2]->surface, NULL, 0, 0);
      wl_surface_commit (subsurfaces[3]->surface);
      wl_surface_commit (subsurfaces[2]->surface);
      wl_surface_commit (subsurfaces[1]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      submit_surface_opaque_region (subsurfaces[1]->surface, 0, 0, 300,
				    300);
      wl_surface_attach (subsurfaces[1]->surface, big_png, 0, 0);
      wl_surface_damage (subsurfaces[1]->surface, 0, 0, 300, 300);
      wl_surface_commit (subsurfaces[1]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      submit_surface_opaque_region (subsurfaces[1]->surface, 0, 0, 150,
				    150);
      wl_surface_attach (subsurfaces[1]->surface, small_png, 0, 0);
      wl_surface_damage (subsurfaces[1]->surface, 0, 0, 150, 150);
      wl_surface_commit (subsurfaces[1]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      test_single_step (SUBSURFACE_STACKING_1_KIND);
      break;

    case SUBSURFACE_STACKING_1_KIND:
      /* Move the second subsurface above the first.  Then, move the
	 third subsurface above the fourth, and the fourth below the
	 second.  */
      wl_subsurface_place_above (subsurfaces[1]->subsurface,
				 subsurfaces[0]->surface);
      wl_subsurface_place_above (subsurfaces[2]->subsurface,
				 subsurfaces[3]->surface);
      wl_subsurface_place_below (subsurfaces[3]->subsurface,
				 subsurfaces[1]->surface);

      /* Next, attach some buffers to the third and fourth
	 surface.  */
      wl_surface_attach (subsurfaces[2]->surface, gradient_png, 0, 0);
      wl_surface_attach (subsurfaces[3]->surface, cow_transparent_png,
			 0, 0);
      submit_surface_opaque_region (subsurfaces[2]->surface, 0, 0,
				    100, 300);
      wl_surface_commit (subsurfaces[2]->surface);
      wl_surface_commit (subsurfaces[3]->surface);

      /* Commit the first subsurface.  */
      wl_surface_commit (subsurfaces[1]->surface);
      wait_frame_callback (wayland_surface);

      /* The result of the test should be as follows.  Above the first
	 subsurface lies the fourth subsurface (cow_transparent.png),
	 above which is the second subsurface (small.png), above which
	 is the third subsurface (gradient.png).  */
      sleep_or_verify ();

      /* Next, test moving the third subsurface immediately below the
	 fourth.  */
      wl_subsurface_place_below (subsurfaces[2]->subsurface,
				 subsurfaces[3]->surface);
      wl_surface_commit (subsurfaces[1]->surface);
      wait_frame_callback (wayland_surface);

      /* The result of the test should be as follows.  Above the first
	 subsurface lies the third subsurface (gradient.png), above
	 which lies the fourth subsurface (cow_transparent.png), and
	 finally the third (small.png).  */
      sleep_or_verify ();

      test_single_step (SUBSURFACE_DESYNC_KIND);
      break;

    case SUBSURFACE_DESYNC_KIND:
      /* Hide every other subsurface other than subsurfaces[0].  */

      wl_surface_attach (subsurfaces[1]->surface, NULL, 0, 0);
      wl_surface_attach (subsurfaces[2]->surface, NULL, 0, 0);
      wl_surface_attach (subsurfaces[3]->surface, NULL, 0, 0);
      wl_surface_commit (subsurfaces[3]->surface);
      wl_surface_commit (subsurfaces[2]->surface);
      wl_surface_commit (subsurfaces[1]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Create a tree of subsurfaces as follows:

	 subsurfaces[4] (as a child of subsurfaces[0])
	   subsurfaces[5]
	   subsurfaces[6]
	     subsurfaces[7]
	     subsurfaces[8]  */

      subsurfaces[4] = make_test_subsurface_with_parent (subsurfaces[0]);

      if (!subsurfaces[4])
	report_test_failure ("failed to create subsurfaces");

      subsurfaces[5]
	= make_test_subsurface_with_parent (subsurfaces[4]);
      subsurfaces[6]
	= make_test_subsurface_with_parent (subsurfaces[4]);

      if (!subsurfaces[5] || !subsurfaces[6])
	report_test_failure ("failed to create subsurfaces");

      subsurfaces[7]
	= make_test_subsurface_with_parent (subsurfaces[6]);
      subsurfaces[8]
	= make_test_subsurface_with_parent (subsurfaces[6]);

      if (!subsurfaces[7] || !subsurfaces[8])
	report_test_failure ("failed to create subsurfaces");

      /* Confirm all the children.  Attach tiny_png to prevent some
	 subsurfaces from being unmapped.  */
      wl_surface_attach (subsurfaces[4]->surface, tiny_png, 0, 0);
      wl_surface_attach (subsurfaces[6]->surface, tiny_png, 0, 0);
      wl_surface_damage (subsurfaces[4]->surface, 0, 0, 4, 4);
      wl_surface_damage (subsurfaces[6]->surface, 0, 0, 4, 4);

      wl_surface_commit (subsurfaces[6]->surface);
      wl_surface_commit (subsurfaces[4]->surface);
      wl_surface_commit (subsurfaces[0]->surface);

      /* Now, make subsurfaces[8] desynchronous.  */
      wl_subsurface_set_desync (subsurfaces[8]->subsurface);

      /* Attach gradient.png to subsurfaces[8].  While subsurfaces[8]
	 is desync, 6 and 4 are both synchronous.  */
      wl_surface_attach (subsurfaces[8]->surface, gradient_png, 0, 0);
      wl_surface_damage (subsurfaces[8]->surface, 0, 0, 100, 300);
      wl_surface_commit (subsurfaces[8]->surface);

      /* Thus, the state should not appear.  */
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Next, make subsurfaces[6] desynchronous.  */
      wl_subsurface_set_desync (subsurfaces[6]->subsurface);

      /* gradient.png should still not appear.  */
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Finally, make subsurfaces[0] and subsurfaces[4]
	 desynchronous.  As subsurfaces[0] has cached state (it was
	 committed earlier), its state and that of all its children
	 should be applied.  As a result, gradient.png should appear
	 without having to commit subsurfaces[8] again.  */
      wl_subsurface_set_desync (subsurfaces[4]->subsurface);
      wl_subsurface_set_desync (subsurfaces[0]->subsurface);

      /* Commit wayland_surface to get a frame callback.  */
      wait_frame_callback (wayland_surface);

      /* gradient.png should now appear onscreen.  */
      sleep_or_verify ();

      /* Next, make subsurfaces[6] synchronous.  Attach small.png to
	 subsurfaces[8] and gradient.png to subsurfaces[5].  */
      wl_subsurface_set_sync (subsurfaces[6]->subsurface);

      wl_surface_attach (subsurfaces[8]->surface, small_png, 0, 0);
      wl_surface_damage (subsurfaces[8]->surface, 0, 0, 150, 150);
      wl_surface_commit (subsurfaces[8]->surface);

      /* Commit subsurfaces[4] for a frame callback.  Verify that the
	 contents did not change, as subsurfaces[6] is
	 synchronous.  */
      wait_frame_callback (subsurfaces[4]->surface);
      sleep_or_verify ();

      /* Commit subsurfaces[6], then commit subsurfaces[4] to apply
	 the pending state.  The state in subsurfaces[8] should be
	 applied.  */
      wl_surface_commit (subsurfaces[6]->surface);
      wait_frame_callback (subsurfaces[4]->surface);
      sleep_or_verify ();

      /* Now, attach big_png to subsurfaces[8].  */
      wl_surface_attach (subsurfaces[8]->surface, big_png, 0, 0);
      wl_surface_damage (subsurfaces[8]->surface, 0, 0, 300, 300);
      wl_surface_commit (subsurfaces[8]->surface);

      /* Make subsurfaces[6]->surface desynchronous.  Nothing should
	 appear despite subsurfaces[8] having become desynchronous, as
	 subsurfaces[6] has no cached state.  subsurfaces[4] is only
	 committed to get a frame callback.  */
      wl_subsurface_set_desync (subsurfaces[6]->subsurface);
      wait_frame_callback (subsurfaces[4]->surface);
      sleep_or_verify ();

      /* Now, apply a buffer transform to subsurfaces[8].  Upon
	 commit, big.png should be displayed rotated 90 degrees
	 clockwise, by the buffer transform being merged with the
	 cached state of the surface prior to being committed.  */
      wl_surface_set_buffer_transform (subsurfaces[8]->surface,
				       WL_OUTPUT_TRANSFORM_90);
      wait_frame_callback (subsurfaces[8]->surface);
      sleep_or_verify ();

      /* Now, attach subsurface_1.png in subsurfaces[7].  Nothing
	 should be displayed.  */
      wl_surface_attach (subsurfaces[7]->surface, subsurface_1_png,
			 0, 0);
      wl_surface_damage (subsurfaces[7]->surface, 0, 0, 256, 256);
      wl_surface_commit (subsurfaces[7]->surface);

      /* Also move the subsurface a little.  */
      wl_subsurface_set_position (subsurfaces[7]->subsurface, 100, 100);

      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Commit subsurfaces[6].  As it is now desynchronous,
	 subsurface_1.png should be displayed.  */
      wait_frame_callback (subsurfaces[6]->surface);
      sleep_or_verify ();

      test_single_step (SUBSURFACE_COMPLEX_DAMAGE_KIND);
      break;

    case SUBSURFACE_COMPLEX_DAMAGE_KIND:
      /* Hide subsurfaces[8].  After this, subsurfaces[7].png should
	 be displayed entirely.  */
      wl_surface_attach (subsurfaces[8]->surface, NULL, 0, 0);
      wait_frame_callback (subsurfaces[8]->surface);
      sleep_or_verify ();

      /* Move subsurfaces[8] to 270, 270.  This adds some damage to
	 quadrant D.  */
      wl_subsurface_set_position (subsurfaces[8]->subsurface, 600, 600);

      /* Make subsurfaces[8] synchronous.  */
      wl_subsurface_set_sync (subsurfaces[8]->subsurface);

      /* Attach a buffer to subsurfaces[8].  */
      wl_surface_set_buffer_transform (subsurfaces[8]->surface,
				       WL_OUTPUT_TRANSFORM_NORMAL);
      wl_surface_attach (subsurfaces[8]->surface, small_png, 0, 0);
      wl_surface_damage (subsurfaces[8]->surface, 0, 0, 150, 150);
      wl_surface_commit (subsurfaces[8]->surface);

      /* Nothing should appear.  */
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Attach subsurface_1_complex.png to subsurfaces[7], and apply
	 detailed damage.  Verify that the result remains correct
	 despite the subcompositor performing damage
	 simplification.  */
      subsurface_1_complex_png
	= load_png_image (display, "subsurface_1_complex.png");

      if (!subsurface_1_complex_png)
	report_test_failure ("failed to load subsurface_1_complex.png");

      subsurface_transparency_damage_png
	= load_png_image (display, "subsurface_transparency_damage.png");

      if (!subsurface_transparency_damage_png)
	report_test_failure ("failed to load subsurface_transparency_damage.png");

      /* Set a complex opaque region.  */
      region = wl_compositor_create_region (display->compositor);
      wl_region_add (region, 0, 0, 256, 256);
      wl_region_subtract (region, 128, 128, 70, 70);
      wl_surface_set_opaque_region (subsurfaces[0]->surface, region);
      wl_region_destroy (region);

      /* Attach subsurface_1_complex.png.  */
      wl_surface_attach (subsurfaces[7]->surface, subsurface_1_complex_png,
			 0, 0);
      wl_surface_damage (subsurfaces[7]->surface, 20, 24, 139, 55);
      wl_surface_damage (subsurfaces[7]->surface, 31, 108, 25, 24);
      wl_surface_damage (subsurfaces[7]->surface, 16, 179, 10, 9);
      wl_surface_damage (subsurfaces[7]->surface, 80, 85, 73, 43);
      wl_surface_damage (subsurfaces[7]->surface, 153, 56, 39, 53);
      wl_surface_damage (subsurfaces[7]->surface, 125, 56, 28, 29);
      wl_surface_damage (subsurfaces[7]->surface, 128, 128, 70, 70);
      wl_surface_commit (subsurfaces[7]->surface);

      /* Make subsurfaces[6] and subsurfaces[4] synchronous.  */
      wl_subsurface_set_sync (subsurfaces[4]->subsurface);
      wl_subsurface_set_sync (subsurfaces[6]->subsurface);

      /* Commit subsurfaces[6]->surface and
	 subsurfaces[4]->surface.  */
      wl_surface_commit (subsurfaces[6]->surface);
      wl_surface_commit (subsurfaces[4]->surface);

      /* Now, apply damage to quadrants C and D.  Attach
	 subsurface_transparency_damage.png to subsurfaces[0].  */
      wl_surface_attach (subsurfaces[0]->surface,
			 subsurface_transparency_damage_png,
			 0, 0);
      wl_surface_damage (subsurfaces[0]->surface, 52, 882, 810, 58);
      wait_frame_callback (subsurfaces[0]->surface);
      sleep_or_verify ();

      test_single_step (SUBSURFACE_SCALE_KIND);
      break;

    case SUBSURFACE_SCALE_KIND:
      /* Set the global output scale to two, then three.  Verify the
	 subcompositor tree each time.  */
      test_set_scale (display, 2);
      wait_frame_callback (wayland_surface);

      /* Sleep for 1 second to wait for the scale hooks to completely
	 run.  */
      sleep (1);
      sleep_or_verify ();

      test_set_scale (display, 3);
      wait_frame_callback (wayland_surface);

      /* Sleep for 1 second to wait for the scale hooks to completely
	 run.  */
      sleep (1);
      sleep_or_verify ();

      /* Reset the scale.  */
      test_set_scale (display, 1);
      wait_frame_callback (wayland_surface);

      /* Sleep for 1 second to wait for the scale hooks to completely
	 run.  */
      sleep (1);

      test_single_step (SUBSURFACE_REPARENT_KIND);
      break;

    case SUBSURFACE_REPARENT_KIND:
      subsurface_stack_1_png
	= load_png_image (display, "subsurface_stack_1.png");

      if (!subsurface_stack_1_png)
	report_test_failure ("failed to load subsurface_stack_1.png");

      subsurface_stack_2_png
	= load_png_image (display, "subsurface_stack_2.png");

      if (!subsurface_stack_2_png)
	report_test_failure ("failed to load subsurface_stack_2.png");

      /* Delete subsurfaces[6].  Verify that after doing so,
	 subsurfaces[7] and subsurfaces[8] disappear.  */
      delete_subsurface_role (subsurfaces[6]);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Next, recreate the subsurface.  The children should not
	 become visible, as subsurfaces[4] was not committed.  */
      recreate_subsurface (subsurfaces[6],
			   subsurfaces[4]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Finally, create two new subsurfaces.  Both are children of
	 subsurfaces[6].  The first, subsurfaces[9] has
	 subsurface_stack_1 applied, and is placed at 600, 600.  */
      subsurfaces[9]
	= make_test_subsurface_with_parent (subsurfaces[6]);

      if (!subsurfaces[9])
	report_test_failure ("failed to create subsurface");

      wl_surface_attach (subsurfaces[9]->surface, subsurface_stack_1_png,
			 0, 0);
      wl_surface_damage (subsurfaces[9]->surface, 0, 0, 100, 100);
      wl_surface_commit (subsurfaces[9]->surface);
      wl_subsurface_set_position (subsurfaces[9]->subsurface, 600, 600);

      /* The second, subsurfaces[10] is a child of subsurfaces[6].  It
	 is also placed at 600, 600, on top of subsurfaces[9].  */
      subsurfaces[10]
	= make_test_subsurface_with_parent (subsurfaces[6]);

      if (!subsurfaces[10])
	report_test_failure ("failed to create subsurface");

      wl_surface_attach (subsurfaces[10]->surface, subsurface_stack_2_png,
			 0, 0);
      wl_surface_damage (subsurfaces[10]->surface, 0, 0, 100, 100);
      wl_surface_commit (subsurfaces[10]->surface);
      wl_subsurface_set_position (subsurfaces[10]->subsurface, 600, 600);

      /* Now, commit subsurfaces[6].  Nothing should become
	 visible.  */
      wl_surface_commit (subsurfaces[6]->surface);
      wait_frame_callback (wayland_surface);
      sleep_or_verify ();

      /* Finally, commit subsurfaces[4] and then subsurfaces[0].
	 Everything should now show up: a completely white W on top of
	 a translucent red cube, partly obscuring small.png, at 600,
	 600.  */
      wl_surface_commit (subsurfaces[4]->surface);
      wait_frame_callback (subsurfaces[0]->surface);
      sleep_or_verify ();

      /* Test that subsurface actions are applied in the right order
	 for newly unparented (pending) subsurfaces.  Destroy and
	 recreate subsurfaces[9] and subsurfaces[10].  */
      delete_subsurface_role (subsurfaces[9]);
      delete_subsurface_role (subsurfaces[10]);
      recreate_subsurface (subsurfaces[9], subsurfaces[6]->surface);
      recreate_subsurface (subsurfaces[10], subsurfaces[6]->surface);
      wl_subsurface_set_position (subsurfaces[9]->subsurface, 600, 600);
      wl_subsurface_set_position (subsurfaces[10]->subsurface, 600, 600);

      /* Place subsurfaces[9] above subsurfaces[10].  At this point,
	 the subsurfaces have not yet been confirmed.  */
      wl_subsurface_place_above (subsurfaces[9]->subsurface,
				 subsurfaces[10]->surface);

      /* Commit the parents.  W should now be tinted red.  */
      wl_surface_commit (subsurfaces[6]->surface);
      wl_surface_commit (subsurfaces[4]->surface);
      wait_frame_callback (subsurfaces[0]->surface);
      sleep_or_verify ();

      break;
    }

  if (kind == LAST_TEST)
    test_complete ();
}



static void
handle_test_surface_mapped (void *data, struct test_surface *surface,
			    uint32_t xid, const char *display_string)
{
  /* Sleep for 1 second to ensure that the window is exposed and
     redirected.  */
  sleep (1);

  /* Start the test.  */
  test_surface_window = xid;

  /* Run the test again.  */
  test_single_step (SUBSURFACE_UNDER_KIND);
}

static const struct test_surface_listener test_surface_listener =
  {
    handle_test_surface_mapped,
  };



static void
handle_wl_callback_done (void *data, struct wl_callback *callback,
			   uint32_t callback_data)
{
  bool *flag;

  wl_callback_destroy (callback);

  /* Now tell wait_frame_callback to break out of the loop.  */
  flag = data;
  *flag = true;
}

static const struct wl_callback_listener wl_callback_listener =
  {
    handle_wl_callback_done,
  };



static void
wait_frame_callback (struct wl_surface *surface)
{
  struct wl_callback *callback;
  bool flag;

  /* Commit surface and wait for a frame callback.  */

  callback = wl_surface_frame (surface);
  flag = false;

  wl_callback_add_listener (callback, &wl_callback_listener,
			    &flag);
  wl_surface_commit (surface);

  while (!flag)
    {
      if (wl_display_dispatch (display->display) == -1)
        die ("wl_display_dispatch");
    }
}

static void
run_test (void)
{
  if (!make_test_surface (display, &wayland_surface,
			  &test_surface))
    report_test_failure ("failed to create test surface");

  test_surface_add_listener (test_surface, &test_surface_listener,
			     NULL);
  test_single_step (MAP_WINDOW_KIND);

  while (true)
    {
      if (wl_display_dispatch (display->display) == -1)
        die ("wl_display_dispatch");
    }
}

int
main (void)
{
  test_init ();
  display = open_test_display (test_interfaces,
			       ARRAYELTS (test_interfaces));

  if (!display)
    report_test_failure ("failed to open display");

  run_test ();
}
