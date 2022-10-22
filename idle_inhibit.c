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

#include "compositor.h"
#include "idle-inhibit-unstable-v1.h"

typedef struct _IdleInhibitDataRecord IdleInhibitDataRecord;
typedef struct _IdleInhibitor IdleInhibitor;
typedef enum _IdleInhibition IdleInhibition;

/* Idle inhibition is tricky because there is no threshold that tells
   the protocol translator whether or not to apply idle inhibition for
   surfaces that are already focused.  So, contrary to the protocol
   specification, we inhibit idle-ness as long as a surface with an
   idle inhibitor is focused, even if the user was already idle at the
   time the idle inhibitor was created.  */

struct _IdleInhibitor
{
  /* The next and last idle inhibitors on this surface.  */
  IdleInhibitor *next, *last;

  /* The next and last idle inhibitors globally.  */
  IdleInhibitor *global_next, *global_last;

  /* The corresponding Surface.  */
  Surface *surface;

  /* The corresponding wl_resource.  */
  struct wl_resource *resource;
};

struct _IdleInhibitDataRecord
{
  /* List of idle inhibitors on this surface.  */
  IdleInhibitor inhibitors;
};

enum _IdleInhibition
  {
    IdleAllowed,
    IdleInhibited,
  };

/* The idle inhibit manager global.  */
static struct wl_global *idle_inhibit_manager_global;

/* List of all idle inhibitors.  */
static IdleInhibitor all_inhibitors;

/* The current idle inhibition.  */
static IdleInhibition current_inhibition;

/* Commands run while idle.  The first command is run once upon idle
   being inhibited; the second is run every N seconds while idle is
   inhibited, and the third command is run every time idle is
   deinhibited.  */
static char **inhibit_command, **timer_command, **deinhibit_command;

/* How many seconds the protocol translator waits before running the
   timer command.  */
static int timer_seconds;

/* Timer used to run the timer command.  */
static Timer *command_timer;

/* Process queue used to run those commands.  */
static ProcessQueue *process_queue;

static void
HandleCommandTimer (Timer *timer, void *data, struct timespec time)
{
  /* The timer shouldn't have been started if the command is NULL.  */
  RunProcess (process_queue, timer_command);
}

static void
ChangeInhibitionTo (IdleInhibition inhibition)
{
  if (current_inhibition == inhibition)
    /* Nothing changed.  */
    return;

  current_inhibition = inhibition;

  if (current_inhibition == IdleInhibited)
    {
      /* Run the idle inhibit command, if it exists.  */

      if (inhibit_command)
	RunProcess (process_queue, inhibit_command);

      /* Schedule a timer to run the timer command.  */

      if (timer_command)
	command_timer = AddTimer (HandleCommandTimer, NULL,
				  MakeTimespec (timer_seconds, 0));
    }
  else
    {
      /* Cancel the command timer.  */
      if (command_timer)
	RemoveTimer (command_timer);
      command_timer = NULL;

      /* Run the deinhibit command.  */

      if (deinhibit_command)
	RunProcess (process_queue, deinhibit_command);
    }
}

static void
DetectSurfaceIdleInhibit (void)
{
  IdleInhibitor *inhibitor;

  inhibitor = all_inhibitors.global_next;
  while (inhibitor != &all_inhibitors)
    {
      if (inhibitor->surface->num_focused_seats)
	{
	  ChangeInhibitionTo (IdleInhibited);
	  return;
	}

      inhibitor = inhibitor->global_next;
    }

  /* There are no live idle inhibitors for focused seats.  */
  ChangeInhibitionTo (IdleAllowed);

  return;
}

static void
NoticeSurfaceFocused (Surface *surface)
{
  IdleInhibitDataRecord *record;

  record = XLSurfaceFindClientData (surface, IdleInhibitData);

  if (!record)
    return;

  if (record->inhibitors.next == &record->inhibitors)
    return;

  /* There is an idle inhibitor for this idle surface.  */
  ChangeInhibitionTo (IdleInhibited);
}



static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct zwp_idle_inhibitor_v1_interface idle_inhibitor_impl =
  {
    .destroy = Destroy,
  };

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  IdleInhibitor *inhibitor;

  inhibitor = wl_resource_get_user_data (resource);

  if (inhibitor->surface)
    {
      /* Unlink the inhibitor.  */
      inhibitor->next->last = inhibitor->last;
      inhibitor->last->next = inhibitor->next;
      inhibitor->global_next->global_last = inhibitor->global_last;
      inhibitor->global_last->global_next = inhibitor->global_next;
    }

  /* Free the inhibitor; then, check if any other idle inhibitors are
     still active.  */
  XLFree (inhibitor);
  DetectSurfaceIdleInhibit ();
}



static void
FreeIdleInhibitData (void *data)
{
  IdleInhibitDataRecord *record;
  IdleInhibitor *inhibitor, *last;

  record = data;

  /* Loop through each idle inhibitor.  Unlink it.  */
  inhibitor = record->inhibitors.next;
  while (inhibitor != &record->inhibitors)
    {
      last = inhibitor;
      inhibitor = inhibitor->next;

      last->next = NULL;
      last->last = NULL;
      last->global_next->global_last = last->global_last;
      last->global_last->global_next = last->global_next;
      last->surface = NULL;
    }

  /* Check if any idle inhibitors are still active.  */
  DetectSurfaceIdleInhibit ();
}

static void
InitIdleInhibitData (IdleInhibitDataRecord *record)
{
  if (record->inhibitors.next)
    /* The data is already initialized.  */
    return;

  record->inhibitors.next = &record->inhibitors;
  record->inhibitors.last = &record->inhibitors;
}

static void
CreateInhibitor (struct wl_client *client, struct wl_resource *resource,
		 uint32_t id, struct wl_resource *surface_resource)
{
  Surface *surface;
  IdleInhibitor *inhibitor;
  IdleInhibitDataRecord *record;

  inhibitor = XLSafeMalloc (sizeof *inhibitor);

  if (!inhibitor)
    {
      wl_resource_post_no_memory (resource);
      return;
    }

  memset (inhibitor, 0, sizeof *inhibitor);
  inhibitor->resource
    = wl_resource_create (client, &zwp_idle_inhibitor_v1_interface,
			  wl_resource_get_version (resource), id);

  surface = wl_resource_get_user_data (surface_resource);
  record = XLSurfaceGetClientData (surface, IdleInhibitData,
				   sizeof *record, FreeIdleInhibitData);
  InitIdleInhibitData (record);

  /* Set the inhibitor's surface.  */
  inhibitor->surface = surface;

  /* And link it onto the list of all idle inhibitors on both the
     surface and globally.  */
  inhibitor->next = record->inhibitors.next;
  inhibitor->last = &record->inhibitors;
  record->inhibitors.next->last = inhibitor;
  record->inhibitors.next = inhibitor;
  inhibitor->global_next = all_inhibitors.global_next;
  inhibitor->global_last = &all_inhibitors;
  all_inhibitors.global_next->global_last = inhibitor;
  all_inhibitors.global_next = inhibitor;

  if (surface->num_focused_seats)
    /* See the comment at the beginning of the file.  */
    ChangeInhibitionTo (IdleInhibited);

  /* Set the implementation.  */
  wl_resource_set_implementation (inhibitor->resource, &idle_inhibitor_impl,
				  inhibitor, HandleResourceDestroy);
}

static struct zwp_idle_inhibit_manager_v1_interface idle_inhibit_manager_impl =
  {
    .create_inhibitor = CreateInhibitor,
  };

static void
HandleBind (struct wl_client *client, void *data,
	    uint32_t version, uint32_t id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client,
				 &zwp_idle_inhibit_manager_v1_interface,
				 version, id);

  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }

  wl_resource_set_implementation (resource, &idle_inhibit_manager_impl,
				  NULL, NULL);
}

static char **
ReadCommandResource (const char *name, const char *class)
{
  XrmDatabase rdb;
  XrmName namelist[3];
  XrmClass classlist[3];
  XrmValue value;
  XrmRepresentation type;
  char **arguments;
  size_t num_args;

  rdb = XrmGetDatabase (compositor.display);

  if (!rdb)
    return NULL;

  namelist[1] = XrmStringToQuark (name);
  namelist[0] = app_quark;
  namelist[2] = NULLQUARK;

  classlist[1] = XrmStringToQuark (class);
  classlist[0] = resource_quark;
  classlist[2] = NULLQUARK;

  if (XrmQGetResource (rdb, namelist, classlist,
		       &type, &value)
      && type == QString)
    {
      ParseProcessString ((const char *) value.addr,
			  &arguments, &num_args);

      return arguments;
    }

  return NULL;
}

static int
ReadIntegerResource (const char *name, const char *class,
		     int default_value)
{
  XrmDatabase rdb;
  XrmName namelist[3];
  XrmClass classlist[3];
  XrmValue value;
  XrmRepresentation type;
  int result;

  rdb = XrmGetDatabase (compositor.display);

  if (!rdb)
    return default_value;

  namelist[1] = XrmStringToQuark (name);
  namelist[0] = app_quark;
  namelist[2] = NULLQUARK;

  classlist[1] = XrmStringToQuark (class);
  classlist[0] = resource_quark;
  classlist[2] = NULLQUARK;

  if (XrmQGetResource (rdb, namelist, classlist,
		       &type, &value)
      && type == QString)
    {
      result = atoi ((char *) value.addr);

      if (!result)
	return default_value;

      return result;
    }

  return default_value;
}

void
XLInitIdleInhibit (void)
{
  idle_inhibit_manager_global
    = wl_global_create (compositor.wl_display,
			&zwp_idle_inhibit_manager_v1_interface,
			1, NULL, HandleBind);

  all_inhibitors.global_next = &all_inhibitors;
  all_inhibitors.global_last = &all_inhibitors;

  /* Read various commands from resources.  */
  inhibit_command = ReadCommandResource ("idleInhibitCommand",
					 "IdleInhibitCommand");
  timer_command = ReadCommandResource ("idleIntervalCommand",
				       "IdleInhibitCommand");
  deinhibit_command = ReadCommandResource ("idleDeinhibitCommand",
					   "IdleDeinhibitCommand");

  /* Initialize the default value for timer_seconds.  */
  timer_seconds = ReadIntegerResource ("idleCommandInterval",
				       "IdleCommandInterval",
				       60);

  /* Initialize the process queue.  */
  process_queue = MakeProcessQueue ();
}

void
XLIdleInhibitNoticeSurfaceFocused (Surface *surface)
{
  NoticeSurfaceFocused (surface);
}

void
XLDetectSurfaceIdleInhibit (void)
{
  DetectSurfaceIdleInhibit ();
}
