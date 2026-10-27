# Khronos Vulkan-Loader (vulkan-1.dll)

The Vulkan loader is the DLL that Vulkan programs (and `vulkaninfo`) link to. It reads the ICD
manifests that are registered under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` and dispatches the Vulkan
calls to the driver DLL that a manifest names. Without it, the lavapipe ICD (`vulkan_lvp.dll`,
`dll/3rdparty/mesa3d`) cannot be reached by any program that does not load the ICD by hand.

## What is here

| Path | What |
|---|---|
| `amd64/vulkan-1.dll` | The prebuilt loader (680 KB), copied to `system32` by `CMakeLists.txt` (amd64, export version >= 0x600) |
| `build-loader.sh` | Builds the loader for amd64 and i386 from the pinned tags |
| `gen-licenses.sh` | Writes `vulkan-loader-licenses.txt` from the sources |
| `vulkan-loader-licenses.txt` | Apache-2.0 text, NOTICE statement, MIT/HPND notices of bundled files and the toolchain runtime; installed as `system32\vulkan-loader-licenses.txt` next to the DLL (the same way `mesa3d-licenses.txt` and `llvm-licenses.txt` travel with the lavapipe pack) |
| `SHA256SUMS` | Hashes of the DLL and the licence file |

## Version and build

* Vulkan-Loader `v1.4.366`, commit `3187e1e4d463ddf7f60f0bbaf26c7d7b7e40ca34`
  (`https://github.com/KhronosGroup/Vulkan-Loader`), Apache-2.0.
* Vulkan-Headers `v1.4.366`, commit `73814d006638aa00bd553215b054f0f02e457e22` (the version the loader's
  `known_good.json` pins).
* Toolchain: Ubuntu 24.04 `gcc-mingw-w64-{x86-64,i686}-win32` (GCC 13-win32, mingw-w64 11.0.1), CMake 3.28,
  Ninja; GAS trampolines (`USE_GAS=ON`), `-static-libgcc`, stripped, no PE timestamp. The DLL imports only
  `kernel32`, `advapi32`, `cfgmgr32` and `msvcrt`.
* `./build-loader.sh WORKDIR` with `WORKDIR/src/loader` and `WORKDIR/src/headers` checked out at the tags
  above. Building in two different directories gave identical DLLs (checked 2026-10-27).
* Built with the system mingw and not the RosBE compiler (RosBE strips `.eh_frame` from 32-bit images,
  which ReactOS rejects as "not a valid Win32 application"). The i386 DLL builds too but is not shipped:
  there is no 32-bit ICD.

## How the system uses it

`syssetup` (`RegisterVulkanIcds`) and the featmgr lavapipe pack write the absolute path of
`lvp_icd.json` as a value name (REG_DWORD 0 = enabled) of `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`; that is
the key and format the loader reads (`VK_DRIVERS_INFO_REGISTRY_LOC`). The manifest's `library_path` is
`vulkan_lvp.dll`, which the loader loads from `system32`.

## Test

`vkprobe.c` loads `vulkan-1.dll`, creates an instance and lists the physical devices into a file (the serial
port is not reliable on the goldens). Build it with the system mingw
(`x86_64-w64-mingw32-gcc -I<Vulkan-Headers>/include vkprobe.c -o vkprobe.exe`) and run it in the guest; on
the default amd64 image it reports `llvmpipe (LLVM 19.1.7, 256 bits)`, Vulkan 1.4.354.
