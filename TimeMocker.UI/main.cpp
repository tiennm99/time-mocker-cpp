// =============================================================================
// TimeMocker.UI — Console controller
//
// Commands:
//   list              — list running processes (filtered to accessible ones)
//   inject <pid>      — inject into process and apply current fake time
//   eject  <pid>      — remove hook from process
//   time   <datetime> — set fake time  e.g.  time "2024-06-15 14:30:00"
//   time   now        — reset fake time to real time
//   status            — show injected processes and current time offset
//   rule add <glob>   — add auto-inject glob rule
//   rule add -r <rx>  — add auto-inject regex rule
//   rule list         — list auto-inject rules
//   rule del <n>      — remove rule by index
//   watch start       — start auto-inject watcher (default: on startup)
//   watch stop        — stop auto-inject watcher
//   quit / exit       — exit
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <io.h>
#include <fcntl.h>

#include "../TimeMocker.Injector/InjectionManager.h"
#include "../TimeMocker.Injector/ProcessWatcher.h"

#pragma comment(lib, "Psapi.lib")

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void EnableAnsi()
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

static const char* C_RESET  = "\033[0m";
static const char* C_CYAN   = "\033[36m";
static const char* C_GREEN  = "\033[32m";
static const char* C_YELLOW = "\033[33m";
static const char* C_RED    = "\033[31m";
static const char* C_GRAY   = "\033[90m";

static void Log(const wchar_t* msg)
{
    // Convert wide to UTF-8 for console output
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, msg, -1, buf, sizeof(buf), nullptr, nullptr);
    printf("%s[LOG]%s %s\n", C_GRAY, C_RESET, buf);
}

// Parse "YYYY-MM-DD HH:MM:SS" → SYSTEMTIME (local)
static bool ParseDateTime(const std::string& s, SYSTEMTIME& st)
{
    memset(&st, 0, sizeof(st));
    int Y, M, D, h, m, sec;
    if (sscanf_s(s.c_str(), "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) == 6 ||
        sscanf_s(s.c_str(), "%d/%d/%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) == 6)
    {
        st.wYear   = static_cast<WORD>(Y);
        st.wMonth  = static_cast<WORD>(M);
        st.wDay    = static_cast<WORD>(D);
        st.wHour   = static_cast<WORD>(h);
        st.wMinute = static_cast<WORD>(m);
        st.wSecond = static_cast<WORD>(sec);
        return true;
    }
    return false;
}

static std::string FormatDelta(LONGLONG deltaTicks)
{
    // deltaTicks: 100-ns units
    bool neg = deltaTicks < 0;
    LONGLONG abs = neg ? -deltaTicks : deltaTicks;

    LONGLONG secs  = abs / 10000000LL;
    LONGLONG mins  = secs / 60;   secs  %= 60;
    LONGLONG hours = mins / 60;   mins  %= 60;
    LONGLONG days  = hours / 24;  hours %= 24;

    char buf[128] = {};
    if (days)  sprintf_s(buf, "%s%lldd%02lldh%02lldm%02llds", neg?"-":"+", days, hours, mins, secs);
    else if (hours) sprintf_s(buf, "%s%lldh%02lldm%02llds", neg?"-":"+", hours, mins, secs);
    else if (mins)  sprintf_s(buf, "%s%lldm%02llds", neg?"-":"+", mins, secs);
    else            sprintf_s(buf, "%s%llds", neg?"-":"+", secs);
    return buf;
}

// ---------------------------------------------------------------------------
// Enumerate accessible processes
// ---------------------------------------------------------------------------
struct ProcInfo { DWORD pid; std::wstring name, path; };

static std::vector<ProcInfo> EnumProcesses(const std::wstring& filter = L"")
{
    std::vector<ProcInfo> result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return result; }

    do
    {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!hProc) continue;

        wchar_t pathBuf[MAX_PATH] = {}; DWORD len = MAX_PATH;
        QueryFullProcessImageNameW(hProc, 0, pathBuf, &len);
        CloseHandle(hProc);

        ProcInfo info;
        info.pid  = pe.th32ProcessID;
        info.name = pe.szExeFile;
        info.path = pathBuf;

        if (!filter.empty())
        {
            auto contains = [&](const std::wstring& hay, const std::wstring& needle)
            {
                std::wstring lh = hay, ln = needle;
                std::transform(lh.begin(), lh.end(), lh.begin(), ::towlower);
                std::transform(ln.begin(), ln.end(), ln.begin(), ::towlower);
                return lh.find(ln) != std::wstring::npos;
            };
            if (!contains(info.name, filter) && !contains(info.path, filter)) continue;
        }

        result.push_back(info);

    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);
    std::sort(result.begin(), result.end(), [](const ProcInfo& a, const ProcInfo& b)
    {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return result;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main()
{
    // Switch console to UTF-8
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT); // redirect wide to UTF-8
    _setmode(_fileno(stdout), _O_TEXT);   // reset (we'll use printf)
    EnableAnsi();

    printf("%s╔══════════════════════════════════════════════╗%s\n", C_CYAN, C_RESET);
    printf("%s║        TimeMocker — C++ / MS Detours         ║%s\n", C_CYAN, C_RESET);
    printf("%s╚══════════════════════════════════════════════╝%s\n", C_CYAN, C_RESET);
    printf("Type %shelp%s for command list.\n\n", C_YELLOW, C_RESET);

    // Check elevation
    {
        HANDLE hToken;
        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken);
        TOKEN_ELEVATION elev;
        DWORD sz = sizeof(elev);
        GetTokenInformation(hToken, TokenElevation, &elev, sz, &sz);
        CloseHandle(hToken);
        if (!elev.TokenIsElevated)
        {
            printf("%s[WARN] Not running as Administrator. Injection into protected processes will fail.%s\n\n", C_YELLOW, C_RESET);
        }
    }

    InjectionManager mgr;
    mgr.OnLog = [](const std::wstring& msg) { Log(msg.c_str()); };

    ProcessWatcher watcher(mgr);
    watcher.OnLog = [](const std::wstring& msg) { Log(msg.c_str()); };
    watcher.OnAutoInjected = [&](DWORD pid, const std::wstring& name, const std::wstring& /*path*/)
    {
        char buf[256]; WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
        printf("%s[AutoInject]%s [%lu] %s\n", C_GREEN, C_RESET, pid, buf);
    };

    LONGLONG fakeUtcTicks = TimeUtil::RealUtcTicks(); // start at real time (delta = 0)
    std::vector<PatternRule> ruleList; // local copy for display

    watcher.SetFakeUtcTicks(fakeUtcTicks);
    watcher.Start(1500);
    printf("%s[Watcher]%s Auto-inject watcher started.\n\n", C_GREEN, C_RESET);

    std::string line;
    while (true)
    {
        printf("%stimemocker>%s ", C_CYAN, C_RESET);
        fflush(stdout);
        if (!std::getline(std::cin, line)) break;

        // Tokenize
        std::istringstream iss(line);
        std::vector<std::string> tokens;
        {
            std::string tok;
            // Handle quoted tokens
            bool inQ = false; std::string cur;
            for (char c : line)
            {
                if (c == '"') { inQ = !inQ; }
                else if (c == ' ' && !inQ && !cur.empty()) { tokens.push_back(cur); cur.clear(); }
                else { cur += c; }
            }
            if (!cur.empty()) tokens.push_back(cur);
        }
        if (tokens.empty()) continue;

        std::string cmd = tokens[0];
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

        // ---- quit -----------------------------------------------------------
        if (cmd == "quit" || cmd == "exit") break;

        // ---- help -----------------------------------------------------------
        else if (cmd == "help")
        {
            printf(
                "  %slist%s [filter]              list running processes\n"
                "  %sinject%s <pid>               inject hook DLL into process\n"
                "  %seject%s  <pid>               remove hook from process\n"
                "  %stime%s   YYYY-MM-DD HH:MM:SS set fake time (local)\n"
                "  %stime%s   now                 reset to real time\n"
                "  %sstatus%s                     show injected processes + delta\n"
                "  %srule add%s <pattern>         add glob auto-inject rule\n"
                "  %srule add%s -r <pattern>      add regex auto-inject rule\n"
                "  %srule list%s                  list rules\n"
                "  %srule del%s <index>           remove rule by index\n"
                "  %swatch start%s / %sstop%s         control auto-inject watcher\n"
                "  %squit%s                       exit\n",
                C_YELLOW,C_RESET, C_YELLOW,C_RESET, C_YELLOW,C_RESET,
                C_YELLOW,C_RESET, C_YELLOW,C_RESET, C_YELLOW,C_RESET,
                C_YELLOW,C_RESET, C_YELLOW,C_RESET, C_YELLOW,C_RESET,
                C_YELLOW,C_RESET, C_YELLOW,C_RESET, C_YELLOW,C_RESET,
                C_YELLOW,C_RESET, C_YELLOW,C_RESET);
        }

        // ---- list -----------------------------------------------------------
        else if (cmd == "list")
        {
            std::wstring filter;
            if (tokens.size() > 1)
            {
                std::string fs = tokens[1];
                filter = std::wstring(fs.begin(), fs.end());
            }

            auto procs = EnumProcesses(filter);
            printf("%s%-8s %-28s %s%s\n", C_GRAY, "PID", "Name", "Path", C_RESET);
            for (auto& p : procs)
            {
                char name[128] = {}, path[512] = {};
                WideCharToMultiByte(CP_UTF8, 0, p.name.c_str(), -1, name, sizeof(name), nullptr, nullptr);
                WideCharToMultiByte(CP_UTF8, 0, p.path.c_str(), -1, path, sizeof(path), nullptr, nullptr);
                bool inj = mgr.IsInjected(p.pid);
                printf("%-8lu %-28s %s%s\n", p.pid, name, path, inj ? " ✓" : "");
            }
            printf("  %lu processes shown\n", (DWORD)procs.size());
        }

        // ---- inject ---------------------------------------------------------
        else if (cmd == "inject")
        {
            if (tokens.size() < 2) { printf("%sUsage: inject <pid>%s\n", C_RED, C_RESET); continue; }
            DWORD pid = (DWORD)atoul(tokens[1].c_str());
            std::wstring err;
            if (mgr.Inject(pid, fakeUtcTicks, &err))
            {
                auto delta = TimeUtil::ComputeDelta(fakeUtcTicks);
                printf("%s[OK]%s Injected pid=%lu  delta=%s\n",
                       C_GREEN, C_RESET, pid, FormatDelta(delta).c_str());
            }
            else
            {
                char ebuf[512]; WideCharToMultiByte(CP_UTF8,0,err.c_str(),-1,ebuf,sizeof(ebuf),nullptr,nullptr);
                printf("%s[FAIL]%s %s\n", C_RED, C_RESET, ebuf);
            }
        }

        // ---- eject ----------------------------------------------------------
        else if (cmd == "eject")
        {
            if (tokens.size() < 2) { printf("%sUsage: eject <pid>%s\n", C_RED, C_RESET); continue; }
            DWORD pid = (DWORD)atoul(tokens[1].c_str());
            if (mgr.Eject(pid))
                printf("%s[OK]%s Ejected pid=%lu\n", C_GREEN, C_RESET, pid);
            else
                printf("%s[FAIL]%s pid=%lu not injected\n", C_RED, C_RESET, pid);
        }

        // ---- time -----------------------------------------------------------
        else if (cmd == "time")
        {
            if (tokens.size() < 2) { printf("%sUsage: time YYYY-MM-DD HH:MM:SS  |  time now%s\n", C_RED, C_RESET); continue; }

            std::string arg = tokens[1];
            if (tokens.size() >= 3) arg += " " + tokens[2]; // join date and time

            if (arg == "now")
            {
                fakeUtcTicks = TimeUtil::RealUtcTicks();
            }
            else
            {
                SYSTEMTIME st;
                if (!ParseDateTime(arg, st))
                {
                    printf("%sInvalid format. Use: YYYY-MM-DD HH:MM:SS%s\n", C_RED, C_RESET);
                    continue;
                }
                fakeUtcTicks = TimeUtil::LocalSystemTimeToUtcTicks(st);
            }

            mgr.SetFakeTimeAll(fakeUtcTicks);
            watcher.SetFakeUtcTicks(fakeUtcTicks);

            auto delta = TimeUtil::ComputeDelta(fakeUtcTicks);
            // Display local time
            SYSTEMTIME disp;
            FILETIME ft;
            ULARGE_INTEGER ui; ui.QuadPart = (ULONGLONG)fakeUtcTicks;
            ft.dwLowDateTime = ui.LowPart; ft.dwHighDateTime = ui.HighPart;
            FILETIME lft; FileTimeToLocalFileTime(&ft, &lft);
            FileTimeToSystemTime(&lft, &disp);

            printf("%s[Time]%s Fake time set to %04d-%02d-%02d %02d:%02d:%02d (local)  delta=%s\n",
                   C_GREEN, C_RESET,
                   disp.wYear, disp.wMonth, disp.wDay,
                   disp.wHour, disp.wMinute, disp.wSecond,
                   FormatDelta(delta).c_str());
        }

        // ---- status ---------------------------------------------------------
        else if (cmd == "status")
        {
            LONGLONG delta = TimeUtil::ComputeDelta(fakeUtcTicks);

            // Show local fake time
            FILETIME ft; ULARGE_INTEGER ui; ui.QuadPart = (ULONGLONG)fakeUtcTicks;
            ft.dwLowDateTime = ui.LowPart; ft.dwHighDateTime = ui.HighPart;
            FILETIME lft; FileTimeToLocalFileTime(&ft, &lft);
            SYSTEMTIME st; FileTimeToSystemTime(&lft, &st);

            printf("Fake time : %04d-%02d-%02d %02d:%02d:%02d (local)  delta=%s\n",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond,
                   FormatDelta(delta).c_str());

            printf("Injected processes:\n");
            bool any = false;
            mgr.ForEach([&](const InjectedProcessInfo& p)
            {
                any = true;
                char name[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, p.ProcessName.c_str(), -1, name, sizeof(name), nullptr, nullptr);
                printf("  %s[%lu]%s %s\n", C_GREEN, p.Pid, C_RESET, name);
            });
            if (!any) printf("  (none)\n");
        }

        // ---- rule -----------------------------------------------------------
        else if (cmd == "rule")
        {
            if (tokens.size() < 2) { printf("Subcommands: add, list, del\n"); continue; }
            std::string sub = tokens[1];
            std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);

            if (sub == "list")
            {
                if (ruleList.empty()) { printf("  (no rules)\n"); continue; }
                for (size_t i = 0; i < ruleList.size(); i++)
                {
                    char pat[512]; WideCharToMultiByte(CP_UTF8, 0, ruleList[i].Pattern.c_str(), -1, pat, sizeof(pat), nullptr, nullptr);
                    printf("  [%zu] %s%s%s  (%s)\n",
                           i, C_YELLOW, pat, C_RESET,
                           ruleList[i].UseRegex ? "regex" : "glob");
                }
            }
            else if (sub == "add")
            {
                if (tokens.size() < 3) { printf("Usage: rule add [-r] <pattern>\n"); continue; }
                bool isRegex = false;
                std::string patStr;
                if (tokens[2] == "-r" || tokens[2] == "--regex")
                {
                    isRegex = true;
                    if (tokens.size() < 4) { printf("Missing pattern after -r\n"); continue; }
                    patStr = tokens[3];
                }
                else
                {
                    patStr = tokens[2];
                }

                PatternRule rule;
                rule.Pattern  = std::wstring(patStr.begin(), patStr.end());
                rule.UseRegex = isRegex;
                rule.Enabled  = true;

                watcher.AddRule(rule);
                ruleList.push_back(rule);

                printf("%s[OK]%s Rule added: '%s' (%s)\n",
                       C_GREEN, C_RESET, patStr.c_str(), isRegex ? "regex" : "glob");
            }
            else if (sub == "del")
            {
                if (tokens.size() < 3) { printf("Usage: rule del <index>\n"); continue; }
                size_t idx = (size_t)atoi(tokens[2].c_str());
                if (idx >= ruleList.size()) { printf("%sIndex out of range%s\n", C_RED, C_RESET); continue; }

                watcher.RemoveRule(ruleList[idx].Pattern);
                ruleList.erase(ruleList.begin() + idx);
                printf("%s[OK]%s Rule removed\n", C_GREEN, C_RESET);
            }
            else
            {
                printf("Unknown rule subcommand: %s\n", sub.c_str());
            }
        }

        // ---- watch ----------------------------------------------------------
        else if (cmd == "watch")
        {
            if (tokens.size() < 2) { printf("Usage: watch start|stop\n"); continue; }
            std::string sub = tokens[1];
            if (sub == "start") { watcher.Start(); printf("%s[Watcher]%s Started\n", C_GREEN, C_RESET); }
            else if (sub == "stop") { watcher.Stop(); printf("%s[Watcher]%s Stopped\n", C_YELLOW, C_RESET); }
        }

        else
        {
            printf("%sUnknown command: %s%s  (type help)\n", C_RED, cmd.c_str(), C_RESET);
        }
    }

    watcher.Stop();
    printf("\nGoodbye.\n");
    return 0;
}
