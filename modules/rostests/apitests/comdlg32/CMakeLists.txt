
list(APPEND SOURCE
    ItemDialog.c
    PrintDlg.c
    SaveDialogDefExt.c
    SaveDialogFolder.c
    SaveDialogLongName.c
    testlist.c)

add_executable(comdlg32_apitest ${SOURCE})
target_link_libraries(comdlg32_apitest wine uuid)
set_module_type(comdlg32_apitest win32cui)
add_importlibs(comdlg32_apitest comdlg32 gdi32 shell32 ole32 user32 advapi32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET comdlg32_apitest)
