#!/usr/bin/env python3
"""
build.py -- build Service Manager (srvman) as a native x64 or x86 binary.

The original project ships only a Visual Studio 2008 ``.vcproj`` (srvman.sln also
references the long-gone ``bzslib`` layout).  Modern MSBuild cannot consume that
format, so this script drives the compiler directly:

  * locates a Visual Studio installation with the MSVC toolset + ATL,
  * provisions the WTL headers the UI code needs (WTL is not part of the SDK),
  * compiles the handful of BazisLib translation units srvman links against,
  * compiles srvman's own sources and the resource script,
  * links ``srvman.exe`` with an embedded manifest (comctl32 v6).

BazisLib is vendored: the exact subset srvman needs lives in ``src/BazisLib``,
so a build requires no checkout next to this repository.  See
``src/BazisLib/UPSTREAM.md`` for provenance and how to refresh it.

Layout
------
  <repo>/src/...             srvman sources
  <repo>/src/BazisLib/...    vendored BazisLib subset (LGPL-3.0)
  <repo>/third_party/wtl     WTL headers, downloaded on first run (--wtl to override)
  <repo>/build/<arch>/<Config>  build output
  <repo>/dist               release archives, named after src/version.h (--package)

Usage
-----
  python build.py                          # Release, x64
  python build.py --arch x86               # Release, 32-bit
  python build.py --arch both              # both architectures
  python build.py --config Debug
  python build.py --rebuild --verbose
  python build.py --arch both --package    # build and zip up the binaries
  python build.py --bazislib ../BazisLib            # build against a checkout
  python build.py --sync-bazislib ../BazisLib       # refresh the vendored copy
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
SRC_DIR = REPO_ROOT / "src"

# The version number lives in the source tree, not here: the resource script and
# the About box need it too.  --package names the archives after it.
VERSION_HEADER = SRC_DIR / "version.h"
DIST_DIR = REPO_ROOT / "dist"

# The slice of BazisLib that lives in this repository, so a build needs nothing
# but a compiler.  Refresh it from an upstream checkout with --sync-bazislib.
VENDORED_BAZISLIB = SRC_DIR / "BazisLib"

# --------------------------------------------------------------------------
# Targets
# --------------------------------------------------------------------------

ARCHITECTURES = ("x64", "x86")

# vcvarsall.bat target arguments, most preferred first.  The x86 build uses the
# 64-bit hosted cross compiler where possible: it is what modern Visual Studio
# installs ship by default, and it does not depend on the 32-bit host tools
# being present.  Plain "x86" is the fallback for a 32-bit host.
VCVARS_ARGS = {
    "x64": ["x64"],
    "x86": ["x64_x86", "x86"] if platform.machine().endswith("64") else ["x86"],
}

LINK_MACHINE = {"x64": "X64", "x86": "X86"}

# --------------------------------------------------------------------------
# Sources
# --------------------------------------------------------------------------

SRVMAN_SOURCES = [
    "cmdline.cpp",
    "MainDlg.cpp",
    "PropertiesDlg.cpp",
    "runassrv.cpp",
    "srvman.cpp",
    "stdafx.cpp",
    "Theme.cpp",
]

# Only the user-mode Win32 parts of BazisLib are needed; every other .cpp in the
# library guards its whole body with a platform #if and would compile to nothing.
# win32_socket.cpp is deliberately absent: srvman does no networking and that TU
# pulls in BazisLib's DNS resolver.
BAZISLIB_SOURCES = [
    "bzscore/Win32/path.cpp",
    "bzscore/Win32/registry_win32.cpp",
    "bzscore/Win32/security.cpp",
    "bzscore/Win32/win32_status.cpp",
    "bzscore/Win32/win32_string.cpp",
    "bzshlp/Win32/services.cpp",
]

# Exact transitive include closure of the sources above plus srvman's own
# <bzscore/...> / <bzshlp/...> includes, as reported by cl /showIncludes.
# The upstream directory layout is preserved so the library's own relative
# includes keep resolving.
BAZISLIB_HEADERS = [
    "bzscore/Win32/atomic.h",
    "bzscore/Win32/datetime.h",
    "bzscore/Win32/file.h",
    "bzscore/Win32/path.h",
    "bzscore/Win32/registry.h",
    "bzscore/Win32/security.h",
    "bzscore/Win32/security_common.h",
    "bzscore/Win32/status_defs.h",
    "bzscore/Win32/stdafx.h",
    "bzscore/Win32/sync.h",
    "bzscore/Win32/tls.h",
    "bzscore/assert.h",
    "bzscore/atomic.h",
    "bzscore/autolock.h",
    "bzscore/buffer.h",
    "bzscore/cmndef.h",
    "bzscore/datetime.h",
    "bzscore/file.h",
    "bzscore/fileflags.h",
    "bzscore/filehlp.h",
    "bzscore/objmgr.h",
    "bzscore/path.h",
    "bzscore/platform.h",
    "bzscore/splitstr.h",
    "bzscore/status.h",
    "bzscore/stdafx.h",
    "bzscore/strbase.h",
    "bzscore/strfast.h",
    "bzscore/string.h",
    "bzscore/strop.h",
    "bzscore/sync.h",
    "bzscore/tchar_compat.h",
    "bzshlp/Win32/cmndef.h",
    "bzshlp/Win32/services.h",
    "bzshlp/Win32/stdafx.h",
]

# BazisLib is LGPL-3.0; the licence travels with the vendored sources.
BAZISLIB_EXTRA_FILES = ["LICENSE"]

BAZISLIB_UPSTREAM_URL = "https://github.com/sysprogs/BazisLib"

SYSTEM_LIBS = [
    "kernel32.lib",
    "user32.lib",
    "gdi32.lib",
    "advapi32.lib",
    "shell32.lib",
    "shlwapi.lib",
    "comctl32.lib",
    "comdlg32.lib",
    "uxtheme.lib",
    "dwmapi.lib",
    "ole32.lib",
    "oleaut32.lib",
    "uuid.lib",
    "version.lib",
    "ws2_32.lib",
]

# --------------------------------------------------------------------------
# WTL
# --------------------------------------------------------------------------

WTL_VERSION = "10.0.10320"
WTL_ARCHIVE = "WTL10_10320_Release.zip"
WTL_SHA512 = (
    "086a6cf6a49a4318a8c519136ba6019ded7aa7f2c1d85f78c30b21183654537b"
    "3428a400a64fcdacba3a7a10a9ef05137b6f2119f59594da300d55f9ebfb1309"
)
WTL_URLS = [
    "https://downloads.sourceforge.net/project/wtl/WTL%2010/"
    "WTL%2010.0.10320%20Release/WTL10_10320_Release.zip",
    "https://sourceforge.net/projects/wtl/files/WTL%2010/"
    "WTL%2010.0.10320%20Release/WTL10_10320_Release.zip/download",
]


class BuildError(Exception):
    pass


def log(msg: str) -> None:
    print(msg, flush=True)


# --------------------------------------------------------------------------
# Version
# --------------------------------------------------------------------------


def read_version() -> str:
    """Return the version string defined in src/version.h, e.g. "1.1.0".

    The header spells the number twice -- a numeric triplet for VERSIONINFO and a
    ready-made string literal, because rc.exe does not concatenate adjacent string
    literals and so cannot assemble one from the parts.  Both spellings are parsed
    here and checked against each other, so the two cannot quietly drift apart.
    """
    try:
        text = VERSION_HEADER.read_text(encoding="utf-8")
    except OSError as exc:
        raise BuildError(f"cannot read {VERSION_HEADER}: {exc}") from exc

    def define(name: str) -> str:
        match = re.search(rf"^#define\s+{name}\s+(\S+)", text, re.MULTILINE)
        if not match:
            raise BuildError(f"{VERSION_HEADER} does not define {name}")
        return match.group(1)

    version = define("SRVMAN_VERSION_STR").strip('"')
    triplet = ".".join(
        define(f"SRVMAN_VERSION_{part}") for part in ("MAJOR", "MINOR", "PATCH")
    )
    if version != triplet:
        raise BuildError(
            f"{VERSION_HEADER} disagrees with itself: "
            f"SRVMAN_VERSION_STR is {version!r}, but the MAJOR/MINOR/PATCH macros "
            f"spell {triplet!r}. Update both."
        )
    return version


# --------------------------------------------------------------------------
# Visual Studio discovery
# --------------------------------------------------------------------------


def find_vswhere() -> Path:
    for env in ("ProgramFiles(x86)", "ProgramFiles"):
        base = os.environ.get(env)
        if not base:
            continue
        candidate = Path(base) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if candidate.is_file():
            return candidate
    found = shutil.which("vswhere")
    if found:
        return Path(found)
    raise BuildError(
        "vswhere.exe not found. Install Visual Studio 2017 or newer "
        "(or the Build Tools) and retry."
    )


def find_vcvars() -> Path:
    """Return the path to vcvarsall.bat of a VS install that has MSVC + ATL.

    The x86.x64 toolset component covers both targets this script builds, and the
    ATL component ships the 32-bit and 64-bit ATL libraries together, so one
    query serves every architecture.
    """
    vswhere = find_vswhere()
    cmd = [
        str(vswhere),
        "-latest",
        "-prerelease",
        "-products", "*",
        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-requires", "Microsoft.VisualStudio.Component.VC.ATL",
        "-format", "json",
        "-utf8",
    ]
    out = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8").stdout
    installs = json.loads(out) if out.strip() else []
    if not installs:
        # Retry without the ATL requirement so we can give a precise diagnostic.
        cmd_noatl = [c for c in cmd if c != "Microsoft.VisualStudio.Component.VC.ATL"]
        out = subprocess.run(
            cmd_noatl, capture_output=True, text=True, encoding="utf-8"
        ).stdout
        if json.loads(out) if out.strip() else []:
            raise BuildError(
                "Visual Studio was found, but the ATL component is missing.\n"
                "srvman is an ATL/WTL application. Install "
                '"C++ ATL for latest v143 build tools (x86 & x64)" '
                "via the Visual Studio Installer."
            )
        raise BuildError(
            "No Visual Studio installation with the MSVC x86/x64 toolset was found."
        )

    path = Path(installs[0]["installationPath"])
    vcvars = path / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
    if not vcvars.is_file():
        raise BuildError(f"vcvarsall.bat not found under {path}")
    log(f"[vs]   {installs[0].get('displayName', path.name)}  ({path})")
    return vcvars


def _try_msvc_env(vcvars: Path, target: str) -> dict[str, str] | None:
    marker = "__SRVMAN_ENV__"
    # Passed as a raw string: cmd.exe /s /c strips exactly one layer of quotes,
    # which keeps the quoted vcvarsall path intact.
    command = f'cmd.exe /s /c "call "{vcvars}" {target} >nul && echo {marker} && set"'
    proc = subprocess.run(
        command,
        capture_output=True,
        text=True,
        encoding="mbcs",
        errors="replace",
    )
    if proc.returncode != 0:
        return None

    env: dict[str, str] = {}
    seen_marker = False
    for line in proc.stdout.splitlines():
        if not seen_marker:
            seen_marker = line.strip() == marker
            continue
        key, sep, value = line.partition("=")
        if sep:
            env[key] = value
    if "INCLUDE" not in env or "LIB" not in env:
        return None
    return env


def msvc_env(vcvars: Path, arch: str) -> dict[str, str]:
    """Run vcvarsall.bat for the given target and capture the resulting env."""
    for target in VCVARS_ARGS[arch]:
        env = _try_msvc_env(vcvars, target)
        if env is not None:
            return env
    raise BuildError(
        f"vcvarsall.bat produced no usable {arch} environment "
        f"(tried: {', '.join(VCVARS_ARGS[arch])}).\n"
        "For a 32-bit build, make sure the MSVC x86 target and the 32-bit ATL "
        "libraries are installed."
    )


# --------------------------------------------------------------------------
# WTL provisioning
# --------------------------------------------------------------------------


def _sha512(path: Path) -> str:
    digest = hashlib.sha512()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _patch_wtl(include_dir: Path) -> None:
    """WTL 10.0.10320 has a Unicode bug in atlmisc.h (SF bug #329)."""
    atlmisc = include_dir / "atlmisc.h"
    if not atlmisc.is_file():
        return
    text = atlmisc.read_text(encoding="utf-8", errors="surrogateescape")
    broken = 'ATL::CString strResult("file://");'
    if broken in text:
        atlmisc.write_text(
            text.replace(broken, 'ATL::CString strResult(_T("file://"));'),
            encoding="utf-8",
            errors="surrogateescape",
        )
        log("[wtl]  applied atlmisc.h Unicode fix")


def _locate_existing_wtl() -> Path | None:
    """Pick up WTL from the environment or a vcpkg installation, if present."""
    for var in ("WTL_INCLUDE", "WTLDIR", "WTL_ROOT"):
        raw = os.environ.get(var)
        if not raw:
            continue
        for candidate in (Path(raw), Path(raw) / "Include", Path(raw) / "include"):
            if (candidate / "atlapp.h").is_file():
                return candidate

    vcpkg = shutil.which("vcpkg")
    if vcpkg:
        root = Path(vcpkg).resolve().parent
        for triplet in ("x64-windows", "x64-windows-static", "x86-windows"):
            candidate = root / "installed" / triplet / "include"
            if (candidate / "atlapp.h").is_file():
                return candidate
    return None


def ensure_wtl(explicit: Path | None, offline: bool) -> Path:
    if explicit:
        for candidate in (explicit, explicit / "Include", explicit / "include"):
            if (candidate / "atlapp.h").is_file():
                return candidate
        raise BuildError(f"atlapp.h not found under --wtl path: {explicit}")

    local = REPO_ROOT / "third_party" / f"wtl-{WTL_VERSION}" / "Include"
    if (local / "atlapp.h").is_file():
        return local

    found = _locate_existing_wtl()
    if found:
        log(f"[wtl]  using existing headers: {found}")
        return found

    if offline:
        raise BuildError(
            "WTL headers not found and --offline was given.\n"
            "Pass --wtl <path-to-WTL> or set WTL_INCLUDE."
        )

    log(f"[wtl]  downloading WTL {WTL_VERSION} ...")
    local.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / WTL_ARCHIVE
        last_error: Exception | None = None
        for url in WTL_URLS:
            try:
                request = urllib.request.Request(
                    url, headers={"User-Agent": "srvman-build/1.0"}
                )
                with urllib.request.urlopen(request, timeout=120) as response, \
                        archive.open("wb") as out:
                    shutil.copyfileobj(response, out)
                break
            except Exception as exc:  # network hiccup, mirror down, ...
                last_error = exc
        else:
            raise BuildError(
                f"Could not download WTL: {last_error}\n"
                "Download WTL manually and pass --wtl <path>."
            )

        actual = _sha512(archive)
        if actual != WTL_SHA512:
            raise BuildError(
                "WTL archive checksum mismatch.\n"
                f"  expected {WTL_SHA512}\n  actual   {actual}"
            )

        with zipfile.ZipFile(archive) as zf:
            members = [
                n for n in zf.namelist()
                if n.lower().startswith("include/") and n.lower().endswith(".h")
            ]
            if not members:
                raise BuildError("WTL archive does not contain an Include/ folder.")
            zf.extractall(local.parent, members)

    _patch_wtl(local)
    log(f"[wtl]  installed into {local}")
    return local


# --------------------------------------------------------------------------
# Compilation
# --------------------------------------------------------------------------


class Compiler:
    def __init__(self, env: dict[str, str], verbose: bool) -> None:
        self.env = env
        self.verbose = verbose
        self._tools: dict[str, str] = {}

    def tool(self, name: str) -> str:
        """Resolve a toolchain executable.

        CreateProcess searches the *parent* process' PATH, not the environment
        handed to subprocess, so every tool has to be resolved explicitly
        against the environment vcvarsall produced.
        """
        if name not in self._tools:
            # `set` reports the search path as "Path" on Windows, not "PATH".
            search = next(
                (v for k, v in self.env.items() if k.upper() == "PATH"), ""
            )
            found = shutil.which(name, path=search)
            if not found:
                raise BuildError(
                    f"{name} not found in the Visual Studio build environment."
                )
            self._tools[name] = found
        return self._tools[name]

    def run(self, args: list[str], cwd: Path | None = None) -> None:
        args = [self.tool(args[0]), *args[1:]]
        if self.verbose:
            log("  > " + subprocess.list2cmdline(args))
        proc = subprocess.run(
            args,
            cwd=str(cwd) if cwd else None,
            env=self.env,
            capture_output=True,
            text=True,
            encoding="mbcs",
            errors="replace",
        )
        output = (proc.stdout or "") + (proc.stderr or "")
        if proc.returncode != 0:
            sys.stdout.write(output)
            raise BuildError(f"{Path(args[0]).name} failed with exit code {proc.returncode}")
        # cl.exe echoes the source file name; keep that, drop the noise.
        for line in output.splitlines():
            if line.strip():
                log("  " + line.rstrip())


# --------------------------------------------------------------------------
# Vendoring
# --------------------------------------------------------------------------


def _upstream_revision(upstream: Path) -> str:
    try:
        proc = subprocess.run(
            ["git", "-C", str(upstream), "log", "-1", "--format=%H %ad", "--date=short"],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        if proc.returncode == 0 and proc.stdout.strip():
            return proc.stdout.strip()
    except OSError:
        pass
    return "unknown (not a git checkout)"


def sync_bazislib(upstream: Path, dest: Path) -> None:
    """Refresh the vendored BazisLib subset from an upstream checkout."""
    if not (upstream / "bzscore" / "cmndef.h").is_file():
        raise BuildError(f"{upstream} does not look like a BazisLib checkout.")

    wanted = BAZISLIB_HEADERS + BAZISLIB_SOURCES + BAZISLIB_EXTRA_FILES
    copied = 0
    for rel in wanted:
        source = upstream / rel
        if not source.is_file():
            raise BuildError(f"missing upstream file: {source}")
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.is_file() or target.read_bytes() != source.read_bytes():
            shutil.copyfile(source, target)
            copied += 1

    revision = _upstream_revision(upstream)
    (dest / "UPSTREAM.md").write_text(
        "# Vendored BazisLib subset\n"
        "\n"
        f"Upstream: {BAZISLIB_UPSTREAM_URL}\n"
        f"Revision: {revision}\n"
        "Licence:  LGPL-3.0 (see LICENSE in this directory)\n"
        "\n"
        "This is not a full copy of BazisLib. It contains exactly the files\n"
        "srvman compiles or includes -- the transitive closure of\n"
        "`<bzscore/...>` and `<bzshlp/Win32/services.h>` for a Win32 user-mode\n"
        "build. The upstream directory layout is preserved so the library's own\n"
        "relative includes keep resolving unchanged. The same file set serves\n"
        "both the x64 and the x86 target.\n"
        "\n"
        f"{len(BAZISLIB_SOURCES)} source files, {len(BAZISLIB_HEADERS)} headers.\n"
        "\n"
        "## Refreshing\n"
        "\n"
        "Do not edit these files by hand. Point the build script at an upstream\n"
        "checkout instead:\n"
        "\n"
        "    python build.py --sync-bazislib <path-to-BazisLib>\n"
        "\n"
        "The authoritative file list lives in `build.py`\n"
        "(`BAZISLIB_HEADERS` / `BAZISLIB_SOURCES`). If a future change to srvman\n"
        "pulls in a header that is not vendored, the compile fails with a plain\n"
        "'cannot open include file' -- add it to that list and re-sync.\n",
        encoding="utf-8",
    )
    log(
        f"[sync] {len(wanted)} file(s) from {upstream} -> {dest} "
        f"({copied} updated)"
    )
    log(f"[sync] upstream revision: {revision}")


def newest_header_mtime(*roots: Path) -> float:
    newest = 0.0
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.suffix.lower() in (".h", ".hpp", ".inl"):
                try:
                    newest = max(newest, path.stat().st_mtime)
                except OSError:
                    pass
    return newest


def obj_path(obj_dir: Path, source: Path, base: Path) -> Path:
    rel = source.relative_to(base)
    flat = "_".join(rel.parts)
    return obj_dir / (flat[: -len(source.suffix)] + ".obj")


# --------------------------------------------------------------------------
# Build
# --------------------------------------------------------------------------


def build_arch(
    arch: str,
    args: argparse.Namespace,
    bazislib: Path,
    wtl_include: Path,
    vcvars: Path,
) -> Path:
    """Compile and link one architecture. Returns the path to srvman.exe."""
    build_dir = REPO_ROOT / "build" / arch / args.config
    env = msvc_env(vcvars, arch)

    if args.rebuild:
        shutil.rmtree(build_dir, ignore_errors=True)
    obj_dir = build_dir / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)

    debug = args.config == "Debug"
    cc = Compiler(env, args.verbose)

    includes = [SRC_DIR, bazislib, wtl_include]
    defines = [
        "WIN32",          # BazisLib keys BZSLIB_WINUSER off this, x64 included
        "_WINDOWS",
        "STRICT",
        "UNICODE",
        "_UNICODE",
        "_CRT_SECURE_NO_WARNINGS",
        "_WINSOCK_DEPRECATED_NO_WARNINGS",
        "SECURITY_WIN32",  # bzscore/security.cpp -> sspi.h
        "WINVER=0x0601",   # Windows 7; WTL rejects anything below 0x0501
        "_WIN32_WINNT=0x0601",
        "_DEBUG" if debug else "NDEBUG",
    ]

    cflags = [
        "/nologo", "/c", "/EHsc", "/W3", "/GR", "/FS",
        "/Zc:forScope", "/Zc:wchar_t",
        "/wd4996",  # deprecated CRT / Win32 calls
        "/wd4005",  # BazisLib's own stdafx.h redefines a few of our -D macros
    ]
    cflags += ["/Od", "/MDd", "/RTC1", "/Zi"] if debug else ["/O2", "/MT", "/Gy", "/GS"]
    cflags += [f"/D{d}" for d in defines]
    cflags += [f"/I{p}" for p in includes]
    if debug:
        cflags += [f"/Fd{build_dir / 'srvman.pdb'}"]

    # ---- collect work -----------------------------------------------------
    jobs: list[tuple[Path, Path]] = []
    for name in SRVMAN_SOURCES:
        source = SRC_DIR / name
        if not source.is_file():
            raise BuildError(f"missing source file: {source}")
        jobs.append((source, obj_path(obj_dir, source, SRC_DIR)))
    for rel in BAZISLIB_SOURCES:
        source = bazislib / rel
        if not source.is_file():
            raise BuildError(f"missing BazisLib source: {source}")
        jobs.append((source, obj_path(obj_dir, source, bazislib)))

    header_stamp = newest_header_mtime(SRC_DIR, bazislib / "bzscore", bazislib / "bzshlp")

    def needs_build(source: Path, obj: Path) -> bool:
        if not obj.is_file():
            return True
        obj_time = obj.stat().st_mtime
        return obj_time < max(source.stat().st_mtime, header_stamp)

    pending = [(s, o) for s, o in jobs if needs_build(s, o)]
    log(f"[cc]   {arch}: {len(pending)} of {len(jobs)} translation unit(s) to compile")

    errors: list[BaseException] = []

    def compile_one(item: tuple[Path, Path]) -> None:
        source, obj = item
        cc.run(
            ["cl.exe", *cflags, f"/Fo{obj}", str(source)],
            cwd=source.parent,  # BazisLib .cpp files include "stdafx.h" relatively
        )

    if pending:
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            for future in [pool.submit(compile_one, item) for item in pending]:
                try:
                    future.result()
                except BaseException as exc:  # noqa: BLE001 - report all failures
                    errors.append(exc)
    if errors:
        for exc in errors:
            log(f"  {type(exc).__name__}: {exc}")
        raise BuildError(f"{len(errors)} translation unit(s) failed to compile")

    # ---- resources --------------------------------------------------------
    res_file = build_dir / "srvman.res"
    rc_source = SRC_DIR / "srvman.rc"
    if not res_file.is_file() or res_file.stat().st_mtime < max(
        rc_source.stat().st_mtime, header_stamp
    ):
        log("[rc]   srvman.rc")
        cc.run(
            [
                "rc.exe", "/nologo",
                "/l", "0x409",
                f"/d{'_DEBUG' if debug else 'NDEBUG'}",
                f"/I{SRC_DIR}",
                f"/I{wtl_include}",
                f"/fo{res_file}",
                str(rc_source),
            ],
            cwd=SRC_DIR,
        )

    # ---- link -------------------------------------------------------------
    exe = build_dir / "srvman.exe"
    inputs = [o for _, o in jobs] + [res_file]
    up_to_date = exe.is_file() and all(
        exe.stat().st_mtime >= i.stat().st_mtime for i in inputs
    )

    log(f"[link] {exe}" + ("  (up to date)" if up_to_date else ""))
    link_flags = [
        "/nologo",
        f"/MACHINE:{LINK_MACHINE[arch]}",
        "/SUBSYSTEM:WINDOWS",
        "/MANIFEST:EMBED",
        "/DYNAMICBASE", "/NXCOMPAT",
        f"/OUT:{exe}",
    ]
    if arch == "x86":
        # 64-bit images carry unwind data instead; /SAFESEH is x86-only and the
        # linker rejects it outright on other targets.
        link_flags.append("/SAFESEH")
    if debug:
        link_flags += ["/DEBUG", f"/PDB:{build_dir / 'srvman.pdb'}", "/INCREMENTAL:NO"]
    else:
        link_flags += ["/OPT:REF", "/OPT:ICF", "/INCREMENTAL:NO"]

    if not up_to_date:
        cc.run(
            ["link.exe", *link_flags,
             *[str(o) for _, o in jobs], str(res_file), *SYSTEM_LIBS]
        )

    return exe


# --------------------------------------------------------------------------
# Packaging
# --------------------------------------------------------------------------


def package(
    version: str,
    config: str,
    built: list[tuple[str, Path]],
    bazislib: Path,
) -> list[Path]:
    """Zip each built binary into dist/, one archive per architecture.

    The version is the suffix that distinguishes one release from the next:
    ``srvman-1.1.0-x64.zip``, unpacking to a directory of the same name.  Debug
    archives carry the configuration too, so they cannot be mistaken for a
    release build.
    """
    DIST_DIR.mkdir(parents=True, exist_ok=True)

    archives: list[Path] = []
    for arch, exe in built:
        stem = f"srvman-{version}-{arch}"
        if config != "Release":
            stem += f"-{config}"
        archive = DIST_DIR / f"{stem}.zip"

        payload: list[tuple[Path, str]] = [(exe, exe.name)]
        pdb = exe.with_suffix(".pdb")
        if pdb.is_file():
            payload.append((pdb, pdb.name))
        payload.append((REPO_ROOT / "README.md", "README.md"))
        # BazisLib is linked statically and is LGPL-3.0, so its licence has to
        # travel with the binary, not just with the sources.
        licence = bazislib / "LICENSE"
        if licence.is_file():
            payload.append((licence, "BazisLib-LICENSE.txt"))
        else:
            log(f"[pack] WARNING: {licence} not found; archive ships without it")

        archive.unlink(missing_ok=True)
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
            for source, name in payload:
                zf.write(source, f"{stem}/{name}")

        log(f"[pack] {archive}  ({archive.stat().st_size:,} bytes)")
        archives.append(archive)

    return archives


def main() -> int:
    version = read_version()

    parser = argparse.ArgumentParser(
        description=f"Build srvman {version} for Windows (x64 or x86).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--version", action="version", version=f"srvman {version}",
        help="print the version from src/version.h and exit",
    )
    parser.add_argument(
        "--arch", choices=(*ARCHITECTURES, "both"), default="x64",
        help="target architecture (default: x64)",
    )
    parser.add_argument(
        "--config", choices=("Release", "Debug"), default="Release",
        help="build configuration (default: Release)",
    )
    parser.add_argument(
        "--bazislib", type=Path, default=None,
        help="build against an external BazisLib checkout instead of the "
             "vendored copy in src/BazisLib",
    )
    parser.add_argument(
        "--sync-bazislib", type=Path, metavar="UPSTREAM", default=None,
        help="refresh src/BazisLib from an upstream BazisLib checkout and exit",
    )
    parser.add_argument(
        "--wtl", type=Path, default=None,
        help="path to WTL (its Include folder, or the folder containing it)",
    )
    parser.add_argument(
        "--offline", action="store_true",
        help="never download anything; fail if WTL is missing",
    )
    parser.add_argument(
        "--rebuild", action="store_true", help="discard existing objects first",
    )
    parser.add_argument(
        "--package", action="store_true",
        help=f"zip each built binary into dist/srvman-{version}-<arch>.zip",
    )
    parser.add_argument(
        "--clean", action="store_true", help="remove the build directory and exit",
    )
    parser.add_argument(
        "-j", "--jobs", type=int, default=os.cpu_count() or 4,
        help="parallel compiler jobs",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if os.name != "nt":
        raise BuildError("srvman is a Win32 application; this script requires Windows.")

    if args.clean:
        shutil.rmtree(REPO_ROOT / "build", ignore_errors=True)
        shutil.rmtree(DIST_DIR, ignore_errors=True)
        log("[clean] removed build/ and dist/")
        return 0

    if args.sync_bazislib:
        sync_bazislib(args.sync_bazislib.resolve(), VENDORED_BAZISLIB)
        return 0

    bazislib = (args.bazislib or VENDORED_BAZISLIB).resolve()
    if not (bazislib / "bzscore" / "cmndef.h").is_file():
        raise BuildError(
            f"BazisLib not found at {bazislib}\n"
            "The vendored copy lives in src/BazisLib; restore it with\n"
            "  python build.py --sync-bazislib <path-to-BazisLib>\n"
            "or point the build at a checkout with --bazislib <path>."
        )
    log(f"[ver]  srvman {version}  ({VERSION_HEADER})")

    vendored = bazislib == VENDORED_BAZISLIB.resolve()
    log(f"[deps] BazisLib: {bazislib}" + ("  (vendored)" if vendored else "  (external)"))

    wtl_include = ensure_wtl(args.wtl, args.offline)
    log(f"[deps] WTL:      {wtl_include}")

    vcvars = find_vcvars()

    targets = ARCHITECTURES if args.arch == "both" else (args.arch,)
    built: list[tuple[str, Path]] = []
    for arch in targets:
        built.append((arch, build_arch(arch, args, bazislib, wtl_include, vcvars)))

    log("")
    for arch, exe in built:
        log(
            f"[done] {exe}  ({exe.stat().st_size:,} bytes, "
            f"srvman {version} {args.config} {arch})"
        )

    if args.package:
        package(version, args.config, built, bazislib)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BuildError as error:
        print(f"\nERROR: {error}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        sys.exit(130)
