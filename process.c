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

#include <sys/resource.h>
#include <sys/errno.h>
#include <sys/wait.h>

#include <signal.h>
#include <string.h>
#include <spawn.h>
#include <stdio.h>
#include <unistd.h>

#include "compositor.h"

typedef struct _ProcessDescription ProcessDescription;

/* Subprocess control and management.  This module implements a
   "process queue", which is an ordered list of commands to run.  */

struct _ProcessDescription
{
  /* The next and last descriptions in the queue.  */
  ProcessDescription *next, *last;

  /* NULL-terminated array of arguments.  */
  char **arguments;

  /* Size of the argument list excluding the terminating NULL.  */
  size_t num_arguments;
};

struct _ProcessQueue
{
  /* The next process queue in the chain.  */
  ProcessQueue *next;

  /* List of commands that have not yet been run.  */
  ProcessDescription descriptions;

  /* The process currently being run.  SIGCHLD must be blocked while
     reading from this field.  */
  pid_t process;
};

/* Whether or not the process queue SIGCHLD handler has been
   installed.  */
static Bool child_handler_installed;

/* List of all process queues.  */
static ProcessQueue *all_queues;

/* Whether or not child processes should be checked.  */
static volatile sig_atomic_t check_child_processes;

static void
HandleChild (int signal, siginfo_t *siginfo, void *ucontext)
{
  ProcessQueue *considering;
  int temp_errno, status;
  pid_t pid;

  /* SIGCHILD should now be blocked here.  This function cannot call
     malloc or any other async-signal unsafe functions.  That includes
     free and posix_spawn, so the queue is drained in ProcessPoll.  */

  /* Reap the process(es) that exited.  */

  do
    {
      temp_errno = errno;
      pid = TEMP_FAILURE_RETRY (waitpid (-1, &status, WNOHANG));
      errno = temp_errno;

      if (pid == (pid_t) -1)
	break;

      considering = all_queues;
      for (; considering; considering = considering->next)
	{
	  if (considering->process == pid)
	    {
	      /* This process has finished.  Set considering->process to
		 (pid_t) -1.  The next queued process will then be run
		 later.  */
	      considering->process = (pid_t) -1;

	      return;
	    }
	}
    }
  while (pid != -1);
}

static void
MaybeInstallChildHandler (void)
{
  struct sigaction act;

  /* Install the SIGCHLD handler used to drive the process queues if
     it has not already been installed.  */

  if (child_handler_installed)
    return;

  child_handler_installed = True;
  memset (&act, 0, sizeof act);

  act.sa_flags = SA_SIGINFO;
  act.sa_sigaction = HandleChild;

  if (sigaction (SIGCHLD, &act, NULL))
    {
      perror ("sigaction");
      abort ();
    }
}

static void
Block (sigset_t *oldset)
{
  sigset_t sigset;

  sigemptyset (&sigset);
  sigaddset (&sigset, SIGCHLD);

  if (sigprocmask (SIG_BLOCK, &sigset, oldset))
    {
      perror ("sigprocmask");
      abort ();
    }
}

static void
Unblock (void)
{
  sigset_t sigset;

  sigemptyset (&sigset);
  sigaddset (&sigset, SIGCHLD);

  if (sigprocmask (SIG_UNBLOCK, &sigset, NULL))
    {
      perror ("sigprocmask");
      abort ();
    }
}

static void
RunNext (ProcessQueue *queue)
{
  ProcessDescription *description, *last;
  int rc;
  pid_t pid;

  description = queue->descriptions.last;
  while (description != &queue->descriptions)
    {
      last = description;
      description = description->last;

      rc = posix_spawnp (&pid, last->arguments[0], NULL, NULL,
			 last->arguments, environ);

      /* Unlink the description.  */
      last->next->last = last->last;
      last->last->next = last->next;
      XLFree (last);

      if (!rc)
	{
	  /* The child has been spawned.  Set queue->process and
	     return.  */
	  queue->process = pid;
	  return;
	}
      else
	/* Print an error and continue.  */
	fprintf (stderr, "Subprocess creation failed: %s\n",
		 strerror (errno));
    }
}

static void
ProcessPendingDescriptions (Bool need_block)
{
  ProcessQueue *queue;

  if (need_block)
    Block (NULL);

  for (queue = all_queues; queue; queue = queue->next)
    {
      if (queue->process == (pid_t) -1)
	RunNext (queue);
    }

  if (need_block)
    Unblock ();
}

static void
ProcessEscapes (char *string)
{
  int i, j;
  Bool escaped;

  /* Naively process escapes in STRING.  */
  escaped = False;
  i = 0;
  j = 0;

  while (string[j] != '\0')
    {
      if (escaped)
	{
	  string[i++] = string[j];
	  escaped = False;
	}
      else if (*string == '\\')
	escaped = True;
      else
	string[i++] = string[j];

      j++;
    }

  string[i] = '\0';
}

void
ParseProcessString (const char *string, char ***arguments_return,
		    size_t *arg_count)
{
  char **arguments;
  const char *start;
  Bool escaped, quoted, non_whitespace_seen;
  size_t nargs;

  /* This is the NULL termination.  */
  arguments = XLCalloc (1, sizeof *arguments);
  nargs = 0;
  start = string;
  escaped = False;
  quoted = False;
  non_whitespace_seen = False;

#define AppendArg()						\
  do								\
    {								\
      arguments							\
	= XLRealloc (arguments,					\
		     sizeof *arguments * (++nargs + 1));	\
      arguments[nargs - 1] = XLMalloc (string - start + 1);	\
      memcpy (arguments[nargs - 1], start, string - start);	\
      arguments[nargs - 1][string - start] = '\0';		\
      ProcessEscapes (arguments[nargs - 1]);			\
    }								\
  while (0)

  while (*string != '\0')
    {
      if (!escaped)
	{
	  if (*string == '\\')
	    escaped = True;

	  if (*string == '"')
	    {
	      if (!quoted)
		{
		  quoted = True;

		  if (non_whitespace_seen)
		    AppendArg ();

		  start = string + 1;
		  non_whitespace_seen = False;
		}
	      else
		{
		  quoted = False;

		  /* Append the argument now.  */
		  AppendArg ();

		  /* Set start to the character after string.  */
		  start = string + 1;
		  non_whitespace_seen = False;
		}
	    }
	  else if (!quoted)
	    {
	      if (*string == ' ')
		{
		  if (non_whitespace_seen)
		    AppendArg ();

		  start = string + 1;
		  non_whitespace_seen = False;
		}
	      else
		non_whitespace_seen = True;
	    }
	}
      else
	{
	  escaped = False;
	  non_whitespace_seen = True;
	}

      string++;

      if (*string == '\0' && non_whitespace_seen)
	AppendArg ();
    }

#undef AppendArg

  /* NULL-terminate the argument array.  */
  arguments[nargs] = NULL;

  if (arg_count)
    *arg_count = nargs;

  *arguments_return = arguments;
}

void
RunProcess (ProcessQueue *queue, char **arguments)
{
  ProcessDescription *desc;

  MaybeInstallChildHandler ();

  if (!arguments[0])
    /* There is no executable, so just return.  */
    return;

  /* First, allocate a process description.  */
  desc = XLMalloc (sizeof *desc);

  /* Link it onto the queue.  */
  desc->next = queue->descriptions.next;
  desc->last = &queue->descriptions;

  /* Save the command line.  */
  desc->arguments = arguments;

  /* Determine how many arguments there are.  Note that the caller
     owns the command line.  */
  desc->num_arguments = 0;

  while (arguments[desc->num_arguments])
    ++desc->num_arguments;

  /* Finish the link.  */
  queue->descriptions.next->last = desc;
  queue->descriptions.next = desc;

  /* Process pending process descriptions.  */
  ProcessPendingDescriptions (True);
}

ProcessQueue *
MakeProcessQueue (void)
{
  ProcessQueue *queue;

  queue = XLCalloc (1, sizeof *queue);
  queue->next = all_queues;
  queue->descriptions.next = &queue->descriptions;
  queue->descriptions.last = &queue->descriptions;
  queue->process = (pid_t) -1;

  Block (NULL);
  all_queues = queue;
  Unblock ();

  return queue;
}

int
ProcessPoll (struct pollfd *fds, nfds_t nfds,
	     struct timespec *timeout)
{
  sigset_t oldset;
  int rc;

  /* Block SIGPOLL.  If SIGCHLD arrives before, then the process will
     be run by ProcessPendingDescriptions.  If it arrives after, then
     ppoll will be interrupted with EINTR.  */
  Block (&oldset);
  ProcessPendingDescriptions (False);
  rc = ppoll (fds, nfds, timeout, &oldset);
  Unblock ();

  return rc;
}
