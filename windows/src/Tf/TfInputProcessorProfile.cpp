#include "Private.h"
#include "TfInputProcessorProfile.h"

CTfInputProcessorProfile::CTfInputProcessorProfile()
{
    _pInputProcessorProfile = nullptr;
}

CTfInputProcessorProfile::~CTfInputProcessorProfile()
{
    if (_pInputProcessorProfile)
    {
        _pInputProcessorProfile->Release();
        _pInputProcessorProfile = nullptr;
    }
}

HRESULT CTfInputProcessorProfile::CreateInstance()
{
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfiles, (void **)&_pInputProcessorProfile);

    return hr;
}

HRESULT CTfInputProcessorProfile::GetCurrentLanguage(_Out_ LANGID *plangid)
{
    if (_pInputProcessorProfile)
    {
        return _pInputProcessorProfile->GetCurrentLanguage(plangid);
    }
    else
    {
        return E_FAIL;
    }
}

HRESULT CTfInputProcessorProfile::GetDefaultLanguageProfile(LANGID langid, REFGUID catid, _Out_ CLSID *pclsid,
                                                            _Out_ GUID *pguidProfile)
{
    if (_pInputProcessorProfile)
    {
        return _pInputProcessorProfile->GetDefaultLanguageProfile(langid, catid, pclsid, pguidProfile);
    }
    else
    {
        return E_FAIL;
    }
}
