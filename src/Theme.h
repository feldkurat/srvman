// Theme.h : dark/light/system theme support
//
// One theme per process, so this is a free-function namespace rather than a class:
// CMainDlg, CPropertiesDlg and CAboutDlg all need access and none of them owns it.
//
// In light mode every entry point here is inert - HandleCtlColor() returns false and
// ApplyToControl() reverts controls to their default theme. That is what keeps the
// Windows 7 / pre-1809 path identical to the untouched application by construction
// rather than by testing.
//
#pragma once

namespace Theme
{
	//! What the user asked for. Persisted as a REG_DWORD; do not renumber.
	enum Preference
	{
		prefLight  = 0,
		prefDark   = 1,
		prefSystem = 2,
	};

	//! What we actually render, once the preference and the OS have been reconciled.
	enum Mode
	{
		modeLight = 0,
		modeDark  = 1,
	};

	//! Must run before the first window is created: the uxtheme app-mode switch only
	//! affects common controls instantiated after it.
	void Init();
	void Shutdown();

	//! False on Win7/8 and pre-1809 Win10, and whenever High Contrast is active.
	bool IsDarkModeSupported();

	Preference GetPreference();
	//! Persists the preference and recomputes the effective mode.
	//! \returns true if the effective mode changed and the caller should re-apply.
	bool SetPreference(Preference pref);

	Mode GetEffectiveMode();
	inline bool IsDark() { return GetEffectiveMode() == modeDark; }

	//! Re-reads the OS app mode. Call on WM_SETTINGCHANGE/"ImmersiveColorSet".
	//! \returns true if the effective mode changed.
	bool RefreshFromSystem();

	COLORREF Background();			//!< dialog surface
	COLORREF ControlBackground();	//!< edit/list field
	COLORREF Text();
	COLORREF DisabledText();
	COLORREF HotBackground();		//!< menu bar item under the cursor or opened

	//! Cached for the lifetime of the process. Callers must NOT delete these.
	HBRUSH BackgroundBrush();
	HBRUSH ControlBackgroundBrush();

	//! Title bar + a walk over every child control. Safe to call repeatedly.
	void ApplyToWindow(HWND hwndTop);
	void ApplyToControl(HWND hwndCtl);
	void ApplyToListView(HWND hwndList);
	//! Forces the frame, the client area and the menu bar to redraw after a switch.
	void Repaint(HWND hwndTop);

	//! Handles WM_CTLCOLORDLG/STATIC/EDIT/LISTBOX/BTN.
	//! \returns false when the caller should fall through to DefDlgProc - which is
	//!          always the case in light mode.
	bool HandleCtlColor(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT &lResult);

	//! Paints the menu bar strip, which SetPreferredAppMode() does not reach: USER32
	//! draws it from COLOR_MENU in the non-client area. Handles the undocumented
	//! WM_UAHDRAWMENU / WM_UAHDRAWMENUITEM pair; see THEME_MENUBAR_HANDLERS().
	//! \returns false when the caller should let USER32 draw the bar itself.
	bool HandleMenuBarMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT &lResult);

	//! Paints over the 1px light separator USER32 leaves below the menu bar. Must run
	//! after default WM_NCPAINT / WM_NCACTIVATE processing, which is what draws it.
	void DrawMenuBarBottomLine(HWND hWnd);
}

//! Drop this in as the first line of a BEGIN_MSG_MAP block. It relies on the
//! parameter names ATL gives ProcessWindowMessage(). WM_CTLCOLORMSGBOX (0x0132)
//! through WM_CTLCOLORSTATIC (0x0138) is a contiguous range, so the guard costs
//! one compare pair on messages we do not care about.
#define THEME_CTLCOLOR_HANDLERS()										\
	if (uMsg >= WM_CTLCOLORMSGBOX && uMsg <= WM_CTLCOLORSTATIC)			\
	{																	\
		if (Theme::HandleCtlColor(uMsg, wParam, lParam, lResult))		\
			return TRUE;												\
	}

//! Companion to the above, for the one dialog that carries a menu bar. The guarded
//! range is the undocumented UAH block: WM_UAHDRAWMENU (0x0091) through
//! WM_UAHMEASUREMENUITEM (0x0094). Note that a window also needs the WM_NCPAINT and
//! WM_NCACTIVATE handlers described at Theme::DrawMenuBarBottomLine().
#define THEME_MENUBAR_HANDLERS()											\
	if (uMsg >= 0x0091 && uMsg <= 0x0094)									\
	{																		\
		if (Theme::HandleMenuBarMessage(hWnd, uMsg, wParam, lParam, lResult))	\
			return TRUE;													\
	}
