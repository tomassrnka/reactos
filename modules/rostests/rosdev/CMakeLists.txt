# rosdev: test-only guest agent (see rosdev.h). It is deliberately never placed on any ISO or
# image (no add_cd_file or add_rostests_file); the injector tool copies it into test-VM overlays.
list(APPEND SOURCE
    main.c
    proto.c
    jobs.c
    sha256.c
    rosdev.h)

add_executable(rosdev ${SOURCE})
set_module_type(rosdev win32gui UNICODE)
add_importlibs(rosdev ws2_32 advapi32 user32 gdi32 msvcrt kernel32)

# A hung window for host/repro-force-reboot.sh; test VMs only, never installed either
add_executable(rosdev_hungwin hungwin.c)
set_module_type(rosdev_hungwin win32gui UNICODE)
add_importlibs(rosdev_hungwin user32 msvcrt kernel32)

# ptrblink: GUI load for the mouse pointer blink test (host/ptrblink-test); not placed on any ISO either
add_executable(ptrblink ptrblink.c)
set_module_type(ptrblink win32gui)
add_importlibs(ptrblink msimg32 user32 gdi32 msvcrt kernel32)
