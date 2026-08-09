// Dpi.h : scaling helpers for the system-DPI-aware build
//
// srvman declares <dpiAware>true</dpiAware> in src/srvman.manifest, so Windows hands it
// real pixels instead of stretching a 96 DPI bitmap. Everything the dialog resources
// describe scales on its own - USER32 sizes dialog units from the dialog font, which is
// picked at the DPI in effect - but the few sizes the code states in pixels do not, and
// those are what this header is for.
//
// One DPI per process: the manifest asks for system awareness, not per-monitor, so the
// value is fixed once at startup and stays valid for the lifetime of the process.
//
#pragma once

namespace Dpi
{
	//! The DPI the dialog resources and the hardcoded pixel sizes were authored against.
	enum { BaselineDpi = 96 };

	//! The system DPI, i.e. 96 at 100%, 120 at 125%, 144 at 150%.
	/*! GetDpiForSystem() would say the same thing but only exists on Windows 10 1607 and
		later; the screen DC has reported the process DPI since XP. */
	inline int Current()
	{
		static int s_Dpi = 0;
		if (!s_Dpi)
		{
			HDC hdc = ::GetDC(NULL);
			if (hdc)
			{
				s_Dpi = ::GetDeviceCaps(hdc, LOGPIXELSX);
				::ReleaseDC(NULL, hdc);
			}
			if (s_Dpi <= 0)
				s_Dpi = BaselineDpi;
		}
		return s_Dpi;
	}

	//! Converts a size written for 96 DPI into device pixels.
	inline int Scale(int Pixels)
	{
		return ::MulDiv(Pixels, Current(), BaselineDpi);
	}

	//! Adds an icon resource to an image list, rendered at the size the list was created with.
	/*! LoadIcon() always returns the SM_CXICON-sized image and leaves comctl32 to squeeze
		it into the list, which is visibly rough once the list is no longer 16x16.
		LoadImage() picks the closest image in the .ico and scales that one instead.

		The handle it returns is not shared, so it has to be released again - the image
		list keeps its own copy of the bitmaps. The LoadIcon() fallback exists only to
		keep the indices of the icons added after a failure from shifting; every icon
		here is compiled into the image, so it should never be taken.

		\returns the index of the new image, or -1 if the icon could not be added. */
	inline int AddIconToImageList(HIMAGELIST hImageList, UINT nResourceID, int cx, int cy)
	{
		HICON hIcon = (HICON)::LoadImage(_Module.GetResourceInstance(), MAKEINTRESOURCE(nResourceID),
			IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
		if (!hIcon)
			return ::ImageList_AddIcon(hImageList, ::LoadIcon(_Module.GetResourceInstance(), MAKEINTRESOURCE(nResourceID)));

		int Index = ::ImageList_AddIcon(hImageList, hIcon);
		::DestroyIcon(hIcon);
		return Index;
	}
}
