// version.h : the srvman version number, in one place
//
/////////////////////////////////////////////////////////////////////////////

#ifndef SRVMAN_VERSION_H
#define SRVMAN_VERSION_H

//! Bumped by hand. Consumed by srvman.rc (VERSIONINFO), the About box, the
//! command-line banner, and by build.py, which parses SRVMAN_VERSION_STR out of
//! this file to name the release archives.
#define SRVMAN_VERSION_MAJOR 1
#define SRVMAN_VERSION_MINOR 1
#define SRVMAN_VERSION_PATCH 0

//! The same number as a string literal. It has to be spelled out rather than
//! assembled from the three macros above: rc.exe does not concatenate adjacent
//! string literals, so a version built with "." glue would not compile in the
//! resource script. build.py checks the two spellings against each other.
#define SRVMAN_VERSION_STR "1.1.0"

//! TCHAR flavour for application code. The two-step expansion matters -- the
//! usual _T(SRVMAN_VERSION_STR) pastes L onto the macro *name*, not its value.
#define SRVMAN_WIDEN_(x) L##x
#define SRVMAN_WIDEN(x) SRVMAN_WIDEN_(x)

#ifdef _UNICODE
#define SRVMAN_VERSION_STRT SRVMAN_WIDEN(SRVMAN_VERSION_STR)
#else
#define SRVMAN_VERSION_STRT SRVMAN_VERSION_STR
#endif

#endif // SRVMAN_VERSION_H
