NTFS checker: ntfsck of ntfsprogs-plus, ported to ReactOS
==========================================================

What is here
------------
ntfsprogs-plus/   Files imported from https://github.com/ntfsprogs-plus/ntfsprogs-plus at commit
                  3fe890434082973aa98fd441d65ae84e5d6e5fc8 (2026-08-26): src/ntfsck.c with src/utils.c,
                  src/utils.h, src/list.h, src/cluster.h, the libntfs files the checker links
                  (libntfs/*.c, 29 files) and include/*.h (40 files), and COPYING. Unchanged except the
                  patches listed below.
*.c, *.h          ReactOS files: config.h (replaces the autoconf config.h), compat_ros.h and compat.c
                  (C library calls ntdll.dll does not export), ntdevice.c and ntio.c (libntfs device
                  operations on NT native volume I/O), stats.c (the figures of the summary),
                  ntfschk.c (NtfsckRun: the check, reported as the NTFS CHKDSK of Windows Server 2008
                  R2 reports it), ntfsck_lib.h and ntio.h (interfaces between the two halves).
posix/            Empty pwd.h and grp.h: included upstream, nothing of them is used.

ntfscklib holds the imported files with ntdevice.c and stats.c; ntfsckros holds the NT-side files.
They are separate because the imported headers clash with the NT headers (types.h, debug.h, SID,
ACL, STATUS_NOT_FOUND). dll/win32/ntfsck builds ntfsck.dll from both; untfs.dll loads it for each
check (the checker keeps its state in globals that only a fresh load initializes).

Licences
--------
ntfsprogs-plus/src/ntfsck.c, libntfs/fsck.c, libntfs/problem.c, include/fsck.h,
include/problem.h: "SPDX-License-Identifier : GPL-2.0", that is GPL-2.0-only (Copyright LG
Electronics). Every other imported file with a licence header is GPL-2.0-or-later (ntfs-3g and
Linux-NTFS authors); libntfs/lib_utils.c, include/lib_utils.h, libntfs/realpath.c and
include/realpath.h carry no licence header and fall under the project's GPL-2.0 (COPYING). The
ReactOS files are GPL-2.0-or-later. ntfsck.dll as a whole is therefore distributed under
GPL-2.0-only. It must not be linked into one image with GPL-3.0 code (vfatlib's dosfstools
check is GPL-3.0-or-later).

Patches to the imported files
-----------------------------
src/ntfsck.c   Library mode (NTFSCK_LIBRARY): main(), usage(), version() and the option table
               are not built; the part of main() after the option parsing is ntfsck_check(), used
               by main() and by ntfsck_lib_run(); its exit() calls return instead and unmount the
               volume. A read-only check no longer tries to clear the dirty flag. With the Windows
               tolerance, a stale FILE_NAME data size in an index entry is counted, not reported as
               an error. In library mode the check also fills the caller's ntfsck_lib_result: the
               volume figures (ntfsck_lib_collect() after the mount and after repairs), the
               cluster-bitmap and MFT-bitmap mismatch counts apart, the ntfsng journal sequence at the
               end. A label before a declaration gets an empty statement (GCC 8).
libntfs/fsck.c The scratch-file close() moves inside the Linux-only scratch code.
               ntfs_fsck_mount() refuses a cluster count or an $MFT allocated size whose bitmap
               arrays would not fit in size_t (a damaged $MFT wrapped the allocation on 32-bit
               builds), and an $MFT larger than the volume.

Updating to a newer upstream
----------------------------
1. git clone https://github.com/ntfsprogs-plus/ntfsprogs-plus; check out the old commit above.
2. diff -u each imported file against this directory: that is the carried patch (it must equal
   the list above).
3. Check out the new commit; copy the same files over this directory; apply the carried patch
   (or git merge-file old-upstream/new-upstream/ours per file); add any libntfs file the new
   ntfsck.c needs (link errors name them).
4. Build ntfsck.dll for i386 and amd64, then rerun the calibration volumes and the damage corpus
   (work/briefs/ntfs-chkdsk.md, "Test plan") before the update goes in.
