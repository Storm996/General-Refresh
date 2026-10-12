/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// Used for dynamically linking to dbghelp.dll functions.

// keep this always as first entry
DBGHELP(SymInitialize,
        BOOL,
        (HANDLE hProcess, PCSTR UserSearchPath, BOOL fInvadeProcess))

DBGHELP(SymGetOptions,
        DWORD,
        ())

DBGHELP(SymSetOptions,
        DWORD,
        (DWORD SymOptions))

// GeneralsX @bugfix 11/10/2026 The legacy 32-bit-only dbghelp entry points (StackWalk,
// SymFunctionTableAccess, SymGetModuleBase, SymGetSymFromAddr, SymGetLineFromAddr) are
// not exported by dbghelp.dll on x64; use the 64-bit variants there.
#if defined(_WIN64)
DBGHELP(StackWalk64,
        BOOL,
        (DWORD MachineType, HANDLE hProcess, HANDLE hThread, LPSTACKFRAME64 StackFrame,
        LPVOID ContextRecord, PREAD_PROCESS_MEMORY_ROUTINE64 ReadMemoryRoutine,
        PFUNCTION_TABLE_ACCESS_ROUTINE64 FunctionTableAccessRoutine,
        PGET_MODULE_BASE_ROUTINE64 GetModuleBaseRoutine,
        PTRANSLATE_ADDRESS_ROUTINE TranslateAddress))

DBGHELP(SymFunctionTableAccess64,
        LPVOID,
        (HANDLE hProcess, DWORD64 AddrBase))

DBGHELP(SymGetModuleBase64,
        DWORD64,
        (HANDLE hProcess, DWORD64 qwAddr))

DBGHELP(SymGetSymFromAddr64,
        BOOL,
        (HANDLE hProcess, DWORD64 Address, PDWORD64 Displacement,
        PIMAGEHLP_SYMBOL Symbol))

DBGHELP(SymGetLineFromAddr64,
        BOOL,
        (HANDLE hProcess, DWORD64 dwAddr, PDWORD64 pdwDisplacement,
        PIMAGEHLP_LINE Line))
#else
DBGHELP(StackWalk,
        BOOL,
        (DWORD MachineType, HANDLE hProcess, HANDLE hThread, LPSTACKFRAME StackFrame,
        LPVOID ContextRecord, PREAD_PROCESS_MEMORY_ROUTINE ReadMemoryRoutine,
        PFUNCTION_TABLE_ACCESS_ROUTINE FunctionTableAccessRoutine,
        PGET_MODULE_BASE_ROUTINE GetModuleBaseRoutine,
        PTRANSLATE_ADDRESS_ROUTINE TranslateAddress))

DBGHELP(SymFunctionTableAccess,
        LPVOID,
        (HANDLE hProcess, DWORD AddrBase))

DBGHELP(SymGetModuleBase,
        DWORD,
        (HANDLE hProcess, DWORD dwAddr))

DBGHELP(SymGetSymFromAddr,
        BOOL,
        (HANDLE hProcess, DWORD Address, LPDWORD Displacement,
        PIMAGEHLP_SYMBOL Symbol))

DBGHELP(SymGetLineFromAddr,
        BOOL,
        (HANDLE hProcess, DWORD dwAddr, PDWORD pdwDisplacement,
        PIMAGEHLP_LINE Line))
#endif

// keep this always as last entry
DBGHELP(SymCleanup,
        BOOL,
        (HANDLE hProcess))
