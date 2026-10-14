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
