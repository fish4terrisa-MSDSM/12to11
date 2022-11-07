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

#include "12to11-test.h"

/* This file is included by seat.c at the very bottom, so it does not
   have to include anything itself! */

typedef struct _TestSeatController TestSeatController;
typedef struct _TestXIModifierState TestXIModifierState;
typedef struct _TestXIValuatorState TestXIValuatorState;
typedef struct _TestXIButtonState TestXIButtonState;

/* The current test seat counter.  */
static unsigned int test_seat_counter;

/* The test serial counter.  */
static unsigned long request_serial_counter;

struct _TestSeatController
{
  /* The associated seat.  */
  Seat *seat;

  /* The associated controller resource.  */
  struct wl_resource *resource;
};

struct _TestXIModifierState
{
  /* Modifier state.  These fields mean the same as they do in
     XIModifierState.  */
  int base;
  int latched;
  int locked;
  int effective;
};

struct _TestXIValuatorState
{
  /* The mask of set valuators.  */
  unsigned char *mask;

  /* Sparse array of valuators.  */
  double *values;

  /* The length of the mask.  */
  size_t mask_len;

  /* The number of valuators set.  */
  int num_valuators;
};

struct _TestXIButtonState
{
  /* Mask of set buttons.  Always between 0 and 8.  */
  unsigned char mask[XIMaskLen (8)];
};



static void
DestroyXIModifierState (struct wl_client *client,
			struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
SetValues (struct wl_client *client, struct wl_resource *resource,
	   int32_t base, int32_t latched, int32_t locked, int32_t effective)
{
  TestXIModifierState *state;

  state = wl_resource_get_user_data (resource);
  state->base = base;
  state->latched = latched;
  state->locked = locked;
  state->effective = effective;
}

static const struct test_XIModifierState_interface XIModifierState_impl =
  {
    .destroy = DestroyXIModifierState,
    .set_values = SetValues,
  };

static void
HandleXIModifierStateDestroy (struct wl_resource *resource)
{
  TestXIModifierState *state;

  state = wl_resource_get_user_data (resource);
  XLFree (state);
}



static void
DestroyXIButtonState (struct wl_client *client,
		      struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
AddButton (struct wl_client *client, struct wl_resource *resource,
	   uint32_t button)
{
  TestXIButtonState *state;

  state = wl_resource_get_user_data (resource);

  if (button < 1 || button > 8)
    /* The button is invalid.  */
    wl_resource_post_error (resource, TEST_MANAGER_ERROR_INVALID_BUTTON,
			    "invalid button specified");
  else
    SetMask (state->mask, button);
}

static void
RemoveButton (struct wl_client *client, struct wl_resource *resource,
	      uint32_t button)
{
  TestXIButtonState *state;

  state = wl_resource_get_user_data (resource);

  if (button < 1 || button > 8)
    wl_resource_post_error (resource, TEST_MANAGER_ERROR_INVALID_BUTTON,
			    "invalid button specified");
  else
    ClearMask (state->mask, button);
}

static const struct test_XIButtonState_interface XIButtonState_impl =
  {
    .destroy = DestroyXIButtonState,
    .add_button = AddButton,
    .remove_button = RemoveButton,
  };

static void
HandleXIButtonStateDestroy (struct wl_resource *resource)
{
  TestXIButtonState *state;

  state = wl_resource_get_user_data (resource);
  XLFree (state);
}



static void
AddValuatorToTestXIValuatorState (struct wl_client *client,
				  struct wl_resource *resource,
				  uint32_t valuator,
				  wl_fixed_t value)
{
  TestXIValuatorState *state;
  double *old_values, *new_values;
  size_t i, j;

  if (valuator < 1 || valuator > 65535)
    {
      /* The valuator cannot be represented.  */
      wl_resource_post_error (resource, TEST_MANAGER_ERROR_INVALID_VALUATOR,
			      "the specified valuator cannot be represented");
      return;
    }

  state = wl_resource_get_user_data (resource);

  /* Check if the valuator is already present and post a
     value_exists error if so.  */
  if (XIMaskLen (valuator) <= state->mask_len
      && MaskIsSet (state->mask, valuator))
    wl_resource_post_error (resource, TEST_MANAGER_ERROR_VALUE_EXISTS,
			    "the specified valuator is already set");
  else
    {
      /* If the mask needs to be expanded, do it now.  */
      if (state->mask_len < XIMaskLen (valuator))
	{
	  state->mask = XLRealloc (state->mask,
				   state->mask_len);

	  /* Clear the newly allocated part of the mask.  */
	  memset (state->mask + state->mask_len,
		  0, XIMaskLen (valuator) - state->mask_len);
	}

      SetMask (state->mask, valuator);
      state->num_valuators++;

      /* Now, rewrite the sparse array of values.  */
      old_values = state->values;
      new_values = XLCalloc (state->num_valuators,
			     sizeof *state->values);

      for (i = 0, j = 0; i < MAX (state->mask_len,
				  XIMaskLen (valuator)) * 8; ++i)
	{
	  if (i == valuator)
	    /* Insert the new value.  */
	    new_values[j++] = wl_fixed_to_double (value);
	  else if (XIMaskIsSet (state->mask, valuator))
	    /* Use the old value.  */
	    new_values[j++] = *old_values++;
	}

      /* Free the old values.  */
      XLFree (state->values);

      /* Assign the new values and mask length to the state.  */
      state->values = new_values;
      state->mask_len = MAX (state->mask_len,
			     XIMaskLen (valuator));
    }
}

static const struct test_XIValuatorState_interface XIValuatorState_impl =
  {
    .add_valuator = AddValuatorToTestXIValuatorState,
  };

static void
HandleXIValuatorStateDestroy (struct wl_resource *resource)
{
  TestXIValuatorState *state;

  state = wl_resource_get_user_data (resource);

  /* Free the mask.  */
  XLFree (state->mask);

  /* Free the values.  */
  XLFree (state->values);

  /* Free the state itself.  */
  XLFree (state);
}



static void
DestroyTestSeatController (struct wl_client *client,
			   struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
BindSeat (struct wl_client *client, struct wl_resource *resource,
	  uint32_t version, uint32_t id)
{
  TestSeatController *controller;

  controller = wl_resource_get_user_data (resource);

  if (!version || version > 8)
    wl_resource_post_error (resource, TEST_MANAGER_ERROR_BAD_SEAT_VERSION,
			    "the specified version of the wl_seat interface"
			    " is not supported");
  else
    /* Bind the resource to the seat.  */
    HandleBind1 (client, controller->seat, version, id);
}

static void
GetXIModifierState (struct wl_client *client, struct wl_resource *resource,
		    uint32_t id)
{
  TestXIModifierState *state;
  struct wl_resource *state_resource;

  state = XLSafeMalloc (sizeof *state);

  if (!state)
    {
      /* Allocating the state structure failed.  */
      wl_resource_post_no_memory (resource);
      return;
    }

  /* Now, try to create the modifier state resource.  */
  state_resource
    = wl_resource_create (client, &test_XIModifierState_interface,
			  wl_resource_get_version (resource), id);

  if (!state_resource)
    {
      /* Allocating the resource failed.  */
      XLFree (state);
      wl_resource_post_no_memory (resource);

      return;
    }

  /* Clear the state.  */
  memset (state, 0, sizeof *state);

  /* Set the resource data.  */
  wl_resource_set_implementation (state_resource, &XIModifierState_impl,
				  state, HandleXIModifierStateDestroy);
}

static void
GetXIButtonState (struct wl_client *client, struct wl_resource *resource,
		  uint32_t id)
{
  TestXIButtonState *state;
  struct wl_resource *state_resource;

  state = XLSafeMalloc (sizeof *state);

  if (!state)
    {
      /* Allocating the state structure failed.  */
      wl_resource_post_no_memory (resource);
      return;
    }

  /* Now, try to create the button state resource.  */
  state_resource
    = wl_resource_create (client, &test_XIButtonState_interface,
			  wl_resource_get_version (resource), id);

  if (!state_resource)
    {
      /* Allocating the resource failed.  */
      XLFree (state);
      wl_resource_post_no_memory (resource);

      return;
    }

  /* Clear the state.  */
  memset (state, 0, sizeof *state);

  /* Set the resource data.  */
  wl_resource_set_implementation (state_resource, &XIButtonState_impl,
				  state, HandleXIButtonStateDestroy);
}

static void
GetXIValuatorState (struct wl_client *client, struct wl_resource *resource,
		    uint32_t id)
{
  TestXIValuatorState *state;
  struct wl_resource *state_resource;

  state = XLSafeMalloc (sizeof *state);

  if (!state)
    {
      /* Allocating the state structure failed.  */
      wl_resource_post_no_memory (resource);
      return;
    }

  /* Now, try to create the button state resource.  */
  state_resource
    = wl_resource_create (client, &test_XIValuatorState_interface,
			  wl_resource_get_version (resource), id);

  if (!state_resource)
    {
      /* Allocating the resource failed.  */
      XLFree (state);
      wl_resource_post_no_memory (resource);

      return;
    }

  /* Clear the state.  */
  memset (state, 0, sizeof *state);

  /* Set the resource data.  */
  wl_resource_set_implementation (state_resource, &XIValuatorState_impl,
				  state, HandleXIValuatorStateDestroy);
}



static void
TranslateTestButtons (struct wl_resource *resource, XIButtonState *buttons)
{
  TestXIButtonState *state;

  if (!resource)
    {
      /* Use default values if nothing was specified.  */

      buttons->mask_len = 0;
      buttons->mask = NULL;

      return;
    }

  /* The mask in buttons will be destroyed along with the resource! */
  state = wl_resource_get_user_data (resource);
  buttons->mask_len = sizeof state->mask;
  buttons->mask = state->mask;
}

static void
TranslateTestValuators (struct wl_resource *resource,
			XIValuatorState *valuators)
{
  TestXIValuatorState *state;

  if (!resource)
    {
      /* Use default values if nothing was specified.  */
      valuators->mask_len = 0;
      valuators->values = NULL;
      valuators->mask = NULL;

      return;
    }

  state = wl_resource_get_user_data (resource);
  valuators->mask_len = state->mask_len;
  valuators->mask = state->mask;
  valuators->values = state->values;
}

static void
TranslateTestModifiers (struct wl_resource *resource,
			XIModifierState *modifiers)
{
  TestXIModifierState *state;

  if (!resource)
    {
      /* Use default values if nothing was specified.  */
      modifiers->base = 0;
      modifiers->latched = 0;
      modifiers->locked = 0;
      modifiers->effective = 0;

      return;
    }

  state = wl_resource_get_user_data (resource);
  modifiers->base = state->base;
  modifiers->latched = state->latched;
  modifiers->locked = state->locked;
  modifiers->effective = state->effective;
}

static void
DispatchTestEvent (TestSeatController *controller, Window window,
		   XIEvent *event)
{
  Surface *surface;
  Subcompositor *subcompositor;

  /* Look up a test surface with the given window and dispatch the
     event to it.  */

  surface = XLLookUpTestSurface (window, &subcompositor);

  if (!surface)
    /* The client submitted an invalid event window! */
    return;

  if (event->evtype == XI_FocusIn)
    DispatchFocusIn (surface, (XIFocusInEvent *) event);
  else if (event->evtype == XI_FocusOut)
    DispatchFocusOut (surface, (XIFocusOutEvent *) event);
  else if (event->evtype == XI_Enter
	   || event->evtype == XI_Leave)
    DispatchEntryExit (subcompositor, (XIEnterEvent *) event);
  else if (event->evtype == XI_Motion)
    DispatchMotion (subcompositor, (XIDeviceEvent *) event);
  else if (event->evtype == XI_ButtonPress
	   || event->evtype == XI_ButtonRelease)
    DispatchButton (subcompositor, (XIDeviceEvent *) event);
  else if (event->evtype == XI_KeyPress
	   || event->evtype == XI_KeyRelease)
    DispatchKey ((XIDeviceEvent *) event);
  else if (event->evtype == XI_BarrierHit)
    DispatchBarrierHit ((XIBarrierEvent *) event);
  else if (event->evtype == XI_GesturePinchBegin
	   || event->evtype == XI_GesturePinchUpdate
	   || event->evtype == XI_GesturePinchEnd)
    DispatchGesturePinch (subcompositor, (XIGesturePinchEvent *) event);
  else if (event->evtype == XI_GestureSwipeBegin
	   || event->evtype == XI_GestureSwipeUpdate
	   || event->evtype == XI_GestureSwipeEnd)
    DispatchGestureSwipe (subcompositor, (XIGestureSwipeEvent *) event);
}

#define GenerateCrossingEvent(event_type, controller, test_event)	\
  test_event.type = GenericEvent;					\
  test_event.serial = request_serial_counter++;				\
  test_event.send_event = True;						\
  test_event.display = compositor.display;				\
  test_event.extension = xi2_opcode;					\
  test_event.evtype = event_type;					\
  test_event.time = time;						\
  test_event.deviceid = controller->seat->master_pointer;		\
  test_event.sourceid = sourceid;					\
  test_event.detail = detail;						\
  test_event.root = root;						\
  test_event.event = event;						\
  test_event.child = child;						\
  test_event.root_x = wl_fixed_to_double (root_x);			\
  test_event.root_y = wl_fixed_to_double (root_y);			\
  test_event.event_x = wl_fixed_to_double (event_x);			\
  test_event.event_y = wl_fixed_to_double (event_y);			\
  test_event.mode = mode;						\
  test_event.focus = focus;						\
  test_event.same_screen = same_screen;					\
  TranslateTestButtons (buttons_resource, &test_event.buttons);		\
  TranslateTestModifiers (mods_resource, &test_event.mods);		\
  TranslateTestModifiers (group_resource, &test_event.group)

static void
DispatchXIEnter (struct wl_client *client, struct wl_resource *resource,
		 uint32_t time, int32_t sourceid, int32_t detail,
		 uint32_t root, uint32_t event, uint32_t child,
		 wl_fixed_t root_x, wl_fixed_t root_y,
		 wl_fixed_t event_x, wl_fixed_t event_y, int32_t mode,
		 int32_t focus, int32_t same_screen,
		 struct wl_resource *buttons_resource,
		 struct wl_resource *mods_resource,
		 struct wl_resource *group_resource)
{
  TestSeatController *controller;
  XIEnterEvent test_event;

  controller = wl_resource_get_user_data (resource);
  GenerateCrossingEvent (XI_Enter, controller, test_event);

  /* Now dispatch the event.  */
  DispatchTestEvent (controller, event, (XIEvent *) &test_event);
}

static void
DispatchXILeave (struct wl_client *client, struct wl_resource *resource,
		 uint32_t time, int32_t sourceid, int32_t detail,
		 uint32_t root, uint32_t event, uint32_t child,
		 wl_fixed_t root_x, wl_fixed_t root_y,
		 wl_fixed_t event_x, wl_fixed_t event_y, int32_t mode,
		 int32_t focus, int32_t same_screen,
		 struct wl_resource *buttons_resource,
		 struct wl_resource *mods_resource,
		 struct wl_resource *group_resource)
{
  TestSeatController *controller;
  XILeaveEvent test_event;

  controller = wl_resource_get_user_data (resource);
  GenerateCrossingEvent (XI_Leave, controller, test_event);

  /* Now dispatch the event.  */
  DispatchTestEvent (controller, event, (XIEvent *) &test_event);
}

#define GenerateDeviceEvent(event_type, controller, test_event)		\
  test_event.type = GenericEvent;					\
  test_event.serial = request_serial_counter++;				\
  test_event.send_event = True;						\
  test_event.display = compositor.display;				\
  test_event.extension = xi2_opcode;					\
  test_event.evtype = event_type;					\
  test_event.time = time;						\
  test_event.deviceid = controller->seat->master_pointer;		\
  test_event.sourceid = sourceid;					\
  test_event.detail = detail;						\
  test_event.root = root;						\
  test_event.child = child;						\
  test_event.event = event;						\
  test_event.root_x = wl_fixed_to_double (root_x);			\
  test_event.root_y = wl_fixed_to_double (root_y);			\
  test_event.event_x = wl_fixed_to_double (event_x);			\
  test_event.event_y = wl_fixed_to_double (event_y);			\
  test_event.flags = flags;						\
  TranslateTestButtons (buttons_resource, &test_event.buttons);		\
  TranslateTestValuators (valuators_resource, &test_event.valuators);	\
  TranslateTestModifiers (mods_resource, &test_event.mods);		\
  TranslateTestModifiers (group_resource, &test_event.group)

static void
DispatchXIMotion (struct wl_client *client, struct wl_resource *resource,
		  uint32_t time, int32_t sourceid, int32_t detail,
		  uint32_t root, uint32_t event, uint32_t child,
		  wl_fixed_t root_x, wl_fixed_t root_y, wl_fixed_t event_x,
		  wl_fixed_t event_y, int32_t flags,
		  struct wl_resource *buttons_resource,
		  struct wl_resource *valuators_resource,
		  struct wl_resource *mods_resource,
		  struct wl_resource *group_resource)
{
  TestSeatController *controller;
  XIDeviceEvent test_event;

  controller = wl_resource_get_user_data (resource);
  GenerateDeviceEvent (XI_Motion, controller, test_event);

  /* Now dispatch the event.  */
  DispatchTestEvent (controller, event, (XIEvent *) &test_event);
}

static void
DispatchXIButtonPress (struct wl_client *client, struct wl_resource *resource,
		       uint32_t time, int32_t sourceid, int32_t detail,
		       uint32_t root, uint32_t event, uint32_t child,
		       wl_fixed_t root_x, wl_fixed_t root_y,
		       wl_fixed_t event_x, wl_fixed_t event_y, int32_t flags,
		       struct wl_resource *buttons_resource,
		       struct wl_resource *valuators_resource,
		       struct wl_resource *mods_resource,
		       struct wl_resource *group_resource)
{
  TestSeatController *controller;
  XIDeviceEvent test_event;

  controller = wl_resource_get_user_data (resource);
  GenerateDeviceEvent (XI_ButtonPress, controller, test_event);

  /* Now dispatch the event.  */
  DispatchTestEvent (controller, event, (XIEvent *) &test_event);
}

static void
DispatchXIButtonRelease (struct wl_client *client, struct wl_resource *resource,
			 uint32_t time, int32_t sourceid, int32_t detail,
			 uint32_t root, uint32_t event, uint32_t child,
			 wl_fixed_t root_x, wl_fixed_t root_y,
			 wl_fixed_t event_x, wl_fixed_t event_y, int32_t flags,
			 struct wl_resource *buttons_resource,
			 struct wl_resource *valuators_resource,
			 struct wl_resource *mods_resource,
			 struct wl_resource *group_resource)
{
  TestSeatController *controller;
  XIDeviceEvent test_event;

  controller = wl_resource_get_user_data (resource);
  GenerateDeviceEvent (XI_ButtonRelease, controller, test_event);

  /* Now dispatch the event.  */
  DispatchTestEvent (controller, event, (XIEvent *) &test_event);
}

static const struct test_seat_controller_interface seat_controller_impl =
  {
    .destroy = DestroyTestSeatController,
    .bind_seat = BindSeat,
    .get_XIModifierState = GetXIModifierState,
    .get_XIButtonState = GetXIButtonState,
    .get_XIValuatorState = GetXIValuatorState,
    .dispatch_XI_Enter = DispatchXIEnter,
    .dispatch_XI_Leave = DispatchXILeave,
    .dispatch_XI_Motion = DispatchXIMotion,
    .dispatch_XI_ButtonPress = DispatchXIButtonPress,
    .dispatch_XI_ButtonRelease = DispatchXIButtonRelease,
  };

static void
HandleControllerResourceDestroy (struct wl_resource *resource)
{
  TestSeatController *controller;
  Seat *seat;

  controller = wl_resource_get_user_data (resource);
  seat = controller->seat;

  /* Make the seat inert and remove it from live_seats.  */
  seat->flags |= IsInert;

  /* Set the focus surface to NULL, so surfaces don't mistakenly
     treat themselves as still focused.  */

  SetFocusSurface (seat, NULL);

  /* Run destroy handlers.  */

  RunDestroyListeners (seat);

  /* Since the seat is now inert, remove it from the assoc
     table and destroy the global.  */

  XLDeleteAssoc (seats, seat->master_keyboard);
  XLDeleteAssoc (seats, seat->master_pointer);

  /* Also remove it from the list of live seats.  */

  live_seats = XLListRemove (live_seats, seat);

  /* Run and remove all resize completion callbacks.  */

  RunResizeDoneCallbacks (seat);

  /* And release the seat.  */

  ReleaseSeat (seat);

  /* Free the controller resource.  */
  XLFree (controller);
}



void
XLGetTestSeat (struct wl_client *client, struct wl_resource *resource,
	       uint32_t id)
{
  TestSeatController *controller;
  Seat *seat;
  char name_format[sizeof "test seat: " + 40];

  controller = XLSafeMalloc (sizeof *controller);

  if (!controller)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (controller, 0, sizeof *controller);
  controller->resource
    = wl_resource_create (client, &test_seat_controller_interface,
			  wl_resource_get_version (resource), id);

  if (!controller->resource)
    {
      wl_resource_post_no_memory (resource);
      XLFree (controller);
      return;
    }

  seat = XLCalloc (1, sizeof *seat);

  /* Allocate a "device ID" for the seat.  Device IDs are unsigned 16
     bit values, so any larger value is guaranteed to be okay for our
     own use.  */

  if (!test_seat_counter)
    test_seat_counter = 65555;
  test_seat_counter++;

  /* Initialize some random bogus values.  */
  seat->master_pointer = test_seat_counter;
  seat->master_keyboard = test_seat_counter;

  /* Add a unique seat name.  */
  sprintf (name_format, "test seat %u", test_seat_counter);
  seat->name = XLStrdup (name_format);

  /* Refrain from creating a global for this seat.  */
  seat->global = NULL;

  InitSeatCommon (seat);

  /* Associate the dummy device with the seat.  */
  XLMakeAssoc (seats, test_seat_counter, seat);
  seat->flags |= IsTestSeat;

  /* Add the seat to the live seat list.  */
  live_seats = XLListPrepend (live_seats, seat);

  /* Retain the seat.  */
  RetainSeat (seat);
  controller->seat = seat;

  wl_resource_set_implementation (controller->resource, &seat_controller_impl,
				  controller, HandleControllerResourceDestroy);
}
