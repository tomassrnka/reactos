
remove_definitions(-D_WIN32_WINNT=0x502)

# virtio-win guest drivers (sdk/lib/3rdparty/virtio-win)
set(VIRTIO_WIN_DIR ${REACTOS_SOURCE_DIR}/sdk/lib/3rdparty/virtio-win)

list(APPEND VIRTIO_WIN_SOURCE
    ${VIRTIO_WIN_DIR}/VirtIO/VirtIOPCICommon.c
    ${VIRTIO_WIN_DIR}/VirtIO/VirtIOPCILegacy.c
    ${VIRTIO_WIN_DIR}/VirtIO/VirtIOPCIModern.c
    ${VIRTIO_WIN_DIR}/VirtIO/VirtIORing.c
    ${VIRTIO_WIN_DIR}/VirtIO/VirtIORing-Packed.c
    ${VIRTIO_WIN_DIR}/VirtIO/WDF/Callbacks.c
    ${VIRTIO_WIN_DIR}/VirtIO/WDF/Dma.c
    ${VIRTIO_WIN_DIR}/VirtIO/WDF/MemPortIO.c
    ${VIRTIO_WIN_DIR}/VirtIO/WDF/PCI.c
    ${VIRTIO_WIN_DIR}/VirtIO/WDF/VirtIOWdf.c)

# The VirtIO library with its KMDF layer, for KMDF drivers
add_library(virtio_wdf STATIC ${VIRTIO_WIN_SOURCE})
add_dependencies(virtio_wdf bugcodes xdk)
target_include_directories(virtio_wdf PUBLIC
    ${VIRTIO_WIN_DIR}/VirtIO
    ${VIRTIO_WIN_DIR}/VirtIO/WDF
    ${REACTOS_SOURCE_DIR}/sdk/include/wdf/kmdf/1.17)
target_compile_definitions(virtio_wdf PUBLIC
    WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000)

if(NOT MSVC)
    target_compile_options(virtio_wdf PRIVATE
        -Wno-unused-function
        -Wno-unknown-pragmas
        -Wno-attributes
        -Wno-old-style-declaration
        # vp_notify returns bool where the ring expects void (same convention), and
        # WdfObjectAllocateContext gets a typed pointer: warnings, not errors
        -Wno-error=incompatible-pointer-types
        -Wno-unused-variable
        -Wno-discarded-qualifiers
        -Wno-parentheses)
endif()

add_subdirectory(balloon)
