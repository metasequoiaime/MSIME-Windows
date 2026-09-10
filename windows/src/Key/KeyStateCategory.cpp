#include "KeyStateCategory.h"
#include "Globals.h"
#include "Ipc.h"
#include "MetasequoiaIMEBaseStructure.h"
#include <debugapi.h>

thread_local CKeyStateCategoryFactory *CKeyStateCategoryFactory::_instance = nullptr;

CKeyStateCategoryFactory::CKeyStateCategoryFactory()
{
    _instance = nullptr;
}

CKeyStateCategoryFactory *CKeyStateCategoryFactory::Instance()
{
    if (nullptr == _instance)
    {
        _instance = new (std::nothrow) CKeyStateCategoryFactory();
    }

    return _instance;
}

CKeyStateCategory *CKeyStateCategoryFactory::MakeKeyStateCategory(KEYSTROKE_CATEGORY keyCategory,
                                                                  _In_ CMetasequoiaIME *pTextService)
{
    CKeyStateCategory *pKeyState = nullptr;

    switch (keyCategory)
    {
    case CATEGORY_NONE:
        pKeyState = new (std::nothrow) CKeyStateNull(pTextService);
        break;

    case CATEGORY_COMPOSING:
        pKeyState = new (std::nothrow) CKeyStateComposing(pTextService);
        break;

    case CATEGORY_CANDIDATE:
        pKeyState = new (std::nothrow) CKeyStateCandidate(pTextService);
        break;

    default:
        pKeyState = new (std::nothrow) CKeyStateNull(pTextService);
        break;
    }
    return pKeyState;
}

void CKeyStateCategoryFactory::Release()
{
    if (_instance)
    {
        delete _instance;
        _instance = nullptr;
    }
}

/*
class CKeyStateCategory
*/
CKeyStateCategory::CKeyStateCategory(_In_ CMetasequoiaIME *pTextService)
{
    _pTextService = pTextService;
}

CKeyStateCategory::~CKeyStateCategory(void)
{
}

HRESULT CKeyStateCategory::KeyStateHandler(KEYSTROKE_FUNCTION function, KeyHandlerEditSessionDTO dto)
{
    switch (function)
    {
    case FUNCTION_INPUT:
        return HandleKeyInput(dto);

    case FUNCTION_FINALIZE_TEXTSTORE_AND_INPUT:
        return HandleKeyFinalizeTextStoreAndInput(dto);

    case FUNCTION_FINALIZE_TEXTSTORE:
        return HandleKeyFinalizeTextStore(dto);

    case FUNCTION_FINALIZE_CANDIDATELIST_AND_INPUT:
        return HandleKeyFinalizeCandidatelistAndInput(dto);

    case FUNCTION_FINALIZE_CANDIDATELIST:
        return HandleKeyFinalizeCandidatelist(dto);
    case FUNCTION_FINALIZE_CANDIDATELISTForVKReturn:
        return HandleKeyFinalizeCandidatelistForVKReturn(dto);

    case FUNCTION_INSERT_TEXT:
        return _pTextService->_HandleInsertText(dto.ec, dto.pContext, dto.prefetchedText);

    case FUNCTION_UPDATE_VOICE_COMPOSITION:
        return _pTextService->_HandleUpdateVoiceComposition(dto.ec, dto.pContext, dto.prefetchedText);

    case FUNCTION_COMMIT_VOICE_COMPOSITION:
        return _pTextService->_HandleCommitVoiceComposition(dto.ec, dto.pContext, dto.prefetchedText);

    case FUNCTION_CANCEL_VOICE_COMPOSITION:
        return _pTextService->_HandleCancelVoiceComposition(dto.ec, dto.pContext);

    case FUNCTION_CONVERT: {
        return HandleKeyConvert(dto);
    }

    case FUNCTION_CONVERT_WILDCARD:
        return HandleKeyConvertWildCard(dto);

    case FUNCTION_CANCEL:
        return HandleKeyCancel(dto);

    case FUNCTION_TOGGLE_IME_MODE:
        return HandleKeyToogleIMEMode(dto);

    case FUNCTION_BACKSPACE:
        return HandleKeyBackspace(dto);

    case FUNCTION_DELETE:
        return HandleKeyDelete(dto);

    case FUNCTION_MOVE_LEFT:
    case FUNCTION_MOVE_RIGHT:
        return HandleKeyArrow(dto);

    case FUNCTION_MOVE_UP:
    case FUNCTION_MOVE_DOWN:
    case FUNCTION_MOVE_PAGE_UP:
    case FUNCTION_MOVE_PAGE_DOWN:
    case FUNCTION_MOVE_PAGE_TOP:
    case FUNCTION_MOVE_PAGE_BOTTOM:
        return HandleKeyArrow(dto);

    case FUNCTION_DOUBLE_SINGLE_BYTE:
        return HandleKeyDoubleSingleByte(dto);

    case FUNCTION_PUNCTUATION:
        return HandleKeyPunctuation(dto);

    case FUNCTION_SELECT_BY_NUMBER:
        return HandleKeySelectByNumber(dto);

    case FUNCTION_NONE:
        break;
    }
    return E_INVALIDARG;
}

void CKeyStateCategory::Release()
{
    delete this;
}

// _HandleCompositionInput
HRESULT CKeyStateCategory::HandleKeyInput(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

// HandleKeyFinalizeTextStore
HRESULT CKeyStateCategory::HandleKeyFinalizeTextStore(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}
// HandleKeyCompositionFinalizeTextStoreAndInput
HRESULT CKeyStateCategory::HandleKeyFinalizeTextStoreAndInput(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

// HandleKeyCompositionFinalizeCandidatelistAndInput
HRESULT CKeyStateCategory::HandleKeyFinalizeCandidatelistAndInput(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

// HandleKeyCompositionFinalizeCandidatelist
HRESULT CKeyStateCategory::HandleKeyFinalizeCandidatelist(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

// HandleKeyCompositionFinalizeCandidatelistForVKReturn
HRESULT CKeyStateCategory::HandleKeyFinalizeCandidatelistForVKReturn(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

// HandleKeyConvert
HRESULT CKeyStateCategory::HandleKeyConvert(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

// HandleKeyConvertWildCard
HRESULT CKeyStateCategory::HandleKeyConvertWildCard(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

//_HandleCancel
HRESULT CKeyStateCategory::HandleKeyCancel(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

HRESULT CKeyStateCategory::HandleKeyToogleIMEMode(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

//_HandleCompositionBackspace
HRESULT CKeyStateCategory::HandleKeyBackspace(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

//_HandleCompositionDelete
HRESULT CKeyStateCategory::HandleKeyDelete(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

//_HandleCompositionArrowKey
HRESULT CKeyStateCategory::HandleKeyArrow(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

//_HandleCompositionDoubleSingleByte
HRESULT CKeyStateCategory::HandleKeyDoubleSingleByte(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

//_HandleCompositionPunctuation
HRESULT CKeyStateCategory::HandleKeyPunctuation(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

HRESULT CKeyStateCategory::HandleKeySelectByNumber(KeyHandlerEditSessionDTO dto)
{
    dto;
    return E_NOTIMPL;
}

/*
class CKeyStateComposing
*/
CKeyStateComposing::CKeyStateComposing(_In_ CMetasequoiaIME *pTextService) : CKeyStateCategory(pTextService)
{
}

HRESULT CKeyStateComposing::HandleKeyInput(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCompositionInput(dto.ec, dto.pContext, dto.wch, dto.requestId);
}

HRESULT CKeyStateComposing::HandleKeyFinalizeTextStoreAndInput(KeyHandlerEditSessionDTO dto)
{
    _pTextService->_HandleCompositionFinalize(dto.ec, dto.pContext, FALSE);
    return _pTextService->_HandleCompositionInput(dto.ec, dto.pContext, dto.wch, dto.requestId);
}

HRESULT CKeyStateComposing::HandleKeyFinalizeTextStore(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCompositionFinalize(dto.ec, dto.pContext, FALSE);
}

HRESULT CKeyStateComposing::HandleKeyFinalizeCandidatelistAndInput(KeyHandlerEditSessionDTO dto)
{
    _pTextService->_HandleCompositionFinalize(dto.ec, dto.pContext, TRUE);
    return _pTextService->_HandleCompositionInput(dto.ec, dto.pContext, dto.wch, dto.requestId);
}

HRESULT CKeyStateComposing::HandleKeyFinalizeCandidatelist(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCompositionFinalize(dto.ec, dto.pContext, TRUE);
}

HRESULT CKeyStateComposing::HandleKeyConvert(KeyHandlerEditSessionDTO dto)
{
    if (dto.code == VK_SPACE)
    {
        return _pTextService->_HandleCandidateFinalize(dto.ec, dto.pContext, dto.requestId, dto.prefetchedText);
    }
    // VK_SPACE
    return _pTextService->_HandleCompositionConvert(dto.ec, dto.pContext, FALSE);
}

HRESULT CKeyStateComposing::HandleKeyConvertWildCard(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCompositionConvert(dto.ec, dto.pContext, TRUE);
}

HRESULT CKeyStateComposing::HandleKeyCancel(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCancel(dto.ec, dto.pContext);
}

HRESULT CKeyStateComposing::HandleKeyToogleIMEMode(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleToogleIMEMode(dto.ec, dto.pContext);
}

HRESULT CKeyStateComposing::HandleKeyBackspace(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCompositionBackspace(dto.ec, dto.pContext, dto.requestId);
}

HRESULT CKeyStateComposing::HandleKeyDelete(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCompositionDelete(dto.ec, dto.pContext, dto.requestId);
}

HRESULT CKeyStateComposing::HandleKeyArrow(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCompositionArrowKey(dto.ec, dto.pContext, dto.arrowKey, dto.requestId);
}

HRESULT CKeyStateComposing::HandleKeyDoubleSingleByte(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCompositionDoubleSingleByte(dto.ec, dto.pContext, dto.wch);
}

HRESULT CKeyStateComposing::HandleKeyPunctuation(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCompositionPunctuation(dto.ec, dto.pContext, dto.code, dto.wch, dto.requestId,
                                                        dto.prefetchedText);
}

/*
class CKeyStateCandidate
*/
CKeyStateCandidate::CKeyStateCandidate(_In_ CMetasequoiaIME *pTextService) : CKeyStateCategory(pTextService)
{
}

// _HandleCandidateInput
HRESULT CKeyStateCandidate::HandleKeyFinalizeCandidatelist(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCandidateFinalize(dto.ec, dto.pContext, dto.requestId, dto.prefetchedText);
}

// _HandleCandidateInput
HRESULT CKeyStateCandidate::HandleKeyFinalizeCandidatelistForVKReturn(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCandidateFinalizeForVKReturn(dto.ec, dto.pContext);
}

// HandleKeyFinalizeCandidatelistAndInput
HRESULT CKeyStateCandidate::HandleKeyFinalizeCandidatelistAndInput(KeyHandlerEditSessionDTO dto)
{
    _pTextService->_HandleCandidateFinalize(dto.ec, dto.pContext, dto.requestId, dto.prefetchedText);
    return _pTextService->_HandleCompositionInput(dto.ec, dto.pContext, dto.wch, dto.requestId);
}

//_HandleCandidateConvert
HRESULT CKeyStateCandidate::HandleKeyConvert(KeyHandlerEditSessionDTO dto)
{
    if (dto.code == VK_SPACE)
    {
        return _pTextService->_HandleCandidateFinalize(dto.ec, dto.pContext, dto.requestId, dto.prefetchedText);
    }
    // Send candidate string to client when pressing VK_SPACE
    return _pTextService->_HandleCandidateConvert(dto.ec, dto.pContext, dto.requestId, dto.prefetchedText);
}

//_HandleCancel
HRESULT CKeyStateCandidate::HandleKeyCancel(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCancel(dto.ec, dto.pContext);
}

//_HandleCandidateArrowKey
HRESULT CKeyStateCandidate::HandleKeyArrow(KeyHandlerEditSessionDTO dto)
{
    return _pTextService->_HandleCandidateArrowKey(dto.ec, dto.pContext, dto.arrowKey, dto.requestId);
}

//_HandleCandidateSelectByNumber
HRESULT CKeyStateCandidate::HandleKeySelectByNumber(KeyHandlerEditSessionDTO dto)
{
    // return _pTextService->_HandleCandidateSelectByNumber(dto.ec, dto.pContext, dto.code);
    return _pTextService->_HandleCandidateFinalize(dto.ec, dto.pContext, dto.requestId, dto.prefetchedText);
}
