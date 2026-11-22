#include "shell.h"
#include <shellapi.h>
#include <shellscalingapi.h>
#include <dbghelp.h>
#include <shlwapi.h>
#include <share.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shlwapi.lib")

bool g_quit = false;
bool g_restart = false;
bool g_reloading = false;   // quitting only to come straight back (rebuild / reload): no lock, no taskbar flash
static bool s_reloaded = false;

// ---------- Windows taskbars: hidden on every screen while Liquid runs, restored on exit ----------
static UINT g_trayState = 0;
static bool g_trayTouched = false;

static BOOL CALLBACK HideSecondary(HWND h, LPARAM show) {
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Shell_SecondaryTrayWnd")) {
        if (show) ShowWindow(h, SW_SHOW);
        else if (IsWindowVisible(h)) ShowWindow(h, SW_HIDE);
    }
    return TRUE;
}

// the taskbar's own setting before Liquid touched it, kept in a file so a reload or a crash
// doesn't lose it (restored when Liquid really quits)
static std::wstring TrayFile() { return ExeDir() + L"\\tray-state.txt"; }
static void HideTaskbars() {
    HWND t = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!t) return;
    APPBARDATA a{sizeof(a)};
    a.hWnd = t;
    UINT cur = (UINT)SHAppBarMessage(ABM_GETSTATE, &a);
    if (!g_trayTouched) {
        FILE* f = nullptr;
        if (!_wfopen_s(&f, TrayFile().c_str(), L"r") && f) { unsigned v = 0; if (fwscanf_s(f, L"%u", &v) == 1) g_trayState = v; fclose(f); }
        else {
            g_trayState = cur;
            if (!_wfopen_s(&f, TrayFile().c_str(), L"w") && f) { fwprintf(f, L"%u", cur); fclose(f); }
        }
        g_trayTouched = true;
    }
    // hidden AND auto-hide, every time: Windows sometimes switches auto-hide back (sign-in, Explorer
    // restart), and a hidden taskbar that isn't auto-hide still reserves a 48 px strip at the bottom
    if (!(cur & ABS_AUTOHIDE)) {
        a.lParam = ABS_AUTOHIDE;
        SHAppBarMessage(ABM_SETSTATE, &a);
    }
    if (IsWindowVisible(t)) ShowWindow(t, SW_HIDE);
    EnumWindows(HideSecondary, 0);
}

static void RestoreTaskbars() {
    if (!g_trayTouched) return;
    HWND t = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!t) return;
    APPBARDATA a{sizeof(a)};
    a.hWnd = t;
    a.lParam = g_trayState;
    SHAppBarMessage(ABM_SETSTATE, &a);
    ShowWindow(t, SW_SHOW);
    EnumWindows(HideSecondary, 1);
    DeleteFileW(TrayFile().c_str());
}

// --lens-test: a big clear lens + pill in the middle of the main screen, to judge refraction over real content
static bool s_lensTest = false;
static std::wstring s_iconStyle;
static Overlay s_lens;
static LRESULT CALLBACK LensProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}
static void TickLens(bool nf) {
    if (!s_lens.hwnd || !(nf || g.snapPending)) return;
    std::vector<GlassShape> sh;
    GlassShape pill{40, 60, 520, 110};
    pill.radius = 55; pill.blur = 0.0f; pill.tint = 0.0f; pill.bezel = 55; pill.thick = 2.4f;
    pill.shadow = 0.0f; pill.highlight = 0.0f;
    GlassShape orb{600, 30, 170, 170};
    orb.radius = 85; orb.blur = 0.0f; orb.tint = 0.0f; orb.bezel = 85; orb.thick = 2.4f;
    orb.shadow = 0.0f; orb.highlight = 0.0f;
    sh.push_back(pill);
    sh.push_back(orb);
    float tint[3] = {1, 1, 1};
    s_lens.Render(1.f, sh, tint, [](ID2D1DeviceContext*) {});
}

// Liquid Keys (passkeys, keys\README.md): its local service lets the browser extension reach the
// vault. One copy runs (it holds Local\LiquidKeysServer); Liquid starts it if it isn't up.
static void StartKeysService() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\LiquidKeysServer");
    if (m) { CloseHandle(m); return; }
    std::wstring exe = ExeDir() + L"\\keys\\host\\LiquidKeys.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    Launch(exe, L"--serve", SW_HIDE);
    Log(L"keys: started the passkey service");
}

static ULONGLONG s_lastTray = 0, s_start = 0;
static double s_snapAfter = -1, s_exitAfter = -1;
static bool s_autoSnapDone = false;
static void SaveSnapIfPending();

// One frame of the whole shell. Also runs inside nested loops (glass menus), so test
// timers and snapshot saving live here: layers grabbed last frame are saved this frame.
void TickShell() {
    if (g.snapPending && !g.snapLayers.empty()) SaveSnapIfPending();
    double t = (GetTickCount64() - s_start) / 1000.0;
    if (!s_autoSnapDone && s_snapAfter >= 0 && t >= s_snapAfter) { g.snapPending = true; s_autoSnapDone = true; }
    if (s_exitAfter >= 0 && t >= s_exitAfter) g_quit = true;
    remote::Tick();   // before PollAll: a stream starting switches the glass to the wallpaper
    bool nf = g.PollAll();
    for (auto& mb : menubars) mb->Tick(nf);
    dock.Tick(nf);
    cc.Tick(nf);
    TickLens(nf);
    osd::Tick(nf);
    nc::Tick(nf);
    spotlight::Tick(nf);
    lock::Tick(nf);
    chrome::Tick(nf);
    island::Tick(nf);
    prefs::Tick(nf);
    about::Tick(nf);
    switcher::Tick(nf);
    anim::Tick(nf);
    widgets::Tick(nf);
    deskicons::Tick(nf);
    sys::AudioTick();
    spaces::Tick();
    airring::Tick();
    ULONGLONG now = GetTickCount64();
    if (now - s_lastTray > 1000) { HideTaskbars(); s_lastTray = now; }
}

static std::wstring s_snapPath;

static void SaveSnapIfPending() {
    if (!g.snapPending) return;
    std::wstring path = s_snapPath;
    if (path.empty()) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t b[64];
        swprintf_s(b, L"\\snapshots\\liquid-%04d%02d%02d-%02d%02d%02d.png",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        path = ExeDir() + b;
    }
    g.SaveSnapshot(path);
    g.snapPending = false;
}

struct MonInfo { RECT rc; HMONITOR hm; bool primary; };
static BOOL CALLBACK CollectMon(HMONITOR hm, HDC, LPRECT rc, LPARAM lp) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    ((std::vector<MonInfo>*)lp)->push_back({*rc, hm, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

// Watchdog: a tiny second copy of Liquid that waits for the real one. A normal quit, restart
// or reload exits with 0 and is left alone; anything else (a crash, being killed) brings Liquid
// back, locked. Gives up after 5 restarts in a row so a crash loop can't spin forever.
static int s_crashRestarts = 0;
static void WatchdogNote(const wchar_t* fmt, DWORD a, int b) {
    FILE* f = _wfsopen((ExeDir() + L"\\watchdog.log").c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(f, L"[%04d-%02d-%02d %02d:%02d:%02d] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    fwprintf(f, fmt, a, b);
    fputwc(L'\n', f);
    fclose(f);
}
static int Watchdog(DWORD pid, int restarts) {
    HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return 0;
    WaitForSingleObject(p, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(p, &code);
    CloseHandle(p);
    WatchdogNote(L"Liquid exited with code 0x%08lX (restarts so far %d)", code, restarts);
    if (code == 0 || restarts >= 5) return 0;
    Sleep(1500);
    if (GetSystemMetrics(SM_SHUTTINGDOWN)) return 0;   // Windows is signing out / shutting down
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"LiquidShell.Single");
    if (m) { CloseHandle(m); return 0; }   // something already started a new copy
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t args[64];
    swprintf_s(args, L"--crash-restarts %d", restarts + 1);
    ShellExecuteW(nullptr, L"open", self, args, nullptr, SW_SHOWNORMAL);
    return 0;
}

// Called every frame until the watchdog is running. It is started through Explorer so it lives
// outside Task Scheduler's / a terminal's process group; at early boot Explorer may not be up
// yet, so this keeps trying for a few minutes before settling for a plain child process.
static bool s_watchdogUp = false;
static ULONGLONG s_watchdogNext = 0, s_watchdogGiveUp = 0;
static void StartWatchdog();
static void WatchdogTick() {
    if (s_watchdogUp) return;
    ULONGLONG now = GetTickCount64();
    if (!s_watchdogGiveUp) s_watchdogGiveUp = now + 5 * 60 * 1000;
    if (now < s_watchdogNext) return;
    s_watchdogNext = now + 3000;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t args[64];
    swprintf_s(args, L"--watchdog %lu %d", GetCurrentProcessId(), s_crashRestarts);
    if (LaunchViaExplorer(self, args)) { s_watchdogUp = true; Log(L"watchdog: started"); return; }
    if (now > s_watchdogGiveUp) { StartWatchdog(); s_watchdogUp = true; }
}

static void StartWatchdog() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t cmd[MAX_PATH + 64];
    swprintf_s(cmd, L"\"%s\" --watchdog %lu %d", self, GetCurrentProcessId(), s_crashRestarts);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    // break away from any job so whatever started us (a terminal in another app) can't take it down too
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) &&
        !CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"watchdog: could not start (%lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

// a crash writes where it happened to liquid.log and a small dump to crash.dmp, then exits
static LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &mod))
        GetModuleFileNameW(mod, name, MAX_PATH);
    Log(L"CRASH: exception 0x%08lX at %s+0x%llX", ep->ExceptionRecord->ExceptionCode, PathFindFileNameW(name),
        (unsigned long long)((BYTE*)addr - (BYTE*)mod));
    HANDLE f = CreateFileW((ExeDir() + L"\\crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpWithThreadInfo, &mei, nullptr, nullptr);
        CloseHandle(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);   // a crash exits at once so the watchdog can act
    SetUnhandledExceptionFilter(OnCrash);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    double snapAfter = -1, exitAfter = -1;
    std::vector<std::wstring> scripted;   // --open cc|wifi|sound|menu|dockmenu for testing
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--quit") {
            HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"LiquidShell.Quit");
            if (e) { SetEvent(e); CloseHandle(e); }
            return 0;
        }
        if (a == L"--quit-for-reload") {   // used by tools/reload.cmd: the running copy steps aside quietly
            HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"LiquidShell.Reload");
            if (e) { SetEvent(e); CloseHandle(e); }
            for (int k = 0; k < 300; k++) {   // wait up to 15 s for it to exit
                HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"LiquidShell.Single");
                if (!m) break;
                CloseHandle(m);
                Sleep(50);
            }
            return 0;
        }
        if (a == L"--remote" && i + 1 < argc) {   // on|off: treat the screen as streamed (any other streaming tool)
            HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"LiquidShell.RemoteView");
            if (e) { if (wcscmp(argv[i + 1], L"on") == 0) SetEvent(e); else ResetEvent(e); CloseHandle(e); }
            return 0;
        }
        // --about / --settings [pane]: open those windows in the running Liquid (for testing)
        if (a == L"--about" || a == L"--settings") {
            if (a == L"--settings" && i + 1 < argc) {   // the pane's name goes through memory Liquid shares for it
                HANDLE m = OpenFileMappingW(FILE_MAP_WRITE, FALSE, L"Local\\LiquidOpenPane");
                if (m) {
                    if (auto* v = (wchar_t*)MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, 128)) {
                        wcsncpy_s(v, 64, argv[i + 1], _TRUNCATE);
                        UnmapViewOfFile(v);
                    }
                    CloseHandle(m);
                }
            }
            HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, a == L"--about" ? L"Local\\LiquidOpenAbout" : L"Local\\LiquidOpenSettings");
            if (e) { SetEvent(e); CloseHandle(e); }
            return 0;
        }
        if (a == L"--dump-backdrop") {   // during a stream: save what the glass sees (snapshots\backdrop-N.png)
            HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"LiquidShell.DumpBackdrop");
            if (e) { SetEvent(e); CloseHandle(e); }
            return 0;
        }
        if (a == L"--watchdog" && i + 2 < argc) return Watchdog((DWORD)_wtoi(argv[i + 1]), _wtoi(argv[i + 2]));
        if (a == L"--crash-restarts" && i + 1 < argc) s_crashRestarts = _wtoi(argv[++i]);
        if (a == L"--reloaded") s_reloaded = true;
        if (a == L"--snap" && i + 1 < argc) snapAfter = _wtof(argv[++i]);
        if (a == L"--snap-path" && i + 1 < argc) s_snapPath = argv[++i];
        if (a == L"--exit-after" && i + 1 < argc) exitAfter = _wtof(argv[++i]);
        if (a == L"--open" && i + 1 < argc) scripted.push_back(argv[++i]);
        if (a == L"--lens-test") s_lensTest = true;
        if (a == L"--icon-style" && i + 1 < argc) s_iconStyle = argv[++i];
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"LiquidShell.Single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    // Task Scheduler starts tasks below normal priority (and the watchdog's restarts inherit it), so
    // any busy program (a game, an AI upscale) got the CPU first and the bars stuttered. The shell is
    // light (well under one core), so it runs ahead of background work instead.
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    if (!scripted.empty() || snapAfter >= 0 || exitAfter >= 0) s_watchdogUp = true;   // test runs: no watchdog
    if (s_crashRestarts) Log(L"came back after an unexpected exit (%d in a row)", s_crashRestarts);
    HANDLE quitEv = CreateEventW(nullptr, TRUE, FALSE, L"LiquidShell.Quit");
    HANDLE reloadEv = CreateEventW(nullptr, TRUE, FALSE, L"LiquidShell.Reload");
    ResetEvent(quitEv);     // a signal meant for the copy we replaced must not close this one
    ResetEvent(reloadEv);

    LoadConfig();
    if (!s_iconStyle.empty()) cfg.iconStyle = s_iconStyle;
    if (!g.Init()) {
        MessageBoxW(nullptr, L"Liquid couldn't start its graphics engine. See liquid.log.", L"Liquid", MB_ICONERROR);
        return 1;
    }
    sys::Init();
    // boot: cover every screen with the lock screen before anything else is built
    lock::Init();
    std::wstring relockFlag = ExeDir() + L"\\relock.flag";
    bool relock = DeleteFileW(relockFlag.c_str()) != FALSE;   // the previous copy was locked when it stepped aside
    bool startLocked = cfg.lockScreen && scripted.empty() && snapAfter < 0 && exitAfter < 0 &&
                       ((cfg.lockOnStart && !s_reloaded) || relock);
    if (startLocked) { lock::Lock(false, true); lock::Tick(true); }

    std::vector<MonInfo> mons;
    EnumDisplayMonitors(nullptr, nullptr, CollectMon, (LPARAM)&mons);
    HideTaskbars();
    bool ok = true;
    RECT primaryRc{};
    float primaryScale = 1;
    for (auto& m : mons) {
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(m.hm, MDT_EFFECTIVE_DPI, &dx, &dy);
        float sc = dx / 96.f;
        auto mb = std::make_unique<MenuBar>();
        ok &= mb->Init(m.rc, sc, m.primary);
        menubars.push_back(std::move(mb));
        if (m.primary) { primaryRc = m.rc; primaryScale = sc; }
        Log(L"monitor %dx%d at (%d,%d) scale %.2f%s", m.rc.right - m.rc.left, m.rc.bottom - m.rc.top,
            m.rc.left, m.rc.top, sc, m.primary ? L" primary" : L"");
    }
    ok = ok && dock.Init(primaryRc, primaryScale) && cc.Init(primaryScale);
    if (ok) { bool a1 = nc::Init(), a2 = spotlight::Init(); chrome::Init(); island::Init(); prefs::Init(); about::Init(); StartKeysService(); anim::Init(); layout::Load(); widgets::Init(); deskicons::Init(); Log(L"notification center %d, spotlight %d", (int)a1, (int)a2); }
    if (!ok) {
        RestoreTaskbars();
        MessageBoxW(nullptr, L"Liquid couldn't create its windows. See liquid.log.", L"Liquid", MB_ICONERROR);
        return 1;
    }
    HWND hotkeyWnd = menubars[0]->ov.hwnd;
    for (auto& mb : menubars) if (mb->primary) hotkeyWnd = mb->ov.hwnd;
    RegisterHotKey(hotkeyWnd, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'Q');
    RegisterHotKey(hotkeyWnd, 2, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'S');
    RegisterHotKey(hotkeyWnd, 3, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'R');   // reload Liquid
    gestures::Init();
    spaces::Init();
    if (s_lensTest) {
        int lw = 820, lh = 240;
        s_lens.Create(L"LiquidLensTest", LensProc, primaryRc.left + (primaryRc.right - primaryRc.left - lw) / 2,
                      primaryRc.top + 300, lw, lh, nullptr);
        s_lens.SetClickThrough(true);
    }

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER due;
    due.QuadPart = -10000LL * 16;
    SetWaitableTimer(timer, &due, 16, nullptr, nullptr, FALSE);

    CreateDirectoryW((ExeDir() + L"\\snapshots").c_str(), nullptr);
    ULONGLONG start = GetTickCount64();
    s_start = start;
    s_snapAfter = snapAfter;
    s_exitAfter = exitAfter;
    bool scriptedDone = false;
    HANDLE waits[3] = {timer, quitEv, reloadEv};
    Log(L"running");

    while (!g_quit) {
        DWORD r = MsgWaitForMultipleObjects(3, waits, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0 + 1) break;
        if (r == WAIT_OBJECT_0 + 2) { g_reloading = true; break; }
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) g_quit = true;
            if (m.message == WM_LIQUID_GESTURE && !m.hwnd) { gestures::Handle(m.wParam, m.lParam); continue; }
            if (m.message == WM_LIQUID_SWITCH && !m.hwnd) { switcher::Command((int)m.wParam); continue; }
            if (m.message == WM_LIQUID_BORDERLESS && !m.hwnd) { chrome::ToggleBorderlessWindow(GetForegroundWindow()); continue; }
            if (m.message == WM_LIQUID_VOLKEY && !m.hwnd) { osd::VolumeKey((int)m.wParam); continue; }
            if (m.message == WM_LIQUID_LOCKKEY && !m.hwnd) { if (m.wParam == 0) lock::Lock(); else lock::Key((int)m.wParam, (wchar_t)m.lParam); continue; }
            if (m.message == WM_LIQUID_SPOTKEY && !m.hwnd) { if (m.wParam == 0) spotlight::Toggle(); else spotlight::Key((int)m.wParam, (wchar_t)m.lParam); continue; }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (r != WAIT_OBJECT_0 || g_quit) continue;

        double t = (GetTickCount64() - start) / 1000.0;
        if (!scriptedDone && t > 1.0) {
            scriptedDone = true;
            for (auto& sc : scripted) {
                if (sc == L"cc") cc.Toggle(primaryRc, 0);
                if (sc == L"wifi") cc.Toggle(primaryRc, 1);
                if (sc == L"sound") cc.Toggle(primaryRc, 2);
                if (sc == L"menu") for (auto& mb : menubars) if (mb->primary) mb->Click(1);
                if (sc == L"dockmenu") dock.BackgroundMenu();
                if (sc == L"volume") osd::ShowVolume();
                if (sc == L"nc") nc::Toggle(primaryRc);
                if (sc == L"lock") lock::Lock();
                if (sc == L"lockset") lock::Lock(true);
                if (sc == L"spotlight") spotlight::Toggle();
                if (sc.rfind(L"spotlight:", 0) == 0) { spotlight::Toggle(); for (wchar_t ch : sc.substr(10)) spotlight::Key(ch == L' ' ? VK_SPACE : 'A', ch); }
                if (sc == L"desktop") DesktopMenu({primaryRc.left + (primaryRc.right - primaryRc.left) * 2 / 3, primaryRc.top + 300});
            }
        }
        TickShell();
        WatchdogTick();
    }
    if (g.snapPending && !g.snapLayers.empty()) SaveSnapIfPending();

    Log(L"shutting down");
    // still locked while restarting (display change, reload): the next copy must come back locked
    if (lock::IsLocked()) {
        HANDLE f = CreateFileW((ExeDir() + L"\\relock.flag").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    gestures::Shutdown();
    spaces::Shutdown();
    cc.Close();
    for (auto& mb : menubars) mb->Shutdown();
    dock.Shutdown();
    osd::Shutdown();
    nc::Shutdown();
    spotlight::Shutdown();
    lock::Shutdown();
    chrome::Shutdown();
    island::Shutdown();
    prefs::Shutdown();
    about::Shutdown();
    switcher::Shutdown();
    anim::Shutdown();
    widgets::Shutdown();
    deskicons::Shutdown();
    cc.ov.Destroy();
    if (!g_reloading && !g_restart) RestoreTaskbars();   // coming straight back: keep it hidden
    sys::Shutdown();
    ReleaseMutex(mtx);
    CloseHandle(mtx);
    if (g_restart) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        Sleep(150);
        ShellExecuteW(nullptr, L"open", self, L"--reloaded", nullptr, SW_SHOWNORMAL);
    }
    return 0;
}
