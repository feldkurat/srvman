// srvman.cpp : main source file for srvman.exe
//

#include "stdafx.h"

#include "resource.h"

#include "Font.h"
#include "MainDlg.h"
#include "Theme.h"
#include "cmdline.h"
#include "runassrv.h"

CAppModule _Module;

int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPTSTR /*lpstrCmdLine*/, int /*nCmdShow*/)
{
	HRESULT hRes = ::CoInitialize(NULL);
// If you are running on NT 4.0 or higher you can use the following call instead to 
// make the EXE free threaded. This means that calls come in on a random RPC thread.
//	HRESULT hRes = ::CoInitializeEx(NULL, COINIT_MULTITHREADED);
	ATLASSERT(SUCCEEDED(hRes));

	LPCWSTR lpCmdLine = GetCommandLineW();
	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(lpCmdLine, &argc);
	if ((argc > 1) && argv)
	{
		if (!_tcsnicmp(argv[1], _T("/RunAsService:"), 14))
		{
			LPCTSTR lpServiceName = argv[1] + 14;
			LPCTSTR lpOriginalCmdLine = _tcsstr(lpCmdLine, argv[1]);
			if (lpOriginalCmdLine)
				return RunCommandLineAsService(lpOriginalCmdLine + _tcslen(argv[1]) + 1, lpServiceName);
		}
		else
		{
			bool ConsoleExisted = (AttachConsole(ATTACH_PARENT_PROCESS) != FALSE);
			if (!ConsoleExisted)
				AllocConsole();

			SetConsoleOutputCP(CP_ACP);
			SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), ENABLE_INSERT_MODE);

			int ret = ConsoleMain(argc, argv, ConsoleExisted);
			LocalFree(argv);
			FreeConsole();
			return ret;
		}
	}

	// this resolves ATL window thunking problem when Microsoft Layer for Unicode (MSLU) is used
	::DefWindowProc(NULL, 0, 0, 0L);

	AtlInitCommonControls(ICC_BAR_CLASSES);	// add flags to support other controls

	hRes = _Module.Init(NULL, hInstance);
	ATLASSERT(SUCCEEDED(hRes));

	// Must precede the first window: the app-mode switch is only observed by common
	// controls created after it.
	Theme::Init();
	// Same, for a different reason: the font is read out of the dialog template as USER32
	// instantiates it.
	Font::Init();

	int nRet = 0;
	// BLOCK: Run application
	{
		// Picking a font ends the dialog with MainDlgRestart rather than applying it in
		// place, because the layout USER32 computed from the old font cannot be revised -
		// see Font.h. A fresh CMainDlg each time round: the ATL thunk is single-use.
		do
		{
			CMainDlg dlgMain;
			nRet = (int)dlgMain.DoModal();
		} while (nRet == MainDlgRestart);
	}

	Theme::Shutdown();

	_Module.Term();
	::CoUninitialize();

	return nRet;
}
