# Vendored BazisLib subset

Upstream: https://github.com/sysprogs/BazisLib
Revision: 7363a16ad27b8b79d1d13855befe686ad3abb534 2021-09-22
Licence:  LGPL-3.0 (see LICENSE in this directory)

This is not a full copy of BazisLib. It contains exactly the files
srvman compiles or includes -- the transitive closure of
`<bzscore/...>` and `<bzshlp/Win32/services.h>` for a Win32 user-mode
build. The upstream directory layout is preserved so the library's own
relative includes keep resolving unchanged. The same file set serves
both the x64 and the x86 target.

6 source files, 35 headers.

## Refreshing

Do not edit these files by hand. Point the build script at an upstream
checkout instead:

    python build.py --sync-bazislib <path-to-BazisLib>

The authoritative file list lives in `build.py`
(`BAZISLIB_HEADERS` / `BAZISLIB_SOURCES`). If a future change to srvman
pulls in a header that is not vendored, the compile fails with a plain
'cannot open include file' -- add it to that list and re-sync.
