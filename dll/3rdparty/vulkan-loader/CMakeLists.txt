# Khronos Vulkan-Loader (vulkan-1.dll), prebuilt by build-loader.sh; see README.md
#
# amd64 only: the only Vulkan ICD the system ships is the amd64 lavapipe (dll/3rdparty/mesa3d). The
# loader imports kernel32 InitOnceExecuteOnce and the msvcrt *_s functions, which the export
# tables of ReactOS only have from 0x600 on, so it is not installed in 5.2 (0x502) images.
if(ARCH STREQUAL "amd64" AND DLL_EXPORT_VERSION GREATER_EQUAL 0x600)
    add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/amd64/vulkan-1.dll DESTINATION reactos/system32 FOR all)
    add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/vulkan-loader-licenses.txt DESTINATION reactos/system32 FOR all)
endif()
