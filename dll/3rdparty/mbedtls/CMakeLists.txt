
spec2def(mbedtls.dll mbedtls.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    # crypto
    aes.c
    aesni.c
    asn1parse.c
    asn1write.c
    bignum.c
    bignum_core.c
    bignum_mod.c
    bignum_mod_raw.c
    block_cipher.c
    ccm.c
    chacha20.c
    chachapoly.c
    cipher.c
    cipher_wrap.c
    constant_time.c
    ctr_drbg.c
    dhm.c
    ecdh.c
    ecdsa.c
    ecp.c
    ecp_curves.c
    entropy.c
    entropy_poll.c
    gcm.c
    hkdf.c
    md.c
    md5.c
    oid.c
    pk.c
    pk_ecc.c
    pk_wrap.c
    pkparse.c
    platform.c
    platform_util.c
    poly1305.c
    psa_crypto.c
    psa_crypto_aead.c
    psa_crypto_cipher.c
    psa_crypto_client.c
    psa_crypto_driver_wrappers_no_static.c
    psa_crypto_ecp.c
    psa_crypto_ffdh.c
    psa_crypto_hash.c
    psa_crypto_mac.c
    psa_crypto_pake.c
    psa_crypto_random.c
    psa_crypto_rsa.c
    psa_crypto_se.c
    psa_crypto_slot_management.c
    psa_crypto_storage.c
    psa_util.c
    rsa.c
    rsa_alt_helpers.c
    sha1.c
    sha256.c
    sha512.c
    threading.c
    # x509
    x509.c
    x509_crt.c
    # tls
    mps_reader.c
    mps_trace.c
    ssl_ciphersuites.c
    ssl_client.c
    ssl_debug_helpers_generated.c
    ssl_msg.c
    ssl_tls.c
    ssl_tls12_client.c
    ssl_tls13_client.c
    ssl_tls13_generic.c
    ssl_tls13_keys.c
    # ReactOS
    ros_platform.c)

add_library(mbedtls MODULE
    ${SOURCE}
    mbedtls.rc
    ${CMAKE_CURRENT_BINARY_DIR}/mbedtls.def)
target_include_directories(mbedtls PUBLIC ${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs/mbedtls)

if(NOT MSVC)
    target_compile_options(mbedtls PRIVATE -Wno-pointer-sign -Wno-unused-function)
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(mbedtls PRIVATE -Wno-tautological-constant-compare)
endif()

set_module_type(mbedtls win32dll)
add_importlibs(mbedtls advapi32 msvcrt kernel32 ntdll)

add_cd_file(TARGET mbedtls DESTINATION reactos/system32 FOR all)
