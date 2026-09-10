#include "Private.h"
#include "Globals.h"
#include "Ipc.h"
#include "MetasequoiaIME.h"
#include "CompositionProcessorEngine.h"

BOOL CMetasequoiaIME::VerifyMetasequoiaIMECLSID(_In_ REFCLSID clsid)
{
    if (IsEqualCLSID(clsid, Global::MetasequoiaIMECLSID))
    {
        return TRUE;
    }
    return FALSE;
}

//+---------------------------------------------------------------------------
//
// ITfActiveLanguageProfileNotifySink::OnActivated
//
// Sink called by the framework when changes activate language profile.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnActivated(_In_ REFCLSID clsid, _In_ REFGUID guidProfile, _In_ BOOL isActivated)
{
    guidProfile;

    if (FALSE == VerifyMetasequoiaIMECLSID(clsid))
    {
        return S_OK;
    }

    if (isActivated)
    {
        // ActivateEx covers the first load; this callback covers later
        // profile switches in an already-running host. Use the same
        // ShellExecute-based wake path so packaged/UWP hosts do not pass
        // their package identity to the Server.
        _WakeServerIfNeeded();
        _AddTextProcessorEngine();
    }
    else
    {
        // Profile deactivation is the earliest authoritative signal that the
        // user selected another TIP. Deactivate() sends the same idempotent
        // event again during teardown, but may run after the Main pipe has
        // already become unavailable.
        FlushNamedpipeImeDeactivation();
    }

    if (nullptr == _pCompositionProcessorEngine)
    {
        return S_OK;
    }

    if (isActivated)
    {
        _pCompositionProcessorEngine->ShowAllLanguageBarIcons();

        _pCompositionProcessorEngine->ConversionModeCompartmentUpdated(_pThreadMgr);
    }
    else
    {
        _DeleteCandidateList(FALSE, nullptr);

        _pCompositionProcessorEngine->HideAllLanguageBarIcons();
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _InitActiveLanguageProfileNotifySink
//
// Advise a active language profile notify sink.
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_InitActiveLanguageProfileNotifySink()
{
    ITfSource *pSource = nullptr;
    BOOL ret = FALSE;

    if (_pThreadMgr->QueryInterface(IID_ITfSource, (void **)&pSource) != S_OK)
    {
        return ret;
    }

    if (pSource->AdviseSink(IID_ITfActiveLanguageProfileNotifySink, (ITfActiveLanguageProfileNotifySink *)this,
                            &_activeLanguageProfileNotifySinkCookie) != S_OK)
    {
        _activeLanguageProfileNotifySinkCookie = TF_INVALID_COOKIE;
        goto Exit;
    }

    ret = TRUE;

Exit:
    pSource->Release();
    return ret;
}

//+---------------------------------------------------------------------------
//
// _UninitActiveLanguageProfileNotifySink
//
// Unadvise a active language profile notify sink.  Assumes we have advised one already.
//----------------------------------------------------------------------------

void CMetasequoiaIME::_UninitActiveLanguageProfileNotifySink()
{
    ITfSource *pSource = nullptr;

    if (_activeLanguageProfileNotifySinkCookie == TF_INVALID_COOKIE)
    {
        return; // never Advised
    }

    if (_pThreadMgr->QueryInterface(IID_ITfSource, (void **)&pSource) == S_OK)
    {
        pSource->UnadviseSink(_activeLanguageProfileNotifySinkCookie);
        pSource->Release();
    }

    _activeLanguageProfileNotifySinkCookie = TF_INVALID_COOKIE;
}
