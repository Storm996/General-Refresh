/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

#include "DbgHelpLoader.h"

#include "Allocator/SystemAllocator.h"
#include "Utility/lazy_static.h"
#include "Utility/STLUtils.h"
#include "Utility/stringex.h"
#include <new>
#include <set>


namespace
{

typedef BOOL (WINAPI *SymInitialize_t)(HANDLE hProcess, PCSTR UserSearchPath, BOOL fInvadeProcess);
typedef BOOL (WINAPI *SymCleanup_t)(HANDLE hProcess);
// GeneralsX @build win64 11/10/2026 Address-sized types so these match the *64 dbghelp exports on x64
typedef DbghelpAddr (WINAPI *SymLoadModule_t)(HANDLE hProcess, HANDLE hFile, PCSTR ImageName,
	PCSTR ModuleName, DbghelpAddr BaseOfDll, DWORD SizeOfDll);
typedef DbghelpAddr (WINAPI *SymGetModuleBase_t)(HANDLE hProcess, DbghelpAddr dwAddr);
typedef BOOL (WINAPI *SymUnloadModule_t)(HANDLE hProcess, DbghelpAddr BaseOfDll);
typedef BOOL (WINAPI *SymGetSymFromAddr_t)(HANDLE hProcess, DbghelpAddr dwAddr, DbghelpAddrOut pdwDisplacement, DbghelpSymbolPtr Symbol);
typedef BOOL (WINAPI *SymGetLineFromAddr_t)(HANDLE hProcess, DbghelpAddr dwAddr, DbghelpAddrOut pdwDisplacement, PIMAGEHLP_LINE Line);
typedef DWORD (WINAPI *SymSetOptions_t)(DWORD SymOptions);
typedef PVOID (WINAPI *SymFunctionTableAccess_t)(HANDLE hProcess, DbghelpAddr AddrBase);
typedef BOOL (WINAPI *StackWalk_t)(DWORD MachineType, HANDLE hProcess, HANDLE hThread, LPSTACKFRAME StackFrame,
	PVOID ContextRecord, PREAD_PROCESS_MEMORY_ROUTINE ReadMemoryRoutine,
	PFUNCTION_TABLE_ACCESS_ROUTINE FunctionTableAccessRoutine, PGET_MODULE_BASE_ROUTINE GetModuleBaseRoutine,
	PTRANSLATE_ADDRESS_ROUTINE TranslateAddress);
typedef BOOL (WINAPI *MiniDumpWriteDump_t)(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
	PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam, PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
	PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

SymInitialize_t SymInitializePtr = nullptr;
SymCleanup_t SymCleanupPtr = nullptr;
SymLoadModule_t SymLoadModulePtr = nullptr;
SymGetModuleBase_t SymGetModuleBasePtr = nullptr;
SymUnloadModule_t SymUnloadModulePtr = nullptr;
SymGetSymFromAddr_t SymGetSymFromAddrPtr = nullptr;
SymGetLineFromAddr_t SymGetLineFromAddrPtr = nullptr;
SymSetOptions_t SymSetOptionsPtr = nullptr;
SymFunctionTableAccess_t SymFunctionTableAccessPtr = nullptr;
StackWalk_t StackWalkPtr = nullptr;
MiniDumpWriteDump_t MiniDumpWriteDumpPtr = nullptr;

typedef std::set<HANDLE, std::less<HANDLE>, stl::system_allocator<HANDLE> > Processes;

// Is created on the first load, because the loader can be used before static initialization of this file is done.
Processes* InitializedProcesses = nullptr;
HMODULE Module = HMODULE(nullptr);
int ReferenceCount = 0;
bool Failed = false;
bool LoadedFromSystem = false;

// Uses the plain Windows critical section, because it must not allocate with new.
class CriticalSection
{
public:
	CriticalSection() { ::InitializeCriticalSection(&m_criticalSection); }

	void lock() { ::EnterCriticalSection(&m_criticalSection); }
	void unlock() { ::LeaveCriticalSection(&m_criticalSection); }

private:
	CRITICAL_SECTION m_criticalSection;
};

// Is constructed on first use, because the loader can be used during the static initialization of
// other files. Is never destroyed, so that it stays usable during the static destruction too.
lazy_static<CriticalSection> Lock;

class ScopedLock
{
public:
	ScopedLock() { Lock.get().lock(); }
	~ScopedLock() { Lock.get().unlock(); }
};

// Resolves one dbghelp entry point into a function pointer of its own type.
template<typename T>
static void resolveInto(T& out, LPCSTR exportName)
{
	out = reinterpret_cast<T>(::GetProcAddress(Module, exportName));
}

static void resolveAll()
{
	// These exports have the same name and address-sized signature on both ABIs.
	resolveInto(SymInitializePtr, "SymInitialize");
	resolveInto(SymCleanupPtr, "SymCleanup");
	resolveInto(SymSetOptionsPtr, "SymSetOptions");
	resolveInto(MiniDumpWriteDumpPtr, "MiniDumpWriteDump");

	// GeneralsX @build win64 11/10/2026 The address walking and symbol lookup exports only exist
	// under their *64 names on x64; the legacy 32-bit names are not usable in a 64-bit process.
#ifdef _WIN64
	resolveInto(SymLoadModulePtr, "SymLoadModule64");
	resolveInto(SymGetModuleBasePtr, "SymGetModuleBase64");
	resolveInto(SymUnloadModulePtr, "SymUnloadModule64");
	resolveInto(SymGetSymFromAddrPtr, "SymGetSymFromAddr64");
	resolveInto(SymGetLineFromAddrPtr, "SymGetLineFromAddr64");
	resolveInto(SymFunctionTableAccessPtr, "SymFunctionTableAccess64");
	resolveInto(StackWalkPtr, "StackWalk64");
#else
	resolveInto(SymLoadModulePtr, "SymLoadModule");
	resolveInto(SymGetModuleBasePtr, "SymGetModuleBase");
	resolveInto(SymUnloadModulePtr, "SymUnloadModule");
	resolveInto(SymGetSymFromAddrPtr, "SymGetSymFromAddr");
	resolveInto(SymGetLineFromAddrPtr, "SymGetLineFromAddr");
	resolveInto(SymFunctionTableAccessPtr, "SymFunctionTableAccess");
	resolveInto(StackWalkPtr, "StackWalk");
#endif
}

static void freeResources()
{
	while (!InitializedProcesses->empty())
	{
		DbgHelp::SymCleanup(*InitializedProcesses->begin());
	}

	if (Module != HMODULE(nullptr))
	{
		::FreeLibrary(Module);
		Module = HMODULE(nullptr);
	}

	SymInitializePtr = nullptr;
	SymCleanupPtr = nullptr;
	SymLoadModulePtr = nullptr;
	SymGetModuleBasePtr = nullptr;
	SymUnloadModulePtr = nullptr;
	SymGetSymFromAddrPtr = nullptr;
	SymGetLineFromAddrPtr = nullptr;
	SymSetOptionsPtr = nullptr;
	SymFunctionTableAccessPtr = nullptr;
	StackWalkPtr = nullptr;
	MiniDumpWriteDumpPtr = nullptr;

	LoadedFromSystem = false;
}

} // namespace


bool DbgHelpLoader::isLoaded()
{
	ScopedLock lock;

	return Module != HMODULE(nullptr);
}

bool DbgHelpLoader::isLoadedFromSystem()
{
	ScopedLock lock;

	return isLoaded() && LoadedFromSystem;
}

bool DbgHelpLoader::isFailed()
{
	ScopedLock lock;

	return Failed;
}

bool DbgHelpLoader::load()
{
	ScopedLock lock;

	if (InitializedProcesses == nullptr)
	{
		// Cannot use new/delete here when this is loaded during game memory initialization.
		void* p = ::GlobalAlloc(GMEM_FIXED, sizeof(Processes));
		if (p == nullptr)
			return false;
		InitializedProcesses = new (p) Processes();
	}

	// Always increment the reference count.
	++ReferenceCount;

	// Optimization: return early if it failed before.
	if (Failed)
		return false;

	// Return early if someone else already loaded it.
	if (ReferenceCount > 1)
		return true;

	// Try load dbghelp.dll from the system directory first.
	char dllFilename[MAX_PATH];
	const UINT dllFilenameLen = ::GetSystemDirectoryA(dllFilename, sizeof(dllFilename));
	if (dllFilenameLen == 0 || dllFilenameLen >= sizeof(dllFilename) ||
		strlcat(dllFilename, "\\dbghelp.dll", sizeof(dllFilename)) >= sizeof(dllFilename)) {
		Failed = true;
		return false;
	}

	Module = ::LoadLibraryA(dllFilename);
	if (Module == HMODULE(nullptr))
	{
		// Not found. Try load dbghelp.dll from the work directory.
		Module = ::LoadLibraryA("dbghelp.dll");
		if (Module == HMODULE(nullptr))
		{
			Failed = true;
			return false;
		}
	}
	else
	{
		LoadedFromSystem = true;
	}

	resolveAll();

	if (SymInitializePtr == nullptr || SymCleanupPtr == nullptr)
	{
		freeResources();
		Failed = true;
		return false;
	}

	return true;
}

void DbgHelpLoader::unload()
{
	ScopedLock lock;

	if (InitializedProcesses == nullptr)
		return;

	if (--ReferenceCount != 0)
		return;

	freeResources();
	Failed = false;

	InitializedProcesses->~Processes();
	::GlobalFree(InitializedProcesses);
	InitializedProcesses = nullptr;
}


// The dbghelp functions below stand in for the imports of dbghelp.dll. An unresolved function
// returns a neutral value.

namespace DbgHelp
{

BOOL WINAPI SymInitialize(
	HANDLE hProcess,
	PCSTR UserSearchPath,
	BOOL fInvadeProcess)
{
	ScopedLock lock;

	if (InitializedProcesses == nullptr)
		return FALSE;

	if (InitializedProcesses->find(hProcess) != InitializedProcesses->end())
	{
		// Was already initialized.
		return TRUE;
	}

	if (SymInitializePtr != nullptr)
	{
		if (SymInitializePtr(hProcess, UserSearchPath, fInvadeProcess) != FALSE)
		{
			// Is now initialized.
			InitializedProcesses->insert(hProcess);
			return TRUE;
		}
	}

	return FALSE;
}

BOOL WINAPI SymCleanup(
	HANDLE hProcess)
{
	ScopedLock lock;

	if (InitializedProcesses == nullptr)
		return FALSE;

	if (stl::find_and_erase(*InitializedProcesses, hProcess))
	{
		if (SymCleanupPtr != nullptr)
		{
			return SymCleanupPtr(hProcess);
		}
	}

	return FALSE;
}

DbghelpAddr WINAPI SymLoadModule(
	HANDLE hProcess,
	HANDLE hFile,
	PCSTR ImageName,
	PCSTR ModuleName,
	DbghelpAddr BaseOfDll,
	DWORD SizeOfDll)
{
	ScopedLock lock;

	if (SymLoadModulePtr != nullptr)
		return SymLoadModulePtr(hProcess, hFile, ImageName, ModuleName, BaseOfDll, SizeOfDll);

	return 0;
}

DbghelpAddr WINAPI SymGetModuleBase(
	HANDLE hProcess,
	DbghelpAddr dwAddr)
{
	ScopedLock lock;

	if (SymGetModuleBasePtr != nullptr)
		return SymGetModuleBasePtr(hProcess, dwAddr);

	return 0u;
}

BOOL WINAPI SymUnloadModule(
	HANDLE hProcess,
	DbghelpAddr BaseOfDll)
{
	ScopedLock lock;

	if (SymUnloadModulePtr != nullptr)
		return SymUnloadModulePtr(hProcess, BaseOfDll);

	return FALSE;
}

BOOL WINAPI SymGetSymFromAddr(
	HANDLE hProcess,
	DbghelpAddr dwAddr,
	DbghelpAddrOut pdwDisplacement,
	DbghelpSymbolPtr Symbol)
{
	ScopedLock lock;

	if (SymGetSymFromAddrPtr != nullptr)
		return SymGetSymFromAddrPtr(hProcess, dwAddr, pdwDisplacement, Symbol);

	return FALSE;
}

BOOL WINAPI SymGetLineFromAddr(
	HANDLE hProcess,
	DbghelpAddr dwAddr,
	DbghelpAddrOut pdwDisplacement,
	PIMAGEHLP_LINE Line)
{
	ScopedLock lock;

	if (SymGetLineFromAddrPtr != nullptr)
		return SymGetLineFromAddrPtr(hProcess, dwAddr, pdwDisplacement, Line);

	return FALSE;
}

DWORD WINAPI SymSetOptions(
	DWORD SymOptions)
{
	ScopedLock lock;

	if (SymSetOptionsPtr != nullptr)
		return SymSetOptionsPtr(SymOptions);

	return 0u;
}

PVOID WINAPI SymFunctionTableAccess(
	HANDLE hProcess,
	DbghelpAddr AddrBase)
{
	ScopedLock lock;

	if (SymFunctionTableAccessPtr != nullptr)
		return SymFunctionTableAccessPtr(hProcess, AddrBase);

	return nullptr;
}

BOOL WINAPI StackWalk(
	DWORD MachineType,
	HANDLE hProcess,
	HANDLE hThread,
	LPSTACKFRAME StackFrame,
	PVOID ContextRecord,
	PREAD_PROCESS_MEMORY_ROUTINE ReadMemoryRoutine,
	PFUNCTION_TABLE_ACCESS_ROUTINE FunctionTableAccessRoutine,
	PGET_MODULE_BASE_ROUTINE GetModuleBaseRoutine,
	PTRANSLATE_ADDRESS_ROUTINE TranslateAddress)
{
	ScopedLock lock;

	if (StackWalkPtr != nullptr)
		return StackWalkPtr(MachineType, hProcess, hThread, StackFrame, ContextRecord, ReadMemoryRoutine,
			FunctionTableAccessRoutine, GetModuleBaseRoutine, TranslateAddress);

	return FALSE;
}

BOOL WINAPI MiniDumpWriteDump(
	HANDLE hProcess,
	DWORD ProcessId,
	HANDLE hFile,
	MINIDUMP_TYPE DumpType,
	PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
	PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
	PMINIDUMP_CALLBACK_INFORMATION CallbackParam)
{
	ScopedLock lock;

	if (MiniDumpWriteDumpPtr != nullptr)
		return MiniDumpWriteDumpPtr(hProcess, ProcessId, hFile, DumpType, ExceptionParam, UserStreamParam, CallbackParam);

	return FALSE;
}

} // namespace DbgHelp
