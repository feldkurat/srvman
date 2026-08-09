// Font.cpp : user-selectable UI font. See Font.h for what is and is not covered.
//

#include "stdafx.h"

#include "Font.h"
#include "Dpi.h"

#include <bzscore/Win32/registry.h>

namespace
{
	//! Under HKCU, unlike everything else srvman persists. Two reasons: a font is a
	//! personal preference rather than a machine-wide one, and the HKLM root is opened
	//! for write, which fails unelevated and turns every write into a silent no-op.
	const TCHAR tszParamsKey[] = _T("SOFTWARE\\SysProgs\\SrvMan");

	const TCHAR tszFaceValue[]      = _T("FontFaceName");
	const TCHAR tszPointSizeValue[] = _T("FontPointSize");	//!< tenths of a point, as ChooseFont() reports it
	const TCHAR tszWeightValue[]    = _T("FontWeight");
	const TCHAR tszItalicValue[]    = _T("FontItalic");
	const TCHAR tszCharsetValue[]   = _T("FontCharset");

	//! The font IDD_MAINDLG names in srvman.rc, and therefore the one every hardcoded
	//! width in the code was measured against. If the templates ever name a different
	//! font, this has to follow or ScaleTextWidth() will be off by that difference.
	const TCHAR tszBaselineFace[] = _T("MS Sans Serif");
	enum { BaselineTenthsOfPoint = 80 };

	//! An empty face name is what "no custom font" is stored as: BazisLib's RegistryKey
	//! can write a value but not delete one.
	TCHAR s_tszFaceName[LF_FACESIZE] = {0};
	int s_TenthsOfPoint = 0;
	int s_Weight = FW_NORMAL;
	bool s_bItalic = false;
	BYTE s_Charset = DEFAULT_CHARSET;

	//! Measuring means creating a font and a DC, and the column code asks once per column.
	//! Zero means "not measured yet"; Choose() and Reset() put it back.
	int s_AverageCharWidth = 0;
	int s_BaselineCharWidth = 0;

	bool IsCustomFontSet()
	{
		return s_tszFaceName[0] != 0 && s_TenthsOfPoint > 0;
	}

	void FillLogFont(LOGFONT *plf, LPCTSTR ptszFace, int TenthsOfPoint, int Weight, bool bItalic, BYTE Charset)
	{
		memset(plf, 0, sizeof(*plf));
		//Negative is character height rather than cell height, which is what a point size
		//means. 720 = 72 points per inch, times the ten the size is stored in.
		plf->lfHeight = -::MulDiv(TenthsOfPoint, Dpi::Current(), 720);
		plf->lfWeight = Weight;
		plf->lfItalic = bItalic ? TRUE : FALSE;
		plf->lfCharSet = Charset;
		_tcsncpy(plf->lfFaceName, ptszFace, LF_FACESIZE - 1);
	}

	//! The font a dialog would be created with right now.
	void EffectiveLogFont(LOGFONT *plf)
	{
		if (IsCustomFontSet())
			FillLogFont(plf, s_tszFaceName, s_TenthsOfPoint, s_Weight, s_bItalic, s_Charset);
		else
			FillLogFont(plf, tszBaselineFace, BaselineTenthsOfPoint, FW_NORMAL, false, DEFAULT_CHARSET);
	}

	//! The average character width MapDialogRect() works from: the 52 ASCII letters
	//! divided by 26, rounded on the half. Absolute values are meaningless here - only
	//! the ratio of two of them is used.
	int MeasureAverageCharWidth(const LOGFONT &lf)
	{
		HDC hdc = ::GetDC(NULL);
		if (!hdc)
			return 0;

		int Result = 0;
		HFONT hFont = ::CreateFontIndirect(&lf);
		if (hFont)
		{
			static const TCHAR tszAlphabet[] =
				_T("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");

			HFONT hOldFont = (HFONT)::SelectObject(hdc, hFont);
			SIZE sz;
			if (::GetTextExtentPoint32(hdc, tszAlphabet, __countof(tszAlphabet) - 1, &sz))
				Result = (sz.cx / 26 + 1) / 2;
			::SelectObject(hdc, hOldFont);
			::DeleteObject(hFont);
		}

		::ReleaseDC(NULL, hdc);
		return Result;
	}

	void Persist()
	{
		BazisLib::Win32::RegistryKey key(HKEY_CURRENT_USER, tszParamsKey);
		key[tszFaceValue] = s_tszFaceName;
		key[tszPointSizeValue] = s_TenthsOfPoint;
		key[tszWeightValue] = s_Weight;
		key[tszItalicValue] = s_bItalic;
		key[tszCharsetValue] = (int)s_Charset;
	}

	// ---- dialog template surgery ------------------------------------------------------

#pragma pack(push, 2)
	//! The header of a DLGTEMPLATEEX, which is what rc.exe emits for a DIALOGEX. The SDK
	//! documents the layout but declares no struct for it, because of the variable-length
	//! fields that follow. Packed: helpID onwards would otherwise be padded apart.
	struct DialogTemplateExHeader
	{
		WORD wDlgVer;
		WORD wSignature;
		DWORD dwHelpID;
		DWORD dwExStyle;
		DWORD dwStyle;
		WORD cDlgItems;
		short x, y, cx, cy;
	};
#pragma pack(pop)

	//! The template is UTF-16 whatever the build charset is; srvman is Unicode-only, so
	//! the face name can be copied into it as it stands.
	static_assert(sizeof(TCHAR) == sizeof(WCHAR), "the dialog template is always UTF-16");

	const BYTE *SkipString(const BYTE *p)
	{
		const WCHAR *pwsz = (const WCHAR *)p;
		while (*pwsz++)
			;
		return (const BYTE *)pwsz;
	}

	//! The menu and window class fields are sz_Or_Ord: a lone zero WORD for "none",
	//! 0xFFFF followed by an ordinal, or a null-terminated string.
	const BYTE *SkipStringOrOrdinal(const BYTE *p)
	{
		const WORD *pw = (const WORD *)p;
		if (*pw == 0x0000)
			return (const BYTE *)(pw + 1);
		if (*pw == 0xFFFF)
			return (const BYTE *)(pw + 2);
		return SkipString(p);
	}

	size_t AlignToDword(size_t Offset)
	{
		return (Offset + 3) & ~(size_t)3;
	}
}

void Font::Init()
{
	BazisLib::String Face;
	BazisLib::Win32::RegistryKey key(HKEY_CURRENT_USER, tszParamsKey, 0, false);

	if (!key[tszFaceValue].ReadValue(&Face).Successful() || Face.empty())
		return;

	int TenthsOfPoint = 0;
	if (!key[tszPointSizeValue].ReadValue(&TenthsOfPoint).Successful() || TenthsOfPoint <= 0)
		return;

	_tcsncpy(s_tszFaceName, Face.c_str(), LF_FACESIZE - 1);
	s_TenthsOfPoint = TenthsOfPoint;

	//The rest is cosmetic; a missing value just means the default.
	key[tszWeightValue].ReadValue(&s_Weight);
	key[tszItalicValue].ReadValue(&s_bItalic);

	int Charset = DEFAULT_CHARSET;
	key[tszCharsetValue].ReadValue(&Charset);
	s_Charset = (BYTE)Charset;
}

bool Font::IsCustom()
{
	return IsCustomFontSet();
}

bool Font::Choose(HWND hwndParent)
{
	LOGFONT lf;
	EffectiveLogFont(&lf);

	//No CF_EFFECTS: colour, underline and strikeout are not carried by a dialog template,
	//so offering them would be a promise this cannot keep.
	CFontDialog dlg(&lf, CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS | CF_FORCEFONTEXIST,
		NULL, hwndParent);
	if (dlg.DoModal(hwndParent) != IDOK)
		return false;

	TCHAR tszOldFace[LF_FACESIZE];
	_tcscpy(tszOldFace, s_tszFaceName);
	const int OldTenths = s_TenthsOfPoint, OldWeight = s_Weight;
	const bool bOldItalic = s_bItalic;
	const BYTE OldCharset = s_Charset;

	_tcsncpy(s_tszFaceName, dlg.m_lf.lfFaceName, LF_FACESIZE - 1);
	s_TenthsOfPoint = dlg.GetSize();
	s_Weight = dlg.m_lf.lfWeight;
	s_bItalic = (dlg.m_lf.lfItalic != 0);
	s_Charset = dlg.m_lf.lfCharSet;

	if (_tcscmp(tszOldFace, s_tszFaceName) == 0 && OldTenths == s_TenthsOfPoint &&
		OldWeight == s_Weight && bOldItalic == s_bItalic && OldCharset == s_Charset)
		return false;

	s_AverageCharWidth = 0;
	Persist();
	return true;
}

bool Font::Reset()
{
	if (!IsCustomFontSet())
		return false;

	s_tszFaceName[0] = 0;
	s_TenthsOfPoint = 0;
	s_Weight = FW_NORMAL;
	s_bItalic = false;
	s_Charset = DEFAULT_CHARSET;

	s_AverageCharWidth = 0;
	Persist();
	return true;
}

int Font::AverageCharWidth()
{
	if (!s_AverageCharWidth)
	{
		LOGFONT lf;
		EffectiveLogFont(&lf);
		s_AverageCharWidth = MeasureAverageCharWidth(lf);
	}
	return s_AverageCharWidth;
}

int Font::ScaleTextWidth(int Pixels)
{
	if (!IsCustomFontSet())
		return Pixels;

	if (!s_BaselineCharWidth)
	{
		LOGFONT lf;
		FillLogFont(&lf, tszBaselineFace, BaselineTenthsOfPoint, FW_NORMAL, false, DEFAULT_CHARSET);
		s_BaselineCharWidth = MeasureAverageCharWidth(lf);
	}

	const int Current = AverageCharWidth();
	if (Current <= 0 || s_BaselineCharWidth <= 0)
		return Pixels;	//a measurement failed; better unscaled than zero

	return ::MulDiv(Pixels, Current, s_BaselineCharWidth);
}

LPCDLGTEMPLATE Font::PatchTemplate(UINT nID)
{
	if (!IsCustomFontSet())
		return NULL;

	HINSTANCE hInstance = _Module.GetResourceInstance();
	HRSRC hResInfo = ::FindResource(hInstance, MAKEINTRESOURCE(nID), RT_DIALOG);
	if (!hResInfo)
		return NULL;

	HGLOBAL hResData = ::LoadResource(hInstance, hResInfo);
	if (!hResData)
		return NULL;

	const BYTE *pSrc = (const BYTE *)::LockResource(hResData);
	const DWORD SrcSize = ::SizeofResource(hInstance, hResInfo);
	if (!pSrc || SrcSize < sizeof(DialogTemplateExHeader))
		return NULL;

	//Only the extended form is handled, and every dialog in srvman.rc is a DIALOGEX. The
	//classic DLGTEMPLATE puts a shorter font block in a different place; there is nothing
	//to fall back to, so an unexpected template is simply left alone.
	const DialogTemplateExHeader *pHeader = (const DialogTemplateExHeader *)pSrc;
	if (pHeader->wSignature != 0xFFFF || pHeader->wDlgVer != 1)
		return NULL;

	const BYTE *p = pSrc + sizeof(DialogTemplateExHeader);
	p = SkipStringOrOrdinal(p);		//menu
	p = SkipStringOrOrdinal(p);		//window class
	p = SkipString(p);				//title

	const BYTE *pFontBlock = p;
	if (pHeader->dwStyle & DS_SETFONT)
	{
		p += 2 * sizeof(WORD) + 2 * sizeof(BYTE);	//point size, weight, italic, charset
		p = SkipString(p);							//typeface
	}
	if (p > pSrc + SrcSize)
		return NULL;			//truncated, or the walk above lost its place

	const size_t SrcItemsOffset = AlignToDword(p - pSrc);
	if (SrcItemsOffset > SrcSize)
		return NULL;

	const size_t FaceBytes = (_tcslen(s_tszFaceName) + 1) * sizeof(WCHAR);
	const size_t NewHeaderSize = (pFontBlock - pSrc) + 2 * sizeof(WORD) + 2 * sizeof(BYTE) + FaceBytes;
	const size_t NewItemsOffset = AlignToDword(NewHeaderSize);
	const size_t ItemsSize = SrcSize - SrcItemsOffset;

	//The control array is copied verbatim, so every DLGITEMTEMPLATEEX in it has to keep
	//the DWORD alignment it had in the resource. Both offsets are DWORD-aligned, so the
	//distance the array moves by is a multiple of four and the alignment survives.
	BYTE *pNew = new BYTE[NewItemsOffset + ItemsSize];
	memcpy(pNew, pSrc, pFontBlock - pSrc);

	//DS_FIXEDSYS alongside DS_SETFONT is DS_SHELLFONT, which asks USER32 to ignore the
	//typeface in the template and substitute the shell dialog font - the very thing being
	//overridden here. IDD_SERVICEPROPS carries it.
	((DialogTemplateExHeader *)pNew)->dwStyle = (pHeader->dwStyle | DS_SETFONT) & ~DS_FIXEDSYS;

	BYTE *pDest = pNew + (pFontBlock - pSrc);
	*(WORD *)pDest = (WORD)((s_TenthsOfPoint + 5) / 10);	//the template holds whole points
	pDest += sizeof(WORD);
	*(WORD *)pDest = (WORD)s_Weight;
	pDest += sizeof(WORD);
	*pDest++ = s_bItalic ? TRUE : FALSE;
	*pDest++ = s_Charset;
	memcpy(pDest, s_tszFaceName, FaceBytes);
	pDest += FaceBytes;

	memset(pDest, 0, NewItemsOffset - NewHeaderSize);	//alignment padding
	memcpy(pNew + NewItemsOffset, pSrc + SrcItemsOffset, ItemsSize);

	return (LPCDLGTEMPLATE)pNew;
}

void Font::FreeTemplate(LPCDLGTEMPLATE pTemplate)
{
	delete[] (BYTE *)pTemplate;
}
