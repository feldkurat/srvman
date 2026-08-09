# AGENTS.md

Working notes for coding agents in this repository. Read this before touching anything —
most of it is stuff that only shows up as a confusing failure an hour later.

## What this is

srvman 1.0, a native Win32 service manager. **C++ with ATL and WTL 10**, Unicode/TCHAR,
targeting Windows 7 and later (`WINVER = _WIN32_WINNT = 0x0601`). About 2,900 lines of
first-party code across seven translation units. The UI is dialog-based — there is no
frame window; `_tWinMain` calls `CMainDlg::DoModal()` directly.

There is no test suite. Verification means building and looking at the result.

## Build

```
python build.py                      # Release x64
python build.py --arch x86           # 32-bit
python build.py --arch both          # both, before you call anything done
python build.py --config Debug
python build.py --rebuild --verbose
```

Output: `build/<arch>/<Config>/srvman.exe`. Add `--package` to zip the binaries into
`dist/srvman-<version>-<arch>.zip`.

**Always build both architectures before declaring a change finished.** The x86 target is
not exotic — it links `/SAFESEH` and has a different pointer size, and it is the one that
catches sloppy casts.

If you add a `.cpp`, add it to `SRVMAN_SOURCES` in `build.py`. If you need a new import
library, add it to `SYSTEM_LIBS`. Nothing else in the build script needs to change.

There is **no precompiled header**. Put new system includes in the `.cpp` that needs them,
not in `stdafx.h`. Also note `newest_header_mtime()` rebuilds every translation unit when
any header under `src/` changes — a one-line header edit costs a full rebuild.

## Verifying a UI change

You cannot judge a Win32 UI change from a successful compile. Launch the binary and
capture the window with `PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT)` — it renders the
window whether or not it is in the foreground, so it does not steal focus and does not
capture whatever else is on the user's desktop:

```powershell
$proc = Start-Process "build\x64\Release\srvman.exe" -PassThru
Start-Sleep -Milliseconds 2500
$proc.Refresh()
# GetWindowRect + PrintWindow($proc.MainWindowHandle, $hdc, 2) into a Bitmap
```

Do not use `CopyFromScreen` on the whole desktop; it captures the user's other windows.

To drive the app without a human, `PostMessage` its own command IDs:
`WM_COMMAND` with `1001` opens Properties, `0xE140` opens About, `32804`/`32805`/`32806`
switch the theme, `32808` resets the font. Use `PostMessage`, not `SendMessage` — these open
modal dialogs and would deadlock the sender. `32807` is the font chooser, which is a system
modal dialog and cannot be driven this way; to test a font, write `FontFaceName` and
`FontPointSize` (tenths of a point) under `HKCU\SOFTWARE\SysProgs\SrvMan` and start the app.

Note that a font change re-creates the main dialog, so its `HWND` changes and
`Process.MainWindowHandle` goes stale — find the window again by title.

## Layout

```
build.py              build system - the VS2008 .sln/.vcproj in src/ are dead, ignore them
src/*.cpp, *.h        first-party code
src/srvman.rc         all dialogs, the menu, version info
src/srvman.manifest   DPI awareness; merged into the linker-generated manifest
src/Dpi.h, Font.*     scaling and the user-selectable UI font - see below
src/resource.h        control and command IDs
src/version.h         the version number - the only place it is spelled out
src/BazisLib/         VENDORED - see below
third_party/wtl-*/    downloaded by the build script
```

## Do not hand-edit `src/BazisLib`

It is a vendored subset of an upstream LGPL library, refreshed with
`python build.py --sync-bazislib <checkout>`. The exact file list lives in `build.py`
(`BAZISLIB_HEADERS` / `BAZISLIB_SOURCES`). If new code includes a BazisLib header that is
not vendored, the compile fails with a plain "cannot open include file" — add it to that
list and re-sync rather than copying the file in by hand.

Same for `third_party/` — it is downloaded and checksum-verified, never edited.

## Code style

Match the surrounding code; it is consistent and old-school.

- **Tabs** for indentation, Allman braces.
- `CMainDlg`-style class names, `m_` members, `s_` file-scope statics, `dw`/`lp`/`psz`/`tsz`
  Hungarian on Win32-facing locals. Enum values are prefixed (`prefDark`, `icoGreen`,
  `vcfPathChanged`).
- Unicode build: use `_T("...")` and `TCHAR` in application code. Where an API is
  explicitly wide (`GetClassNameW`, `DrawTextW`, `lstrcmpiW`), say so and use `WCHAR`.
- Comments explain *why*, not what. Doxygen `//!` for declarations in headers. Keep the
  density low — the existing code comments the surprising parts and nothing else.
- Handlers go in the message map in the same order as their declarations.

## High DPI

The process is **system DPI aware** (`src/srvman.manifest`, merged in by `/MANIFESTINPUT`).
Without that declaration Windows renders the app at 96 DPI and stretches the result, which
is why the text used to be blurry above 100%.

- Dialog templates need nothing: USER32 sizes dialog units from the dialog font, which it
  picks at the DPI in effect. `SystemParametersInfo` and `GetSystemMetrics` likewise
  return scaled values now — that is what `SM_CXSMICON` is for in the image list code.
- Anything the code states in **pixels** must go through `Dpi::Scale()` (`src/Dpi.h`). The
  list view column defaults in `MainDlg.cpp` are the existing case; they also go through
  `Font::ScaleTextWidth()`, which is the orthogonal question of which font is in use.
- **Sizes read back from the registry are already in device pixels** and must not be
  scaled again — `SaveState()` writes what `GetColumnWidth()` reported. Only the built-in
  defaults are 96 DPI values; that is why the width is scaled on the `ReadValue()` failure
  path and nowhere else.
- `Dpi::Current()` caches, which is correct only as long as awareness stays *system*.
  Moving to per-monitor means a `WM_DPICHANGED` handler that rebuilds the image lists,
  the menu font in `Theme.cpp` and the column widths — do not declare `permonitorv2` in
  the manifest without writing it.

## The font module

`src/Font.h` + `src/Font.cpp` implement View → Font. The one thing to understand before
touching it: **a dialog's font lives in its template, not in its windows.** USER32 derives
the dialog base units from it while instantiating the template, and every coordinate in
`srvman.rc` is in those units. So:

- The font is applied by **rewriting the font block of the template** on its way to
  `DialogBoxIndirectParam()` — `Font::PatchTemplate()` plus the `CUserFontDialogImpl<T>`
  mixin, which every dialog derives from instead of `CDialogImpl<T>`. With no custom font
  it defers to the base class and the resource is used untouched.
- `DS_FIXEDSYS` has to be **cleared** when the block is rewritten. Next to `DS_SETFONT` it
  is `DS_SHELLFONT`, which tells USER32 to ignore the typeface and substitute the shell
  dialog font. `IDD_SERVICEPROPS` carries it.
- The control array is copied over verbatim, so it has to keep its DWORD phase. Both the
  old and the new offsets are DWORD-aligned, which is what makes that safe — preserve that
  property if you touch the copy.
- **A live dialog cannot be re-fonted.** `WM_SETFONT` would enlarge the text inside
  rectangles already computed from the old font. Changing it ends the main dialog with
  `MainDlgRestart` and `_tWinMain` builds a new one; a new `CMainDlg` each time round,
  because the ATL thunk is single-use.
- The service list needs nothing: controls inherit the dialog font at creation.
- **The menu bar is out of reach** and staying that way. It is non-client area painted by
  USER32 from the system-wide menu font; owner-drawing every menu would mean re-doing
  check marks, mnemonics, the accelerator column and the greyed state by hand, in both
  themes. The upside is that View → Font → Default is reachable at any font size.
- The preference is under **HKCU**, unlike every other setting. `m_ParamsRoot` is HKLM and
  opened for write, which no-ops unelevated (see the landmine below); a font the user
  cannot keep would be worse than none.

## Landmines

**Column widths have two fields for a reason.** `_ColumnDescription::DefaultWidth` is the
width the column was authored with — 96 DPI, and the font `srvman.rc` names — and is never
written to. `Width` is what is in effect: either what a previous session saved, or the
default put through `Dpi::Scale()` and `Font::ScaleTextWidth()`. `s_Columns` is file-scope
static and the main dialog is re-created on a font change, so scaling in place would
compound on the second instantiation.

**`*(BOOL *)NULL` handler calls.** `MainDlg.cpp` and `PropertiesDlg.cpp` call handlers
directly with a null-dereferenced `bHandled` argument (search for `*(BOOL *)`). These
survive only because those specific handlers never touch `bHandled`. If you add a write to
`bHandled` in `CMainDlg::OnSelChanged`, `CPropertiesDlg::OnRawPathChanged`,
`CPropertiesDlg::OnWin32PathChanged` or `CPropertiesDlg::OnServiceTypeChanged`, you
introduce a null-pointer write.

**Settings silently do not persist when unelevated.** Everything lives under
`HKLM\SOFTWARE\SysProgs\SrvMan`, and `BazisLib::Win32::RegistryKey`'s default constructor
asks for `KEY_ALL_ACCESS`. srvman runs *asInvoker*, so unelevated that call fails, the
handle stays null, and every read and write becomes a no-op. This is pre-existing
behaviour, not a bug to fix in passing. Pass `RequireWrite = false` when you only need to
read — that path works unelevated.

**`srvman.rc` has exactly one `IDR_MENU1`**, and it sits inside the `LANG_RUSSIAN` block at
the top of the file. The English resources reference it by ID and get it through resource
language fallback. Edit it once, in that block. The file is pure 7-bit ASCII despite the
`#pragma code_page(1251)`; keep it that way.

**`rc.exe` does not concatenate adjacent string literals.** `"a" MACRO "b"` is a syntax
error in a resource script, not a spliced string — which is why `src/version.h` spells the
version out as a whole literal (`SRVMAN_VERSION_STR`) instead of gluing the three numeric
macros together, and why the About box text is set at runtime in `CAboutDlg::OnInitDialog`
against an `IDC_ABOUTTEXT` control that the dialog template leaves empty. In C++ the same
macro needs `SRVMAN_VERSION_STRT` for a TCHAR literal: `_T(SRVMAN_VERSION_STR)` pastes `L`
onto the macro name and fails to compile.

**`resource.h` has intentional duplicate IDs** — `IDC_DELETESERVICE`/`IDC_INTERNALNAME` are
both 1008, `IDC_EXIT`/`IDC_VISIBLENAME` both 1009. They belong to different dialogs, so it
works. Do not renumber them "to clean up". When adding command IDs, take the next value and
bump `_APS_NEXT_COMMAND_VALUE`.

**`WM_SETTINGCHANGE` `lParam` is frequently NULL.** Null-check before any string compare.

**Console mode never initialises the GUI.** `_tWinMain` returns from the `ConsoleMain`
branch before `Theme::Init()` and before the dialog is created. Do not add GUI setup above
that branch.

## The theme module

`src/Theme.h` + `src/Theme.cpp` implement Light / Dark / Use-system. Rules for changing it:

- **Light mode must remain a total no-op.** `HandleCtlColor()` returns false,
  `ApplyToControl()` passes `NULL` to `SetWindowTheme` (which reverts to the control's
  default — *not* the same as naming a light theme), and no custom draw runs. That property
  is what makes the Windows 7 path safe without testing it on Windows 7.
- **Everything version-specific is resolved at runtime.** Build detection uses
  `RtlGetNtVersionNumbers` (`GetVersionEx` lies without a `<supportedOS>` manifest, and
  this binary has none). Dark mode is hard-gated on build >= 17763.
- **uxtheme ordinals 135, 104 and 136 are undocumented.** They are resolved by
  `GetProcAddress` against an already-loaded `uxtheme.dll`; a null result must degrade to
  "dark unsupported", never to a crash. Do not add the ordinal-49 `OpenNcThemeData` IAT
  hook — it is the most fragile trick in the Win32 dark-mode folklore and buys one
  scrollbar here.
- **`WM_UAHDRAWMENU` / `WM_UAHDRAWMENUITEM` (0x0091 / 0x0092) are undocumented too.** They
  are what makes the menu bar dark; the structs in `Theme.cpp` are reverse-engineered. The
  1px separator below the bar is drawn by default non-client painting, so
  `DrawMenuBarBottomLine()` has to run *after* `DefWindowProc` — see `CMainDlg::OnNcPaint`.
- `THEME_CTLCOLOR_HANDLERS()` and `THEME_MENUBAR_HANDLERS()` go **first** inside
  `BEGIN_MSG_MAP` and rely on ATL's parameter names (`hWnd`, `uMsg`, `wParam`, `lParam`,
  `lResult`). Returning a brush handle from a plain `MESSAGE_HANDLER` works because ATL's
  `DialogProc` special-cases the `WM_CTLCOLOR*` range and returns the result directly.
- **Every top-level window re-applies unconditionally on `WM_SETTINGCHANGE`**, rather than
  gating on `RefreshFromSystem()`'s return value. Whichever window is served first would
  otherwise absorb the state change and leave the others stale.
- New dialogs need three things: the macro in the message map, `Theme::ApplyToWindow(m_hWnd)`
  at the end of `OnInitDialog`, and a `WM_SETTINGCHANGE` handler.

Brushes and the menu font are cached for the life of the process and freed in
`Theme::Shutdown()`. Never create a brush inside a `WM_CTLCOLOR*` handler — USER32 keeps
using the handle after you return.

## Out of scope by nature

`MessageBox`, `CFileDialog` and the console window are owned by the system or the shell.
They follow Explorer, not srvman, and no amount of `SetPreferredAppMode` changes that.
Do not try to theme them.
