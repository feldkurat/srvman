// MainDlg.h : interface of the CMainDlg class
//
/////////////////////////////////////////////////////////////////////////////

#pragma once

#include <bzscore/Win32/registry.h>
#include <bzshlp/Win32/services.h>

#include "Theme.h"
#include "version.h"

//! Replaces WTL's CSimpleDialog<IDD_ABOUTBOX>, whose message map is sealed and so cannot
//! be taught about WM_CTLCOLOR*. The template does nothing else we need: IDD_ABOUTBOX has
//! no DLGINIT resource, and the template already carries DS_CENTER.
class CAboutDlg : public CDialogImpl<CAboutDlg>
{
public:
	enum { IDD = IDD_ABOUTBOX };

	BEGIN_MSG_MAP(CAboutDlg)
		THEME_CTLCOLOR_HANDLERS()
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		COMMAND_RANGE_HANDLER(IDOK, IDNO, OnCloseCmd)
	END_MSG_MAP()

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		//! IDC_ABOUTTEXT is empty in the dialog template: the version comes from
		//! version.h, and rc.exe cannot splice a macro into a control's text.
		SetDlgItemText(IDC_ABOUTTEXT,
			_T("Service Manager for Windows\r\n Version ") SRVMAN_VERSION_STRT
			_T("\r\n http://www.sysprogs.org/"));
		Theme::ApplyToWindow(m_hWnd);
		return TRUE;
	}

	LRESULT OnCloseCmd(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{
		EndDialog(wID);
		return 0;
	}
};

class CMainDlg : public CDialogImpl<CMainDlg>, public CDialogResize<CMainDlg>
{
private:
	CListViewCtrl m_ListView;
	CEdit         m_FilterBox;
	CImageList    m_ImageList;
	CMenuHandle   m_MainMenu;
	BazisLib::Win32::RegistryKey m_ParamsRoot;
	//! Lowercased substring typed into the filter box; empty means "show everything".
	BazisLib::String m_Filter;
	enum
	{
		imServiceType,
		imServiceState
	}			  m_IconDisplayMode;
	BazisLib::String m_RestartPendingServiceName;

public:
	enum { IDD = IDD_MAINDLG };

	BEGIN_MSG_MAP(CMainDlg)
		THEME_CTLCOLOR_HANDLERS()
		THEME_MENUBAR_HANDLERS()
		MESSAGE_HANDLER(WM_NCPAINT, OnNcPaint)
		MESSAGE_HANDLER(WM_NCACTIVATE, OnNcPaint)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_TIMER, OnTimer)
		MESSAGE_HANDLER(WM_CLOSE, OnClose)
		MESSAGE_HANDLER(WM_SETTINGCHANGE, OnSettingChange)
		COMMAND_HANDLER(IDC_FILTER, EN_CHANGE, OnFilterChanged)
		COMMAND_HANDLER(IDC_EXIT, BN_CLICKED, OnBnClickedExit)
		COMMAND_HANDLER(IDC_PROPERTIES, BN_CLICKED, OnBnClickedProperties)
		COMMAND_HANDLER(ID_SERVICE_PROPERTIES, BN_CLICKED, OnBnClickedProperties)
		NOTIFY_HANDLER(IDC_LIST1, NM_DBLCLK, OnListViewDblClick)
		COMMAND_HANDLER(IDC_ADDSERVICE, BN_CLICKED, OnBnClickedAddservice)
		COMMAND_HANDLER(ID_SERVICE_ADDSERVICE, BN_CLICKED, OnBnClickedAddservice)
		COMMAND_HANDLER(IDC_DELETESERVICE, BN_CLICKED, OnBnClickedDeleteservice)
		COMMAND_HANDLER(ID_SERVICE_DELETESERVICE, BN_CLICKED, OnBnClickedDeleteservice)
		CHAIN_MSG_MAP(CDialogResize)
		COMMAND_ID_HANDLER(ID_APP_ABOUT, OnAppAbout)
		COMMAND_ID_HANDLER(ID_HELP_COMMANDLINEHELP, OnCommandLineHelp)
		COMMAND_ID_HANDLER(ID_VIEW_INTERNALNAME,	OnViewFlagsChanged)
		COMMAND_ID_HANDLER(ID_VIEW_STATE,			OnViewFlagsChanged)
		COMMAND_ID_HANDLER(ID_VIEW_TYPE,			OnViewFlagsChanged)
		COMMAND_ID_HANDLER(ID_VIEW_DISPLAYNAME,		OnViewFlagsChanged)
		COMMAND_ID_HANDLER(ID_VIEW_STARTTYPE,		OnViewFlagsChanged)
		COMMAND_ID_HANDLER(ID_VIEW_BINARYFILE,		OnViewFlagsChanged)
		COMMAND_ID_HANDLER(ID_VIEW_ACCOUNTNAME,		OnViewFlagsChanged)
		COMMAND_ID_HANDLER(ID_VIEW_REFRESH,			OnRefreshSelected)
		COMMAND_ID_HANDLER(ID_ICONMEANING_SERVICETYPE,		OnIconModeChanged)
		COMMAND_ID_HANDLER(ID_ICONMEANING_SERVICESTATE,		OnIconModeChanged)
		COMMAND_ID_HANDLER(ID_THEME_LIGHT,			OnThemeChanged)
		COMMAND_ID_HANDLER(ID_THEME_DARK,			OnThemeChanged)
		COMMAND_ID_HANDLER(ID_THEME_SYSTEM,			OnThemeChanged)
		COMMAND_ID_HANDLER(ID_HELP_OPENPROJECTPAGE,	OnOpenWebpage)
		NOTIFY_HANDLER(IDC_LIST1, LVN_ITEMCHANGED, OnSelChanged)
		COMMAND_ID_HANDLER(IDC_STARTSTOP,		StartStopService)
		COMMAND_ID_HANDLER(ID_CONTROL_START,	StartStopService)
		COMMAND_ID_HANDLER(ID_CONTROL_STOP,		StartStopService)
		COMMAND_ID_HANDLER(IDC_RESTART,			StartStopService)
	END_MSG_MAP()

	BEGIN_DLGRESIZE_MAP(CMainDlg)
		DLGRESIZE_CONTROL(IDC_FILTER, DLSZ_SIZE_X)
		DLGRESIZE_CONTROL(IDC_LIST1, DLSZ_SIZE_X | DLSZ_SIZE_Y)
		DLGRESIZE_CONTROL(IDC_PROPERTIES, DLSZ_MOVE_Y)
		DLGRESIZE_CONTROL(IDC_STARTSTOP, DLSZ_SIZE_X | DLSZ_MOVE_Y)
		DLGRESIZE_CONTROL(IDC_RESTART, DLSZ_MOVE_X | DLSZ_MOVE_Y)
		DLGRESIZE_CONTROL(IDC_ADDSERVICE, DLSZ_MOVE_Y)
		DLGRESIZE_CONTROL(IDC_DELETESERVICE, DLSZ_SIZE_X | DLSZ_MOVE_Y)
		DLGRESIZE_CONTROL(IDC_EXIT, DLSZ_MOVE_X | DLSZ_MOVE_Y)
	END_DLGRESIZE_MAP()

	CMainDlg();

private:
	void ReloadServiceList();
	bool MatchesFilter(LPCTSTR lpServiceName, LPCTSTR lpDisplayName);
	void UpdateServiceIcon(unsigned Index, DWORD dwType, DWORD dwLoad, DWORD dwState);

	void UpdateServiceStates();
	void UpdateServiceInfo(BazisLib::Win32::ServiceControlManager *pMgr, unsigned Index, bool UpdateExtendedInfo);

	void SetListItemText(unsigned Index, unsigned Subindex, LPCTSTR pszText);

	void SaveState();

	//! Re-applies the current theme to this dialog and repaints it. Also refreshes the
	//! bits of the window that are configured rather than painted - the list view's grid
	//! lines - and the radio check in the Theme submenu.
	void RefreshTheme();

	BazisLib::ActionStatus OpenSelectedService(BazisLib::Win32::Service *pService, DWORD dwAccess = SERVICE_ALL_ACCESS, BazisLib::String *pName = NULL);

public:
// Handler prototypes (uncomment arguments if needed):
//	LRESULT MessageHandler(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
//	LRESULT CommandHandler(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
//	LRESULT NotifyHandler(int /*idCtrl*/, LPNMHDR /*pnmh*/, BOOL& /*bHandled*/)

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnTimer(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnAppAbout(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnCommandLineHelp(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnBnClickedExit(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnFilterChanged(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	
	LRESULT OnViewFlagsChanged(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnRefreshSelected(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnIconModeChanged(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnThemeChanged(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnSettingChange(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnNcPaint(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnOpenWebpage(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);

	LRESULT OnSelChanged(int /*idCtrl*/, LPNMHDR /*pnmh*/, BOOL& /*bHandled*/);
	LRESULT OnListViewDblClick(int /*idCtrl*/, LPNMHDR /*pnmh*/, BOOL& /*bHandled*/);

	LRESULT StartStopService(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);

private:
	enum BuiltInIcon
	{
		icoGrey,
		icoYellow,
		icoGreen,
		icoRed,
		icoDriver,
		icoFsDriver,
		icoService,
		icoMultiService,
		icoInteractive,
	};
public:
	LRESULT OnBnClickedProperties(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnBnClickedAddservice(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnBnClickedDeleteservice(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
};
