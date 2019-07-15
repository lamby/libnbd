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
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <assert.h>
#include <limits.h>

#include "internal.h"

static int
wait_for_command (struct nbd_handle *h, int64_t cookie)
{
  int r;

  while ((r = nbd_unlocked_aio_command_completed (h, cookie)) == 0) {
    if (nbd_unlocked_poll (h, -1) == -1)
      return -1;
  }

  return r == -1 ? -1 : 0;
}

/* Issue a read command and wait for the reply. */
int
nbd_unlocked_pread (struct nbd_handle *h, void *buf,
                    size_t count, uint64_t offset, uint32_t flags)
{
  int64_t ch;

  ch = nbd_unlocked_aio_pread (h, buf, count, offset, flags);
  if (ch == -1)
    return -1;

  return wait_for_command (h, ch);
}

/* Issue a read command with callbacks and wait for the reply. */
int
nbd_unlocked_pread_structured (struct nbd_handle *h, void *buf,
                               size_t count, uint64_t offset,
                               void *opaque, read_fn read, uint32_t flags)
{
  int64_t ch;

  ch = nbd_unlocked_aio_pread_structured (h, buf, count, offset,
                                          opaque, read, flags);
  if (ch == -1)
    return -1;

  return wait_for_command (h, ch);
}

/* Issue a write command and wait for the reply. */
int
nbd_unlocked_pwrite (struct nbd_handle *h, const void *buf,
                     size_t count, uint64_t offset, uint32_t flags)
{
  int64_t ch;

  ch = nbd_unlocked_aio_pwrite (h, buf, count, offset, flags);
  if (ch == -1)
    return -1;

  return wait_for_command (h, ch);
}

/* Issue a flush command and wait for the reply. */
int
nbd_unlocked_flush (struct nbd_handle *h, uint32_t flags)
{
  int64_t ch;

  ch = nbd_unlocked_aio_flush (h, flags);
  if (ch == -1)
    return -1;

  return wait_for_command (h, ch);
}

/* Issue a trim command and wait for the reply. */
int
nbd_unlocked_trim (struct nbd_handle *h,
                   uint64_t count, uint64_t offset, uint32_t flags)
{
  int64_t ch;

  ch = nbd_unlocked_aio_trim (h, count, offset, flags);
  if (ch == -1)
    return -1;

  return wait_for_command (h, ch);
}

/* Issue a cache command and wait for the reply. */
int
nbd_unlocked_cache (struct nbd_handle *h,
                    uint64_t count, uint64_t offset, uint32_t flags)
{
  int64_t ch;

  ch = nbd_unlocked_aio_cache (h, count, offset, flags);
  if (ch == -1)
    return -1;

  return wait_for_command (h, ch);
}

/* Issue a zero command and wait for the reply. */
int
nbd_unlocked_zero (struct nbd_handle *h,
                   uint64_t count, uint64_t offset, uint32_t flags)
{
  int64_t ch;

  ch = nbd_unlocked_aio_zero (h, count, offset, flags);
  if (ch == -1)
    return -1;

  return wait_for_command (h, ch);
}

/* Issue a block status command and wait for the reply. */
int
nbd_unlocked_block_status (struct nbd_handle *h,
                           uint64_t count, uint64_t offset,
                           void *data, extent_fn extent, uint32_t flags)
{
  int64_t ch;

  ch = nbd_unlocked_aio_block_status (h, count, offset, data, extent, flags);
  if (ch == -1)
    return -1;

  return wait_for_command (h, ch);
}

int64_t
nbd_internal_command_common (struct nbd_handle *h,
                             uint16_t flags, uint16_t type,
                             uint64_t offset, uint64_t count, void *data,
                             struct command_cb *cb)
{
  struct command_in_flight *cmd, *prev_cmd;

  if (h->disconnect_request) {
      set_error (EINVAL, "cannot request more commands after NBD_CMD_DISC");
      return -1;
  }
  if (h->in_flight == INT_MAX) {
      set_error (ENOMEM, "too many commands already in flight");
      return -1;
  }

  switch (type) {
    /* Commands which send or receive data are limited to MAX_REQUEST_SIZE. */
  case NBD_CMD_READ:
  case NBD_CMD_WRITE:
    if (count > MAX_REQUEST_SIZE) {
      set_error (ERANGE, "request too large: maximum request size is %d",
                 MAX_REQUEST_SIZE);
      return -1;
    }
    break;

    /* Other commands are currently limited by the 32 bit field in the
     * command structure on the wire, but in future we hope to support
     * 64 bit values here with a change to the NBD protocol which is
     * being discussed upstream.
     */
  default:
    if (count > UINT32_MAX) {
      set_error (ERANGE, "request too large: maximum request size is %" PRIu32,
                 UINT32_MAX);
      return -1;
    }
    break;
  }

  cmd = calloc (1, sizeof *cmd);
  if (cmd == NULL) {
    set_error (errno, "calloc");
    return -1;
  }
  cmd->flags = flags;
  cmd->type = type;
  cmd->cookie = h->unique++;
  cmd->offset = offset;
  cmd->count = count;
  cmd->data = data;
  if (cb)
    cmd->cb = *cb;

  /* If structured replies were negotiated then we trust the server to
   * send back sufficient data to cover the whole buffer.  It's tricky
   * to check this, so an easier thing is simply to zero the buffer
   * ahead of time which avoids any security problems.  I measured the
   * overhead of this and for non-TLS there is no measurable overhead
   * in the highly intensive loopback case.  For TLS we get a
   * performance gain, go figure.
   */
  if (h->structured_replies && cmd->data && type == NBD_CMD_READ)
    memset (cmd->data, 0, cmd->count);

  /* Add the command to the end of the queue. Kick the state machine
   * if there is no other command being processed, otherwise, it will
   * be handled automatically on a future cycle around to READY.
   */
  if (h->cmds_to_issue != NULL) {
    assert (nbd_internal_is_state_processing (get_next_state (h)));
    prev_cmd = h->cmds_to_issue;
    while (prev_cmd->next)
      prev_cmd = prev_cmd->next;
    prev_cmd->next = cmd;
  }
  else {
    h->cmds_to_issue = cmd;
    if (nbd_internal_is_state_ready (get_next_state (h)) &&
        nbd_internal_run (h, cmd_issue) == -1)
      return -1;
  }

  h->in_flight++;
  return cmd->cookie;
}

int64_t
nbd_unlocked_aio_pread (struct nbd_handle *h, void *buf,
                        size_t count, uint64_t offset, uint32_t flags)
{
  return nbd_unlocked_aio_pread_notify (h, buf, count, offset, NULL, NULL,
                                        flags);
}

int64_t
nbd_unlocked_aio_pread_notify (struct nbd_handle *h, void *buf,
                               size_t count, uint64_t offset,
                               void *opaque, notify_fn notify, uint32_t flags)
{
  struct command_cb cb = { .opaque = opaque, .notify = notify, };

  /* We could silently accept flag DF, but it really only makes sense
   * with callbacks, because otherwise there is no observable change
   * except that the server may fail where it would otherwise succeed.
   */
  if (flags != 0) {
    set_error (EINVAL, "invalid flag: %" PRIu32, flags);
    return -1;
  }

  return nbd_internal_command_common (h, 0, NBD_CMD_READ, offset, count,
                                      buf, &cb);
}

int64_t
nbd_unlocked_aio_pread_structured (struct nbd_handle *h, void *buf,
                                   size_t count, uint64_t offset,
                                   void *opaque, read_fn read, uint32_t flags)
{
  return nbd_unlocked_aio_pread_structured_notify (h, buf, count, offset,
                                                   opaque, read, NULL, flags);
}

int64_t
nbd_unlocked_aio_pread_structured_notify (struct nbd_handle *h, void *buf,
                                          size_t count, uint64_t offset,
                                          void *opaque, read_fn read,
                                          notify_fn notify, uint32_t flags)
{
  struct command_cb cb = { .opaque = opaque, .fn.read = read,
                           .notify = notify, };

  if ((flags & ~LIBNBD_CMD_FLAG_DF) != 0) {
    set_error (EINVAL, "invalid flag: %" PRIu32, flags);
    return -1;
  }

  if ((flags & LIBNBD_CMD_FLAG_DF) != 0 &&
      nbd_unlocked_can_df (h) != 1) {
    set_error (EINVAL, "server does not support the DF flag");
    return -1;
  }

  return nbd_internal_command_common (h, flags, NBD_CMD_READ, offset, count,
                                      buf, &cb);
}

int64_t
nbd_unlocked_aio_pwrite (struct nbd_handle *h, const void *buf,
                         size_t count, uint64_t offset,
                         uint32_t flags)
{
  return nbd_unlocked_aio_pwrite_notify (h, buf, count, offset, NULL, NULL,
                                         flags);
}

int64_t
nbd_unlocked_aio_pwrite_notify (struct nbd_handle *h, const void *buf,
                                size_t count, uint64_t offset,
                                void *opaque, notify_fn notify, uint32_t flags)
{
  struct command_cb cb = { .opaque = opaque, .notify = notify, };

  if (nbd_unlocked_read_only (h) == 1) {
    set_error (EINVAL, "server does not support write operations");
    return -1;
  }

  if ((flags & ~LIBNBD_CMD_FLAG_FUA) != 0) {
    set_error (EINVAL, "invalid flag: %" PRIu32, flags);
    return -1;
  }

  if ((flags & LIBNBD_CMD_FLAG_FUA) != 0 &&
      nbd_unlocked_can_fua (h) != 1) {
    set_error (EINVAL, "server does not support the FUA flag");
    return -1;
  }

  return nbd_internal_command_common (h, flags, NBD_CMD_WRITE, offset, count,
                                      (void *) buf, &cb);
}

int64_t
nbd_unlocked_aio_flush (struct nbd_handle *h, uint32_t flags)
{
  return nbd_unlocked_aio_flush_notify (h, NULL, NULL, flags);
}

int64_t
nbd_unlocked_aio_flush_notify (struct nbd_handle *h, void *opaque,
                               notify_fn notify, uint32_t flags)
{
  struct command_cb cb = { .opaque = opaque, .notify = notify, };

  if (nbd_unlocked_can_flush (h) != 1) {
    set_error (EINVAL, "server does not support flush operations");
    return -1;
  }

  if (flags != 0) {
    set_error (EINVAL, "invalid flag: %" PRIu32, flags);
    return -1;
  }

  return nbd_internal_command_common (h, 0, NBD_CMD_FLUSH, 0, 0,
                                      NULL, &cb);
}

int64_t
nbd_unlocked_aio_trim (struct nbd_handle *h,
                       uint64_t count, uint64_t offset,
                       uint32_t flags)
{
  return nbd_unlocked_aio_trim_notify (h, count, offset, NULL, NULL, flags);
}

int64_t
nbd_unlocked_aio_trim_notify (struct nbd_handle *h,
                              uint64_t count, uint64_t offset,
                              void *opaque, notify_fn notify, uint32_t flags)
{
  struct command_cb cb = { .opaque = opaque, .notify = notify, };

  if (nbd_unlocked_read_only (h) == 1) {
    set_error (EINVAL, "server does not support write operations");
    return -1;
  }

  if ((flags & ~LIBNBD_CMD_FLAG_FUA) != 0) {
    set_error (EINVAL, "invalid flag: %" PRIu32, flags);
    return -1;
  }

  if ((flags & LIBNBD_CMD_FLAG_FUA) != 0 &&
      nbd_unlocked_can_fua (h) != 1) {
    set_error (EINVAL, "server does not support the FUA flag");
    return -1;
  }

  if (count == 0) {             /* NBD protocol forbids this. */
    set_error (EINVAL, "count cannot be 0");
    return -1;
  }

  return nbd_internal_command_common (h, flags, NBD_CMD_TRIM, offset, count,
                                      NULL, &cb);
}

int64_t
nbd_unlocked_aio_cache (struct nbd_handle *h,
                        uint64_t count, uint64_t offset, uint32_t flags)
{
  return nbd_unlocked_aio_cache_notify (h, count, offset, NULL, NULL, flags);
}

int64_t
nbd_unlocked_aio_cache_notify (struct nbd_handle *h,
                               uint64_t count, uint64_t offset,
                               void *opaque, notify_fn notify, uint32_t flags)
{
  struct command_cb cb = { .opaque = opaque, .notify = notify, };

  /* Actually according to the NBD protocol document, servers do exist
   * that support NBD_CMD_CACHE but don't advertise the
   * NBD_FLAG_SEND_CACHE bit, but we ignore those.
   */
  if (nbd_unlocked_can_cache (h) != 1) {
    set_error (EINVAL, "server does not support cache operations");
    return -1;
  }

  if (flags != 0) {
    set_error (EINVAL, "invalid flag: %" PRIu32, flags);
    return -1;
  }

  return nbd_internal_command_common (h, 0, NBD_CMD_CACHE, offset, count,
                                      NULL, &cb);
}

int64_t
nbd_unlocked_aio_zero (struct nbd_handle *h,
                       uint64_t count, uint64_t offset,
                       uint32_t flags)
{
  return nbd_unlocked_aio_zero_notify (h, count, offset, NULL, NULL, flags);
}

int64_t
nbd_unlocked_aio_zero_notify (struct nbd_handle *h,
                              uint64_t count, uint64_t offset,
                              void *opaque, notify_fn notify, uint32_t flags)
{
  struct command_cb cb = { .opaque = opaque, .notify = notify, };

  if (nbd_unlocked_read_only (h) == 1) {
    set_error (EINVAL, "server does not support write operations");
    return -1;
  }

  if ((flags & ~(LIBNBD_CMD_FLAG_FUA | LIBNBD_CMD_FLAG_NO_HOLE)) != 0) {
    set_error (EINVAL, "invalid flag: %" PRIu32, flags);
    return -1;
  }

  if ((flags & LIBNBD_CMD_FLAG_FUA) != 0 &&
      nbd_unlocked_can_fua (h) != 1) {
    set_error (EINVAL, "server does not support the FUA flag");
    return -1;
  }

  if (count == 0) {             /* NBD protocol forbids this. */
    set_error (EINVAL, "count cannot be 0");
    return -1;
  }

  return nbd_internal_command_common (h, flags, NBD_CMD_WRITE_ZEROES, offset,
                                      count, NULL, &cb);
}

int64_t
nbd_unlocked_aio_block_status (struct nbd_handle *h,
                               uint64_t count, uint64_t offset,
                               void *data, extent_fn extent,
                               uint32_t flags)
{
  return nbd_unlocked_aio_block_status_notify (h, count, offset, data, extent,
                                               NULL, flags);
}

int64_t
nbd_unlocked_aio_block_status_notify (struct nbd_handle *h,
                                      uint64_t count, uint64_t offset,
                                      void *data, extent_fn extent,
                                      notify_fn notify, uint32_t flags)
{
  struct command_cb cb = { .opaque = data, .fn.extent = extent,
                           .notify = notify, };

  if (!h->structured_replies) {
    set_error (ENOTSUP, "server does not support structured replies");
    return -1;
  }

  if (h->meta_contexts == NULL) {
    set_error (ENOTSUP, "did not negotiate any metadata contexts, "
               "either you did not call nbd_add_meta_context before "
               "connecting or the server does not support it");
    return -1;
  }

  if ((flags & ~LIBNBD_CMD_FLAG_REQ_ONE) != 0) {
    set_error (EINVAL, "invalid flag: %" PRIu32, flags);
    return -1;
  }

  if (count == 0) {             /* NBD protocol forbids this. */
    set_error (EINVAL, "count cannot be 0");
    return -1;
  }

  return nbd_internal_command_common (h, flags, NBD_CMD_BLOCK_STATUS, offset,
                                      count, NULL, &cb);
}
