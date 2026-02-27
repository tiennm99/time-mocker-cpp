#pragma once
// =============================================================================
// ProcessWatcher — polls running processes and auto-injects those matching
// a set of glob/regex patterns.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <regex>
#include <algorithm>

#include "InjectionManager.h"

struct PatternRule
{
    std::wstring Pattern;
    bool         UseRegex = false;
    bool         Enabled  = true;

    bool IsMatch(const std::wstring& path) const
    {
        if (path.empty()) return false;
        std::wstring regexStr;
        if (UseRegex)
        {
            regexStr = Pattern;
        }
        else
        {
            regexStr = L"^";
            for (wchar_t c : Pattern)
            {
                switch (c)
                {
                case L'*':  regexStr += L".*";   break;
                case L'?':  regexStr += L'.';    break;
                case L'.':  regexStr += L"\\.";  break;
                case L'\\': regexStr += L"\\\\"; break;
                default:    regexStr += c;       break;
                }
            }
            regexStr += L'$';
        }
        try
        {
            std::wregex re(regexStr, std::regex_constants::icase);
            return std::regex_match(path, re) || std::regex_search(path, re);
        }
        catch (...) { return false; }
    }
};

class ProcessWatcher
{
public:
    explicit ProcessWatcher(InjectionManager& mgr)
        : m_mgr(mgr)
    {
        m_fakeUtcTicks = TimeUtil::RealUtcTicks();
    }

    ~ProcessWatcher() { Stop(); }

    void AddRule(PatternRule rule)
    {
        std::lock_guard<std::mutex> lk(m_rulesMutex);
        m_rules.push_back(std::move(rule));
    }

    void RemoveRule(const std::wstring& pattern)
    {
        std::lock_guard<std::mutex> lk(m_rulesMutex);
        m_rules.erase(
            std::remove_if(m_rules.begin(), m_rules.end(),
                [&](const PatternRule& r){ return r.Pattern == pattern; }),
            m_rules.end());
    }

    void ClearRules()
    {
        std::lock_guard<std::mutex> lk(m_rulesMutex);
        m_rules.clear();
    }

    void SetFakeUtcTicks(LONGLONG ticks) { m_fakeUtcTicks.store(ticks); }

    void Start(DWORD pollIntervalMs = 1500)
    {
        if (m_running.exchange(true)) return;
        m_thread = std::thread([this, pollIntervalMs]()
        {
            while (m_running.load())
            {
                Scan();
                for (DWORD e = 0; m_running.load() && e < pollIntervalMs; e += 100)
                    Sleep(100);
            }
        });
    }

    void Stop()
    {
        if (!m_running.exchange(false)) return;
        if (m_thread.joinable()) m_thread.join();
    }

    // Callbacks
    std::function<void(DWORD pid, const std::wstring& name, const std::wstring& path)> OnAutoInjected;
    std::function<void(const std::wstring&)> OnLog;

private:
    void Scan()
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return;

        PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
        if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return; }

        std::lock_guard<std::mutex> ruleLk(m_rulesMutex);

        do
        {
            DWORD pid = pe.th32ProcessID;
            { std::lock_guard<std::mutex> lk(m_seenMutex); if (m_seenPids.count(pid)) continue; }
            if (m_mgr.IsInjected(pid)) continue;

            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            std::wstring fullPath;
            if (hProc)
            {
                wchar_t buf[MAX_PATH] = {}; DWORD len = MAX_PATH;
                QueryFullProcessImageNameW(hProc, 0, buf, &len);
                fullPath = buf;
                CloseHandle(hProc);
            }

            std::wstring procName = pe.szExeFile;

            for (auto& rule : m_rules)
            {
                if (!rule.Enabled) continue;
                if (!rule.IsMatch(fullPath) && !rule.IsMatch(procName)) continue;

                { std::lock_guard<std::mutex> lk(m_seenMutex); m_seenPids.insert(pid); }

                std::wstring err;
                if (m_mgr.Inject(pid, m_fakeUtcTicks.load(), &err))
                {
                    DoLog(L"[AutoInject] '%ls' → [%lu] %ls", rule.Pattern.c_str(), pid, procName.c_str());
                    if (OnAutoInjected) OnAutoInjected(pid, procName, fullPath);
                }
                else
                {
                    DoLog(L"[AutoInject] FAIL [%lu] %ls: %ls", pid, procName.c_str(), err.c_str());
                }
                break;
            }
        } while (Process32NextW(snap, &pe));

        CloseHandle(snap);
    }

    void DoLog(const wchar_t* fmt, ...) const
    {
        if (!OnLog) return;
        wchar_t buf[1024]; va_list va; va_start(va, fmt);
        vswprintf_s(buf, _countof(buf), fmt, va); va_end(va);
        OnLog(buf);
    }

    InjectionManager&          m_mgr;
    mutable std::mutex         m_rulesMutex;
    std::vector<PatternRule>   m_rules;
    std::mutex                 m_seenMutex;
    std::unordered_set<DWORD>  m_seenPids;
    std::atomic<LONGLONG>      m_fakeUtcTicks{ 0 };
    std::atomic<bool>          m_running{ false };
    std::thread                m_thread;
};
