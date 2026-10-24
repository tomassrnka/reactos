#!/bin/bash
# Reproducible build of the Mesa3D OpenGL driver (WGL ICD) for ReactOS, i386 and amd64.
# Host: Ubuntu 24.04 with gcc-mingw-w64-{i686,x86-64}-win32, g++-mingw-w64-{i686,x86-64}-win32,
# python3-venv, ninja-build, bison, flex, curl, xz-utils, patch. Needs network for the pinned downloads.
# Usage: build-mesa.sh [WORKDIR]   Output: WORKDIR/out/{i386,amd64}/libgallium_wgl.dll, mesa3d-licenses.txt, SHA256SUMS
set -euo pipefail

MESA_VERSION=26.2.4
MESA_URL=https://archive.mesa3d.org/mesa-${MESA_VERSION}.tar.xz
MESA_SHA256=bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9
PIP_PINS="meson==1.12.1 mako==1.4.3 MarkupSafe==3.0.4 PyYAML==6.0.3 packaging==26.3 ply==3.11"
GALLIUM_DRIVERS=${GALLIUM_DRIVERS:-softpipe}
OPT=${OPT:-2}
JOBS=${JOBS:-8}

HERE=$(dirname "$(realpath "$0")")
W=$(realpath -m "${1:-$PWD/mesa-build}")
mkdir -p "$W/dl" "$W/out"
cd "$W"

[ -f "dl/mesa-${MESA_VERSION}.tar.xz" ] || curl -fsSL --retry 8 --retry-all-errors --retry-delay 10 -o "dl/mesa-${MESA_VERSION}.tar.xz" "$MESA_URL"
echo "$MESA_SHA256  dl/mesa-${MESA_VERSION}.tar.xz" | sha256sum -c -
rm -rf src
mkdir src
tar -xf "dl/mesa-${MESA_VERSION}.tar.xz" -C src
for p in "$HERE"/patches/*.patch; do patch -p1 -d "src/mesa-${MESA_VERSION}" < "$p"; done

# the environment is rebuilt whenever the pins differ from the ones it was made with
if [ "$(cat venv/pins 2>/dev/null)" != "$PIP_PINS" ]; then
    rm -rf venv
    python3 -m venv venv
    ./venv/bin/pip -q install $PIP_PINS
    echo "$PIP_PINS" > venv/pins
fi
export PATH="$W/venv/bin:$PATH"
export SOURCE_DATE_EPOCH=1760400000

for arch in i386 amd64; do
    case $arch in i386) tool=i686; fam=x86;; amd64) tool=x86_64; fam=x86_64;; esac
    cat > "cross-$arch.ini" <<EOT
[binaries]
c = '$tool-w64-mingw32-gcc-win32'
cpp = '$tool-w64-mingw32-g++-win32'
ar = '$tool-w64-mingw32-gcc-ar-win32'
strip = '$tool-w64-mingw32-strip'
windres = '$tool-w64-mingw32-windres'
exe_wrapper = 'true'

[host_machine]
system = 'windows'
cpu_family = '$fam'
cpu = '$tool'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOT
    rm -rf "build-$arch"
    # zlib comes from Mesa's own wrap (hash-pinned there), never from the build host
    meson setup "build-$arch" "src/mesa-${MESA_VERSION}" --cross-file "cross-$arch.ini" \
        --force-fallback-for=zlib -Dzlib:default_library=static \
        --buildtype=release -Doptimization="$OPT" -Db_ndebug=true \
        -Dplatforms=windows -Dgallium-drivers="$GALLIUM_DRIVERS" -Dvulkan-drivers= -Dllvm=disabled \
        -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=true \
        -Dmin-windows-version=7 -Dxmlconfig=disabled -Dshader-cache=disabled -Dzstd=disabled \
        -Dlibunwind=disabled -Dvalgrind=disabled -Dbuild-tests=false \
        -Dc_args="-ffile-prefix-map=$W=." -Dcpp_args="-ffile-prefix-map=$W=." \
        -Dc_link_args="-static-libgcc -Wl,--no-insert-timestamp" \
        -Dcpp_link_args="-static-libgcc -static-libstdc++ -Wl,--no-insert-timestamp"
    ninja -C "build-$arch" -j"$JOBS" src/gallium/targets/wgl/libgallium_wgl.dll
    mkdir -p "out/$arch"
    "$tool-w64-mingw32-strip" -s -o "out/$arch/libgallium_wgl.dll" "build-$arch/src/gallium/targets/wgl/libgallium_wgl.dll"
done

# the notices come from the files that were compiled (both builds)
python3 "$HERE/gen-licenses.py" "$W" > out/mesa3d-licenses.txt

(cd out && sha256sum i386/libgallium_wgl.dll amd64/libgallium_wgl.dll mesa3d-licenses.txt | tee SHA256SUMS)
