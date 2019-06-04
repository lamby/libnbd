/* NBD client library in userspace
 * Copyright (C) 2013-2019 Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <inttypes.h>

#include "internal.h"

int
nbd_unlocked_aio_get_fd (struct nbd_handle *h)
{
  if (!h->sock) {
    set_error (EINVAL, "connection is not in a connected state");
    return -1;
  }
  return h->sock->ops->get_fd (h->sock);
}

int
nbd_unlocked_aio_notify_read (struct nbd_handle *h)
{
  return nbd_internal_run (h, notify_read);
}

int
nbd_unlocked_aio_notify_write (struct nbd_handle *h)
{
  return nbd_internal_run (h, notify_write);
}

/* NB: is_locked = false, may_set_error = false. */
int
nbd_unlocked_aio_is_created (struct nbd_handle *h)
{
  return h->state == STATE_START;
}

static int
is_connecting_group (enum state_group group)
{
  switch (group) {
  case GROUP_TOP:
    return 0;
  case GROUP_CONNECT:
  case GROUP_CONNECT_TCP:
  case GROUP_CONNECT_COMMAND:
  case GROUP_MAGIC:
  case GROUP_OLDSTYLE:
  case GROUP_NEWSTYLE:
    return 1;
  default:
    return is_connecting_group (nbd_internal_state_group_parent (group));
  }
}

/* NB: is_locked = false, may_set_error = false. */
int
nbd_unlocked_aio_is_connecting (struct nbd_handle *h)
{
  enum state_group group = nbd_internal_state_group (h->state);

  return is_connecting_group (group);
}

/* NB: is_locked = false, may_set_error = false. */
int
nbd_unlocked_aio_is_ready (struct nbd_handle *h)
{
  return h->state == STATE_READY;
}

static int
is_processing_group (enum state_group group)
{
  switch (group) {
  case GROUP_TOP:
    return 0;
  case GROUP_ISSUE_COMMAND:
  case GROUP_REPLY:
    return 1;
  default:
    return is_processing_group (nbd_internal_state_group_parent (group));
  }
}

/* NB: is_locked = false, may_set_error = false. */
int
nbd_unlocked_aio_is_processing (struct nbd_handle *h)
{
  enum state_group group = nbd_internal_state_group (h->state);

  return is_processing_group (group);
}

/* NB: is_locked = false, may_set_error = false. */
int
nbd_unlocked_aio_is_dead (struct nbd_handle *h)
{
  return h->state == STATE_DEAD;
}

/* NB: is_locked = false, may_set_error = false. */
int
nbd_unlocked_aio_is_closed (struct nbd_handle *h)
{
  return h->state == STATE_CLOSED;
}

int
nbd_unlocked_aio_command_completed (struct nbd_handle *h,
                                    int64_t handle)
{
  struct command_in_flight *prev_cmd, *cmd;
  uint16_t type;
  uint32_t error;

  if (handle < 1) {
    set_error (EINVAL, "invalid aio handle %" PRId64, handle);
    return -1;
  }

  /* Find the command amongst the completed commands. */
  for (cmd = h->cmds_done, prev_cmd = NULL;
       cmd != NULL;
       prev_cmd = cmd, cmd = cmd->next) {
    if (cmd->handle == handle)
      break;
  }
  if (!cmd)
    return 0;

  type = cmd->type;
  error = cmd->error;

  /* Retire it from the list and free it. */
  if (prev_cmd != NULL)
    prev_cmd->next = cmd->next;
  else
    h->cmds_done = cmd->next;

  free (cmd);

  /* If the command was successful, return true. */
  if (error == 0)
    return 1;

  /* The command failed, set an error indication and return an error. */
  set_error (error, "%s: command failed: %s",
             nbd_internal_name_of_nbd_cmd (type), strerror (error));
  return -1;
}

int64_t
nbd_unlocked_aio_peek_command_completed (struct nbd_handle *h)
{
  if (h->cmds_done != NULL)
    return h->cmds_done->handle;

  if (h->cmds_in_flight != NULL || h->cmds_to_issue != NULL) {
    set_error (0, "no in-flight command has completed yet");
    return 0;
  }
  set_error (EINVAL, "no commands are in flight");
  return -1;
}
