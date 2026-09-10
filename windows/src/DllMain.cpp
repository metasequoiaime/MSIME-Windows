#include "Private.h"
#include "Globals.h"

//+---------------------------------------------------------------------------
//
// DllMain
//
//----------------------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID pvReserved)
{
    pvReserved;

    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:

        Global::dllInstanceHandle = hInstance;

        if (!InitializeCriticalSectionAndSpinCount(&Global::CS, 0))
        {
            return FALSE;
        }

        break;

    case DLL_PROCESS_DETACH:

        DeleteCriticalSection(&Global::CS);

        break;

    case DLL_THREAD_ATTACH:

        break;

    case DLL_THREAD_DETACH:

        break;
    }

    return TRUE;
}
