#!/bin/bash
# build-loader.sh WORKDIR  : builds Khronos Vulkan-Loader v1.4.366 as vulkan-1.dll for amd64 + i386 with system mingw
set -euo pipefail
W=$(realpath "$1")
# headers are header-only: install once with the host toolchain
cmake -S $W/src/headers -B $W/hb -G Ninja -DCMAKE_INSTALL_PREFIX=$W/hi >/dev/null
cmake --build $W/hb --target install >/dev/null
for A in amd64 i386; do
  if [ $A = amd64 ]; then T=x86_64-w64-mingw32; P=AMD64; else T=i686-w64-mingw32; P=X86; fi
  cat > $W/tc-$A.cmake <<TC
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR $P)
set(CMAKE_C_COMPILER $T-gcc-win32)
set(CMAKE_CXX_COMPILER $T-g++-win32)
set(CMAKE_RC_COMPILER $T-windres)
TC
  cmake -S $W/src/loader -B $W/lb-$A -G Ninja -DCMAKE_TOOLCHAIN_FILE=$W/tc-$A.cmake -DCMAKE_BUILD_TYPE=Release \
     -DVulkanHeaders_DIR=$W/hi/share/cmake/VulkanHeaders -DBUILD_TESTS=OFF -DUSE_GAS=ON \
     -DCMAKE_SHARED_LINKER_FLAGS="-static-libgcc -s -Wl,--no-insert-timestamp" -DCMAKE_C_FLAGS="-D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -ffile-prefix-map=$W=."
  cmake --build $W/lb-$A -j8
done
