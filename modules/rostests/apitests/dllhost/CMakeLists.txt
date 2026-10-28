
# The test server is a COM class registered by the test under an AppID with an empty
# DllSurrogate value, so that COM activates it in a separate dllhost.exe.
spec2def(dllhost_server.dll dllhost_server.spec)
add_library(dllhost_server MODULE dllhost_server.c ${CMAKE_CURRENT_BINARY_DIR}/dllhost_server.def)
set_module_type(dllhost_server win32dll)
target_link_libraries(dllhost_server uuid)
add_importlibs(dllhost_server ole32 msvcrt kernel32)
add_dependencies(dllhost_server psdk)
add_rostests_file(TARGET dllhost_server)

list(APPEND SOURCE
    Surrogate.c
    testlist.c)

add_executable(dllhost_apitest ${SOURCE})
set_module_type(dllhost_apitest win32cui)
target_link_libraries(dllhost_apitest uuid)
add_importlibs(dllhost_apitest advapi32 user32 ole32 msvcrt kernel32)
add_dependencies(dllhost_apitest dllhost_server psdk)
add_rostests_file(TARGET dllhost_apitest)
