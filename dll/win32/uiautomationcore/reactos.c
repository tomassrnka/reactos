#include "uia_private.h"

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

/*
 * Windows 10 returns S_OK, also when no provider was ever created. The providers of this process that clients
 * are connected to are disconnected.
 */
HRESULT WINAPI UiaDisconnectAllProviders(void)
{
    uia_provider_thread_disconnect_all();
    return S_OK;
}
