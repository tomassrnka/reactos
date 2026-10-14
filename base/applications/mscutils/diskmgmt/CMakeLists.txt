
list(APPEND SOURCE
    diskcollect.c
    diskmgmt.c
    diskmodel.c
    graphview.c
    props.c
    diskmgmt.h
    diskmodel.h
    resource.h)

file(GLOB diskmgmt_rc_deps res/*.*)
add_rc_deps(diskmgmt.rc ${diskmgmt_rc_deps})
add_executable(diskmgmt ${SOURCE} diskmgmt.rc)
set_module_type(diskmgmt win32gui UNICODE)
add_importlibs(diskmgmt user32 gdi32 advapi32 comctl32 shell32 shlwapi msvcrt kernel32)
add_cd_file(TARGET diskmgmt DESTINATION reactos/system32 FOR all)
