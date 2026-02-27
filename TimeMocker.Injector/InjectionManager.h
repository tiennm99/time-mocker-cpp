#pragma once
// =============================================================================
// TimeMocker.Injector — inject/eject Hook DLL + manage shared memory
//
// Usage:
//   InjectionManager mgr;
//   mgr.Inject(pid, fakeTimeUtc);   // inject and set time
//   mgr.SetFakeTime(pid, fakeUtc);  // update time while injected
//   mgr.Eject(pid);                 // detach hook
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>
#include <unordered_map>
#include <functional>
#include <mutex>
#include "../Shared/MockTimeInfo.h"

// ---------------------------------------------------------------------------
// SharedMemoryHandle
// Wraps one named MMF per injected process.
// ---------------------------------------------------------------------------
class SharedMemoryHandle
{
public:
    explicit SharedMemoryHandle(DWORD pid);
    ~SharedMemoryHandle();

    SharedMemoryHandle(const SharedMemoryHandle&)            = delete;
    SharedMemoryHandle& operator=(const SharedMemoryHandle&) = delete;

    bool  IsValid() const { return m_pView != nullptr; }
    void  Write(const MockTimeInfo& info);
    const std::wstring& Name() const { return m_name; }

private:
    std::wstring m_name;
    HANDLE       m_hMap  = nullptr;
    LPVOID       m_pView = nullptr;
};

// ---------------------------------------------------------------------------
// InjectedProcessInfo
// ---------------------------------------------------------------------------
struct InjectedProcessInfo
{
    DWORD       Pid         = 0;
    std::wstring ProcessName;
    std::wstring ProcessPath;
    SharedMemoryHandle* Shm = nullptr;
};

// ---------------------------------------------------------------------------
// InjectionManager
// ---------------------------------------------------------------------------
class InjectionManager
{
public:
    explicit InjectionManager(const std::wstring& hookDllDir = L"");
    ~InjectionManager();

    // Inject hook DLL into process and set initial fake time (UTC FILETIME ticks)
    bool Inject(DWORD pid, LONGLONG fakeUtcTicks, std::wstring* pError = nullptr);

    // Update fake time for an already-injected process
    bool SetFakeTime(DWORD pid, LONGLONG fakeUtcTicks);

    // Set fake time for all injected processes
    void SetFakeTimeAll(LONGLONG fakeUtcTicks);

    // Remove hook from process (best-effort — DLL stays loaded but hooks removed on next call)
    bool Eject(DWORD pid);

    bool IsInjected(DWORD pid) const;

    // Callback for log messages
    std::function<void(const std::wstring&)> OnLog;

    // Iterate injected processes
    void ForEach(std::function<void(const InjectedProcessInfo&)> fn) const;

private:
    std::wstring ResolveHookDll(bool x64) const;
    static bool  IsProcess64Bit(HANDLE hProcess);
    static LONGLONG RealUtcTicks();
    static LONGLONG ToFiletimeDelta(LONGLONG fakeUtcTicks);

    void Log(const wchar_t* fmt, ...) const;

    mutable std::mutex m_mutex;
    std::unordered_map<DWORD, InjectedProcessInfo*> m_injected;
    std::wstring m_hookDllDir; // directory where Hook DLLs live
};

// ---------------------------------------------------------------------------
// Utility: convert a DateTime-style local SYSTEMTIME to UTC FILETIME ticks
// (helper for callers that work with wall-clock time)
// ---------------------------------------------------------------------------
namespace TimeUtil
{
    // Get the current real UTC as FILETIME ticks (100-ns units since Jan 1, 1601)
    LONGLONG RealUtcTicks();

    // Convert a local SYSTEMTIME to UTC FILETIME ticks
    LONGLONG LocalSystemTimeToUtcTicks(const SYSTEMTIME& st);

    // Build a DeltaTicks value: how many ticks ahead/behind of real time
    LONGLONG ComputeDelta(LONGLONG fakeUtcTicks);
}
