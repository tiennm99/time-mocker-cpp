// =============================================================================
// TimeMocker.UI — ImGui + DirectX 11 frontend
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <d3d11.h>
#include <shellapi.h>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"

#include <string>
#include <vector>
#include <algorithm>
#include <mutex>
#include <deque>
#include <cstdio>

#include "../TimeMocker.Injector/InjectionManager.h"
#include "../TimeMocker.Injector/ProcessWatcher.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "Psapi.lib")

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static bool CreateDeviceD3D(HWND);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);

// ─────────────────────────────────────────────────────────────────────────────
// D3D globals
// ─────────────────────────────────────────────────────────────────────────────
static ID3D11Device*            g_pd3dDevice          = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext    = nullptr;
static IDXGISwapChain*          g_pSwapChain           = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// App state
// ─────────────────────────────────────────────────────────────────────────────
struct ProcessRow { DWORD pid; std::string name, path; bool injected; };
struct RuleRow    { std::string pattern; bool useRegex, enabled; };
struct LogEntry   { std::string ts, msg; ImVec4 color; };

static InjectionManager* g_injMgr  = nullptr;
static ProcessWatcher*   g_watcher = nullptr;

static std::vector<ProcessRow> g_procRows;
static char                    g_procFilter[128] = {};
static std::vector<RuleRow>    g_rules;
static std::deque<LogEntry>    g_log;
static std::mutex              g_logMutex;
static bool                    g_logScrollToBottom = false;

static int    g_fakeYear = 2024, g_fakeMon = 1,    g_fakeDay  = 1;
static int    g_fakeHour = 0,    g_fakeMin = 0,    g_fakeSec  = 0;
static LONGLONG g_fakeUtcTicks = 0;
static bool     g_timeApplied  = false;

static char  g_newPattern[256] = {};
static bool  g_newUseRegex     = false;

// ─────────────────────────────────────────────────────────────────────────────
// Palette — terminal green on near-black
// ─────────────────────────────────────────────────────────────────────────────
namespace Pal {
    const ImVec4 Bg         = { 0.07f, 0.08f, 0.09f, 1.f };
    const ImVec4 Panel      = { 0.10f, 0.11f, 0.13f, 1.f };
    const ImVec4 Border     = { 0.18f, 0.22f, 0.25f, 1.f };
    const ImVec4 Accent     = { 0.18f, 0.78f, 0.44f, 1.f };
    const ImVec4 AccentDim  = { 0.10f, 0.48f, 0.26f, 1.f };
    const ImVec4 AccentHot  = { 0.30f, 1.00f, 0.60f, 1.f };
    const ImVec4 Danger     = { 0.82f, 0.22f, 0.22f, 1.f };
    const ImVec4 DangerHot  = { 1.00f, 0.32f, 0.32f, 1.f };
    const ImVec4 Warning    = { 0.92f, 0.68f, 0.08f, 1.f };
    const ImVec4 Muted      = { 0.36f, 0.42f, 0.46f, 1.f };
    const ImVec4 Text       = { 0.80f, 0.88f, 0.82f, 1.f };
    const ImVec4 TextDim    = { 0.46f, 0.52f, 0.48f, 1.f };
    const ImVec4 LogInfo    = { 0.50f, 0.88f, 0.62f, 1.f };
    const ImVec4 LogWarn    = { 0.92f, 0.76f, 0.28f, 1.f };
    const ImVec4 LogErr     = { 0.96f, 0.38f, 0.38f, 1.f };
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static void AppLog(const std::string& msg, ImVec4 color = Pal::LogInfo)
{
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[12]; sprintf_s(ts, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_log.push_back({ ts, msg, color });
    if (g_log.size() > 512) g_log.pop_front();
    g_logScrollToBottom = true;
}

static void WLog(const std::wstring& w)
{
    char b[1024]; WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof(b), nullptr, nullptr);
    AppLog(b);
}

static std::string FormatDelta(LONGLONG d)
{
    if (!d) return "+0s";
    bool neg = d < 0; LONGLONG a = neg ? -d : d;
    LONGLONG s = a/10000000LL, m=s/60; s%=60;
    LONGLONG h = m/60; m%=60; LONGLONG dy=h/24; h%=24;
    char buf[64];
    if (dy)      sprintf_s(buf,"%s%lldd%02lldh%02lldm%02llds",neg?"-":"+",dy,h,m,s);
    else if (h)  sprintf_s(buf,"%s%lldh%02lldm%02llds",neg?"-":"+",h,m,s);
    else if (m)  sprintf_s(buf,"%s%lldm%02llds",neg?"-":"+",m,s);
    else         sprintf_s(buf,"%s%llds",neg?"-":"+",s);
    return buf;
}

static void RefreshProcessList()
{
    g_procRows.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return; }
    do {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t pw[MAX_PATH]={}; DWORD l=MAX_PATH; QueryFullProcessImageNameW(h,0,pw,&l); CloseHandle(h);
        char n[256]={}, p[512]={};
        WideCharToMultiByte(CP_UTF8,0,pe.szExeFile,-1,n,sizeof(n),nullptr,nullptr);
        WideCharToMultiByte(CP_UTF8,0,pw,-1,p,sizeof(p),nullptr,nullptr);
        g_procRows.push_back({pe.th32ProcessID, n, p, g_injMgr->IsInjected(pe.th32ProcessID)});
    } while(Process32NextW(snap,&pe));
    CloseHandle(snap);
    std::sort(g_procRows.begin(),g_procRows.end(),[](auto&a,auto&b){return _stricmp(a.name.c_str(),b.name.c_str())<0;});
}

static void ApplyTime()
{
    SYSTEMTIME st={};
    st.wYear=(WORD)g_fakeYear; st.wMonth=(WORD)g_fakeMon; st.wDay=(WORD)g_fakeDay;
    st.wHour=(WORD)g_fakeHour; st.wMinute=(WORD)g_fakeMin; st.wSecond=(WORD)g_fakeSec;
    g_fakeUtcTicks = TimeUtil::LocalSystemTimeToUtcTicks(st);
    g_injMgr->SetFakeTimeAll(g_fakeUtcTicks);
    g_watcher->SetFakeUtcTicks(g_fakeUtcTicks);
    g_timeApplied = true;
    AppLog("Time applied  delta=" + FormatDelta(TimeUtil::ComputeDelta(g_fakeUtcTicks)));
}

static void ResetTimeToNow()
{
    SYSTEMTIME st; GetLocalTime(&st);
    g_fakeYear=st.wYear; g_fakeMon=st.wMonth; g_fakeDay=st.wDay;
    g_fakeHour=st.wHour; g_fakeMin=st.wMinute; g_fakeSec=st.wSecond;
    g_fakeUtcTicks = TimeUtil::RealUtcTicks();
    g_injMgr->SetFakeTimeAll(g_fakeUtcTicks);
    g_watcher->SetFakeUtcTicks(g_fakeUtcTicks);
    g_timeApplied = false;
    AppLog("Time reset to real time", Pal::LogWarn);
}

// ─────────────────────────────────────────────────────────────────────────────
// ImGui style
// ─────────────────────────────────────────────────────────────────────────────
static void ApplyTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding=6; s.ChildRounding=4; s.FrameRounding=4;
    s.PopupRounding=4; s.TabRounding=5; s.GrabRounding=4;
    s.WindowBorderSize=1; s.FrameBorderSize=1;
    s.ItemSpacing={8,5}; s.FramePadding={8,4};
    s.WindowPadding={12,10}; s.ScrollbarSize=10;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = Pal::Bg;
    c[ImGuiCol_ChildBg]              = Pal::Panel;
    c[ImGuiCol_PopupBg]              = {0.09f,0.10f,0.12f,0.98f};
    c[ImGuiCol_Border]               = Pal::Border;
    c[ImGuiCol_FrameBg]              = {0.13f,0.15f,0.17f,1.f};
    c[ImGuiCol_FrameBgHovered]       = {0.17f,0.20f,0.23f,1.f};
    c[ImGuiCol_FrameBgActive]        = {0.10f,0.32f,0.20f,1.f};
    c[ImGuiCol_TitleBg]              = {0.07f,0.08f,0.09f,1.f};
    c[ImGuiCol_TitleBgActive]        = {0.07f,0.20f,0.13f,1.f};
    c[ImGuiCol_ScrollbarBg]          = {0.07f,0.08f,0.09f,1.f};
    c[ImGuiCol_ScrollbarGrab]        = Pal::AccentDim;
    c[ImGuiCol_ScrollbarGrabHovered] = Pal::Accent;
    c[ImGuiCol_ScrollbarGrabActive]  = Pal::AccentHot;
    c[ImGuiCol_CheckMark]            = Pal::AccentHot;
    c[ImGuiCol_SliderGrab]           = Pal::Accent;
    c[ImGuiCol_SliderGrabActive]     = Pal::AccentHot;
    c[ImGuiCol_Button]               = {0.11f,0.28f,0.18f,1.f};
    c[ImGuiCol_ButtonHovered]        = {0.15f,0.40f,0.24f,1.f};
    c[ImGuiCol_ButtonActive]         = {0.18f,0.55f,0.30f,1.f};
    c[ImGuiCol_Header]               = {0.09f,0.26f,0.16f,1.f};
    c[ImGuiCol_HeaderHovered]        = {0.12f,0.34f,0.21f,1.f};
    c[ImGuiCol_HeaderActive]         = {0.14f,0.42f,0.26f,1.f};
    c[ImGuiCol_Separator]            = Pal::Border;
    c[ImGuiCol_Tab]                  = {0.09f,0.18f,0.12f,1.f};
    c[ImGuiCol_TabHovered]           = {0.13f,0.36f,0.22f,1.f};
    c[ImGuiCol_TabActive]            = {0.12f,0.42f,0.26f,1.f};
    c[ImGuiCol_TabUnfocused]         = {0.07f,0.12f,0.09f,1.f};
    c[ImGuiCol_TabUnfocusedActive]   = {0.09f,0.24f,0.15f,1.f};
    c[ImGuiCol_Text]                 = Pal::Text;
    c[ImGuiCol_TextDisabled]         = Pal::Muted;
    c[ImGuiCol_TableHeaderBg]        = {0.09f,0.20f,0.13f,1.f};
    c[ImGuiCol_TableBorderStrong]    = Pal::Border;
    c[ImGuiCol_TableBorderLight]     = {0.14f,0.16f,0.18f,1.f};
    c[ImGuiCol_TableRowBg]           = {0.10f,0.11f,0.13f,1.f};
    c[ImGuiCol_TableRowBgAlt]        = {0.12f,0.13f,0.15f,1.f};
    c[ImGuiCol_NavHighlight]         = Pal::Accent;
}

// ─────────────────────────────────────────────────────────────────────────────
// UI panels
// ─────────────────────────────────────────────────────────────────────────────

static void RenderHeader()
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f,0.16f,0.11f,1.f));
    ImGui::BeginChild("##hdr", ImVec2(0,46), false);

    SYSTEMTIME real; GetLocalTime(&real);
    char rb[40]; sprintf_s(rb,"REAL  %04d-%02d-%02d  %02d:%02d:%02d",
        real.wYear,real.wMonth,real.wDay,real.wHour,real.wMinute,real.wSecond);
    ImGui::SetCursorPos({12,13});
    ImGui::TextColored(Pal::Muted, "%s", rb);

    // Compute current fake local time (flowing)
    LONGLONG curFake = (g_fakeUtcTicks==0)
        ? TimeUtil::RealUtcTicks()
        : TimeUtil::RealUtcTicks() + TimeUtil::ComputeDelta(g_fakeUtcTicks);
    FILETIME ft; ULARGE_INTEGER ui; ui.QuadPart=(ULONGLONG)curFake;
    ft.dwLowDateTime=ui.LowPart; ft.dwHighDateTime=ui.HighPart;
    FILETIME lft; FileTimeToLocalFileTime(&ft,&lft);
    SYSTEMTIME fk; FileTimeToSystemTime(&lft,&fk);
    char fb[48]; sprintf_s(fb,"MOCK  %04d-%02d-%02d  %02d:%02d:%02d",
        fk.wYear,fk.wMonth,fk.wDay,fk.wHour,fk.wMinute,fk.wSecond);

    float tw=ImGui::CalcTextSize(fb).x, ww=ImGui::GetWindowWidth();
    ImGui::SetCursorPos({(ww-tw)*.5f,13});
    ImGui::TextColored(g_timeApplied ? Pal::AccentHot : Pal::Text, "%s", fb);

    LONGLONG delta=(g_fakeUtcTicks==0)?0:TimeUtil::ComputeDelta(g_fakeUtcTicks);
    std::string ds=FormatDelta(delta);
    float dw2=ImGui::CalcTextSize(ds.c_str()).x;
    ImGui::SetCursorPos({ww-dw2-12,13});
    ImGui::TextColored(delta==0?Pal::Muted:Pal::Warning,"%s",ds.c_str());

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

static void RenderTimePanel()
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.09f,0.10f,0.12f,1.f));
    ImGui::BeginChild("##tp", ImVec2(0,68), true);
    ImGui::TextColored(Pal::AccentDim,"SET FAKE TIME"); ImGui::SameLine(0,18);

    auto IntW=[](const char* id, int* v, int w){
        ImGui::PushItemWidth(w); ImGui::InputInt(id,v,0,0); ImGui::PopItemWidth();
    };
    IntW("##yr",&g_fakeYear,54); ImGui::SameLine(0,3);
    ImGui::TextColored(Pal::Muted,"-"); ImGui::SameLine(0,3);
    IntW("##mo",&g_fakeMon,32); ImGui::SameLine(0,3);
    ImGui::TextColored(Pal::Muted,"-"); ImGui::SameLine(0,3);
    IntW("##dy",&g_fakeDay,32); ImGui::SameLine(0,14);
    IntW("##hr",&g_fakeHour,32); ImGui::SameLine(0,3);
    ImGui::TextColored(Pal::Muted,":"); ImGui::SameLine(0,3);
    IntW("##mn",&g_fakeMin,32); ImGui::SameLine(0,3);
    ImGui::TextColored(Pal::Muted,":"); ImGui::SameLine(0,3);
    IntW("##sc",&g_fakeSec,32);

    g_fakeMon =std::clamp(g_fakeMon,1,12); g_fakeDay =std::clamp(g_fakeDay,1,31);
    g_fakeHour=std::clamp(g_fakeHour,0,23);g_fakeMin =std::clamp(g_fakeMin,0,59);
    g_fakeSec =std::clamp(g_fakeSec,0,59);

    ImGui::SameLine(0,16);
    ImGui::PushStyleColor(ImGuiCol_Button,       ImVec4(0.08f,0.40f,0.22f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.12f,0.54f,0.30f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f,0.66f,0.36f,1.f));
    if(ImGui::Button(" SET ",{54,0})) ApplyTime();
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0,6);
    ImGui::PushStyleColor(ImGuiCol_Button,       ImVec4(0.14f,0.16f,0.18f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.20f,0.23f,0.26f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f,0.32f,0.20f,1.f));
    if(ImGui::Button(" NOW ",{54,0})) ResetTimeToNow();
    ImGui::PopStyleColor(3);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

static void RenderProcessTab()
{
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##pf","Search processes...",g_procFilter,sizeof(g_procFilter));
    ImGui::SameLine();
    if(ImGui::Button(" Refresh ")) RefreshProcessList();

    int ic=0; for(auto&r:g_procRows) if(r.injected) ic++;
    if(ic>0){
        ImGui::SameLine();
        char b[32]; sprintf_s(b," %d injected ",ic);
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.08f,0.28f,0.16f,1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.08f,0.28f,0.16f,1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0.08f,0.28f,0.16f,1.f));
        ImGui::Button(b); ImGui::PopStyleColor(3);
    }
    ImGui::Spacing();

    ImGuiTableFlags tf = ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|
                         ImGuiTableFlags_ScrollY|ImGuiTableFlags_SizingFixedFit|
                         ImGuiTableFlags_Resizable;
    if(ImGui::BeginTable("##pt",4,tf,ImVec2(0,ImGui::GetContentRegionAvail().y)))
    {
        ImGui::TableSetupScrollFreeze(0,1);
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed,62);
        ImGui::TableSetupColumn("Name",ImGuiTableColumnFlags_WidthFixed,165);
        ImGui::TableSetupColumn("Path",ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Hook",ImGuiTableColumnFlags_WidthFixed,52);
        ImGui::TableHeadersRow();

        std::string filt(g_procFilter);
        std::transform(filt.begin(),filt.end(),filt.begin(),::tolower);

        for(auto& row : g_procRows)
        {
            if(!filt.empty()){
                std::string ln=row.name, lp=row.path;
                std::transform(ln.begin(),ln.end(),ln.begin(),::tolower);
                std::transform(lp.begin(),lp.end(),lp.begin(),::tolower);
                if(ln.find(filt)==std::string::npos && lp.find(filt)==std::string::npos) continue;
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(Pal::Muted,"%lu",row.pid);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(row.injected?Pal::AccentHot:Pal::Text,"%s",row.name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(Pal::TextDim,"%s",row.path.c_str());
            ImGui::TableSetColumnIndex(3);
            bool inj=row.injected;
            char id[32]; sprintf_s(id,"##i%lu",row.pid);
            if(inj){
                ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(0.06f,0.26f,0.14f,1.f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(0.08f,0.32f,0.18f,1.f));
            }
            if(ImGui::Checkbox(id,&inj)){
                if(inj){
                    std::wstring err;
                    LONGLONG t=g_fakeUtcTicks==0?TimeUtil::RealUtcTicks():g_fakeUtcTicks;
                    if(g_injMgr->Inject(row.pid,t,&err)){
                        row.injected=true;
                        AppLog("Injected ["+std::to_string(row.pid)+"] "+row.name);
                    } else {
                        char eb[512]; WideCharToMultiByte(CP_UTF8,0,err.c_str(),-1,eb,sizeof(eb),nullptr,nullptr);
                        AppLog("FAILED inject ["+std::to_string(row.pid)+"]: "+eb,Pal::LogErr);
                    }
                } else {
                    g_injMgr->Eject(row.pid);
                    row.injected=false;
                    AppLog("Ejected ["+std::to_string(row.pid)+"] "+row.name,Pal::LogWarn);
                }
            }
            if(inj) ImGui::PopStyleColor(2);
        }
        ImGui::EndTable();
    }
}

static void RenderRulesTab()
{
    ImGui::SetNextItemWidth(340);
    ImGui::InputTextWithHint("##np","e.g. C:\\Games\\*   or   ^.*\\MyApp\\.exe$",g_newPattern,sizeof(g_newPattern));
    ImGui::SameLine(); ImGui::Checkbox("Regex",&g_newUseRegex); ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,       ImVec4(0.08f,0.36f,0.20f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.12f,0.48f,0.26f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f,0.60f,0.32f,1.f));
    if(ImGui::Button(" + Add Rule ") && g_newPattern[0]){
        RuleRow r{g_newPattern,g_newUseRegex,true};
        g_rules.push_back(r);
        PatternRule pr;
        pr.Pattern=std::wstring(r.pattern.begin(),r.pattern.end());
        pr.UseRegex=r.useRegex; pr.Enabled=true;
        g_watcher->AddRule(pr);
        AppLog(std::string("Rule: ")+g_newPattern+(g_newUseRegex?" [regex]":" [glob]"));
        g_newPattern[0]=0;
    }
    ImGui::PopStyleColor(3);

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    ImGuiTableFlags tf=ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|
                       ImGuiTableFlags_ScrollY|ImGuiTableFlags_SizingFixedFit;
    if(ImGui::BeginTable("##rt",4,tf,ImVec2(0,ImGui::GetContentRegionAvail().y)))
    {
        ImGui::TableSetupScrollFreeze(0,1);
        ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed, 28);
        ImGui::TableSetupColumn("Pattern",ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 86);
        ImGui::TableHeadersRow();

        int del=-1;
        for(int i=0;i<(int)g_rules.size();i++){
            auto& r=g_rules[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextColored(Pal::Muted,"%d",i);
            ImGui::TableSetColumnIndex(1); ImGui::TextColored(r.enabled?Pal::Text:Pal::Muted,"%s",r.pattern.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextColored(r.useRegex?Pal::Warning:Pal::AccentDim,"%s",r.useRegex?"regex":"glob");
            ImGui::TableSetColumnIndex(3);
            char eid[32]; sprintf_s(eid,"##e%d",i); bool en=r.enabled;
            if(ImGui::Checkbox(eid,&en)) r.enabled=en;
            ImGui::SameLine(0,6);
            ImGui::PushStyleColor(ImGuiCol_Button,       ImVec4(0.30f,0.09f,0.09f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.48f,0.12f,0.12f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.62f,0.16f,0.16f,1.f));
            char did[32]; sprintf_s(did," X ##d%d",i);
            if(ImGui::Button(did)) del=i;
            ImGui::PopStyleColor(3);
        }
        if(del>=0){
            std::wstring wp(g_rules[del].pattern.begin(),g_rules[del].pattern.end());
            AppLog("Rule removed: "+g_rules[del].pattern,Pal::LogWarn);
            g_watcher->RemoveRule(wp);
            g_rules.erase(g_rules.begin()+del);
        }
        ImGui::EndTable();
    }
}

static void RenderLogTab()
{
    if(ImGui::Button(" Clear ")){
        std::lock_guard<std::mutex> lk(g_logMutex);
        g_log.clear();
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.05f,0.06f,0.07f,1.f));
    ImGui::BeginChild("##lg",ImVec2(0,ImGui::GetContentRegionAvail().y),false,ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        for(auto& e : g_log){
            ImGui::TextColored(Pal::Muted,"%s",e.ts.c_str());
            ImGui::SameLine(0,8);
            ImGui::TextColored(e.color,"%s",e.msg.c_str());
        }
    }
    if(g_logScrollToBottom){ ImGui::SetScrollHereY(1.f); g_logScrollToBottom=false; }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ─────────────────────────────────────────────────────────────────────────────
// Main render
// ─────────────────────────────────────────────────────────────────────────────
static void RenderUI()
{
    const ImGuiViewport* vp=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags wf=ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|
                        ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0);
    ImGui::Begin("##root",nullptr,wf);
    ImGui::PopStyleVar(2);

    // Title bar row
    ImGui::TextColored(Pal::Accent,"TIMEMOCKER");
    ImGui::SameLine(0,10);
    ImGui::TextColored(Pal::Muted,"//  Win32 API hook via MS Detours  //  DirectX 11 UI");

    ImGui::Spacing();
    RenderHeader();
    ImGui::Spacing();
    RenderTimePanel();
    ImGui::Spacing();

    if(ImGui::BeginTabBar("##tabs")){
        if(ImGui::BeginTabItem("  Processes  "))  { ImGui::Spacing(); RenderProcessTab(); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("  Auto-Inject  ")) { ImGui::Spacing(); RenderRulesTab();  ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("  Log  "))         { ImGui::Spacing(); RenderLogTab();    ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
// WinMain
// ─────────────────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    // Elevation check
    {
        HANDLE hTok; OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&hTok);
        TOKEN_ELEVATION e{}; DWORD sz=sizeof(e);
        GetTokenInformation(hTok,TokenElevation,&e,sz,&sz); CloseHandle(hTok);
        if(!e.TokenIsElevated){
            MessageBoxA(nullptr,"TimeMocker requires Administrator.\nRestarting elevated...","Elevation Required",MB_ICONWARNING|MB_OK);
            wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr,p,MAX_PATH);
            ShellExecuteW(nullptr,L"runas",p,nullptr,nullptr,SW_SHOWNORMAL);
            return 0;
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize=sizeof(wc); wc.style=CS_CLASSDC; wc.lpfnWndProc=WndProc;
    wc.hInstance=hInstance; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.lpszClassName=L"TimeMockerWnd";
    RegisterClassExW(&wc);

    HWND hwnd=CreateWindowExW(0,L"TimeMockerWnd",L"TimeMocker",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,1280,780,nullptr,nullptr,hInstance,nullptr);

    if(!CreateDeviceD3D(hwnd)){ CleanupDeviceD3D(); return 1; }
    ShowWindow(hwnd,SW_SHOWDEFAULT); UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO();
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename=nullptr;
    ApplyTheme();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice,g_pd3dDeviceContext);

    g_injMgr  = new InjectionManager();
    g_watcher = new ProcessWatcher(*g_injMgr);
    g_injMgr->OnLog  = WLog;
    g_watcher->OnLog = WLog;
    g_watcher->OnAutoInjected=[](DWORD pid,const std::wstring& name,const std::wstring&){
        char b[256]; WideCharToMultiByte(CP_UTF8,0,name.c_str(),-1,b,sizeof(b),nullptr,nullptr);
        AppLog(std::string("[AutoInject] [")+std::to_string(pid)+"] "+b, Pal::AccentHot);
    };

    ResetTimeToNow();
    RefreshProcessList();
    g_watcher->Start(1500);
    AppLog("TimeMocker started. Watcher active.");

    bool done=false;
    while(!done){
        MSG msg;
        while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){
            TranslateMessage(&msg); DispatchMessage(&msg);
            if(msg.message==WM_QUIT) done=true;
        }
        if(done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderUI();
        ImGui::Render();

        const float cc[4]={0.07f,0.08f,0.09f,1.f};
        g_pd3dDeviceContext->OMSetRenderTargets(1,&g_mainRenderTargetView,nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1,0);
    }

    g_watcher->Stop();
    delete g_watcher; delete g_injMgr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName,hInstance);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// D3D boilerplate
// ─────────────────────────────────────────────────────────────────────────────
static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount=2; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow=hWnd; sd.SampleDesc.Count=1; sd.Windowed=TRUE;
    sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl; const D3D_FEATURE_LEVEL fls[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0};
    HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,fls,2,
        D3D11_SDK_VERSION,&sd,&g_pSwapChain,&g_pd3dDevice,&fl,&g_pd3dDeviceContext);
    if(hr==DXGI_ERROR_UNSUPPORTED)
        hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,fls,2,
            D3D11_SDK_VERSION,&sd,&g_pSwapChain,&g_pd3dDevice,&fl,&g_pd3dDeviceContext);
    if(FAILED(hr)) return false;
    CreateRenderTarget(); return true;
}
static void CleanupDeviceD3D(){
    CleanupRenderTarget();
    if(g_pSwapChain){g_pSwapChain->Release();g_pSwapChain=nullptr;}
    if(g_pd3dDeviceContext){g_pd3dDeviceContext->Release();g_pd3dDeviceContext=nullptr;}
    if(g_pd3dDevice){g_pd3dDevice->Release();g_pd3dDevice=nullptr;}
}
static void CreateRenderTarget(){
    ID3D11Texture2D* bb;
    g_pSwapChain->GetBuffer(0,IID_PPV_ARGS(&bb));
    g_pd3dDevice->CreateRenderTargetView(bb,nullptr,&g_mainRenderTargetView);
    bb->Release();
}
static void CleanupRenderTarget(){
    if(g_mainRenderTargetView){g_mainRenderTargetView->Release();g_mainRenderTargetView=nullptr;}
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if(ImGui_ImplWin32_WndProcHandler(hWnd,msg,wParam,lParam)) return true;
    switch(msg){
    case WM_SIZE:
        if(g_pd3dDevice && wParam!=SIZE_MINIMIZED){
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0,LOWORD(lParam),HIWORD(lParam),DXGI_FORMAT_UNKNOWN,0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if((wParam&0xfff0)==SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd,msg,wParam,lParam);
}
