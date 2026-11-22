// Remote viewing. Liquid's windows are normally left out of screen capture (see Gfx::SetRemoteView),
// and the tools used to remote into this PC stream the screen through that same capture, so they
// showed a desktop with no menu bar and no dock. Every two seconds this checks whether one of them
// is streaming and, while it is, puts Liquid into the stream.
//   Moonlight: Sunshine (Vibeshine) writes "CLIENT CONNECTED" / "CLIENT DISCONNECTED" to its log
//              (Program Files\Sunshine\config\logs, one file per Sunshine start).
//   RustDesk:  while someone is connected it runs its connection manager, "RustDesk.exe --cm",
//              in this session.
//   Axiom:     its service (session 0) starts a "session worker", host-agent.exe, in this session
//              for each remote session and it exits when the viewer disconnects (its parent is the
//              service, which tells it apart from the desktop app's own host-agent; worker logs in
//              ProgramData\Axiom\logs: "session worker starting" ... "session worker finished").
//              It also changes a screen's resolution for the stream, which restarts Liquid; this
//              check runs on Liquid's first frame, so the bars are back in the stream right away.
//   Remote Desktop: Windows says so (SM_REMOTESESSION).
//   Anything else: "Liquid.exe --remote on" / "--remote off".
#include "shell.h"
#include <tlhelp32.h>
#include <winternl.h>
#include <wtsapi32.h>
#include <unordered_map>

namespace remote {
namespace {
std::wstring s_logPath;
ULONGLONG s_logPos = 0, s_lastPoll = 0;
std::string s_partial;
int s_clients = 0;

std::wstring NewestSunshineLog() {
    wchar_t pf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) return L"";
    std::wstring dir = std::wstring(pf) + L"\\Sunshine\\config\\logs";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\sunshine-*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring best;   // sunshine-YYYYMMDD-HHMMSS-mmm.log: the newest sorts last
    do {
        if (best.empty() || _wcsicmp(fd.cFileName, best.c_str()) > 0) best = fd.cFileName;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return dir + L"\\" + best;
}

// Reads whatever Sunshine added to its log since last time and keeps a count of connected clients.
bool SunshineClients() {
    std::wstring p = NewestSunshineLog();
    if (p != s_logPath) { s_logPath = p; s_logPos = 0; s_clients = 0; s_partial.clear(); }
    if (p.empty()) return false;
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return s_clients > 0;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    if ((ULONGLONG)sz.QuadPart < s_logPos) { s_logPos = 0; s_clients = 0; s_partial.clear(); }
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)s_logPos;
    SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
    static char buf[65536];
    DWORD got = 0;
    while ((ULONGLONG)sz.QuadPart > s_logPos && ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) {
        s_logPos += got;
        s_partial.append(buf, got);
        size_t start = 0, nl;
        while ((nl = s_partial.find('\n', start)) != std::string::npos) {
            std::string_view line(s_partial.data() + start, nl - start);
            if (line.find("CLIENT DISCONNECTED") != std::string_view::npos) s_clients = std::max(0, s_clients - 1);
            else if (line.find("CLIENT CONNECTED") != std::string_view::npos) s_clients++;
            start = nl + 1;
        }
        s_partial.erase(0, start);
    }
    CloseHandle(f);
    return s_clients > 0;
}

bool CommandLineHas(DWORD pid, const wchar_t* arg) {
    using NtQIP = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static auto query = (NtQIP)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!query) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    std::vector<BYTE> b(4096);
    ULONG len = 0;
    LONG st = query(h, 60 /*ProcessCommandLineInformation*/, b.data(), (ULONG)b.size(), &len);
    if (st == (LONG)0xC0000004 /*STATUS_INFO_LENGTH_MISMATCH*/ && len > b.size()) {
        b.resize(len);
        st = query(h, 60, b.data(), (ULONG)b.size(), &len);
    }
    CloseHandle(h);
    if (st < 0) return false;
    auto us = (UNICODE_STRING*)b.data();
    return std::wstring_view(us->Buffer, us->Length / 2).find(arg) != std::wstring_view::npos;
}

struct Seen { bool sunshine = false, rustdeskPeer = false, axiomSession = false; };

Seen ScanProcesses() {
    Seen r;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return r;
    DWORD mine = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mine);
    // Session of every process. ProcessIdToSessionId needs to open the process, which a normal
    // app can't do for the SYSTEM ones (Axiom's stream worker); Windows' session list has them all.
    std::unordered_map<DWORD, DWORD> session;
    WTS_PROCESS_INFOW* wp = nullptr;
    DWORD wn = 0;
    if (WTSEnumerateProcessesW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &wp, &wn)) {
        for (DWORD i = 0; i < wn; i++) session[wp[i].ProcessId] = wp[i].SessionId;
        WTSFreeMemory(wp);
    }
    auto inMine = [&](DWORD pid) { auto it = session.find(pid); return it != session.end() && it->second == mine; };
    // Axiom: the desktop app (remotedesk-desktop.exe) keeps a host-agent.exe of its own in this
    // session all the time; the stream's worker is the one whose parent is the service, itself a
    // host-agent.exe.
    std::vector<DWORD> axiomAll, axiomParents;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (!_wcsicmp(pe.szExeFile, L"sunshine.exe")) r.sunshine = true;
        else if (!_wcsicmp(pe.szExeFile, L"host-agent.exe")) {
            axiomAll.push_back(pe.th32ProcessID);
            if (inMine(pe.th32ProcessID)) axiomParents.push_back(pe.th32ParentProcessID);
        } else if (!_wcsicmp(pe.szExeFile, L"rustdesk.exe") && !r.rustdeskPeer && inMine(pe.th32ProcessID) &&
                   CommandLineHas(pe.th32ProcessID, L"--cm"))
            r.rustdeskPeer = true;
    }
    CloseHandle(snap);
    for (DWORD p : axiomParents)
        if (std::find(axiomAll.begin(), axiomAll.end(), p) != axiomAll.end()) r.axiomSession = true;
    return r;
}
}  // namespace

void Tick() {
    ULONGLONG now = GetTickCount64();
    if (s_lastPoll && now - s_lastPoll < 2000) return;
    s_lastPoll = now;
    Seen seen = ScanProcesses();
    bool moonlight = SunshineClients() && seen.sunshine;   // a Sunshine that died mid-stream logs no disconnect
    bool rdp = GetSystemMetrics(SM_REMOTESESSION) != 0;
    static HANDLE force = CreateEventW(nullptr, TRUE, FALSE, L"LiquidShell.RemoteView");
    bool forced = force && WaitForSingleObject(force, 0) == WAIT_OBJECT_0;
    const wchar_t* who = seen.axiomSession ? L"Axiom" : moonlight ? L"Moonlight" : seen.rustdeskPeer ? L"RustDesk"
                       : rdp ? L"Remote Desktop" : forced ? L"\"--remote on\"" : nullptr;
    bool on = who != nullptr;
    if (on == g.remoteView) return;
    if (on) Log(L"remote: %s is streaming this screen, Liquid shows in the stream (glass rebuilt from window captures)", who);
    else Log(L"remote: stream ended, Liquid back out of screen capture (live glass)");
    g.SetRemoteView(on);
}
}  // namespace remote
