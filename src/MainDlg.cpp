// MainDlg.cpp : implementation of the CMainDlg class
//
/////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "resource.h"

#include "MainDlg.h"
#include "cmdline.h"
#include "Dpi.h"
#include "PropertiesDlg.h"

#include <bzscore/file.h>
#include <shlwapi.h>	//StrStrI() for the service list filter

#if !defined(BAZISLIB_VERSION) || (BAZISLIB_VERSION < 0x214)
#error BazisLib >= 2.1.4 is required to build this application
#endif

using namespace BazisLib::Win32;

//! DefaultWidth is the width the column was authored with: 96 DPI, and the font named in
//! srvman.rc. It is never written to - the width in effect goes to Width, which is either
//! what a previous session saved or DefaultWidth put through both scales.
static struct _ColumnDescription
{
	DWORD dwMenuID;
	unsigned DefaultWidth;
	const TCHAR *pRegistryKeyName;
	bool Enabled;
	unsigned Width;
} s_Columns[] = {
	{ID_VIEW_INTERNALNAME,	100, _T("InternalName"),	true},
	{ID_VIEW_STATE,			60,	 _T("State"),			true},
	{ID_VIEW_TYPE,			60,	 _T("Type"),			true},
	{ID_VIEW_DISPLAYNAME,	200, _T("DisplayName"),		true},
	{ID_VIEW_STARTTYPE,		70,	 _T("StartType"),		true},
	{ID_VIEW_BINARYFILE,	240, _T("BinaryFile"),		true},
	{ID_VIEW_ACCOUNTNAME,	200, _T("AccountName"),		false},
};

static const TCHAR tszVisibleColumnsSubkey[] = _T("VisibleColumns");
static const TCHAR tszColumnWidthsSubkey[] = _T("ColumnWidths");

LRESULT CMainDlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& bHandled)
{
	// set icons
	HICON hIcon = (HICON)::LoadImage(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDR_MAINFRAME), 
		IMAGE_ICON, ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
	SetIcon(hIcon, TRUE);
	HICON hIconSmall = (HICON)::LoadImage(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDR_MAINFRAME), 
		IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
	SetIcon(hIconSmall, FALSE);

	DlgResize_Init();

	m_ListView.m_hWnd = GetDlgItem(IDC_LIST1);
	m_FilterBox.m_hWnd = GetDlgItem(IDC_FILTER);
	m_FilterBox.SetCueBannerText(_T("Type to filter by service or display name"));

	m_ListView.AddColumn(_T("Internal name"), 0);
	m_ListView.AddColumn(_T("State"), 1);
	m_ListView.AddColumn(_T("Type"), 2);
	m_ListView.AddColumn(_T("Display name"), 3);
	m_ListView.AddColumn(_T("Start type"), 4);
	m_ListView.AddColumn(_T("Executable"), 5);
	m_ListView.AddColumn(_T("Account name"), 6);

	m_MainMenu.m_hMenu = GetMenu();

	Win32::RegistryKey rkVisibleCols(m_ParamsRoot, tszVisibleColumnsSubkey);
	Win32::RegistryKey rkColumnWidths(m_ParamsRoot, tszColumnWidthsSubkey);

	for (unsigned i = 0; i < __countof(s_Columns); i++)
	{
		//A width a previous session stored is already in device pixels and is taken as it
		//is. Only the built-in default needs the two scales: 96 DPI to this screen, and
		//the font srvman.rc names to the one the user picked.
		if (!rkColumnWidths[s_Columns[i].pRegistryKeyName].ReadValue(&s_Columns[i].Width).Successful())
			s_Columns[i].Width = Dpi::Scale(Font::ScaleTextWidth(s_Columns[i].DefaultWidth));

		rkVisibleCols[s_Columns[i].pRegistryKeyName].ReadValue(&s_Columns[i].Enabled);
		m_ListView.SetColumnWidth(i, s_Columns[i].Enabled ? s_Columns[i].Width : 0);
		m_MainMenu.CheckMenuItem(s_Columns[i].dwMenuID, (s_Columns[i].Enabled) ? MF_CHECKED : MF_UNCHECKED);
	}

	m_ParamsRoot[_T("IconDisplayMode")].ReadValue((unsigned *)&m_IconDisplayMode);

	//The list view sizes its rows from the image list, so the icons have to follow the
	//DPI as well: SM_CXSMICON is the 16x16 small icon size already scaled by Windows.
	const int cxIcon = ::GetSystemMetrics(SM_CXSMICON), cyIcon = ::GetSystemMetrics(SM_CYSMICON);

	m_ImageList.Create(cxIcon, cyIcon, ILC_COLOR4 | ILC_MASK, 4, 0);
	Dpi::AddIconToImageList(m_ImageList, IDI_GREY, cxIcon, cyIcon);
	Dpi::AddIconToImageList(m_ImageList, IDI_YELLOW, cxIcon, cyIcon);
	Dpi::AddIconToImageList(m_ImageList, IDI_GREEN, cxIcon, cyIcon);
	Dpi::AddIconToImageList(m_ImageList, IDI_RED, cxIcon, cyIcon);
	Dpi::AddIconToImageList(m_ImageList, IDI_DRIVER, cxIcon, cyIcon);
	Dpi::AddIconToImageList(m_ImageList, IDI_FSDRIVER, cxIcon, cyIcon);
	Dpi::AddIconToImageList(m_ImageList, IDI_SERV, cxIcon, cyIcon);
	Dpi::AddIconToImageList(m_ImageList, IDI_MULTISERV, cxIcon, cyIcon);
	Dpi::AddIconToImageList(m_ImageList, IDI_INTERACTIVE, cxIcon, cyIcon);

	// Grid lines are dropped in dark mode: comctl32 derives their colour internally
	// rather than from SetTextColor(), and what it picks is all but invisible there.
	m_ListView.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | (Theme::IsDark() ? 0 : LVS_EX_GRIDLINES));
	m_ListView.ModifyStyle(0, LVS_SINGLESEL | LVS_SORTASCENDING );

	m_ListView.SetImageList(m_ImageList, LVSIL_SMALL);
	m_ListView.SetImageList(m_ImageList, LVSIL_STATE);

	m_MainMenu.CheckMenuItem(ID_ICONMEANING_SERVICETYPE, (m_IconDisplayMode == imServiceType) ? MF_CHECKED : MF_UNCHECKED);
	m_MainMenu.CheckMenuItem(ID_ICONMEANING_SERVICESTATE, (m_IconDisplayMode == imServiceState) ? MF_CHECKED : MF_UNCHECKED);
	m_MainMenu.CheckMenuItem(ID_FONT_DEFAULT, Font::IsCustom() ? MF_UNCHECKED : MF_CHECKED);

	ReloadServiceList();

	SetTimer(0, 100);

	m_ListView.SelectItem(0);

	RefreshTheme();

	return bHandled = FALSE;
}

LRESULT CMainDlg::OnAppAbout(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
	CAboutDlg dlg;
	dlg.DoModal(m_hWnd);
	return 0;
}

LRESULT CMainDlg::OnBnClickedExit(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
	SaveState();
	EndDialog(0);
	return 0;
}

LRESULT CMainDlg::OnCommandLineHelp( WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
{
	AllocConsole();
	SetConsoleOutputCP(CP_ACP);
	HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD dwDone = 0;
	WriteFile(hStdOut, szCommandLineHelpMsg, (DWORD)strlen(szCommandLineHelpMsg), &dwDone, NULL);
	WriteFile(hStdOut, szPressAnyKey, (DWORD)strlen(szPressAnyKey), &dwDone, NULL);
//	INPUT_RECORD rec = {0,};
//	ReadConsoleInput(GetStdHandle(STD_INPUT_HANDLE), &rec, 1, &dwDone);
	char ch;
	SetConsoleTitle(_T("SrvMan Console"));
	SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), ENABLE_INSERT_MODE);
	ReadFile(GetStdHandle(STD_INPUT_HANDLE), &ch, 1, &dwDone, NULL);
	FreeConsole();
	return 0;
}

bool CMainDlg::MatchesFilter(LPCTSTR lpServiceName, LPCTSTR lpDisplayName)
{
	if (m_Filter.empty())
		return true;
	//StrStrI() does a case-insensitive substring search, so the filter matches
	//the way users expect without having to lowercase the service names first.
	if (lpServiceName && StrStrI(lpServiceName, m_Filter.c_str()))
		return true;
	if (lpDisplayName && StrStrI(lpDisplayName, m_Filter.c_str()))
		return true;
	return false;
}

void CMainDlg::ReloadServiceList()
{
	m_ListView.SetRedraw(FALSE);
	m_ListView.DeleteAllItems();

	ServiceControlManager mgr(SC_MANAGER_ENUMERATE_SERVICE);
	TypedBuffer<QUERY_SERVICE_CONFIG> pConfig;
	for (ServiceControlManager::iterator it = mgr.begin(); it != mgr.end(); ++it)
	{
		if (!it.Valid())
			continue;
		if (!MatchesFilter(it->lpServiceName, it->lpDisplayName))
			continue;
		int idx = m_ListView.InsertItem(-1, it->lpServiceName);

		m_ListView.SetItem(idx, 1, LVIF_TEXT, Service::GetStateName(it->ServiceStatusProcess.dwCurrentState), 0, 0, 0, 0);
		m_ListView.SetItem(idx, 2, LVIF_TEXT, Service::GetServiceTypeName(it->ServiceStatusProcess.dwServiceType), 0, 0, 0, 0);
		m_ListView.SetItem(idx, 3, LVIF_TEXT, it->lpDisplayName, 0, 0, 0, 0);

		Service srv(&mgr, it->lpServiceName, SERVICE_QUERY_CONFIG);
		if (srv.QueryConfig(&pConfig).Successful())
		{
			m_ListView.SetItem(idx, 4, LVIF_TEXT, Service::GetStartTypeName(pConfig->dwStartType), 0, 0, 0, 0);
//			m_ListView.SetItem(idx, 5, LVIF_TEXT, Service::GetServiceBinaryPath(pConfig->lpBinaryPathName, false).c_str(), 0, 0, 0, 0);
			m_ListView.SetItem(idx, 5, LVIF_TEXT, pConfig->lpBinaryPathName, 0, 0, 0, 0);
			m_ListView.SetItem(idx, 6, LVIF_TEXT, pConfig->lpServiceStartName, 0, 0, 0, 0);
			UpdateServiceIcon(idx, it->ServiceStatusProcess.dwServiceType, pConfig->dwStartType, it->ServiceStatusProcess.dwCurrentState);
		}
		else
			UpdateServiceIcon(idx, it->ServiceStatusProcess.dwServiceType, 0, it->ServiceStatusProcess.dwCurrentState);

	}

	m_ListView.SetRedraw(TRUE);
	m_ListView.Invalidate();
}

LRESULT CMainDlg::OnFilterChanged(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
	TCHAR tsz[256] = {0,};
	m_FilterBox.GetWindowText(tsz, __countof(tsz));
	if (!_tcscmp(tsz, m_Filter.c_str()))
		return 0;

	m_Filter = tsz;
	ReloadServiceList();

	//The list was rebuilt from scratch, so nothing is selected anymore. Select
	//the first match to keep the buttons in sync with what is on screen.
	if (m_ListView.GetItemCount())
		m_ListView.SelectItem(0);
	else
	{
		//OnSelChanged() bails out while nothing is selected, so the per-service
		//buttons have to be greyed out here. It re-enables them as soon as the
		//filter matches something again.
		::EnableWindow(GetDlgItem(IDC_STARTSTOP), FALSE);
		::EnableWindow(GetDlgItem(IDC_RESTART), FALSE);
		m_MainMenu.EnableMenuItem(ID_CONTROL_START, MF_GRAYED);
		m_MainMenu.EnableMenuItem(ID_CONTROL_STOP, MF_GRAYED);
		m_MainMenu.EnableMenuItem(ID_CONTROL_RESTART, MF_GRAYED);
	}
	return 0;
}

LRESULT CMainDlg::OnViewFlagsChanged( WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
{
	Win32::RegistryKey rkVisibleCols(m_ParamsRoot, tszVisibleColumnsSubkey);
	for (unsigned i = 0; i < __countof(s_Columns); i++)
		if (s_Columns[i].dwMenuID == wID)
		{
			s_Columns[i].Enabled = !s_Columns[i].Enabled;
			m_ListView.SetColumnWidth(i, s_Columns[i].Enabled ? s_Columns[i].Width : 0);
			m_MainMenu.CheckMenuItem(s_Columns[i].dwMenuID, (s_Columns[i].Enabled) ? MF_CHECKED : MF_UNCHECKED);
			rkVisibleCols[s_Columns[i].pRegistryKeyName] = s_Columns[i].Enabled;
			break;
		}
	return 0;
}

CMainDlg::CMainDlg()
: m_ParamsRoot(HKEY_LOCAL_MACHINE, _T("SOFTWARE\\SysProgs\\SrvMan"))
, m_IconDisplayMode(imServiceType)
{
}

void CMainDlg::UpdateServiceIcon( unsigned Index, DWORD dwType, DWORD dwLoad, DWORD dwState )
{
	unsigned img = -1;
	switch (m_IconDisplayMode)
	{
	case imServiceType:
		switch (dwType)
		{
		case SERVICE_FILE_SYSTEM_DRIVER:
			img = icoFsDriver;
			break;
		case SERVICE_KERNEL_DRIVER:
			img = icoDriver;
			break;
		case SERVICE_WIN32_OWN_PROCESS:
			img = icoService;
			break;
		case SERVICE_WIN32_SHARE_PROCESS:
			img = icoMultiService;
			break;
		case SERVICE_INTERACTIVE_PROCESS | SERVICE_WIN32_OWN_PROCESS:
		case SERVICE_INTERACTIVE_PROCESS | SERVICE_WIN32_SHARE_PROCESS:
			img = icoInteractive;
			break;
		}
		break;
	case imServiceState:
	default:
		switch (dwState)
		{
		case SERVICE_RUNNING:
			img = icoGreen;
			break;
		case SERVICE_STOPPED:
			if ((dwLoad == SERVICE_DISABLED) || (dwLoad == SERVICE_DEMAND_START))
				img = icoGrey;
			else
				img = icoRed;
			break;
		default:
			img = icoYellow;
			break;
		}
		break;
	}
	m_ListView.SetItem(Index, 0, LVIF_IMAGE, NULL, img, 0, 0, 0);
}

LRESULT CMainDlg::OnIconModeChanged( WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
{
	switch (wID)
	{
	case ID_ICONMEANING_SERVICETYPE:
		m_IconDisplayMode = imServiceType;
		break;
	case ID_ICONMEANING_SERVICESTATE:
		m_IconDisplayMode = imServiceState;
		break;
	}
	m_MainMenu.CheckMenuItem(ID_ICONMEANING_SERVICETYPE, (m_IconDisplayMode == imServiceType) ? MF_CHECKED : MF_UNCHECKED);
	m_MainMenu.CheckMenuItem(ID_ICONMEANING_SERVICESTATE, (m_IconDisplayMode == imServiceState) ? MF_CHECKED : MF_UNCHECKED);
	m_ParamsRoot[_T("IconDisplayMode")] = (unsigned)m_IconDisplayMode;
	UpdateServiceStates();
	return 0;
}

LRESULT CMainDlg::OnThemeChanged( WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
{
	Theme::Preference pref = Theme::prefSystem;
	switch (wID)
	{
	case ID_THEME_LIGHT:
		pref = Theme::prefLight;
		break;
	case ID_THEME_DARK:
		pref = Theme::prefDark;
		break;
	}
	Theme::SetPreference(pref);
	RefreshTheme();
	return 0;
}

LRESULT CMainDlg::OnFontChanged( WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
{
	const int OldCharWidth = Font::AverageCharWidth();

	if (!((wID == ID_FONT_CHOOSE) ? Font::Choose(m_hWnd) : Font::Reset()))
		return 0;	//cancelled, or the same font as before

	//Unlike the theme, a font cannot be applied to a window that already exists: USER32
	//turned it into control positions when it instantiated the template. Save - carrying
	//the column widths over to the new font - and ask _tWinMain for a fresh dialog.
	SaveState(Font::AverageCharWidth(), OldCharWidth);
	EndDialog(MainDlgRestart);
	return 0;
}

LRESULT CMainDlg::OnSettingChange( UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/ )
{
	// Most WM_SETTINGCHANGE broadcasts carry no string at all, so the null check is not
	// optional here. Re-applying is unconditional - see CPropertiesDlg::OnSettingChange
	// for why the return value of RefreshFromSystem() must not gate it.
	if (lParam && !lstrcmpiW((LPCWSTR)lParam, L"ImmersiveColorSet"))
	{
		Theme::RefreshFromSystem();
		RefreshTheme();
	}
	return 0;
}

LRESULT CMainDlg::OnNcPaint( UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/ )
{
	// The separator below the menu bar is drawn by the default non-client painting, so
	// it can only be covered afterwards. DefDlgProc forwards both of these messages
	// straight to DefWindowProc, so calling it directly loses nothing.
	LRESULT lRes = ::DefWindowProc(m_hWnd, uMsg, wParam, lParam);
	Theme::DrawMenuBarBottomLine(m_hWnd);
	return lRes;
}

void CMainDlg::RefreshTheme()
{
	m_ListView.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | (Theme::IsDark() ? 0 : LVS_EX_GRIDLINES));

	Theme::ApplyToWindow(m_hWnd);
	Theme::Repaint(m_hWnd);

	UINT uChecked = ID_THEME_SYSTEM;
	switch (Theme::GetPreference())
	{
	case Theme::prefLight:
		uChecked = ID_THEME_LIGHT;
		break;
	case Theme::prefDark:
		uChecked = ID_THEME_DARK;
		break;
	}
	m_MainMenu.CheckMenuRadioItem(ID_THEME_LIGHT, ID_THEME_SYSTEM, uChecked, MF_BYCOMMAND);

	// Nothing to pick on Windows 7/8 and pre-1809 Windows 10, or while high contrast is
	// on - and high contrast can be switched at any time, so this is not a one-off.
	m_MainMenu.EnableMenuItem(ID_THEME_DARK,
		MF_BYCOMMAND | (Theme::IsDarkModeSupported() ? MF_ENABLED : MF_GRAYED));
}

void CMainDlg::UpdateServiceStates()
{
	ServiceControlManager mgr(SC_MANAGER_ENUMERATE_SERVICE);
	for (int i = 0; i < m_ListView.GetItemCount(); i++)
		UpdateServiceInfo(&mgr, i, false);
}

LRESULT CMainDlg::OnTimer( UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/ )
{
	int firstIdx   = m_ListView.GetTopIndex();
	int lastIdx	   = firstIdx + m_ListView.GetCountPerPage();
	if (lastIdx >= m_ListView.GetItemCount())
		lastIdx = m_ListView.GetItemCount() - 1;

	ServiceControlManager mgr(SC_MANAGER_ENUMERATE_SERVICE);
	for (int i = firstIdx; i <= lastIdx; i++)
		UpdateServiceInfo(&mgr, i, false);

	OnSelChanged(0, 0, *(BOOL *)0);

	if (!m_RestartPendingServiceName.empty())
	{
		ServiceControlManager mgr;
		Service srv(&mgr, m_RestartPendingServiceName.c_str(), SERVICE_QUERY_STATUS | SERVICE_START);
		SERVICE_STATUS_PROCESS srvStatus;
		ActionStatus st = srv.QueryStatus(&srvStatus);
		if (!st.Successful())
		{
			MessageBox((String(_T("Cannot open service object ")) + m_RestartPendingServiceName + _T(": ") + st.GetMostInformativeText()).c_str(), _T("Service Manager"), MB_ICONERROR);
			return 0;
		}
		if (srvStatus.dwCurrentState != SERVICE_STOPPED)
			return 0;
		st = srv.Start();
		if (!st.Successful())
		{
			MessageBox((String(_T("Cannot start service ")) + m_RestartPendingServiceName + _T(": ") + st.GetMostInformativeText()).c_str(), _T("Service Manager"), MB_ICONERROR);
			return 0;
		}
		m_RestartPendingServiceName.clear();
	}
	//Nothing goes here, as service restart code can return control
	return 0;
}

void CMainDlg::UpdateServiceInfo(class BazisLib::Win32::ServiceControlManager *pMgr, unsigned Index, bool UpdateExtendedInfo )
{
	TCHAR tsz[512];
	TypedBuffer<QUERY_SERVICE_CONFIG> pConfig;
	if (!m_ListView.GetItemText(Index, 0, tsz, __countof(tsz)))
		return;
	SERVICE_STATUS_PROCESS status;
	Service srv(pMgr, tsz, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
	if (!srv.QueryStatus(&status).Successful())
		return;
	unsigned startType = 0;
	if (srv.QueryConfig(&pConfig).Successful())
		startType = pConfig->dwStartType;


	UpdateServiceIcon(Index, status.dwServiceType, startType, status.dwCurrentState);
	SetListItemText(Index, 1, Service::GetStateName(status.dwCurrentState));

	if (UpdateExtendedInfo)
	{
		SetListItemText(Index, 2, Service::GetServiceTypeName(status.dwServiceType));
		SetListItemText(Index, 3, pConfig->lpDisplayName);
		SetListItemText(Index, 4, Service::GetStartTypeName(pConfig->dwStartType));
		SetListItemText(Index, 5, pConfig->lpBinaryPathName);
		SetListItemText(Index, 6, pConfig->lpServiceStartName);
	}
}

void CMainDlg::SetListItemText( unsigned Index, unsigned Subindex, LPCTSTR pszText )
{
	TCHAR tsz[512];
	ASSERT(pszText);
	if (m_ListView.GetItemText(Index, Subindex, tsz, __countof(tsz)))
		if (!_tcscmp(pszText, tsz))
			return;
	m_ListView.SetItemText(Index, Subindex, pszText);
}

LRESULT CMainDlg::OnClose( UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/ )
{
	SaveState();
	EndDialog(0);
	return 0;
}

void CMainDlg::SaveState(int nWidthNumerator, int nWidthDenominator)
{
	if (nWidthNumerator <= 0 || nWidthDenominator <= 0)
		nWidthNumerator = nWidthDenominator = 1;	//a measurement failed; store as they are

	Win32::RegistryKey rkColumnWidths(m_ParamsRoot, tszColumnWidthsSubkey);
	for (unsigned i = 0; i < __countof(s_Columns); i++)
		if (s_Columns[i].Enabled)
			rkColumnWidths[s_Columns[i].pRegistryKeyName] =
				::MulDiv(m_ListView.GetColumnWidth(i), nWidthNumerator, nWidthDenominator);
}

LRESULT CMainDlg::OnSelChanged( int /*idCtrl*/, LPNMHDR /*pnmh*/, BOOL& /*bHandled*/ )
{
	Service srv;
	if (!OpenSelectedService(&srv, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS).Successful())
		return 0;
	SERVICE_STATUS_PROCESS proc;
	TypedBuffer<QUERY_SERVICE_CONFIG> pConfig;
	if (srv.QueryStatus(&proc).Successful() && srv.QueryConfig(&pConfig).Successful())
	{
		bool enableStart = false, enableStop = false, enableRestart = false;
		SetDlgItemText(IDC_RESTART, _T("Restart service"));
		switch(proc.dwCurrentState)
		{
		case SERVICE_RUNNING:
			enableStop = enableRestart = true;
			SetDlgItemText(IDC_STARTSTOP, _T("Stop service"));
			break;
		case SERVICE_STOPPED:
			enableStart = true;
			SetDlgItemText(IDC_STARTSTOP, _T("Start service"));
			/*if (pConfig->dwStartType == SERVICE_DISABLED)
			{
				enableRestart = true;
				SetDlgItemText(IDC_RESTART, _T("Enable service"));
			}*/
			break;
		default:
			break;
		}

		if (!m_RestartPendingServiceName.empty())
			enableRestart = false;

		::EnableWindow(GetDlgItem(IDC_STARTSTOP), enableStart || enableStop);
		::EnableWindow(GetDlgItem(IDC_RESTART), enableRestart);
		m_MainMenu.EnableMenuItem(ID_CONTROL_START, enableStart ? MF_ENABLED : MF_GRAYED);
		m_MainMenu.EnableMenuItem(ID_CONTROL_STOP,	enableStop ? MF_ENABLED : MF_GRAYED);
		m_MainMenu.EnableMenuItem(ID_CONTROL_RESTART, enableRestart ? MF_ENABLED : MF_GRAYED);
	}

	return 0;
}

LRESULT CMainDlg::OnOpenWebpage( WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
{
	ShellExecute(m_hWnd, _T("open"), _T("http://tools.sysprogs.org/srvman"), NULL, NULL, SW_SHOW);
	return 0;
}

LRESULT CMainDlg::StartStopService( WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
{
	Service srv;
	String srvName;
	ActionStatus st = OpenSelectedService(&srv, SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP, &srvName);
	enum {doStart, doStop, doRestart} actionToDo;
	if (!st.Successful())
	{
		MessageBox((String(_T("Cannot open service object: ")) + st.GetMostInformativeText()).c_str(), _T("Service Manager"), MB_ICONERROR);
		return 0;
	}

	switch (wID)
	{
	case ID_CONTROL_RESTART:
	case IDC_RESTART:
		actionToDo = doRestart;
		break;
	case ID_CONTROL_START:
		actionToDo = doStart;
		break;
	case ID_CONTROL_STOP:
		actionToDo = doStop;
		break;
	case IDC_STARTSTOP:
		{
			SERVICE_STATUS_PROCESS srvStatus;
			st = srv.QueryStatus(&srvStatus);
			if (!st.Successful())
			{
				MessageBox((String(_T("Cannot query service: ")) + st.GetMostInformativeText()).c_str(), _T("Service Manager"), MB_ICONERROR);
				return 0;
			}
			if (srvStatus.dwCurrentState == SERVICE_RUNNING)
				actionToDo = doStop;
			else
				actionToDo = doStart;
		}
		break;
	}

	switch (actionToDo)
	{
	case doStart:
		st = srv.Start();
		break;
	case doStop:
		st = srv.Stop();
		break;
	case doRestart:
		st = srv.Stop();
		m_RestartPendingServiceName = srvName;
		break;
	}

	if (!st.Successful())
	{
		MessageBox((String(_T("Cannot control service: ")) + st.GetMostInformativeText()).c_str(), _T("Service Manager"), MB_ICONERROR);
		return 0;
	}

	return 0;
}

ActionStatus CMainDlg::OpenSelectedService(BazisLib::Win32::Service *pService, DWORD dwAccess /*= SERVICE_ALL_ACCESS*/, String *pName )
{
	int idx = m_ListView.GetSelectedIndex();
	if (idx == -1)
		return MAKE_STATUS(UnknownError);

	TCHAR tsz[512];
	if (!m_ListView.GetItemText(idx, 0, tsz, __countof(tsz)))
		return MAKE_STATUS(UnknownError);

	if (pName)
		*pName = tsz;

	ServiceControlManager mgr(SC_MANAGER_ENUMERATE_SERVICE);
	return pService->OpenAnotherService(&mgr, tsz, dwAccess);
}

LRESULT CMainDlg::OnRefreshSelected( WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/ )
{
	ReloadServiceList();
	return 0;
}

LRESULT CMainDlg::OnBnClickedProperties(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
	Service srv;
	bool ReadOnly = false;
	String srvName;
	ActionStatus st = OpenSelectedService(&srv, SERVICE_ALL_ACCESS, &srvName);
	if (st.GetErrorCode() == BazisLib::AccessDenied)
		st = OpenSelectedService(&srv, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS), ReadOnly = true;
	if (!st.Successful())
	{
		MessageBox((String(_T("Cannot open service object ")) + m_RestartPendingServiceName + _T(": ") + st.GetMostInformativeText()).c_str(), _T("Service Manager"), MB_ICONERROR);
		return 0;
	}

	CPropertiesDlg dlg(srvName, &srv, ReadOnly);
	dlg.DoModal();

	ServiceControlManager mgr(SC_MANAGER_ENUMERATE_SERVICE);
	int idx = m_ListView.GetSelectedIndex();
	if (idx != -1)
		UpdateServiceInfo(&mgr, idx, true);

	return 0;
}

LRESULT CMainDlg::OnListViewDblClick( int /*idCtrl*/, LPNMHDR /*pnmh*/, BOOL& bHandled )
{
	return OnBnClickedProperties(0, 0, 0, bHandled);
}

LRESULT CMainDlg::OnBnClickedAddservice(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
	CPropertiesDlg dlg;
	if (dlg.DoModal() == IDOK)
		ReloadServiceList();
	return 0;
}

LRESULT CMainDlg::OnBnClickedDeleteservice(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
{
	if (MessageBox(_T("Do you want to delete the selected service?"), _T("Question"), MB_ICONQUESTION | MB_YESNO) == IDYES)
	{
		Service svc;
		OpenSelectedService(&svc, DELETE);
		ActionStatus st = svc.Delete();
		if (!st.Successful())
		{
			MessageBox((String(_T("Cannot delete service: ")) + m_RestartPendingServiceName + _T(": ") + st.GetMostInformativeText()).c_str(), _T("Service Manager"), MB_ICONERROR);
			return 0;
		}
	}
	m_ListView.DeleteItem(m_ListView.GetSelectedIndex());
	return 0;
}
