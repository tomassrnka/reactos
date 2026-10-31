
# Built-in performance counter sets and their providers, shared by pdh.dll and by
# advapi32.dll (HKEY_PERFORMANCE_DATA). It only uses the process heap and ntdll,
# so that advapi32 does not depend on the C runtime DLL.
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x600)

add_library(perfdata STATIC perfdata.c)
add_dependencies(perfdata psdk)
target_include_directories(perfdata INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
