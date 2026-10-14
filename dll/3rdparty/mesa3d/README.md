# Mesa3D software OpenGL driver

This directory holds the Mesa3D OpenGL driver that ReactOS installs by default as the OpenGL
driver for displays that bring none of their own. It is the Gallium software rasterizer
(softpipe) behind the Windows OpenGL driver interface (WGL ICD) that Mesa ships, and it gives
programs OpenGL 3.3 (compatibility profile) with GLSL 3.30. The built-in OpenGL 1.1
implementation in `dll/opengl` stays as the fallback, and as the choice of the OpenGL control
panel ("ReactOS Software Implementation").

Programs that need more than OpenGL 1.1 need this driver: SDL 2 games, the OpenGL video output
of VLC and Direct3D 7 and 9 programs, which Wine's Direct3D layer (wined3d) runs on OpenGL 2.0
or newer.

## What is here

| Path | What |
|---|---|
| `i386/libgallium_wgl.dll`, `amd64/libgallium_wgl.dll` | The prebuilt driver, copied to `system32` of every image by `CMakeLists.txt` |
| `build-mesa.sh` | Builds both DLLs from the pinned Mesa release tarball |
| `patches/` | Changes to Mesa that the build applies (two, see below) |
| `SHA256SUMS` | Hashes of the tarball, the two DLLs and the licence file |
| `mesa3d-licenses.txt` | The licence texts and copyright lines of every file that was compiled into the DLLs (made by `gen-licenses.py`); installed as `system32\mesa3d-licenses.txt` next to the DLL |
| `gen-licenses.py` | Reads the compiled sources and headers of a build and writes `mesa3d-licenses.txt`, with the copyright files of the packages `mingw-w64-common` and `gcc-mingw-w64-base` of the build host appended (the statically linked runtime); `build-mesa.sh` runs it and stops when those files are missing |

The DLLs are committed instead of being built with the image because the build needs Python
(meson), a second toolchain and a download of 68 MB, and a ReactOS build must not depend on any
of them. Anyone can check a committed DLL: the build is reproducible, and `SHA256SUMS` lists
what it must produce.

## Version and build

* Mesa 26.2.4, `https://archive.mesa3d.org/mesa-26.2.4.tar.xz`, SHA-256
  `bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9` (Mesa publishes no hash
  file; this value was computed from the download).
* zlib 1.3.1 is fetched by meson through Mesa's own wrap file, which pins its hash, and is linked
  statically. The C++ and GCC runtimes are linked statically, so the DLL imports only
  `kernel32`, `user32`, `gdi32` and `msvcrt`.
* Toolchain: Ubuntu 24.04 packages `gcc-mingw-w64-{i686,x86-64}-win32` and
  `g++-mingw-w64-{i686,x86-64}-win32` (GCC 13-win32, mingw-w64 11.0.1), meson 1.12.1, Mako 1.4.3,
  PyYAML 6.0.3 (the exact list is in the script), Ninja, Bison, Flex.
* Run `./build-mesa.sh WORKDIR`; it needs about 45 seconds per architecture on 12 jobs and writes
  `WORKDIR/out/{i386,amd64}/libgallium_wgl.dll`. Building the same tarball in two different
  directories gave identical DLLs (checked 2026-10-14).
* The options turn off everything that is not the Windows OpenGL driver, and set
  `min-windows-version=7`, so that the DLL uses no function that Windows 7 lacks.

## Changes to Mesa

* `0001`: softpipe allocated a texture cache for each of its 768 sampler view slots when a
  context was created, about 250 MB of memory per context. A 32-bit process has 2 GB of address
  space and wined3d creates several contexts per Direct3D object, so the persistent buffer
  allocation of wined3d failed and the program crashed. The caches now exist for the slots that
  were used; a context takes about 4 MB.
* `0002`: softpipe built every blit shader when a context was created. That took more than
  100 ms of an idle machine per context, and the time of one `Direct3DCreate9` went from about
  3 seconds to 50 milliseconds on the i386 test VM. The shaders are now made on the first blit,
  with the shader creation function of the driver (the blitter binds them without the wrappers
  of the Draw stages, so they must not be made through the wrappers).

## How the system uses it

`dll/opengl/opengl32` asks the display driver for an OpenGL driver. The display drivers of
ReactOS give none, so it now reads the system default from the default value of
`HKLM\SOFTWARE\ReactOS\OpenGL` ("Mesa3D") and loads the driver that is registered under
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\Mesa3D`. Both keys are in
`boot/bootdata/hivesft.inf`. A display driver that provides its own OpenGL driver still wins,
and so does the value in `HKCU\Software\ReactOS\OpenGL` (the OpenGL control panel writes it).
Windows that draw to a bitmap or a printer keep the built-in implementation. If the DLL is
missing, or the processor lacks SSE2 (the registration carries `RequiresSSE2`, because the DLLs
are compiled for it), the built-in implementation is used.

## Speed and size

Measured on the test VMs (2 vCPUs, QEMU with KVM on a loaded host) with a lit three-gear scene of
300 by 300 pixels, double buffered, no swap interval: softpipe renders 23 frames per second on
amd64 and 72 on i386, the built-in implementation 26 and 102. The speed of softpipe is that of a
reference rasterizer; the point of the driver is the OpenGL version, not the speed. The DLL is
18.6 MB (amd64) and 19.3 MB (i386); the images grow by the compressed size of the DLL, see the
lane note for the measured values.

llvmpipe (LLVM JIT) was built too (LLVM 19.1.7, the same toolchain) and is not used: the DLL is
73 MB on i386, the first OpenGL call takes about 4 seconds, it was slower than softpipe on these
scenes and its rasterizer threads stopped (deadlock) after a few seconds on i386.

## Known differences from Windows

Mesa's pixel formats and a few WGL details differ from what Wine's `opengl32` test expects of a
Windows driver (it offers `PFD_SUPPORT_GDI` together with double buffering, does not reject a
share list that was shared before, has one global swap interval). These are Mesa's choices and
no program of the test set depends on them.

## Updating

Change `MESA_VERSION` and `MESA_SHA256` in `build-mesa.sh`, rebuild, check that the patches
still apply, replace the two DLLs, rewrite `SHA256SUMS` and run
`opengl32_apitest icd_default` and `ddraw_apitest` on both architectures.
