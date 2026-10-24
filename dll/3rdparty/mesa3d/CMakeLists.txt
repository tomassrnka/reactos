# Mesa3D software OpenGL driver (WGL ICD), prebuilt by build-mesa.sh; see README.md
if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64")
    add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/${ARCH}/libgallium_wgl.dll DESTINATION reactos/system32 FOR all)
    add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/mesa3d-licenses.txt DESTINATION reactos/system32 FOR all)
endif()
