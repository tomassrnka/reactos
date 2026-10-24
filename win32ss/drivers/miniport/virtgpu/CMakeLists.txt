list(APPEND SOURCE
    virtgpump.c
    virtgpu_core.c
    virtgpump.h
    virtgpu_vpwrap.h
    virtio_gpu.h)

include_directories(${REACTOS_SOURCE_DIR}/sdk/lib/drivers/virtio)

add_library(virtgpump MODULE ${SOURCE} virtgpump.rc)
set_module_type(virtgpump kernelmodedriver)
add_importlibs(virtgpump videoprt ntoskrnl hal)
target_link_libraries(virtgpump virtio libcntpr)
add_cd_file(TARGET virtgpump DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(virtgpump_reg.inf)
add_driver_inf(virtgpump virtgpump.inf)
