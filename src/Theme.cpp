// Theme.cpp : dark/light/system theme support
//
#include "stdafx.h"
#include "Theme.h"

#include <uxtheme.h>
#include <dwmapi.h>

#include <bzscore/Win32/registry.h>

// Present in recent SDKs, but spell it out so the Windows 7 build target does not
// depend on which SDK happens to be installed.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#define DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 19

namespace
{
	//! Where srvman keeps its settings. Same root as the column widths and the icon
	//! display mode, per the project's existing convention.
	const TCHAR tszParamsKey[]			= _T("SOFTWARE\\SysProgs\\SrvMan");
	const TCHAR tszThemeValue[]			= _T("Theme");

	//! Windows' own "choose your default app mode" setting.
	const TCHAR tszPersonalizeKey[]		= _T("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize");
	const TCHAR tszAppsUseLightTheme[]	= _T("AppsUseLightTheme");

	enum
	{
		//! 1809 - the first build with an app-level dark mode we can opt into.
		kBuildDarkModeIntroduced	= 17763,
		//! 1903 - ordinal 135 becomes SetPreferredAppMode(); FlushMenuThemes() starts working.
		kBuildPreferredAppMode		= 18362,
		//! DWMWA_USE_IMMERSIVE_DARK_MODE moved from 19 to 20 in this build.
		kBuildDarkCaptionAttr		= 18985,
	};

	//! Argument to uxtheme ordinal 135 on 1903 and later.
	enum PreferredAppMode
	{
		pamDefault		= 0,
		pamAllowDark	= 1,
		pamForceDark	= 2,
		pamForceLight	= 3,
	};

	// The UAH (User Anti-Hooking) menu messages. USER32 sends these to a window whose
	// menu bar it is about to draw, giving the application a chance to draw it instead.
	// None of them is documented; none of them has a header. They are only ever received
	// on Windows 10 1809 and later, which is also the only place we act on them.
	const UINT kWmUahDrawMenu		= 0x0091;
	const UINT kWmUahDrawMenuItem	= 0x0092;

	struct UAHMENU
	{
		HMENU hmenu;
		HDC hdc;
		DWORD dwFlags;
	};

	union UAHMENUITEMMETRICS
	{
		struct { DWORD cx; DWORD cy; } rgsizeBar[2];
		struct { DWORD cx; DWORD cy; } rgsizePopup[4];
	};

	struct UAHMENUPOPUPMETRICS
	{
		DWORD rgcx[4];
		DWORD fUpdateMaxWidths : 2;
	};

	struct UAHMENUITEM
	{
		int iPosition;
		UAHMENUITEMMETRICS umim;
		UAHMENUPOPUPMETRICS umpm;
	};

	struct UAHDRAWMENUITEM
	{
		DRAWITEMSTRUCT dis;
		UAHMENU um;
		UAHMENUITEM umi;
	};

	typedef void (WINAPI *PFN_RtlGetNtVersionNumbers)(DWORD *, DWORD *, DWORD *);

	// One signature covers both spellings of ordinal 135: AllowDarkModeForApp(BOOL) on
	// 17763 and SetPreferredAppMode(PreferredAppMode) on 18362+. Both are __stdcall
	// with a single int-sized argument.
	typedef int  (WINAPI *PFN_SetPreferredAppMode)(int);
	typedef void (WINAPI *PFN_RefreshImmersiveColorPolicyState)();
	typedef void (WINAPI *PFN_FlushMenuThemes)();

	DWORD s_Build = 0;
	bool s_DarkSupported = false;
	Theme::Preference s_Preference = Theme::prefSystem;
	Theme::Mode s_Effective = Theme::modeLight;

	HBRUSH s_hbrBackground = NULL;
	HBRUSH s_hbrControlBg = NULL;
	HBRUSH s_hbrHot = NULL;
	HFONT s_hfMenu = NULL;

	PFN_SetPreferredAppMode s_pfnSetPreferredAppMode = NULL;
	PFN_RefreshImmersiveColorPolicyState s_pfnRefreshImmersiveColorPolicyState = NULL;
	PFN_FlushMenuThemes s_pfnFlushMenuThemes = NULL;

	//! GetVersionEx() reports 6.2 for any application without a <supportedOS> manifest
	//! element, and srvman has none. RtlGetNtVersionNumbers() is undocumented but is
	//! exported by name, has been stable since XP and is not subject to the shim.
	DWORD GetNtBuild()
	{
		HMODULE hNtdll = ::GetModuleHandleW(L"ntdll.dll");	// always mapped
		if (!hNtdll)
			return 0;

		PFN_RtlGetNtVersionNumbers pfn = (PFN_RtlGetNtVersionNumbers)::GetProcAddress(hNtdll, "RtlGetNtVersionNumbers");
		if (!pfn)
			return 0;

		DWORD dwMajor = 0, dwMinor = 0, dwBuild = 0;
		pfn(&dwMajor, &dwMinor, &dwBuild);
		return dwBuild & ~0xF0000000;	// the top nibble is a checked/free marker
	}

	bool IsHighContrast()
	{
		HIGHCONTRAST hc = { sizeof(hc), 0, NULL };
		if (!::SystemParametersInfo(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
			return false;
		return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
	}

	//! uxtheme.dll is guaranteed to be mapped already: comctl32 v6 depends on it and we
	//! statically import SetWindowTheme(). GetModuleHandle avoids both an unnecessary
	//! reference count and the LOAD_LIBRARY_SEARCH_SYSTEM32 availability question on Win7.
	void BindUxTheme()
	{
		HMODULE hUxTheme = ::GetModuleHandleW(L"uxtheme.dll");
		if (!hUxTheme)
			return;

		s_pfnSetPreferredAppMode = (PFN_SetPreferredAppMode)::GetProcAddress(hUxTheme, MAKEINTRESOURCEA(135));
		s_pfnRefreshImmersiveColorPolicyState = (PFN_RefreshImmersiveColorPolicyState)::GetProcAddress(hUxTheme, MAKEINTRESOURCEA(104));

		// Ordinal 136 only does anything useful once the popup menus are theme-aware.
		if (s_Build >= kBuildPreferredAppMode)
			s_pfnFlushMenuThemes = (PFN_FlushMenuThemes)::GetProcAddress(hUxTheme, MAKEINTRESOURCEA(136));
	}

	//! Tells comctl32 which palette to render with. Process-wide, and only observed by
	//! controls created afterwards - hence Theme::Init() running before the first window.
	void ApplyAppMode()
	{
		if (!s_pfnSetPreferredAppMode)
			return;

		bool bDark = (s_Effective == Theme::modeDark);
		if (s_Build >= kBuildPreferredAppMode)
			s_pfnSetPreferredAppMode(bDark ? pamForceDark : pamForceLight);
		else
			s_pfnSetPreferredAppMode(bDark ? TRUE : FALSE);	// AllowDarkModeForApp() on 1809

		if (s_pfnRefreshImmersiveColorPolicyState)
			s_pfnRefreshImmersiveColorPolicyState();
		if (s_pfnFlushMenuThemes)
			s_pfnFlushMenuThemes();
	}

	//! Windows' own app-mode setting. A missing value means light, which is also the
	//! right answer on the Windows versions that predate the setting entirely.
	bool OsPrefersDark()
	{
		BazisLib::Win32::RegistryKey key(HKEY_CURRENT_USER, tszPersonalizeKey, 0, false);
		if (!key.Valid())
			return false;

		unsigned uLight = 1;
		if (!key[tszAppsUseLightTheme].ReadValue(&uLight).Successful())
			return false;
		return uLight == 0;
	}

	Theme::Preference LoadPreference()
	{
		// Read-only: the params root lives under HKLM, and RegistryKey's write-capable
		// path asks for KEY_ALL_ACCESS, which fails outright when srvman runs unelevated.
		BazisLib::Win32::RegistryKey key(HKEY_LOCAL_MACHINE, tszParamsKey, 0, false);
		if (!key.Valid())
			return Theme::prefSystem;

		unsigned uPref = Theme::prefSystem;
		if (!key[tszThemeValue].ReadValue(&uPref).Successful())
			return Theme::prefSystem;
		if (uPref > Theme::prefSystem)
			return Theme::prefSystem;
		return (Theme::Preference)uPref;
	}

	//! Silently does nothing when srvman runs unelevated, exactly like the column widths
	//! and the icon display mode already stored under the same HKLM root.
	void SavePreference(Theme::Preference pref)
	{
		BazisLib::Win32::RegistryKey key(HKEY_LOCAL_MACHINE, tszParamsKey);
		if (key.Valid())
			key[tszThemeValue] = (unsigned)pref;
	}

	Theme::Mode ResolveMode()
	{
		if (!s_DarkSupported)
			return Theme::modeLight;

		switch (s_Preference)
		{
		case Theme::prefLight:
			return Theme::modeLight;
		case Theme::prefDark:
			return Theme::modeDark;
		default:
			return OsPrefersDark() ? Theme::modeDark : Theme::modeLight;
		}
	}

	//! Passing NULL reverts a control to its default theme, which is not the same thing
	//! as naming the light theme explicitly - that would change how the control looks
	//! compared to the untouched application.
	void ThemeControl(HWND hWnd, LPCWSTR pszDarkTheme)
	{
		::SetWindowTheme(hWnd, Theme::IsDark() ? pszDarkTheme : NULL, NULL);
	}

	void ApplyTitleBar(HWND hWnd)
	{
		BOOL bDark = Theme::IsDark() ? TRUE : FALSE;
		DWORD dwAttribute = (s_Build >= kBuildDarkCaptionAttr)
			? DWMWA_USE_IMMERSIVE_DARK_MODE
			: DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1;

		// An attribute the running DWM does not know is simply rejected with E_INVALIDARG,
		// so the down-level path costs nothing but the failed call.
		if (FAILED(::DwmSetWindowAttribute(hWnd, dwAttribute, &bDark, sizeof(bDark)))
			&& dwAttribute == DWMWA_USE_IMMERSIVE_DARK_MODE)
		{
			::DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1, &bDark, sizeof(bDark));
		}
	}

	HBRUSH HotBrush()
	{
		if (!s_hbrHot)
			s_hbrHot = ::CreateSolidBrush(Theme::HotBackground());
		return s_hbrHot;
	}

	//! The UAH device context does not come with the menu font selected, so drawing the
	//! bar with DrawText() means supplying it ourselves.
	HFONT MenuFont()
	{
		if (!s_hfMenu)
		{
			NONCLIENTMETRICS ncm = { 0 };
			ncm.cbSize = sizeof(ncm);
			if (::SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
				s_hfMenu = ::CreateFontIndirect(&ncm.lfMenuFont);
		}
		return s_hfMenu;
	}

	BOOL CALLBACK EnumChildProc(HWND hWnd, LPARAM /*lParam*/)
	{
		Theme::ApplyToControl(hWnd);
		return TRUE;
	}
}

void Theme::Init()
{
	s_Build = GetNtBuild();

	// Bind regardless of the current high-contrast state: it can be switched off while
	// srvman runs, and there is no second chance to resolve the entry points.
	if (s_Build >= kBuildDarkModeIntroduced)
		BindUxTheme();

	// Without ordinal 135 there is no way to put the common controls into dark mode, so
	// treat the whole feature as unavailable rather than half-applying it.
	s_DarkSupported = (s_pfnSetPreferredAppMode != NULL) && !IsHighContrast();

	s_Preference = LoadPreference();
	s_Effective = ResolveMode();
	ApplyAppMode();
}

void Theme::Shutdown()
{
	if (s_hbrBackground)
	{
		::DeleteObject(s_hbrBackground);
		s_hbrBackground = NULL;
	}
	if (s_hbrControlBg)
	{
		::DeleteObject(s_hbrControlBg);
		s_hbrControlBg = NULL;
	}
	if (s_hbrHot)
	{
		::DeleteObject(s_hbrHot);
		s_hbrHot = NULL;
	}
	if (s_hfMenu)
	{
		::DeleteObject(s_hfMenu);
		s_hfMenu = NULL;
	}
}

bool Theme::IsDarkModeSupported()
{
	return s_DarkSupported;
}

Theme::Preference Theme::GetPreference()
{
	return s_Preference;
}

bool Theme::SetPreference(Preference pref)
{
	s_Preference = pref;
	SavePreference(pref);

	Mode newMode = ResolveMode();
	if (newMode == s_Effective)
		return false;

	s_Effective = newMode;
	ApplyAppMode();
	return true;
}

Theme::Mode Theme::GetEffectiveMode()
{
	return s_Effective;
}

bool Theme::RefreshFromSystem()
{
	// uxtheme caches the immersive colour policy; without this it keeps handing out the
	// old palette to controls even after the user has flipped the system setting.
	if (s_pfnRefreshImmersiveColorPolicyState)
		s_pfnRefreshImmersiveColorPolicyState();

	// High contrast can be switched on or off at runtime, and it outranks any preference.
	s_DarkSupported = (s_pfnSetPreferredAppMode != NULL) && !IsHighContrast();

	Mode newMode = ResolveMode();
	if (newMode == s_Effective)
		return false;

	s_Effective = newMode;
	ApplyAppMode();
	return true;
}

COLORREF Theme::Background()			{ return RGB(0x20, 0x20, 0x20); }
COLORREF Theme::ControlBackground()		{ return RGB(0x2B, 0x2B, 0x2B); }
COLORREF Theme::Text()					{ return RGB(0xE0, 0xE0, 0xE0); }
COLORREF Theme::DisabledText()			{ return RGB(0x80, 0x80, 0x80); }
COLORREF Theme::HotBackground()			{ return RGB(0x40, 0x40, 0x40); }

HBRUSH Theme::BackgroundBrush()
{
	if (!s_hbrBackground)
		s_hbrBackground = ::CreateSolidBrush(Background());
	return s_hbrBackground;
}

HBRUSH Theme::ControlBackgroundBrush()
{
	if (!s_hbrControlBg)
		s_hbrControlBg = ::CreateSolidBrush(ControlBackground());
	return s_hbrControlBg;
}

void Theme::ApplyToWindow(HWND hwndTop)
{
	if (!hwndTop)
		return;

	ApplyTitleBar(hwndTop);
	ThemeControl(hwndTop, L"DarkMode_Explorer");
	::EnumChildWindows(hwndTop, EnumChildProc, 0);
}

void Theme::ApplyToControl(HWND hwndCtl)
{
	WCHAR wszClass[64] = { 0 };
	if (!::GetClassNameW(hwndCtl, wszClass, _countof(wszClass)))
		return;

	if (!lstrcmpiW(wszClass, WC_LISTVIEWW))
	{
		ApplyToListView(hwndCtl);
	}
	else if (!lstrcmpiW(wszClass, WC_HEADERW))
	{
		ThemeControl(hwndCtl, L"DarkMode_ItemsView");
	}
	else if (!lstrcmpiW(wszClass, L"Edit") || !lstrcmpiW(wszClass, L"ComboBox"))
	{
		ThemeControl(hwndCtl, L"DarkMode_CFD");
	}
	else if (!lstrcmpiW(wszClass, WC_COMBOBOXEXW))
	{
		// ComboBoxEx32 is a wrapper - theming the outer window leaves the combo it hosts
		// untouched, and the combo is the part the user actually sees.
		ThemeControl(hwndCtl, L"DarkMode_CFD");
		HWND hCombo = (HWND)::SendMessage(hwndCtl, CBEM_GETCOMBOCONTROL, 0, 0);
		if (hCombo)
			ThemeControl(hCombo, L"DarkMode_CFD");
	}
	else if (!lstrcmpiW(wszClass, L"Button"))
	{
		ThemeControl(hwndCtl, L"DarkMode_Explorer");
	}
}

void Theme::ApplyToListView(HWND hwndList)
{
	bool bDark = IsDark();

	// DarkMode_Explorer is what gives the list dark scrollbars, hot-track and selection
	// highlight - which is why the OpenNcThemeData hook other dark-mode ports use is
	// not needed here.
	ThemeControl(hwndList, L"DarkMode_Explorer");

	ListView_SetBkColor(hwndList, bDark ? ControlBackground() : ::GetSysColor(COLOR_WINDOW));
	// SetBkColor alone still leaves every item's text drawn on a white background in
	// report view; SetTextBkColor is the one that fixes it.
	ListView_SetTextBkColor(hwndList, bDark ? ControlBackground() : ::GetSysColor(COLOR_WINDOW));
	ListView_SetTextColor(hwndList, bDark ? Text() : ::GetSysColor(COLOR_WINDOWTEXT));

	HWND hHeader = ListView_GetHeader(hwndList);
	if (hHeader)
		ThemeControl(hHeader, L"DarkMode_ItemsView");

	::InvalidateRect(hwndList, NULL, TRUE);
}

void Theme::Repaint(HWND hwndTop)
{
	if (!hwndTop)
		return;

	// Makes the DWM pick up the new caption attribute.
	::SetWindowPos(hwndTop, NULL, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

	::RedrawWindow(hwndTop, NULL, NULL,
		RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);

	if (::GetMenu(hwndTop))
		::DrawMenuBar(hwndTop);
}

bool Theme::HandleCtlColor(UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT &lResult)
{
	if (GetEffectiveMode() != modeDark)
		return false;	// light mode: never intercept anything

	HDC hdc = (HDC)wParam;

	switch (uMsg)
	{
	case WM_CTLCOLORDLG:
	case WM_CTLCOLORBTN:
		::SetTextColor(hdc, Text());
		::SetBkColor(hdc, Background());
		lResult = (LRESULT)BackgroundBrush();
		return true;

	case WM_CTLCOLORSTATIC:
		// Disabled edits arrive here rather than as WM_CTLCOLOREDIT - that is how the
		// read-only Properties dialog is drawn, so keep them visibly disabled.
		::SetTextColor(hdc, ::IsWindowEnabled((HWND)lParam) ? Text() : DisabledText());
		::SetBkColor(hdc, Background());
		lResult = (LRESULT)BackgroundBrush();
		return true;

	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX:
		::SetTextColor(hdc, Text());
		::SetBkColor(hdc, ControlBackground());
		lResult = (LRESULT)ControlBackgroundBrush();
		return true;
	}

	return false;
}

bool Theme::HandleMenuBarMessage(HWND hWnd, UINT uMsg, WPARAM /*wParam*/, LPARAM lParam, LRESULT &lResult)
{
	if (GetEffectiveMode() != modeDark)
		return false;	// light mode: let USER32 draw its own menu bar

	if (uMsg == kWmUahDrawMenu)
	{
		UAHMENU *pUdm = (UAHMENU *)lParam;

		MENUBARINFO mbi = { 0 };
		mbi.cbSize = sizeof(mbi);
		if (!::GetMenuBarInfo(hWnd, OBJID_MENU, 0, &mbi))
			return false;

		// rcBar is in screen coordinates; the UAH device context is the window DC.
		RECT rcWindow = { 0 };
		::GetWindowRect(hWnd, &rcWindow);
		RECT rcBar = mbi.rcBar;
		::OffsetRect(&rcBar, -rcWindow.left, -rcWindow.top);

		::FillRect(pUdm->hdc, &rcBar, BackgroundBrush());

		lResult = 0;
		return true;
	}

	if (uMsg == kWmUahDrawMenuItem)
	{
		UAHDRAWMENUITEM *pUdmi = (UAHDRAWMENUITEM *)lParam;

		WCHAR wszText[128] = { 0 };
		MENUITEMINFOW mii = { 0 };
		mii.cbSize = sizeof(mii);
		mii.fMask = MIIM_STRING;
		mii.dwTypeData = wszText;
		mii.cch = _countof(wszText) - 1;
		if (!::GetMenuItemInfoW(pUdmi->um.hmenu, pUdmi->umi.iPosition, TRUE, &mii))
			return false;

		HBRUSH hbrBack = BackgroundBrush();
		if (pUdmi->dis.itemState & (ODS_HOTLIGHT | ODS_SELECTED))
			hbrBack = HotBrush();

		COLORREF crText = (pUdmi->dis.itemState & (ODS_GRAYED | ODS_DISABLED))
			? DisabledText()
			: Text();

		UINT uFormat = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
		// ODS_NOACCEL means the user has not pressed Alt, so the &-prefixed letter must
		// not be underlined yet.
		if (pUdmi->dis.itemState & ODS_NOACCEL)
			uFormat |= DT_HIDEPREFIX;

		::FillRect(pUdmi->um.hdc, &pUdmi->dis.rcItem, hbrBack);
		::SetBkMode(pUdmi->um.hdc, TRANSPARENT);
		::SetTextColor(pUdmi->um.hdc, crText);

		HFONT hfOld = (HFONT)::SelectObject(pUdmi->um.hdc, MenuFont());
		::DrawTextW(pUdmi->um.hdc, wszText, -1, &pUdmi->dis.rcItem, uFormat);
		if (hfOld)
			::SelectObject(pUdmi->um.hdc, hfOld);

		lResult = 0;
		return true;
	}

	return false;	// WM_UAHINITMENU / WM_UAHMEASUREMENUITEM: default behaviour is correct
}

void Theme::DrawMenuBarBottomLine(HWND hWnd)
{
	if (GetEffectiveMode() != modeDark)
		return;

	MENUBARINFO mbi = { 0 };
	mbi.cbSize = sizeof(mbi);
	if (!::GetMenuBarInfo(hWnd, OBJID_MENU, 0, &mbi))
		return;

	// USER32 draws a light separator in the last non-client row above the client area,
	// after everything the UAH messages let us paint. Cover it in window coordinates.
	RECT rcClient = { 0 };
	::GetClientRect(hWnd, &rcClient);
	::MapWindowPoints(hWnd, NULL, (POINT *)&rcClient, 2);

	RECT rcWindow = { 0 };
	::GetWindowRect(hWnd, &rcWindow);
	::OffsetRect(&rcClient, -rcWindow.left, -rcWindow.top);

	RECT rcLine = rcClient;
	rcLine.bottom = rcLine.top;
	rcLine.top--;

	HDC hdc = ::GetWindowDC(hWnd);
	if (!hdc)
		return;
	::FillRect(hdc, &rcLine, BackgroundBrush());
	::ReleaseDC(hWnd, hdc);
}
