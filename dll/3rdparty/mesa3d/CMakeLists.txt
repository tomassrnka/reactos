# Mesa3D software drivers (prebuilt by build-mesa.sh); see README.md
if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64")
    set(MESA3D_GL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${ARCH})
    set(MESA3D_LICENSES ${CMAKE_CURRENT_SOURCE_DIR}/mesa3d-licenses.txt)

    # amd64: when amd64-llvm/ holds the LLVM build (llvmpipe for OpenGL, lavapipe for Vulkan), it replaces the softpipe-only
    # OpenGL driver and adds the Vulkan ICD. The LLVM DLLs are 66-69 MB each, so they are an option of the image.
    if(ARCH STREQUAL "amd64" AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/amd64-llvm/vulkan_lvp.dll)
        set(MESA3D_GL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/amd64-llvm)
        add_cd_file(FILE ${MESA3D_GL_DIR}/vulkan_lvp.dll DESTINATION reactos/system32 FOR all)
        add_cd_file(FILE ${MESA3D_GL_DIR}/lvp_icd.json DESTINATION reactos/system32 FOR all)
        add_cd_file(FILE ${MESA3D_GL_DIR}/llvm-licenses.txt DESTINATION reactos/system32 FOR all)
        set(MESA3D_LICENSES ${MESA3D_GL_DIR}/mesa3d-licenses.txt)
    endif()

    add_cd_file(FILE ${MESA3D_GL_DIR}/libgallium_wgl.dll DESTINATION reactos/system32 FOR all)
    add_cd_file(FILE ${MESA3D_LICENSES} DESTINATION reactos/system32 FOR all)
endif()
