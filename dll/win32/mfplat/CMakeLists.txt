
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)
add_definitions(
    -D__WINESRC__
    -D_WIN32_WINNT=0x603
    -DWINVER=0x603)

spec2def(mfplat.dll mfplat.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    buffer.c
    main.c
    mediatype.c
    queue.c
    sample.c)

if(DLL_EXPORT_VERSION GREATER_EQUAL 0x603)
    # The work queue exports of mfplat forward to rtworkq.dll, as on Windows 8.1 and later.
    set(MFPLAT_QUEUE_IMPORT rtworkq)
else()
    # rtworkq.dll is held back to 0x603 because programs probe for it. The Windows 7 mfplat
    # has its own queue, so the Rtwq code is linked in and mfplat.spec aliases the exports to it.
    list(APPEND SOURCE ../rtworkq/queue.c)
endif()

add_library(mfplat MODULE
    ${SOURCE}
    version.rc
    ${CMAKE_CURRENT_BINARY_DIR}/mfplat.def
    ${CMAKE_CURRENT_BINARY_DIR}/mfplat_stubs.c)

set_module_type(mfplat win32dll)
target_link_libraries(mfplat wine mfuuid dmoguids uuid pathcch_static oldnames)
add_delay_importlibs(mfplat bcrypt)
add_importlibs(mfplat advapi32 ole32 propsys shlwapi ${MFPLAT_QUEUE_IMPORT} msvcrt kernel32_vista kernel32 ntdll)
add_dependencies(mfplat psdk wineheaders d3d_idl_headers)
add_cd_file(TARGET mfplat DESTINATION reactos/system32 FOR all)
set_wine_module(mfplat)
