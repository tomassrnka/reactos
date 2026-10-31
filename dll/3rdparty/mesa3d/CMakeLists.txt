# Mesa3D software drivers (prebuilt by build-mesa.sh); see README.md
if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64")
    set(MESA3D_GL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${ARCH})
    set(MESA3D_LICENSES ${CMAKE_CURRENT_SOURCE_DIR}/mesa3d-licenses.txt)

    # amd64 ships llvmpipe (OpenGL) + lavapipe (Vulkan) as the DEFAULT GPU of the base image. The
    # ~116 MB pack is not in git, so it is fetched from the S3 cold tier at configure time by
    # stage-amd64-llvm.sh. This is ON by default; -DMESA3D_AMD64_LLVM=OFF, or a build host without
    # rclone / network / the artifact, falls back to the in-tree softpipe-only OpenGL driver below
    # (no Vulkan ICD) so the build never breaks. i386 uses softpipe, except SysWOW64 (below).
    option(MESA3D_AMD64_LLVM "amd64: default to llvmpipe/lavapipe (fetch the LLVM Mesa pack from S3 at configure)" ON)
    if(ARCH STREQUAL "amd64" AND MESA3D_AMD64_LLVM AND NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/amd64-llvm/vulkan_lvp.dll)
        message(STATUS "Mesa3D: staging the llvmpipe/lavapipe pack (amd64 default GPU) ...")
        execute_process(
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/stage-amd64-llvm.sh
            RESULT_VARIABLE _mesa3d_stage_rc
            OUTPUT_VARIABLE _mesa3d_stage_out
            ERROR_VARIABLE _mesa3d_stage_out)
        if(NOT _mesa3d_stage_rc EQUAL 0)
            message(WARNING "Mesa3D: could not stage the amd64 llvmpipe/lavapipe pack (rc=${_mesa3d_stage_rc}); "
                            "keeping the in-tree softpipe OpenGL default. Output:\n${_mesa3d_stage_out}")
        else()
            message(STATUS "Mesa3D: ${_mesa3d_stage_out}")
        endif()
    endif()

    # amd64: when amd64-llvm/ holds the LLVM build (llvmpipe for OpenGL, lavapipe for Vulkan), it replaces the softpipe-only
    # OpenGL driver and adds the Vulkan ICD. The LLVM DLLs are 66-69 MB each, so they are an option of the image.
    if(ARCH STREQUAL "amd64" AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/amd64-llvm/vulkan_lvp.dll)
        set(MESA3D_GL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/amd64-llvm)
        add_cd_file(FILE ${MESA3D_GL_DIR}/vulkan_lvp.dll DESTINATION reactos/system32 FOR all)
        add_cd_file(FILE ${MESA3D_GL_DIR}/lvp_icd.json DESTINATION reactos/system32 FOR all)
        add_cd_file(FILE ${MESA3D_GL_DIR}/llvm-licenses.txt DESTINATION reactos/system32 FOR all)
        set(MESA3D_LICENSES ${MESA3D_GL_DIR}/mesa3d-licenses.txt)
    endif()

    # The i386 part of SysWOW64 (the nested i386 build of an amd64 image) ships llvmpipe as well, so that 32-bit
    # programs get OpenGL 4.6 with compute shaders like the 64-bit side (wgpu needs them; softpipe stops at 3.3).
    # The 73 MB DLL is not in git: stage-i386-llvm.sh fetches it at configure time like the amd64 pack, and without
    # it (-DMESA3D_WOW64_LLVM=OFF, no rclone/network/artifact) SysWOW64 keeps the in-tree softpipe driver. A native
    # i386 image keeps softpipe.
    option(MESA3D_WOW64_LLVM "SysWOW64: ship the i386 llvmpipe OpenGL driver (fetched from S3 at configure)" ON)
    if(ARCH STREQUAL "i386" AND SYSWOW64_GUEST AND MESA3D_WOW64_LLVM AND NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/i386-llvm/libgallium_wgl.dll)
        message(STATUS "Mesa3D: staging the i386 llvmpipe driver (SysWOW64) ...")
        execute_process(
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/stage-i386-llvm.sh
            RESULT_VARIABLE _mesa3d_stage_rc
            OUTPUT_VARIABLE _mesa3d_stage_out
            ERROR_VARIABLE _mesa3d_stage_out)
        if(NOT _mesa3d_stage_rc EQUAL 0)
            message(WARNING "Mesa3D: could not stage the i386 llvmpipe driver (rc=${_mesa3d_stage_rc}); "
                            "SysWOW64 keeps the in-tree softpipe OpenGL driver. Output:\n${_mesa3d_stage_out}")
        else()
            message(STATUS "Mesa3D: ${_mesa3d_stage_out}")
        endif()
    endif()
    if(ARCH STREQUAL "i386" AND SYSWOW64_GUEST AND MESA3D_WOW64_LLVM AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/i386-llvm/libgallium_wgl.dll)
        set(MESA3D_GL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/i386-llvm)
        set(MESA3D_LICENSES ${MESA3D_GL_DIR}/mesa3d-licenses.txt)
    endif()

    add_cd_file(FILE ${MESA3D_GL_DIR}/libgallium_wgl.dll DESTINATION reactos/system32 FOR all)
    add_cd_file(FILE ${MESA3D_LICENSES} DESTINATION reactos/system32 FOR all)
endif()
