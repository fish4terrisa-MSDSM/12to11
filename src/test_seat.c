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
typedef struct _TestDeviceController TestDeviceController;
typedef struct _TestXIModifierState TestXIModifierState;
typedef struct _TestXIValuatorState TestXIValuatorState;
typedef struct _TestXIButtonState TestXIButtonState;
typedef struct _TestXIDeviceInfo TestXIDeviceInfo;

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

struct _TestDeviceController
{
  /* The associated struct wl_resource.  */
  struct wl_resource *resource;

  /* Array of device IDs used by this test device controller.  */
  int *device_ids;

  /* Number of device IDs associated with this controller.  */
  int num_ids;
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

enum
  {
    StateDeviceIdSet   = 1,
    StateNameSet       = 1 << 1,
    StateUseSet	       = 1 << 2,
    StateAttachmentSet = 1 << 3,
    StateEnabledSet    = 1 << 4,
    StateComplete      = 0x1f,
  };

struct _TestXIDeviceInfo
{
  /* The associated resource.  */
  struct wl_resource *resource;

  /* The device name.  */
  char *name;

  /* Array of classes.  */
  XIAnyClassInfo **classes;

  /* The device ID.  */
  int device_id;

  /* The use, attachment.  */
  int use, attachment;

  /* Whether or not the device is enabled.  */
  Bool enabled;

  /* The number of classes there are.  */
  int num_classes;

  /* How many fields are set.  */
  int state;
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
DestroyXIValuatorState (struct wl_client *client,
			struct wl_resource *resource)
{
  wl_resource_destroy (resource);
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
				   XIMaskLen (valuator));

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
	  else if (XIMaskIsSet (state->mask, i))
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
    .destroy = DestroyXIValuatorState,
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
DestroyDeviceInfo (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
SetDeviceId (struct wl_client *client, struct wl_resource *resource,
	     uint32_t device_id)
{
  TestXIDeviceInfo *info;

  info = wl_resource_get_user_data (resource);

  if (device_id < 65536)
    {
      wl_resource_post_error (resource, TEST_MANAGER_ERROR_INVALID_DEVICE_ID,
			      "invalid device id specified");
      return;
    }

  info->device_id = device_id;
  info->state |= StateDeviceIdSet;
}

static void
SetName (struct wl_client *client, struct wl_resource *resource,
	 const char *name)
{
  TestXIDeviceInfo *info;

  info = wl_resource_get_user_data (resource);

  if (info->name)
    XLFree (info->name);
  info->name = XLStrdup (name);
  info->state |= StateNameSet;
}

static void
SetUse (struct wl_client *client, struct wl_resource *resource,
	int32_t use)
{
  TestXIDeviceInfo *info;

  info = wl_resource_get_user_data (resource);
  info->use = use;
  info->state |= StateUseSet;
}

static void
SetAttachment (struct wl_client *client, struct wl_resource *resource,
	       struct wl_resource *attachment_resource)
{
  TestSeatController *controller;
  TestXIDeviceInfo *info;

  controller = wl_resource_get_user_data (attachment_resource);
  info = wl_resource_get_user_data (resource);

  info->attachment = controller->seat->master_pointer;
  info->state |= StateAttachmentSet;
}

static void
SetEnabled (struct wl_client *client, struct wl_resource *resource,
	    uint32_t enabled)
{
  TestXIDeviceInfo *info;

  info = wl_resource_get_user_data (resource);

  if (enabled)
    info->enabled = True;
  else
    info->enabled = False;
  info->state |= StateEnabledSet;
}

static void
AddXIScrollClassInfo (struct wl_client *client, struct wl_resource *resource,
		      int32_t sourceid, int32_t number, int32_t scroll_type,
		      wl_fixed_t increment, int32_t flags)
{
  TestXIDeviceInfo *info;
  XIScrollClassInfo *class;

  if (sourceid < 65536)
    {
      wl_resource_post_error (resource, TEST_MANAGER_ERROR_INVALID_DEVICE_ID,
			      "invalid device ID specified");
      return;
    }

  info = wl_resource_get_user_data (resource);

  class = XLMalloc (sizeof *class);
  class->type = XIScrollClass;
  class->sourceid = sourceid;
  class->number = number;
  class->scroll_type = scroll_type;
  class->increment = wl_fixed_to_double (increment);
  class->flags = flags;

  /* Extend info->classes to hold more classes.  */
  info->num_classes++;
  info->classes = XLRealloc (info->classes,
			     sizeof *info->classes * info->num_classes);

  /* Attach the class.  */
  info->classes[info->num_classes - 1] = (XIAnyClassInfo *) class;
}

static void
AddXIValuatorClassInfo (struct wl_client *client, struct wl_resource *resource,
			int32_t sourceid, int32_t number, const char *label,
			wl_fixed_t min, wl_fixed_t max, wl_fixed_t value,
			int32_t resolution, int32_t mode)
{
  TestXIDeviceInfo *info;
  XIValuatorClassInfo *class;

  if (sourceid < 65536)
    {
      wl_resource_post_error (resource, TEST_MANAGER_ERROR_INVALID_DEVICE_ID,
			      "invalid device ID specified");
      return;
    }

  /* Avoid interning empty strings.  */

  if (!strlen (label))
    {
      wl_resource_post_error (resource, TEST_MANAGER_ERROR_INVALID_LABEL,
			      "the specified label is invalid");
      return;
    }

  info = wl_resource_get_user_data (resource);
  class = XLMalloc (sizeof *class);
  class->type = XIValuatorClass;
  class->sourceid = sourceid;
  class->number = number;
  class->label = InternAtom (label);
  class->min = wl_fixed_to_double (min);
  class->max = wl_fixed_to_double (max);
  class->value = wl_fixed_to_double (value);
  class->resolution = resolution;
  class->mode = mode;

  /* Extend info->classes to hold more classes.  */
  info->num_classes++;
  info->classes = XLRealloc (info->classes,
			     sizeof *info->classes * info->num_classes);

  /* Attach the class.  */
  info->classes[info->num_classes - 1] = (XIAnyClassInfo *) class;
}

static const struct test_XIDeviceInfo_interface XIDeviceInfo_impl =
  {
    .destroy = DestroyDeviceInfo,
    .set_device_id = SetDeviceId,
    .set_name = SetName,
    .set_use = SetUse,
    .set_attachment = SetAttachment,
    .set_enabled = SetEnabled,
    .add_XIScrollClassInfo = AddXIScrollClassInfo,
    .add_XIValuatorClassInfo = AddXIValuatorClassInfo,
  };

static void
HandleXIDeviceInfoDestroy (struct wl_resource *resource)
{
  TestXIDeviceInfo *info;
  int i;

  info = wl_resource_get_user_data (resource);

  /* Free the name.  */
  XLFree (info->name);

  /* Free each of the classes.  */
  for (i = 0; i < info->num_classes; ++i)
    XLFree (info->classes[i]);

  /* Free the class array.  */
  XLFree (info->classes);

  /* Free the info itself.  */
  XLFree (info);
}



static void
DestroyDeviceController (struct wl_client *client,
			 struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
AddDeviceInfo (struct wl_client *client, struct wl_resource *resource,
	       struct wl_resource *device_info)
{
  TestDeviceController *controller;
  TestXIDeviceInfo *info;
  Seat *seat;
  DeviceInfo *deviceinfo;
  int i;
  XIDeviceInfo test_info;

  /* Add a virtual device to the device controller.  */
  controller = wl_resource_get_user_data (resource);
  info = wl_resource_get_user_data (device_info);

  /* First, ensure that the device info is completely specified.  */

  if ((info->state & StateComplete) != StateComplete)
    {
      wl_resource_post_error (resource,
			      TEST_MANAGER_ERROR_INCOMPLETE_DEVICE_INFO,
			      "the specified device information was not"
			      " completely specified");
      return;
    }

  /* Next, check whether or not a device already exists.  */
  seat = XLLookUpAssoc (seats, info->device_id);
  deviceinfo = XLLookUpAssoc (devices, info->device_id);

  if ((seat && seat->flags & IsTestDeviceSpecified) || deviceinfo)
    {
      /* If a device already exists, see whether or not it was created
	 by this test device controller.  */

      for (i = 0; i < controller->num_ids; ++i)
	{
	  if (controller->device_ids[i] == info->device_id)
	    /* It was created by this controller.  Simply update the
	       values.  */
	    goto continue_update;
	}

      /* Otherwise, post an error.  */
      wl_resource_post_error (resource,
			      TEST_MANAGER_ERROR_DEVICE_EXISTS,
			      "the device %d already exists, and was "
			      "not created by this controller",
			      info->device_id);
      return;
    }

 continue_update:

  /* Now, construct the XIDeviceInfo.  */
  test_info.deviceid = info->device_id;
  test_info.name = info->name;
  test_info.use = info->use;
  test_info.attachment = info->attachment;
  test_info.enabled = info->enabled;
  test_info.num_classes = info->num_classes;
  test_info.classes = info->classes;

  /* If the seat exists, repopulate its valuators with that specified
     in the device info.  */

  if (seat)
    {
      FreeValuators (seat);
      UpdateValuators (seat, &test_info);

      /* Next, set a flag that means the seat has its information
	 provided by device info.  */
      seat->flags |= IsTestDeviceSpecified;
    }

  /* Now, record the device info.  */
  RecordDeviceInformation (&test_info);
}

static void
GetDeviceInfo (struct wl_client *client, struct wl_resource *resource,
	       uint32_t id)
{
  TestXIDeviceInfo *info;

  info = XLSafeMalloc (sizeof *info);

  if (!info)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (info, 0, sizeof *info);
  info->resource
    = wl_resource_create (client, &test_XIDeviceInfo_interface,
			  wl_resource_get_version (resource), id);

  if (!info->resource)
    {
      XLFree (info);
      return;
    }

  wl_resource_set_implementation (info->resource, &XIDeviceInfo_impl,
				  info, HandleXIDeviceInfoDestroy);
}

static const struct test_device_controller_interface device_controller_impl =
  {
    .destroy = DestroyDeviceController,
    .add_device_info = AddDeviceInfo,
    .get_device_info = GetDeviceInfo,
  };

static void
HandleTestDeviceControllerDestroy (struct wl_resource *resource)
{
  TestDeviceController *controller;
  int i;
  Seat *seat;

  controller = wl_resource_get_user_data (resource);

  /* Remove each device associated with the device controller.  */
  for (i = 0; i < controller->num_ids; ++i)
    {
      NoticeDeviceDisabled (controller->device_ids[i]);

      /* NoticeDeviceDisabled is special-cased to not free valuators
	 for test seats.  If there is a seat associated with this
	 device ID, free the valuators on it as well.  */
      seat = XLLookUpAssoc (seats, controller->device_ids[i]);
      FreeValuators (seat);

      /* Clear the test device specified flag.  */
      seat->flags &= ~IsTestDeviceSpecified;
    }

  XLFree (controller);
}



static void
DestroySeatController (struct wl_client *client,
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

static void
GetDeviceController (struct wl_client *client, struct wl_resource *resource,
		     uint32_t id)
{
  TestDeviceController *controller;

  controller = XLSafeMalloc (sizeof *controller);

  if (!controller)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (controller, 0, sizeof *controller);
  controller->resource
    = wl_resource_create (client, &test_device_controller_interface,
			  wl_resource_get_version (resource), id);

  if (!controller->resource)
    {
      XLFree (controller);
      wl_resource_post_no_memory (resource);
      return;
    }

  wl_resource_set_implementation (controller->resource,
				  &device_controller_impl,
				  controller,
				  HandleTestDeviceControllerDestroy);
}

static void
SetLastUserTime (struct wl_client *client, struct wl_resource *resource,
		 uint32_t months, uint32_t milliseconds)
{
  Timestamp timestamp;
  TestSeatController *controller;

  timestamp.months = months;
  timestamp.milliseconds = milliseconds;
  controller = wl_resource_get_user_data (resource);

  if (TimestampIs (timestamp, Earlier,
		   controller->seat->last_user_time))
    {
      wl_resource_post_error (resource, TEST_MANAGER_ERROR_INVALID_USER_TIME,
			      "the specified user time (%"PRIu32":%"PRIu32
			      ") lies in the past.  the current time is %u:%u",
			      months, milliseconds,
			      controller->seat->last_user_time.months,
			      controller->seat->last_user_time.milliseconds);
      return;
    }

  controller->seat->last_user_time.months = months;
  controller->seat->last_user_time.milliseconds = milliseconds;
}

static void
DispatchXIFocusIn (struct wl_client *client, struct wl_resource *resource,
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
  XIFocusInEvent test_event;

  controller = wl_resource_get_user_data (resource);
  GenerateCrossingEvent (XI_FocusIn, controller, test_event);

  /* Now dispatch the event.  */
  DispatchTestEvent (controller, event, (XIEvent *) &test_event);
}

static void
DispatchXIFocusOut (struct wl_client *client, struct wl_resource *resource,
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
  XIFocusInEvent test_event;

  controller = wl_resource_get_user_data (resource);
  GenerateCrossingEvent (XI_FocusOut, controller, test_event);

  /* Now dispatch the event.  */
  DispatchTestEvent (controller, event, (XIEvent *) &test_event);
}

#define GenerateRawEvent(event_type, controller, test_event)		\
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
  test_event.flags = flags;						\
  TranslateTestValuators (valuators_resource, &test_event.valuators);	\
  test_event.raw_values = test_event.valuators.values;

static void
DispatchXIRawKeyPress (struct wl_client *client, struct wl_resource *resource,
		       uint32_t time, int32_t sourceid, int32_t detail,
		       int32_t flags, struct wl_resource *valuators_resource)
{
  TestSeatController *controller;
  XIRawEvent test_event;

  controller = wl_resource_get_user_data (resource);
  GenerateRawEvent (XI_RawKeyPress, controller, test_event);

  /* Now dispatch the event.  */
  HandleRawKey (&test_event);
}

static void
DispatchXIRawKeyRelease (struct wl_client *client, struct wl_resource *resource,
			 uint32_t time, int32_t sourceid, int32_t detail,
			 int32_t flags, struct wl_resource *valuators_resource)
{
  TestSeatController *controller;
  XIRawEvent test_event;

  controller = wl_resource_get_user_data (resource);
  GenerateRawEvent (XI_RawKeyRelease, controller, test_event);

  /* Now dispatch the event.  */
  HandleRawKey (&test_event);
}

static void
DispatchXIKeyPress (struct wl_client *client, struct wl_resource *resource,
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
  GenerateDeviceEvent (XI_KeyPress, controller, test_event);

  /* Now dispatch the event.  */
  DispatchTestEvent (controller, event, (XIEvent *) &test_event);
}

static void
DispatchXIKeyRelease (struct wl_client *client, struct wl_resource *resource,
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
  GenerateDeviceEvent (XI_KeyRelease, controller, test_event);

  /* Now dispatch the event.  */
  DispatchTestEvent (controller, event, (XIEvent *) &test_event);
}

static const struct test_seat_controller_interface seat_controller_impl =
  {
    .destroy = DestroySeatController,
    .bind_seat = BindSeat,
    .get_XIModifierState = GetXIModifierState,
    .get_XIButtonState = GetXIButtonState,
    .get_XIValuatorState = GetXIValuatorState,
    .dispatch_XI_Enter = DispatchXIEnter,
    .dispatch_XI_Leave = DispatchXILeave,
    .dispatch_XI_Motion = DispatchXIMotion,
    .dispatch_XI_ButtonPress = DispatchXIButtonPress,
    .dispatch_XI_ButtonRelease = DispatchXIButtonRelease,
    .get_device_controller = GetDeviceController,
    .set_last_user_time = SetLastUserTime,
    .dispatch_XI_FocusIn = DispatchXIFocusIn,
    .dispatch_XI_FocusOut = DispatchXIFocusOut,
    .dispatch_XI_RawKeyPress = DispatchXIRawKeyPress,
    .dispatch_XI_RawKeyRelease = DispatchXIRawKeyRelease,
    .dispatch_XI_KeyPress = DispatchXIKeyPress,
    .dispatch_XI_KeyRelease = DispatchXIKeyRelease,
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

  /* Initialize seat->key_pressed.  */
  seat->key_pressed
    = XLCalloc (MaskLen (xkb_desc->max_key_code
			 - xkb_desc->min_key_code), 1);

  /* Retain the seat.  */
  RetainSeat (seat);
  controller->seat = seat;

  wl_resource_set_implementation (controller->resource, &seat_controller_impl,
				  controller, HandleControllerResourceDestroy);

  /* Send the device ID to the client.  */
  test_seat_controller_send_device_id (controller->resource,
				       seat->master_pointer);
}
