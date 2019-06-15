#!/usr/bin/env bash
# nbd client library in userspace
# Copyright (C) 2019 Red Hat Inc.
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test structured read callbacks.

source ../tests/functions.sh
set -e
set -x

requires qemu-img --version
requires qemu-io --version
requires qemu-nbd --version

files="structured-read.sock structured-read.qcow2"
rm -f $files
cleanup_fn rm -f $files

# Create file with cluster size 512 and contents all 1 except for single
# 512-byte hole at offset 2048
qemu-img create -f qcow2 -o cluster_size=512,compat=v3 structured-read.qcow2 3k
qemu-io -d unmap -f qcow2 -c 'w -P 1 0 3k' -c 'w -zu 2k 512' \
	structured-read.qcow2

qemu-nbd -k $PWD/structured-read.sock -f qcow2 structured-read.qcow2 &
qemu_pid=$!
cleanup_fn kill $qemu_pid

# qemu-nbd --pid not available before 4.1, so ...
for ((i = 0; i < 300; i++)); do
  if [ -r $PWD/structured-read.sock ]; then
    break
  fi
  kill -s 0 $qemu_pid 2>/dev/null
  if test $? != 0; then
    echo "qemu-nbd unexpectedly quit" 2>&1
    exit 1
  fi
  sleep 0.1
done

# Run the test.
$VG ./structured-read structured-read.sock
