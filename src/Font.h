// Font.h : user-selectable UI font
//
// The font belongs to the dialog *template*, not to the dialog: USER32 derives the dialog
// base units from it while instantiating the template, and every control position and size
// in the resource is expressed in those units. That is what makes a dialog scale as a whole
// when the font changes - and also why the font cannot be changed on a live dialog. A
// WM_SETFONT to the children would enlarge the text inside rectangles that were already
// computed from the old font, and the labels would clip.
//
// So the choice is applied by rewriting the font block of the template on its way to
// DialogBoxIndirectParam() (PatchTemplate() + CUserFontDialogImpl), and changing it while
// the main dialog is up re-creates that dialog (see MainDlgRestart).
//
// The service list needs nothing of its own: controls inherit the dialog font when they are
// created, and the list view recomputes its row height from it.
//
// The menu bar is deliberately not covered. It is non-client area painted by USER32 from
// the system-wide menu font; the only way in is to owner-draw every menu, which would mean
// re-implementing check marks, mnemonics, the accelerator column and the greyed state by
// hand, in both themes.
//
#pragma once

namespace Font
{
	//! Reads the preference. There is no matching Shutdown(): unlike Theme, this module
	//! holds no GDI object - the fonts it creates to measure text live for the duration of
	//! the measurement, and the dialog font itself belongs to USER32.
	void Init();

	//! False when the templates are to be instantiated exactly as they were authored.
	bool IsCustom();

	//! Runs the font common dialog and persists the result.
	//! \returns true if the effective font changed, i.e. if the caller has to re-create
	//!          itself for the choice to become visible.
	bool Choose(HWND hwndParent);

	//! Drops the preference and goes back to the font named in srvman.rc.
	//! \returns true if there was one to drop.
	bool Reset();

	//! Average character width of the font currently in effect, measured the way USER32
	//! measures dialog base units. Only the ratio between two of these means anything;
	//! it is what converts a width in pixels from one font to another.
	int AverageCharWidth();

	//! Converts a width that was measured against the font the templates name into one
	//! that holds as much text in the font actually in use. Identity while IsCustom() is
	//! false. Independent of DPI - compose with Dpi::Scale() for a constant that is both
	//! 96 DPI and template-font relative, which the list view column defaults are.
	int ScaleTextWidth(int Pixels);

	//! A copy of dialog template nID whose font block names the user's font, or NULL when
	//! there is none and the resource can be handed to USER32 as it is.
	//! Release with FreeTemplate().
	LPCDLGTEMPLATE PatchTemplate(UINT nID);
	void FreeTemplate(LPCDLGTEMPLATE pTemplate);
}

//! Drop-in replacement for CDialogImpl<T> for every dialog that should follow the user's
//! font. It mirrors ATL's DoModal() - thunk, window data, modal flag - and only swaps
//! DialogBoxParam() for the Indirect form; without a custom font it defers to the base and
//! the resource is used untouched.
/*! Named to stay clear of WTL's CFontDialogImpl, which is the font *chooser*. */
template <class T, class TBase = CWindow>
class CUserFontDialogImpl : public CDialogImpl<T, TBase>
{
public:
	INT_PTR DoModal(HWND hWndParent = ::GetActiveWindow(), LPARAM dwInitParam = NULL)
	{
		LPCDLGTEMPLATE pTemplate = Font::PatchTemplate(static_cast<T *>(this)->IDD);
		if (!pTemplate)
			return CDialogImpl<T, TBase>::DoModal(hWndParent, dwInitParam);

		ATLASSUME(this->m_hWnd == NULL);
		if (!this->m_thunk.Init(NULL, NULL))
		{
			Font::FreeTemplate(pTemplate);
			::SetLastError(ERROR_OUTOFMEMORY);
			return -1;
		}

		_AtlWinModule.AddCreateWndData(&this->m_thunk.cd, (CDialogImplBaseT<TBase> *)this);
#ifdef _DEBUG
		this->m_bModal = true;	//only declared in debug builds, and only used by an assert
#endif
		INT_PTR nRet = ::DialogBoxIndirectParam(_AtlBaseModule.GetResourceInstance(),
			pTemplate, hWndParent, T::StartDialogProc, dwInitParam);

		Font::FreeTemplate(pTemplate);
		return nRet;
	}
};
