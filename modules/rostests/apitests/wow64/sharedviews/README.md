# Shared win32k view placement

Build `w643b5_sharedviews` for i386 and amd64, and `w643b5_probe` plus
`win32u` for amd64, with `ENABLE_ROSTESTS=ON`. Put the native probe DLL at
`C:\harness\w643b5_probe.dll` and native `win32u.dll` in System32. Run each
runner on a ReactOS amd64 image with WoW64 enabled, once per mode:

| Argument | Address pressure before mapping |
| --- | --- |
| 0 | None |
| 1 | Reserve free space below 0x70000000; all shared views must still fit |
| 2 | Reserve all free space below 2 GB before GUI process initialization |
| 3 | Initialize GUI state, then reserve low space before creating GDI objects |
| 4 | Leave a 1 MB hole for the global heap; the GDI table cannot fit |
| 5 | Leave a 0x290000-byte hole for the global heap and GDI table; the desktop cannot fit |

The test prints addresses and allocation ends for the desktop heap, server
info, USER table, GDI table, DC/brush/region attributes and display-DC
attributes. WoW64 addresses and complete allocations must stay below 2 GB.
With no suitable low space, mapping or object creation must fail cleanly.
The native amd64 control must continue to succeed, including placement above
2 GB in the exhaustion cases. A nonzero exit means a failed assertion or
setup failure. Compare the same test binaries against the original and
changed kernels; the original kernel fails the WoW64 exhaustion cases by
returning high mappings.

These fixtures test the kernel mapping contract before guest GUI thunks
exist. The PE32 runner is a real WoW64 process, verified through both the
32-bit API and native `ProcessWow64Information`. A test-only mode switch calls
native `LdrLoadDll` and the probe on the native stack saved by ReactOS
`wow64cpu`. The probe uses native win32u entry points and supplies only the
client-thread-startup callback; any unexpected callback fails the test.
The bridge depends on ReactOS phase-2 runtime offsets and is not a general
Windows or WoW64 bridge. No runtime DLL is patched to run it. The native
control calls the exact same probe without the bridge.

The fixture does not test guest user32 layouts, guest callback dispatch,
window procedures, drawing or GUI application compatibility. It is not
installed in default images.

Brush creation currently allocates its attribute before handle insertion, so
the initial handle-table entry does not expose it. The probe releases and
reallocates that attribute through the stock-attribute APIs once a handle
exists, then checks the published address. It does not repair that separate
native brush-publication issue.
