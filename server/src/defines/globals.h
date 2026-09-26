#pragma once

#include <Windows.h>
#include <unordered_map>

inline float SCALE = 1.0f;
inline int POP_UP_WND_WIDTH = 43;
inline int POP_UP_WND_HEIGHT = 55;
// WebView2 candidate shadow reserve in CSS DIPs. Keep these in sync with the
// two-layer shadows in ui-html/webview2/candwnd/skins. The shadow is biased to
// the lower right, so symmetric padding either clips its soft tail or reserves
// too much transparent hit-test area on the upper-left side.
inline constexpr int CANDIDATE_SHADOW_PAD_LEFT = 16;
inline constexpr int CANDIDATE_SHADOW_PAD_TOP = 14;
inline constexpr int CANDIDATE_SHADOW_PAD_RIGHT = 32;
inline constexpr int CANDIDATE_SHADOW_PAD_BOTTOM = 34;

//
// 候选窗口
//
// Here we make initial window size large enough to make sure the horizontal
// candidate window is not cut off in most situations
//
inline int CANDIDATE_WINDOW_WIDTH = 120;
inline int CANDIDATE_WINDOW_HEIGHT = 232;
// Horizontal (and long vertical) candidates wrap beyond this CSS DIP width.
inline constexpr int CANDIDATE_WINDOW_MAX_WIDTH_DIP = 720;

inline int DEFAULT_WINDOW_WIDTH = CANDIDATE_WINDOW_WIDTH;
inline int DEFAULT_WINDOW_HEIGHT = CANDIDATE_WINDOW_HEIGHT;
inline float DEFAULT_WINDOW_WIDTH_DIP = static_cast<float>(CANDIDATE_WINDOW_WIDTH);
inline float DEFAULT_WINDOW_HEIGHT_DIP = static_cast<float>(CANDIDATE_WINDOW_HEIGHT);

inline const int MAX_HAN_CHARS = 27;
inline const int MAX_LINES = 8;
inline int cand_window_width_array[MAX_HAN_CHARS] = {206};
inline int cand_window_height_array[MAX_LINES] = {417};

inline HWND global_hwnd = NULL;
inline bool is_global_wnd_cand_shown = false;

//
// 托盘区语言栏菜单窗口
// MENU_CONTENT_*_DIP is the measured CSS size; MENU_WINDOW_* is the derived
// physical host size (dip * current scale). Caching DIPs lets live DPI changes
// and each show recompute physical size without a stale pixel cache.
//
inline double MENU_CONTENT_WIDTH_DIP = 240.0;
inline double MENU_CONTENT_HEIGHT_DIP = 232.0;
inline int MENU_WINDOW_WIDTH = 240;
inline int MENU_WINDOW_HEIGHT = 232;

inline HWND global_hwnd_menu = NULL;
inline bool is_global_wnd_menu_shown = false;

//
// settings 窗口
//
inline int SETTINGS_WINDOW_WIDTH = 900;
inline int SETTINGS_WINDOW_HEIGHT = 680;

inline HWND global_hwnd_settings = NULL;
inline bool is_global_wnd_settings_shown = false;

//
// 悬浮工具栏窗口
//
inline int FTB_WND_WIDTH = 207;
// Content bar height in DIPs (matches CSS --ftb-bar-height). Shadow is added on
// the host separately so box-shadow is not clipped.
inline int FTB_WND_HEIGHT = 35;
inline int FTB_WND_SHADOW_WIDTH = 8;
// Last measured .status-bar size in CSS DIPs (0 until first successful measure).
inline double FTB_CONTENT_WIDTH_DIP = 0.0;
inline double FTB_CONTENT_HEIGHT_DIP = 0.0;

inline HWND global_hwnd_ftb = NULL;
inline bool is_global_wnd_ftb_shown = false;
inline HWND global_hwnd_caret_state = NULL;
inline bool is_global_wnd_caret_state_shown = false;

//
// 全屏状态
//
inline std::unordered_map<HWND, bool> g_fullscreen_states;
