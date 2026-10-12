/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// This file includes the dbghelp types of imagehlp.h. The imagehlp.h of VC6 does not have the
// minidump API yet, so a subset of it is added for VC6.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <imagehlp.h> // Must be included after windows.h

#if defined(_MSC_VER) && _MSC_VER < 1300
#include "minidump_subset.h"
#endif

// GeneralsX @build win64 11/10/2026 The 32-bit dbghelp entry points do not exist on x64 - imagehlp.h
// aliases the legacy function and structure names onto their *64 counterparts there, but the plain
// "DWORD" address parameters used by this project's loader shim stay 32-bit wide. These typedefs
// give the address-sized value types so the same source declares correct signatures on both ABIs.
#ifdef _WIN64
	typedef DWORD64 DbghelpAddr;             ///< address / module-base sized value
	typedef PDWORD64 DbghelpAddrOut;         ///< out parameter for an address-sized value
	typedef IMAGEHLP_SYMBOL64 DbghelpSymbol; ///< symbol record layout
	typedef PIMAGEHLP_SYMBOL64 DbghelpSymbolPtr;
#else
	typedef DWORD DbghelpAddr;
	typedef PDWORD DbghelpAddrOut;
	typedef IMAGEHLP_SYMBOL DbghelpSymbol;
	typedef PIMAGEHLP_SYMBOL DbghelpSymbolPtr;
#endif

// GeneralsX @build win64 11/10/2026 Machine type passed to StackWalk for the running process.
#ifdef _WIN64
	#define DBGHELP_MACHINE_TYPE IMAGE_FILE_MACHINE_AMD64
#else
	#define DBGHELP_MACHINE_TYPE IMAGE_FILE_MACHINE_I386
#endif
