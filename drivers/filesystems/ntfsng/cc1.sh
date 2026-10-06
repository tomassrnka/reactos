#!/bin/bash
# cc1.sh FILE... - compile-check fs/ntfs core/shim files with the ReactOS i386 flags (development helper)
D=/root/ros/ntfs-ng/drivers/filesystems/ntfsng
S=/root/ros/ntfs-ng/sdk/include
CC=/root/ros/RosBE-CI/i386/bin/i686-w64-mingw32-gcc
mkdir -p /root/ros/build/ntfsng-scratch
for f in "$@"; do
  $CC -D__REACTOS__ -D_X86_ -D__i386__ -DDBG=1 -I$D/shim/include -I$D/fs -I$D/core -I$S/crt -I$S/vcruntime -I$S/psdk -nostdinc \
    -pipe -fno-strict-aliasing -fno-common -O1 -fno-omit-frame-pointer -march=pentium -mtune=generic \
    -mstackrealign -mpreferred-stack-boundary=3 -std=gnu11 -mno-ms-bitfields -D__KERNEL__ \
    -DCONFIG_NTFS_FS_WOF_COMPRESSION -w -Werror=implicit-function-declaration -fmax-errors=20 \
    -c $D/$f -o /root/ros/build/ntfsng-scratch/$(basename $f .c).o || echo "FAIL $f"
done
