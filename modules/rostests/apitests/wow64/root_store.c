/* System root-store lifetime. SPDX-License-Identifier: LGPL-2.1-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>

static unsigned Checks, Failures;
#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; printf("FAIL line %u: %s error=%lu\n", __LINE__, #x, GetLastError()); } } while (0)

int main(void)
{
    unsigned Pass;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (Pass = 0; Pass < 3; ++Pass)
    {
        HCERTSTORE Store;
        PCCERT_CONTEXT Certificate = NULL;
        unsigned Count = 0;
        printf("ROOT STORE opening pass=%u\n", Pass);
        Store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                              CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_READONLY_FLAG,
                              L"Root");
        CHECK(Store != NULL);
        if (!Store) break;
        while ((Certificate = CertEnumCertificatesInStore(Store, Certificate)))
        {
            BYTE Hash[20];
            DWORD Size = sizeof(Hash);
            CHECK(Certificate->pCertInfo && Certificate->cbCertEncoded);
            CHECK(CertGetCertificateContextProperty(Certificate, CERT_SHA1_HASH_PROP_ID,
                                                    Hash, &Size) && Size == sizeof(Hash));
            ++Count;
        }
        CHECK(GetLastError() == CRYPT_E_NOT_FOUND);
        CHECK(Count > 0);
        CHECK(CertCloseStore(Store, CERT_CLOSE_STORE_CHECK_FLAG));
        printf("ROOT STORE pass=%u certificates=%u\n", Pass, Count);
    }
    printf("ROOT STORE checks=%u failures=%u\n", Checks, Failures);
    return Failures != 0;
}
