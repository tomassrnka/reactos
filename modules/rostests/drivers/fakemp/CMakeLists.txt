
add_definitions(
    -DNDIS51_MINIPORT
    -DNDIS_MINIPORT_DRIVER
    -DNDIS_LEGACY_MINIPORT=1)

# The same miniport, serialized and deserialized
add_library(fakemps MODULE fakemp.c)
set_module_type(fakemps kernelmodedriver)
add_importlibs(fakemps ndis ntoskrnl hal)
add_rostests_file(TARGET fakemps)

add_library(fakempd MODULE fakemp.c)
target_compile_definitions(fakempd PRIVATE FAKEMP_DESERIALIZED)
set_module_type(fakempd kernelmodedriver)
add_importlibs(fakempd ndis ntoskrnl hal)
add_rostests_file(TARGET fakempd)

# Serialized, with its link up: tcpip configures it (ndis_apitest TcpipUnbind)
add_library(fakempt MODULE fakemp.c)
target_compile_definitions(fakempt PRIVATE FAKEMP_LINK_UP)
set_module_type(fakempt kernelmodedriver)
add_importlibs(fakempt ndis ntoskrnl hal)
add_rostests_file(TARGET fakempt)
