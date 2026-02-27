// =============================================================================
// TimeMocker.Hook — MS Detours-based time hooking DLL
//
// Injected into a target process by TimeMocker.Injector.
// Opens a named Memory-Mapped File created by the UI:
//   Name: TimeMocker_<PID>
//   Data: MockTimeInfo { LONGLONG DeltaTicks }
//
// Hooks 5 Win32 time APIs:
//   kernel32!GetSystemTime
//   kernel32!GetLocalTime
//   kernel32!GetSystemTimeAsFileTime
//   kernel32!GetSystemTimePreciseAsFileTime
//   ntdll!NtQuerySystemTime
//
// The delta (DeltaTicks) is added to the real UTC FILETIME on every call.
// When DeltaTicks == 0 the real time passes through unchanged.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <detours.h>
#include <cstdio>
#include <cstdint>

#include "../Shared/MockTimeInfo.h"

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------
static HANDLE  g_hMapFile  = nullptr;
static LPVOID  g_pView     = nullptr;
static wchar_t g_logPath[MAX_PATH];

// ---------------------------------------------------------------------------
// Logging (writes to %TEMP%\TimeMocker.Hook.log)
// ---------------------------------------------------------------------------
static void Log(const char* fmt, ...)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath, L"a") == 0 && f)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);   // NOTE: intentional real time call before hooks install
        fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
        va_list va;
        va_start(va, fmt);
        vfprintf(f, fmt, va);
        va_end(va);
        fprintf(f, "\n");
        fclose(f);
    }
}

// ---------------------------------------------------------------------------
// Read the delta from shared memory (inline, hot path)
// ---------------------------------------------------------------------------
static inline LONGLONG ReadDeltaTicks()
{
    if (!g_pView) return 0LL;
    // Atomic-friendly: LONGLONG is 8-byte aligned, single read is atomic on x86/x64
    return *reinterpret_cast<volatile LONGLONG*>(g_pView);
}

// ---------------------------------------------------------------------------
// FILETIME helpers
// ---------------------------------------------------------------------------
static const LONGLONG FILETIME_EPOCH_BIAS = 116444736000000000LL; // Jan 1, 1601 → Jan 1, 1970

// Convert a FILETIME (100-ns ticks since Jan 1, 1601 UTC) to a SYSTEMTIME UTC
static void FileTimeToSystemTimeLocal(LONGLONG ticks, SYSTEMTIME* pSt, bool localTime)
{
    FILETIME ft;
    ft.dwLowDateTime  = static_cast<DWORD>(ticks & 0xFFFFFFFF);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);

    if (localTime)
    {
        FILETIME localFt;
        FileTimeToLocalFileTime(&ft, &localFt);
        FileTimeToSystemTime(&localFt, pSt);
    }
    else
    {
        FileTimeToSystemTime(&ft, pSt);
    }
}

// Get real UTC as 100-ns ticks (FILETIME ticks)
static inline LONGLONG GetRealUtcTicks()
{
    FILETIME ft;
    // Call the REAL GetSystemTimeAsFileTime (trampoline, set up after DetourAttach)
    // We use a raw read of the real function pointer stored in the trampoline.
    // To avoid recursion during hook installation we use a flag.
    ULARGE_INTEGER ui;
    GetSystemTimeAsFileTime(&ft); // will be redirected after hooks are live; see note below
    ui.LowPart  = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return static_cast<LONGLONG>(ui.QuadPart);
}

// ---------------------------------------------------------------------------
// Original function pointers (Detours trampolines)
// ---------------------------------------------------------------------------
static void  (WINAPI* Real_GetSystemTime)(LPSYSTEMTIME)            = GetSystemTime;
static void  (WINAPI* Real_GetLocalTime)(LPSYSTEMTIME)             = GetLocalTime;
static void  (WINAPI* Real_GetSystemTimeAsFileTime)(LPFILETIME)    = GetSystemTimeAsFileTime;
static void  (WINAPI* Real_GetSystemTimePreciseAsFileTime)(LPFILETIME) = GetSystemTimePreciseAsFileTime;

typedef NTSTATUS (NTAPI* NtQuerySystemTimeFn)(PLARGE_INTEGER SystemTime);
static NtQuerySystemTimeFn Real_NtQuerySystemTime = nullptr;

// ---------------------------------------------------------------------------
// Helper: get fake UTC ticks
// ---------------------------------------------------------------------------
static LONGLONG GetFakeUtcTicks()
{
    // Get real time via the trampoline (not the hook)
    FILETIME ft;
    Real_GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ui;
    ui.LowPart  = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return static_cast<LONGLONG>(ui.QuadPart) + ReadDeltaTicks();
}

// ---------------------------------------------------------------------------
// Hook implementations
// ---------------------------------------------------------------------------

static void WINAPI Hook_GetSystemTime(LPSYSTEMTIME lpSystemTime)
{
    if (!lpSystemTime) return;
    LONGLONG ticks = GetFakeUtcTicks();
    FileTimeToSystemTimeLocal(ticks, lpSystemTime, false);
}

static void WINAPI Hook_GetLocalTime(LPSYSTEMTIME lpLocalTime)
{
    if (!lpLocalTime) return;
    LONGLONG ticks = GetFakeUtcTicks();
    FileTimeToSystemTimeLocal(ticks, lpLocalTime, true);
}

static void WINAPI Hook_GetSystemTimeAsFileTime(LPFILETIME lpFileTime)
{
    if (!lpFileTime) return;
    LONGLONG ticks = GetFakeUtcTicks();
    lpFileTime->dwLowDateTime  = static_cast<DWORD>(ticks & 0xFFFFFFFF);
    lpFileTime->dwHighDateTime = static_cast<DWORD>(ticks >> 32);
}

static void WINAPI Hook_GetSystemTimePreciseAsFileTime(LPFILETIME lpFileTime)
{
    // Same as GetSystemTimeAsFileTime — we can't improve precision beyond real time
    Hook_GetSystemTimeAsFileTime(lpFileTime);
}

static NTSTATUS NTAPI Hook_NtQuerySystemTime(PLARGE_INTEGER SystemTime)
{
    if (!SystemTime) return STATUS_ACCESS_VIOLATION;
    SystemTime->QuadPart = GetFakeUtcTicks();
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Install / remove hooks
// ---------------------------------------------------------------------------
static bool InstallHooks()
{
    // Resolve NtQuerySystemTime from ntdll
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll)
    {
        Log("ERROR: ntdll.dll not found");
        return false;
    }
    Real_NtQuerySystemTime = reinterpret_cast<NtQuerySystemTimeFn>(
        GetProcAddress(hNtdll, "NtQuerySystemTime"));
    if (!Real_NtQuerySystemTime)
    {
        Log("ERROR: NtQuerySystemTime not found in ntdll");
        return false;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    DetourAttach(reinterpret_cast<PVOID*>(&Real_GetSystemTime),
                 reinterpret_cast<PVOID>(Hook_GetSystemTime));
    DetourAttach(reinterpret_cast<PVOID*>(&Real_GetLocalTime),
                 reinterpret_cast<PVOID>(Hook_GetLocalTime));
    DetourAttach(reinterpret_cast<PVOID*>(&Real_GetSystemTimeAsFileTime),
                 reinterpret_cast<PVOID>(Hook_GetSystemTimeAsFileTime));
    DetourAttach(reinterpret_cast<PVOID*>(&Real_GetSystemTimePreciseAsFileTime),
                 reinterpret_cast<PVOID>(Hook_GetSystemTimePreciseAsFileTime));
    DetourAttach(reinterpret_cast<PVOID*>(&Real_NtQuerySystemTime),
                 reinterpret_cast<PVOID>(Hook_NtQuerySystemTime));

    LONG err = DetourTransactionCommit();
    if (err != NO_ERROR)
    {
        Log("ERROR: DetourTransactionCommit failed: %ld", err);
        return false;
    }

    Log("Hooks installed successfully (delta=%lld ticks)", ReadDeltaTicks());
    return true;
}

static void RemoveHooks()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    DetourDetach(reinterpret_cast<PVOID*>(&Real_GetSystemTime),
                 reinterpret_cast<PVOID>(Hook_GetSystemTime));
    DetourDetach(reinterpret_cast<PVOID*>(&Real_GetLocalTime),
                 reinterpret_cast<PVOID>(Hook_GetLocalTime));
    DetourDetach(reinterpret_cast<PVOID*>(&Real_GetSystemTimeAsFileTime),
                 reinterpret_cast<PVOID>(Hook_GetSystemTimeAsFileTime));
    DetourDetach(reinterpret_cast<PVOID*>(&Real_GetSystemTimePreciseAsFileTime),
                 reinterpret_cast<PVOID>(Hook_GetSystemTimePreciseAsFileTime));

    if (Real_NtQuerySystemTime)
        DetourDetach(reinterpret_cast<PVOID*>(&Real_NtQuerySystemTime),
                     reinterpret_cast<PVOID>(Hook_NtQuerySystemTime));

    DetourTransactionCommit();
    Log("Hooks removed");
}

// ---------------------------------------------------------------------------
// Open the shared memory created by the UI
// ---------------------------------------------------------------------------
static bool OpenSharedMemory(DWORD pid)
{
    wchar_t mmfName[64];
    GetMmfName(pid, mmfName, _countof(mmfName));

    g_hMapFile = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mmfName);
    if (!g_hMapFile)
    {
        Log("ERROR: OpenFileMapping('%ls') failed: %lu", mmfName, GetLastError());
        return false;
    }

    g_pView = MapViewOfFile(g_hMapFile, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, MMF_SIZE);
    if (!g_pView)
    {
        Log("ERROR: MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(g_hMapFile);
        g_hMapFile = nullptr;
        return false;
    }

    Log("Shared memory '%ls' opened (delta=%lld)", mmfName, ReadDeltaTicks());
    return true;
}

static void CloseSharedMemory()
{
    if (g_pView)    { UnmapViewOfFile(g_pView);  g_pView    = nullptr; }
    if (g_hMapFile) { CloseHandle(g_hMapFile);   g_hMapFile = nullptr; }
}

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE /*hInst*/, DWORD reason, LPVOID /*reserved*/)
{
    if (DetourIsHelperProcess()) return TRUE;

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        // Build log path: %TEMP%\TimeMocker.Hook.log
        wchar_t tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);
        swprintf_s(g_logPath, _countof(g_logPath), L"%lsTimeMocker.Hook.log", tempDir);

        DWORD pid = GetCurrentProcessId();
        Log("DLL_PROCESS_ATTACH pid=%lu", pid);

        if (!OpenSharedMemory(pid))
        {
            // UI hasn't created the MMF yet — this is fatal for injection
            return FALSE;
        }

        DisableThreadLibraryCalls(/*hInst*/ GetModuleHandleW(nullptr));

        if (!InstallHooks())
        {
            CloseSharedMemory();
            return FALSE;
        }
        break;
    }

    case DLL_PROCESS_DETACH:
        RemoveHooks();
        CloseSharedMemory();
        Log("DLL_PROCESS_DETACH");
        break;
    }

    return TRUE;
}
