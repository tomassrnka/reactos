
remove_definitions(-D_WIN32_WINNT=0x502 -DWINVER=0x502)

add_definitions(
    -D__WINESRC__
    -D_WINE
    -D_KERNELBASE_)

include_directories(BEFORE ${REACTOS_SOURCE_DIR}/sdk/include/wine)

add_subdirectory(wine)

spec2def(kernelbase_ros.dll kernelbase_ros.spec ADD_IMPORTLIB)

add_library(kernelbase_ros MODULE
    DllMain.c
    synch.c
    time.c
    ${CMAKE_CURRENT_BINARY_DIR}/kernelbase_ros_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/kernelbase_ros.def)

set_module_type(kernelbase_ros win32dll ENTRYPOINT DllMain 12)
target_link_libraries(kernelbase_ros psapi_static kernelbase_static wine chkstk)
add_importlibs(kernelbase_ros msvcrt advapi32 advapi32_vista kernel32 kernel32_vista ntdll ntdll_vista)
add_dependencies(kernelbase_ros psdk errcodes)
add_cd_file(TARGET kernelbase_ros DESTINATION reactos/system32 FOR all)

# Fake import library, so consumers can link against it without needing to know about the actual implementation in kernelbase_ros.dll
add_library(libkernelbase INTERFACE)
target_link_libraries(libkernelbase INTERFACE libkernelbase_ros)

# Programs ask for kernelbase.dll by that name (the MSYS2 and Cygwin runtimes load it for the interrupt time functions).
# The module only forwards every export of kernelbase_ros.dll, so that one copy of the code is loaded.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kernelbase_ros.spec)
file(STRINGS kernelbase_ros.spec _kernelbase_spec REGEX "^@ stdcall ")
math(EXPR _kernelbase_version "${DLL_EXPORT_VERSION}")
set(_kernelbase_def "; File generated automatically, do not edit!\n\nNAME kernelbase.dll\n\nEXPORTS\n")
foreach(_line IN LISTS _kernelbase_spec)
    set(_min 0)
    if(_line MATCHES "^@ stdcall -version=(0x[0-9a-fA-F]+)\\+")
        set(_min ${CMAKE_MATCH_1})
    endif()
    math(EXPR _min "${_min}")
    if(_kernelbase_version LESS _min)
        continue()
    endif()
    if(_line MATCHES "^@ stdcall (-[^ ]+ )?([A-Za-z0-9_]+)\\(")
        string(APPEND _kernelbase_def " ${CMAKE_MATCH_2}=kernelbase_ros.${CMAKE_MATCH_2}\n")
    endif()
endforeach()
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/kernelbase.def CONTENT "${_kernelbase_def}")

add_library(kernelbase MODULE
    kernelbase.rc
    ${CMAKE_CURRENT_BINARY_DIR}/kernelbase.def)

set_module_type(kernelbase win32dll ENTRYPOINT 0)
add_dependencies(kernelbase psdk kernelbase_ros)
add_cd_file(TARGET kernelbase DESTINATION reactos/system32 FOR all)
