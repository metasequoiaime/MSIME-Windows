#pragma once
#include "resource.h"

#define IME_NAME L"MetasequoiaImeTsf"

#define TEXTSERVICE_MODEL L"Apartment"
#define TEXTSERVICE_LANGID MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)
#define TEXTSERVICE_ICON_INDEX -IDIS_METASEQUOIAIME
#define TEXTSERVICE_DIC L"MetasequoiaIMESimplifiedQuanPin.txt"
#define TEXTSERVICE_DIC_DB L"cutted_flyciku_with_jp.db"
#define FANYLOGFILE_ "fanydebug.log"
#define FANYLOGFILE L"fanydebug_w.log"

#define IME_MODE_ON_ICON_INDEX IDI_IME_MODE_ON
#define IME_MODE_OFF_ICON_INDEX IDI_IME_MODE_OFF
#define IME_MODE_ON_DARK_ICON_INDEX IDI_IME_MODE_ON_DARK
#define IME_MODE_OFF_DARK_ICON_INDEX IDI_IME_MODE_OFF_DARK
#define IME_MODE_ON_JP_ICON_INDEX IDI_IME_MODE_ON_JP
#define IME_MODE_ON_JP_DARK_ICON_INDEX IDI_IME_MODE_ON_JP_DARK
#define IME_MODE_CAP_ICON_INDEX IDI_IME_MODE_CAP
#define IME_MODE_CAP_DARK_ICON_INDEX IDI_IME_MODE_CAP_DARK
#define IME_DOUBLE_ON_INDEX IDI_DOUBLE_SINGLE_BYTE_ON
#define IME_DOUBLE_OFF_INDEX IDI_DOUBLE_SINGLE_BYTE_OFF
#define IME_PUNCTUATION_ON_INDEX IDI_PUNCTUATION_ON
#define IME_PUNCTUATION_OFF_INDEX IDI_PUNCTUATION_OFF

#define METASEQUOIAIME_FONT_DEFAULT L"Microsoft YaHei UI"
#define METASEQUOIAIME_LOCALE_DEFAULT L"zh-CN"

//---------------------------------------------------------------------
// defined max pinyin input length
//---------------------------------------------------------------------
#define MAX_PINYIN_LENGTH (64) // Maximum number of characters allowed in pinyin input

//---------------------------------------------------------------------
// defined Candidated Window
//---------------------------------------------------------------------
#define CANDWND_ROW_WIDTH (30)
#define CANDWND_BORDER_COLOR (RGB(0x00, 0x00, 0x00))
#define CANDWND_BORDER_WIDTH (2)
#define CANDWND_NUM_COLOR (RGB(0xB4, 0xB4, 0xB4))
// #define CANDWND_SELECTED_ITEM_COLOR (RGB(0xFF, 0xFF, 0xFF))
// #define CANDWND_SELECTED_BK_COLOR (RGB(0xA6, 0xA6, 0x00))
#define CANDWND_SELECTED_ITEM_COLOR (RGB(0x00, 0x00, 0x00))
#define CANDWND_SELECTED_BK_COLOR (RGB(0xFF, 0xFF, 0xFF))
#define CANDWND_ITEM_COLOR (RGB(0x00, 0x00, 0x00))

//---------------------------------------------------------------------
// defined Candidated List Contants
//---------------------------------------------------------------------
#define CANDWND_ITEM_CNT_PER_PAGE (8) // Do not exceed 9

//---------------------------------------------------------------------
// defined modifier
//---------------------------------------------------------------------
#define _TF_MOD_ON_KEYUP_SHIFT_ONLY (0x00010000 | TF_MOD_ON_KEYUP)
#define _TF_MOD_ON_KEYUP_CONTROL_ONLY (0x00020000 | TF_MOD_ON_KEYUP)
#define _TF_MOD_ON_KEYUP_ALT_ONLY (0x00040000 | TF_MOD_ON_KEYUP)

#define CAND_WIDTH (13) // * tmMaxCharWidth

//---------------------------------------------------------------------
// string length of CLSID
//---------------------------------------------------------------------
#define CLSID_STRLEN (38) // strlen("{xxxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxx}")

//
// Message define
//
// #define WM_SHOW_MAIN_WINDOW
// #define WM_HIDE_MAIN_WINDOW
// #define WM_MOVE_CANDIDATE_WINDOW
// #define WM_SET_PARENT_HWND