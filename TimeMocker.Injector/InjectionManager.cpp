// =============================================================================
// TimeMocker.Injector — InjectionManager.cpp
//
// Uses the classic LoadLibrary remote-thread injection technique:
//   1. Open the target process with sufficient rights
//   2. Write the DLL path into the target's address space
//   3. Create a remote thread that calls LoadLibraryW
//
// For new processes, DetourCreateProcessWithDllEx() can alternatively be used
// (see TimeMocker.Injector.CLI for an example).
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <detours.h>
#include <cstdio>
#include <cassert>
#include <cwchar>
#include <memory>
#include <sstream>

#include "InjectionManager.h"

#pragma comment(lib, "Psapi.lib")

// ============================================================================
// SharedMemoryHandle
// ============================================================================

SharedMemoryHandle::SharedMemoryHandle(DWORD pid)
{
    wchar_t buf[64];
    GetMmfName(pid, buf, _countof(buf));
    m_name = buf;

    m_hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, MMF_SIZE,
        m_name.c_str());

    if (!m_hMap) return;

    m_pView = MapViewOfFile(m_hMap, FILE_MAP_ALL_ACCESS, 0, 0, MMF_SIZE);
    if (!m_pView)
    {
        CloseHandle(m_hMap);
        m_hMap = nullptr;
    }
    else
    {
        // Zero-initialise (DeltaTicks = 0 → real time)
        ZeroMemory(m_pView, MMF_SIZE);
    }
}

SharedMemoryHandle::~SharedMemoryHandle()
{
    if (m_pView) UnmapViewOfFile(m_pView);
    if (m_hMap)  CloseHandle(m_hMap);
}

void SharedMemoryHandle::Write(const MockTimeInfo& info)
{
    if (!m_pView) return;
    // Atomic write on aligned 8-byte address on x64/x86
    InterlockedExchange64(reinterpret_cast<LONGLONG*>(m_pView), info.DeltaTicks);
}

// ============================================================================
// TimeUtil
// ============================================================================

LONGLONG TimeUtil::RealUtcTicks()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ui;
    ui.LowPart  = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return static_cast<LONGLONG>(ui.QuadPart);
}

LONGLONG TimeUtil::LocalSystemTimeToUtcTicks(const SYSTEMTIME& st)
{
    FILETIME localFt, utcFt;
    SystemTimeToFileTime(&st, &localFt);
    LocalFileTimeToFileTime(&localFt, &utcFt);
    ULARGE_INTEGER ui;
    ui.LowPart  = utcFt.dwLowDateTime;
    ui.HighPart = utcFt.dwHighDateTime;
    return static_cast<LONGLONG>(ui.QuadPart);
}

LONGLONG TimeUtil::ComputeDelta(LONGLONG fakeUtcTicks)
{
    return fakeUtcTicks - RealUtcTicks();
}

// ============================================================================
// InjectionManager helpers
// ============================================================================

void InjectionManager::Log(const wchar_t* fmt, ...) const
{
    if (!OnLog) return;
    wchar_t buf[1024];
    va_list va;
    va_start(va, fmt);
    vswprintf_s(buf, _countof(buf), fmt, va);
    va_end(va);
    OnLog(buf);
}

std::wstring InjectionManager::ResolveHookDll(bool x64) const
{
    // Prefer explicit directory; fall back to the injector's own directory
    std::wstring dir = m_hookDllDir;
    if (dir.empty())
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) { *(slash + 1) = L'\0'; dir = exe; }
    }
    return dir + (x64 ? L"TimeMocker.Hook.x64.dll" : L"TimeMocker.Hook.x86.dll");
}

bool InjectionManager::IsProcess64Bit(HANDLE hProcess)
{
    BOOL wow64 = FALSE;
    IsWow64Process(hProcess, &wow64);
    // If we are 64-bit and the target is NOT WOW64, target is 64-bit
#ifdef _WIN64
    return !wow64;
#else
    return false; // 32-bit injector can only inject x86
#endif
}

// ============================================================================
// InjectionManager
// ============================================================================

InjectionManager::InjectionManager(const std::wstring& hookDllDir)
    : m_hookDllDir(hookDllDir)
{
}

InjectionManager::~InjectionManager()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& kv : m_injected)
        delete kv.second;
    m_injected.clear();
}

bool InjectionManager::Inject(DWORD pid, LONGLONG fakeUtcTicks, std::wstring* pError)
{
    std::lock_guard<std::mutex> lk(m_mutex);

    if (m_injected.count(pid))
    {
        // Already injected — just update time
        m_injected[pid]->Shm->Write({ TimeUtil::ComputeDelta(fakeUtcTicks) });
        return true;
    }

    // ---- Open target process -----------------------------------------------
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);

    if (!hProcess)
    {
        std::wstring err = L"OpenProcess failed: " + std::to_wstring(GetLastError());
        Log(L"[Inject] %ls", err.c_str());
        if (pError) *pError = err;
        return false;
    }

    bool is64 = IsProcess64Bit(hProcess);
    std::wstring dllPath = ResolveHookDll(is64);

    // ---- Create shared memory (must exist BEFORE DLL is loaded) -------------
    auto* entry = new InjectedProcessInfo();
    entry->Pid  = pid;
    entry->Shm  = new SharedMemoryHandle(pid);

    if (!entry->Shm->IsValid())
    {
        delete entry;
        CloseHandle(hProcess);
        std::wstring err = L"CreateFileMapping failed: " + std::to_wstring(GetLastError());
        Log(L"[Inject] %ls", err.c_str());
        if (pError) *pError = err;
        return false;
    }

    // Write initial delta
    entry->Shm->Write({ TimeUtil::ComputeDelta(fakeUtcTicks) });

    // ---- Collect process name / path ----------------------------------------
    wchar_t pathBuf[MAX_PATH] = {};
    DWORD   pathLen = MAX_PATH;
    QueryFullProcessImageNameW(hProcess, 0, pathBuf, &pathLen);
    entry->ProcessPath = pathBuf;

    const wchar_t* slash = wcsrchr(pathBuf, L'\\');
    entry->ProcessName = slash ? (slash + 1) : pathBuf;

    // ---- Inject the DLL via LoadLibraryW remote thread ----------------------
    SIZE_T dllPathBytes = (dllPath.size() + 1) * sizeof(wchar_t);

    LPVOID remoteStr = VirtualAllocEx(hProcess, nullptr, dllPathBytes,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteStr)
    {
        delete entry;
        CloseHandle(hProcess);
        std::wstring err = L"VirtualAllocEx failed: " + std::to_wstring(GetLastError());
        Log(L"[Inject] %ls", err.c_str());
        if (pError) *pError = err;
        return false;
    }

    WriteProcessMemory(hProcess, remoteStr, dllPath.c_str(), dllPathBytes, nullptr);

    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC pLoadLib = GetProcAddress(hKernel, "LoadLibraryW");

    HANDLE hThread = CreateRemoteThread(
        hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoadLib),
        remoteStr, 0, nullptr);

    if (!hThread)
    {
        VirtualFreeEx(hProcess, remoteStr, 0, MEM_RELEASE);
        delete entry;
        CloseHandle(hProcess);
        std::wstring err = L"CreateRemoteThread failed: " + std::to_wstring(GetLastError());
        Log(L"[Inject] %ls", err.c_str());
        if (pError) *pError = err;
        return false;
    }

    // Wait for LoadLibraryW to return (give it 5 seconds)
    WaitForSingleObject(hThread, 5000);

    // Check the return value (hModule loaded)
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteStr, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    if (!exitCode)
    {
        delete entry;
        std::wstring err = L"LoadLibraryW in target returned NULL — DLL load failed";
        Log(L"[Inject] %ls", err.c_str());
        if (pError) *pError = err;
        return false;
    }

    m_injected[pid] = entry;
    Log(L"[Inject] pid=%lu '%ls' injected ('%ls')", pid, entry->ProcessName.c_str(), dllPath.c_str());
    return true;
}

bool InjectionManager::SetFakeTime(DWORD pid, LONGLONG fakeUtcTicks)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_injected.find(pid);
    if (it == m_injected.end()) return false;
    it->second->Shm->Write({ TimeUtil::ComputeDelta(fakeUtcTicks) });
    return true;
}

void InjectionManager::SetFakeTimeAll(LONGLONG fakeUtcTicks)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    LONGLONG delta = TimeUtil::ComputeDelta(fakeUtcTicks);
    for (auto& kv : m_injected)
        kv.second->Shm->Write({ delta });
}

bool InjectionManager::Eject(DWORD pid)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_injected.find(pid);
    if (it == m_injected.end()) return false;

    // Zero out the delta so the hook passes through real time before we unmap
    it->second->Shm->Write({ 0LL });
    Sleep(50); // let any in-flight hook calls complete

    delete it->second;
    m_injected.erase(it);
    Log(L"[Eject] pid=%lu ejected (shared memory closed)", pid);
    return true;
}

bool InjectionManager::IsInjected(DWORD pid) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_injected.count(pid) != 0;
}

void InjectionManager::ForEach(std::function<void(const InjectedProcessInfo&)> fn) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& kv : m_injected)
        fn(*kv.second);
}
