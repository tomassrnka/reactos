
list(APPEND SOURCE
    CertEnumSystemStore.c
    CertEnumSystemStoreLocation.c
    CryptExportPublicKeyInfoFromBCryptKeyHandle.c
    CryptTimeStamp.c
    testlist.c)

add_executable(crypt32_apitest ${SOURCE})
target_link_libraries(crypt32_apitest wine ${PSEH_LIB})
set_module_type(crypt32_apitest win32cui)
add_importlibs(crypt32_apitest crypt32 bcrypt msvcrt kernel32 ntdll)
add_rostests_file(TARGET crypt32_apitest)
