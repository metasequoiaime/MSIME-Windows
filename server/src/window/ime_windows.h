#pragma once

#include <windows.h>

#include <string>

inline WCHAR szWindowClass[] = L"metasequoiaime_windows";

/* 候选窗口 */
inline WCHAR lpWindowNameCand[] = L"metaseuqoiaimecandwnd";
/* 菜单窗口 */
inline WCHAR lpWindowNameMenu[] = L"metaseuqoiaimemenuwnd";
/* settings 窗口 */
inline WCHAR lpWindowNameSettings[] = L"Settings";
/* floating toolbar 窗口 */
inline WCHAR lpWindowNameFtb[] = L"metaseuqoiaimeftbwnd";

LRESULT RegisterCandidateWindowMessage();
LRESULT RegisterIMEWindowsClass(WNDCLASSEX &, HINSTANCE);
int CreateCandidateWindow(HINSTANCE);
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProcCandWindow(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProcMenuWindow(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProcSettingsWindow(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProcFtbWindow(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
bool ActivateSettingsWindow(HWND hwnd);
void RequestSettingsWindowActivation(HWND hwnd);
void SetCandidateHostCloaked(bool cloaked);
// reason identifies the trigger in the floating-toolbar diagnostic trace. The
// whole class of bugs here is "nothing ever called this", so the caller has to
// be recoverable from the log.
void ApplyConfiguredFloatingToolbarVisibility(const wchar_t *reason = L"unspecified");
// End the post-navigation paint grace (page-ready or fallback timeout) and apply
// the real show/hide decision. Until this runs, the toolbar stays shown.
void ReconcileFloatingToolbarVisibilityAfterReady(const wchar_t *reason = L"ftb-ready");
// Use instead of ShowWindow(SW_HIDE) on the toolbar host: hiding it before its
// WebView2 has painted once permanently breaks the toolbar's rendering.
void HideFloatingToolbarHost();
// One-line snapshot of the tray menu host and its controller for the diagnostic
// trace. A menu that is invisible yet still routes clicks to the right item
// looks identical from the outside whether it was never uncloaked or its
// WebView2 stopped compositing, and only these fields tell the two apart.
std::wstring DescribeTrayMenuHostState();
void ApplyConfiguredFloatingToolbarSize();
void ApplyConfiguredInputScheme();
void ApplyConfiguredShuangpinSchema();
// Tells every TSF client whether a bare lowercase v now starts number mode.
void BroadcastLowercaseNumberModeToTsf();
