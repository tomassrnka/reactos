# Per-process win32k client procedure fixture

Build `win32k_wow64_apitest` for amd64 and i386 (GCC). On a ReactOS amd64
image built with `BUILD_WOW64_ENABLED`, put both executables in one directory:

- amd64 executable: `win32k_wow64_apitest.exe`
- i386 executable: `win32k_wow64_client.exe`

Run `win32k_wow64_apitest.exe ClientPfn`. The native host initializes user32,
then starts two simultaneous WoW64 children. Each child registers different
A/W/worker/module tables, waits until both have registered, and queries its
system classes. The test also checks bad-pointer retries, first-successful
registration semantics, module normalization and unchanged native classes.

The test-only assembly bridge issues native calls with explicit amd64
layouts and handles the native thread-startup callback. It lets this test
run with the phase-2 runtime, without loading a guest user32 or implementing
wow64win. Service numbers come from the ReactOS build's generated tables;
the test skips other operating systems. This is not a test of guest callback
dispatch, system-class window creation, or the future wide user32 DLLs.

The host skips when its PE32 companion is absent. Run paired before/after
kernels with the same two executables; the original global-table kernel
fails the invalid-input and process-specific procedure assertions.
