#!/bin/bash
# Reproducible build of a static LLVM (X86 target only, no tools) for mingw, i386 or amd64: the JIT that llvmpipe
# (OpenGL) and lavapipe (Vulkan) of Mesa compile their shaders with. Called by build-mesa.sh; usable on its own.
# Host: as build-mesa.sh, plus cmake. Needs network for the pinned downloads.
# Usage: build-llvm.sh WORKDIR ARCH   Output: WORKDIR/llvm-inst/ARCH (an LLVM install tree: lib/cmake/llvm, libLLVM*.a)
set -euo pipefail

LLVM_VERSION=19.1.7
LLVM_BASE=https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}
LLVM_SHA256_llvm=96f833c6ad99a3e8e1d9aca5f439b8fd2c7efdcf83b664e0af1c0712c5315910
LLVM_SHA256_cmake=11c5a28f90053b0c43d0dec3d0ad579347fc277199c005206b963c19aae514e3
LLVM_SHA256_third_party=b96deca1d3097c7ffd4ff2bb904a50bdd56bec7ed1413ffb0d1d01af87b72c12
JOBS=${JOBS:-8}

W=$(realpath -m "${1:?usage: build-llvm.sh WORKDIR ARCH}")
arch=${2:?usage: build-llvm.sh WORKDIR ARCH}
case $arch in i386) tool=i686;; amd64) tool=x86_64;; *) echo "unknown ARCH $arch" >&2; exit 2;; esac
mkdir -p "$W/dl"
cd "$W"

for p in llvm cmake third-party; do
    f=dl/$p-${LLVM_VERSION}.src.tar.xz
    [ -f "$f" ] || curl -fsSL --retry 8 --retry-all-errors --retry-delay 10 -o "$f" "${LLVM_BASE}/$p-${LLVM_VERSION}.src.tar.xz"
    var=LLVM_SHA256_${p//-/_}
    echo "${!var}  $f" | sha256sum -c -
    [ -d "llvmsrc/$p" ] || { mkdir -p llvmsrc; tar -xf "$f" -C llvmsrc; mv "llvmsrc/$p-${LLVM_VERSION}.src" "llvmsrc/$p"; }
done

# TableGen runs on the build host
if [ ! -x llvm-native/bin/llvm-tblgen ]; then
    cmake -S llvmsrc/llvm -B llvm-native -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=X86 \
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF
    ninja -C llvm-native -j"$JOBS" llvm-tblgen llvm-min-tblgen
fi

[ -f "llvm-inst/$arch/lib/cmake/llvm/LLVMConfig.cmake" ] && exit 0
cat > "llvm-toolchain-$arch.cmake" <<EOT
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR $tool)
set(CMAKE_C_COMPILER $tool-w64-mingw32-gcc-win32)
set(CMAKE_CXX_COMPILER $tool-w64-mingw32-g++-win32)
set(CMAKE_RC_COMPILER $tool-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
EOT
rm -rf "llvm-build-$arch"
cmake -S llvmsrc/llvm -B "llvm-build-$arch" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$W/llvm-toolchain-$arch.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
    -DCMAKE_INSTALL_PREFIX="$W/llvm-inst/$arch" -DLLVM_HOST_TRIPLE=$tool-w64-windows-gnu -DLLVM_DEFAULT_TARGET_TRIPLE=$tool-w64-windows-gnu \
    -DLLVM_NATIVE_TOOL_DIR="$W/llvm-native/bin" -DLLVM_TABLEGEN="$W/llvm-native/bin/llvm-tblgen" \
    -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_TOOLS=OFF -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_ENABLE_PIC=OFF -DLLVM_ENABLE_THREADS=ON -DLLVM_ENABLE_PLUGINS=OFF -DLLVM_BUILD_LLVM_DYLIB=OFF
ninja -C "llvm-build-$arch" -j"$JOBS"
ninja -C "llvm-build-$arch" install
