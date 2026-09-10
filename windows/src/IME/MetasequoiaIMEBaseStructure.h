#pragma once

#include "stdafx.h"
#include <vector>
#include "assert.h"
#include <iostream>

using std::endl;

//---------------------------------------------------------------------
// defined keyword
//---------------------------------------------------------------------
template <class VALUE> struct _DEFINED_KEYWORD
{
    LPCWSTR _pwszKeyword;
    VALUE _value;
};

//---------------------------------------------------------------------
// enum
//---------------------------------------------------------------------
enum KEYSTROKE_CATEGORY
{
    CATEGORY_NONE = 0,
    CATEGORY_COMPOSING,
    CATEGORY_CANDIDATE,
    CATEGORY_INVOKE_COMPOSITION_EDIT_SESSION
};

enum KEYSTROKE_FUNCTION
{
    FUNCTION_NONE = 0,
    FUNCTION_INPUT,

    FUNCTION_CANCEL,
    FUNCTION_TOGGLE_IME_MODE, // Toggle IME mode
    FUNCTION_FINALIZE_TEXTSTORE,
    FUNCTION_FINALIZE_TEXTSTORE_AND_INPUT,
    FUNCTION_FINALIZE_CANDIDATELIST,
    FUNCTION_FINALIZE_CANDIDATELISTForVKReturn,
    FUNCTION_FINALIZE_CANDIDATELIST_AND_INPUT,
    FUNCTION_CONVERT,
    FUNCTION_CONVERT_WILDCARD,
    FUNCTION_SELECT_BY_NUMBER,
    FUNCTION_BACKSPACE,
    FUNCTION_DELETE,
    FUNCTION_MOVE_LEFT,
    FUNCTION_MOVE_RIGHT,
    FUNCTION_MOVE_UP,
    FUNCTION_MOVE_DOWN,
    FUNCTION_MOVE_PAGE_UP,
    FUNCTION_MOVE_PAGE_DOWN,
    FUNCTION_MOVE_PAGE_TOP,
    FUNCTION_MOVE_PAGE_BOTTOM,

    // Function Double/Single byte
    FUNCTION_DOUBLE_SINGLE_BYTE,

    // Function Punctuation
    FUNCTION_PUNCTUATION,

    // Raw candidate navigation key; Server decides its configured behavior.
    FUNCTION_SERVER_CANDIDATE_KEY,

    // Unsolicited insert (voice ASR via worker pipe). Append-only so existing
    // ordinals stay stable across builds.
    FUNCTION_INSERT_TEXT,
    FUNCTION_UPDATE_VOICE_COMPOSITION,
    FUNCTION_COMMIT_VOICE_COMPOSITION,
    FUNCTION_CANCEL_VOICE_COMPOSITION,
    FUNCTION_TOGGLE_CHARACTER_SET
};

//---------------------------------------------------------------------
// candidate list
//---------------------------------------------------------------------
enum CANDIDATE_MODE
{
    CANDIDATE_NONE = 0,
    CANDIDATE_ORIGINAL,
    CANDIDATE_INCREMENTAL
};

//---------------------------------------------------------------------
// structure
//---------------------------------------------------------------------
struct _KEYSTROKE_STATE
{
    KEYSTROKE_CATEGORY Category;
    KEYSTROKE_FUNCTION Function;
};

struct _PUNCTUATION
{
    WCHAR _Code;
    WCHAR _Punctuation[3];
};

BOOL CLSIDToString(REFGUID refGUID, _Out_writes_(39) WCHAR *pCLSIDString);

HRESULT SkipWhiteSpace(LCID locale, _In_ LPCWSTR pwszBuffer, DWORD_PTR dwBufLen, _Out_ DWORD_PTR *pdwIndex);
HRESULT FindChar(WCHAR wch, _In_ LPCWSTR pwszBuffer, DWORD_PTR dwBufLen, _Out_ DWORD_PTR *pdwIndex);

BOOL IsSpace(LCID locale, WCHAR wch);

template <class T> class CMetasequoiaImeArray
{
    typedef typename std::vector<T> CMetasequoiaImeInnerArray;
    typedef typename std::vector<T>::iterator CMetasequoiaImeInnerIter;

  public:
    CMetasequoiaImeArray() : _innerVect()
    {
    }

    explicit CMetasequoiaImeArray(size_t count) : _innerVect(count)
    {
    }

    virtual ~CMetasequoiaImeArray()
    {
    }

    inline T *GetAt(size_t index)
    {
        assert(index >= 0);
        assert(index < _innerVect.size());

        T &curT = _innerVect.at(index);

        return &(curT);
    }

    inline const T *GetAt(size_t index) const
    {
        assert(index >= 0);
        assert(index < _innerVect.size());

        T &curT = _innerVect.at(index);

        return &(curT);
    }

    void RemoveAt(size_t index)
    {
        assert(index >= 0);
        assert(index < _innerVect.size());

        CMetasequoiaImeInnerIter iter = _innerVect.begin();
        _innerVect.erase(iter + index);
    }

    UINT Count() const
    {
        return static_cast<UINT>(_innerVect.size());
    }

    T *Append()
    {
        T newT;
        _innerVect.push_back(newT);
        T &backT = _innerVect.back();

        return &(backT);
    }

    void reserve(size_t Count)
    {
        _innerVect.reserve(Count);
    }

    void Clear()
    {
        _innerVect.clear();
    }

  private:
    CMetasequoiaImeInnerArray _innerVect;
};

class CCandidateRange
{
  public:
    CCandidateRange(void);
    ~CCandidateRange(void);

    BOOL IsRange(UINT vKey);
    int GetIndex(UINT vKey);

    inline int Count() const
    {
        return _CandidateListIndexRange.Count();
    }
    inline DWORD *GetAt(int index)
    {
        return _CandidateListIndexRange.GetAt(index);
    }
    inline DWORD *Append()
    {
        return _CandidateListIndexRange.Append();
    }

  private:
    CMetasequoiaImeArray<DWORD> _CandidateListIndexRange;
};

class CStringRange
{
  public:
    CStringRange();
    CStringRange(const CStringRange &sr);
    ~CStringRange();

    const WCHAR *Get() const;
    const DWORD_PTR GetLength() const;
    void Clear();
    void Set(const WCHAR *pwch, DWORD_PTR dwLength);
    void Set(CStringRange &sr);
    CStringRange &operator=(const CStringRange &sr);
    void CharNext(_Inout_ CStringRange *pCharNext);
    static int Compare(LCID locale, _In_ CStringRange *pString1, _In_ CStringRange *pString2);
    static BOOL WildcardCompare(LCID locale, _In_ CStringRange *stringWithWildcard, _In_ CStringRange *targetString);
    std::wstring ToWString() const
    {
        if (!_pStringBuf || _stringBufLen == 0)
            return std::wstring();
        return std::wstring(_pStringBuf, _pStringBuf + _stringBufLen);
    }

  protected:
    DWORD_PTR _stringBufLen;  // Length is in character count.
    const WCHAR *_pStringBuf; // Buffer which is not add zero terminate.
};

//---------------------------------------------------------------------
// CCandidateListItem
//	_ItemString - candidate string
//	_FindKeyCode - tailing string
//---------------------------------------------------------------------
struct CCandidateListItem
{
    CStringRange _ItemString;
    CStringRange _FindKeyCode;

    CCandidateListItem &CCandidateListItem::operator=(const CCandidateListItem &rhs)
    {
        _ItemString = rhs._ItemString;   // e.g. 你好
        _FindKeyCode = rhs._FindKeyCode; // e.g. nihao
        return *this;
    }
};

class CPunctuationPair
{
  public:
    CPunctuationPair();
    CPunctuationPair(WCHAR code, const WCHAR *punctuation, const WCHAR *pair);

    struct _PUNCTUATION _punctuation;
    WCHAR _pairPunctuation[3];
    BOOL _isPairToggle;
};

class CPunctuationNestPair
{
  public:
    CPunctuationNestPair();
    CPunctuationNestPair(              //
        WCHAR codeBegin,               //
        const WCHAR *punctuationBegin, //
        const WCHAR *pairBegin,        //
        const WCHAR codeEnd,           //
        const WCHAR *punctuationEnd,   //
        const WCHAR *pairEnd           //
    );

    struct _PUNCTUATION _punctuation_begin;
    WCHAR _pairPunctuation_begin[3];

    struct _PUNCTUATION _punctuation_end;
    WCHAR _pairPunctuation_end[3];

    int _nestCount;
};
