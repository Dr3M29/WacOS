// Fullscreen Spaces, macOS style: maximising a window gives it its own space and switches to it;
// restoring, minimising or closing it sends it back and removes that space.
//
// Two modes:
//  * "screen" (default): every monitor has its own spaces, like macOS. A screen's spaces are its
//    normal desktop followed by one space per maximised window. Switching hides/shows only the
//    windows on that screen, so the other monitors stay put.
//  * "desktop": uses Windows virtual desktops (all monitors switch together). This needs Explorer's
//    internal COM interfaces, checked at start-up.
#include "shell.h"
#include <objectarray.h>
#include <servprov.h>
#include <shobjidl.h>
#include <map>
#include <set>
#include <algorithm>
#include <cstdio>

namespace {
// ===================== shared =====================
std::map<HWND, bool> s_wasZoomed;   // last seen maximise state of every app window
ULONGLONG s_lastTick = 0, s_cooldownUntil = 0;
bool s_screenMode = true, s_ready = false;

struct Collect { std::vector<HWND> wins; };
BOOL CALLBACK CollectWins(HWND h, LPARAM lp) {
    if (IsAppWindow(h) && IsWindowVisible(h)) ((Collect*)lp)->wins.push_back(h);
    return TRUE;
}

// ===================== per-screen spaces =====================
struct Screen {
    HMONITOR mon = nullptr;
    std::vector<HWND> full;          // fullscreen spaces, left to right
    int current = 0;                 // 0 = desktop, k = full[k-1]
    std::vector<HWND> parked;        // desktop windows hidden while a fullscreen space is showing
};
std::vector<Screen> s_screens;
std::set<HWND> s_hidden;             // every window we've hidden (so the dock and restore know)

std::wstring HiddenFile() { return ExeDir() + L"\\hidden-windows.txt"; }

void SaveHidden() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, HiddenFile().c_str(), L"w") || !f) return;
    for (HWND h : s_hidden) fwprintf(f, L"%llu\n", (unsigned long long)(ULONG_PTR)h);
    fclose(f);
}

void RestoreFromFile() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, HiddenFile().c_str(), L"r") || !f) return;
    unsigned long long v;
    int n = 0;
    while (fwscanf_s(f, L"%llu", &v) == 1) {
        HWND h = (HWND)(ULONG_PTR)v;
        if (IsWindow(h) && !IsWindowVisible(h)) { ShowWindowAsync(h, SW_SHOWNOACTIVATE); n++; }
    }
    fclose(f);
    DeleteFileW(HiddenFile().c_str());
    if (n) Log(L"spaces: brought back %d windows left hidden by a previous run", n);
}

void Hide(HWND h) { if (IsWindowVisible(h)) { ShowWindowAsync(h, SW_HIDE); s_hidden.insert(h); } }
void Show(HWND h, bool activate) {
    s_hidden.erase(h);
    if (!IsWindow(h)) return;
    ShowWindow(h, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    // a window that was minimised while hidden comes back minimised (invisible): restore it
    if (activate && IsIconic(h)) ShowWindow(h, SW_RESTORE);
    if (activate) SetForegroundWindow(h);
}

Screen& ScreenFor(HMONITOR m) {
    for (auto& sc : s_screens) if (sc.mon == m) return sc;
    s_screens.push_back(Screen{m});
    return s_screens.back();
}

// hide whatever the screen is currently showing
void HideCurrent(Screen& sc) {
    if (sc.current == 0) {
        Collect c;
        EnumWindows(CollectWins, (LPARAM)&c);
        for (HWND w : c.wins)
            if (MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST) == sc.mon && !IsIconic(w) &&
                std::find(sc.full.begin(), sc.full.end(), w) == sc.full.end()) {
                Hide(w);
                sc.parked.push_back(w);
            }
    } else if (sc.current - 1 < (int)sc.full.size()) {
        Hide(sc.full[sc.current - 1]);
    }
}

void ShowIndex(Screen& sc, int idx) {
    sc.current = idx;
    if (idx == 0) {
        // bring the desktop's windows back in their old stacking order
        for (auto it = sc.parked.rbegin(); it != sc.parked.rend(); ++it) Show(*it, false);
        if (!sc.parked.empty()) SetForegroundWindow(sc.parked.front());
        sc.parked.clear();
    } else {
        Show(sc.full[idx - 1], true);
    }
    SaveHidden();
}

void ScreenEnter(HWND w) {
    Screen& sc = ScreenFor(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST));
    // the newly maximised window is visible right now; hide everything else on this screen
    if (sc.current == 0) {
        Collect c;
        EnumWindows(CollectWins, (LPARAM)&c);
        for (HWND o : c.wins)
            if (o != w && MonitorFromWindow(o, MONITOR_DEFAULTTONEAREST) == sc.mon && !IsIconic(o) &&
                std::find(sc.full.begin(), sc.full.end(), o) == sc.full.end()) {
                Hide(o);
                sc.parked.push_back(o);
            }
    } else if (sc.current - 1 < (int)sc.full.size()) {
        Hide(sc.full[sc.current - 1]);
    }
    sc.full.push_back(w);
    sc.current = (int)sc.full.size();
    SaveHidden();
    Log(L"spaces: window %p got its own space (screen has %d)", w, (int)sc.full.size());
}

bool ScreenOwns(HWND w, Screen** out, int* index) {
    for (auto& sc : s_screens)
        for (size_t i = 0; i < sc.full.size(); i++)
            if (sc.full[i] == w) { *out = &sc; *index = (int)i + 1; return true; }
    return false;
}

void ScreenLeave(HWND w) {
    Screen* sc = nullptr;
    int idx = 0;
    if (!ScreenOwns(w, &sc, &idx)) return;
    bool wasCurrent = sc->current == idx;
    sc->full.erase(sc->full.begin() + (idx - 1));
    if (IsWindow(w) && s_hidden.count(w)) Show(w, false);   // it lives on the desktop again
    if (wasCurrent) ShowIndex(*sc, 0);
    else if (sc->current > idx) sc->current--;
    SaveHidden();
    Log(L"spaces: window %p left its space", w);
}

void ScreenTick() {
    // fullscreen windows that closed, minimised or un-maximised leave their space
    std::vector<HWND> leaving;
    for (auto& sc : s_screens)
        for (HWND w : sc.full) {
            bool hiddenByUs = s_hidden.count(w) > 0;
            if (!IsWindow(w) || (!hiddenByUs && (!IsWindowVisible(w) || IsIconic(w) || !IsZoomed(w)))) leaving.push_back(w);
        }
    for (HWND w : leaving) { ScreenLeave(w); s_cooldownUntil = GetTickCount64() + 400; }
    // safety net: a window we hid that no space accounts for any more (switched away, minimised,
    // screens changed...) must never stay invisible
    std::vector<HWND> orphans;
    for (HWND h : s_hidden) {
        bool owned = false;
        for (auto& sc : s_screens) {
            if (std::find(sc.full.begin(), sc.full.end(), h) != sc.full.end()) owned = true;
            if (sc.current != 0 && std::find(sc.parked.begin(), sc.parked.end(), h) != sc.parked.end()) owned = true;
        }
        if (!owned) orphans.push_back(h);
    }
    for (HWND h : orphans) {
        s_hidden.erase(h);
        if (IsWindow(h) && !IsWindowVisible(h)) { ShowWindowAsync(h, SW_SHOWNOACTIVATE); Log(L"spaces: brought back stray hidden window %p", h); }
    }
    if (!orphans.empty()) SaveHidden();
    // parked windows that the user closed meanwhile
    for (auto& sc : s_screens)
        sc.parked.erase(std::remove_if(sc.parked.begin(), sc.parked.end(), [](HWND h) {
            if (!IsWindow(h)) { s_hidden.erase(h); return true; }
            return false;
        }), sc.parked.end());
}

// ===================== Windows virtual desktops =====================
const CLSID CLSID_ImmersiveShell = {0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};
const GUID SID_VDManagerInternal = {0xC5E0CDCA, 0x7B6E, 0x41B2, {0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B}};
const IID IID_VDManagerInternal = {0x53F5CA0B, 0x158F, 0x4124, {0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27}};
const IID IID_ViewCollection = {0x1841C6D7, 0x4F9D, 0x42C0, {0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5}};
const IID IID_VirtualDesktop = {0x3F07F4BE, 0xB107, 0x441A, {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}};

struct IVirtualDesktop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsViewVisible(IUnknown* view, BOOL* visible) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetId(GUID* id) = 0;
};
struct IVDManagerInternal : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(int* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveViewToDesktop(IUnknown* view, IVirtualDesktop* desktop) = 0;
    virtual HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(IUnknown* view, BOOL* can) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentDesktop(IVirtualDesktop** desktop) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesktops(IObjectArray** desktops) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAdjacentDesktop(IVirtualDesktop* from, int direction, IVirtualDesktop** desktop) = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchDesktop(IVirtualDesktop* desktop) = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchDesktopAndMoveForegroundView(IVirtualDesktop* desktop) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDesktop(IVirtualDesktop** desktop) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveDesktop(IVirtualDesktop* desktop, int index) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveDesktop(IVirtualDesktop* destroy, IVirtualDesktop* fallback) = 0;
};
struct IViewCollection : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetViews(IObjectArray** views) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IObjectArray** views) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(LPCWSTR id, IObjectArray** views) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND hwnd, IUnknown** view) = 0;
};

ComPtr<IVDManagerInternal> s_mgr;
ComPtr<IViewCollection> s_views;
struct Managed { ComPtr<IVirtualDesktop> space, origin; };
std::map<HWND, Managed> s_managed;

int RegistryDesktopCount() {
    BYTE buf[16 * 64];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops",
                     L"VirtualDesktopIDs", RRF_RT_REG_BINARY, nullptr, buf, &sz) != ERROR_SUCCESS) return -1;
    return (int)(sz / 16);
}

bool VDConnect() {
    ComPtr<IServiceProvider> sp;
    if (FAILED(CoCreateInstance(CLSID_ImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sp)))) return false;
    if (FAILED(sp->QueryService(SID_VDManagerInternal, IID_VDManagerInternal, (void**)s_mgr.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(sp->QueryService(IID_ViewCollection, IID_ViewCollection, (void**)s_views.ReleaseAndGetAddressOf()))) return false;
    int count = 0;
    if (FAILED(s_mgr->GetCount(&count)) || count < 1 || count > 64) return false;
    int reg = RegistryDesktopCount();
    if (reg > 0 && reg != count) { Log(L"spaces: count mismatch %d vs %d", count, reg); return false; }
    ComPtr<IVirtualDesktop> cur;
    if (FAILED(s_mgr->GetCurrentDesktop(&cur)) || !cur) return false;
    ComPtr<IUnknown> check;
    if (FAILED(cur->QueryInterface(IID_VirtualDesktop, (void**)check.GetAddressOf()))) return false;
    GUID id{};
    return SUCCEEDED(cur->GetId(&id)) && id != GUID{};
}

bool SameDesktop(IVirtualDesktop* a, IVirtualDesktop* b) {
    GUID ia{}, ib{};
    return a && b && SUCCEEDED(a->GetId(&ia)) && SUCCEEDED(b->GetId(&ib)) && ia == ib;
}

void VDEnter(HWND w) {
    ComPtr<IUnknown> view;
    if (FAILED(s_views->GetViewForHwnd(w, &view)) || !view) return;
    BOOL can = FALSE;
    if (FAILED(s_mgr->CanViewMoveDesktops(view.Get(), &can)) || !can) return;
    Managed m;
    if (FAILED(s_mgr->GetCurrentDesktop(&m.origin)) || FAILED(s_mgr->CreateDesktop(&m.space))) return;
    if (FAILED(s_mgr->MoveViewToDesktop(view.Get(), m.space.Get()))) { s_mgr->RemoveDesktop(m.space.Get(), m.origin.Get()); return; }
    s_mgr->SwitchDesktop(m.space.Get());
    SetForegroundWindow(w);
    s_managed[w] = m;
}

void VDLeave(HWND w, bool alive) {
    auto it = s_managed.find(w);
    if (it == s_managed.end()) return;
    Managed m = it->second;
    s_managed.erase(it);
    if (alive) {
        ComPtr<IUnknown> view;
        if (SUCCEEDED(s_views->GetViewForHwnd(w, &view)) && view) s_mgr->MoveViewToDesktop(view.Get(), m.origin.Get());
    }
    ComPtr<IVirtualDesktop> cur;
    if (SUCCEEDED(s_mgr->GetCurrentDesktop(&cur)) && SameDesktop(cur.Get(), m.space.Get())) s_mgr->SwitchDesktop(m.origin.Get());
    s_mgr->RemoveDesktop(m.space.Get(), m.origin.Get());
    if (alive && !IsIconic(w)) SetForegroundWindow(w);
}

void VDTick() {
    for (auto it = s_managed.begin(); it != s_managed.end();) {
        HWND w = it->first;
        ++it;
        bool alive = IsWindow(w) && IsWindowVisible(w);
        if (!alive || IsIconic(w) || !IsZoomed(w)) { VDLeave(w, alive); s_wasZoomed[w] = false; s_cooldownUntil = GetTickCount64() + 500; }
    }
}
}  // namespace

namespace spaces {
bool Init() {
    if (!cfg.fullscreenSpaces) return false;
    s_screenMode = cfg.spacesMode != L"desktop";
    RestoreFromFile();
    if (s_screenMode) s_ready = true;
    else s_ready = VDConnect();
    Log(s_ready ? (s_screenMode ? L"spaces: per-screen mode" : L"spaces: virtual desktop mode")
                : L"spaces: virtual desktop interface not available on this Windows build, feature off");
    if (!s_ready) return false;
    Collect c;
    EnumWindows(CollectWins, (LPARAM)&c);
    for (HWND w : c.wins) s_wasZoomed[w] = IsZoomed(w) != FALSE;   // already-maximised windows stay put
    return true;
}

bool PerScreen() { return s_ready && s_screenMode; }
bool IsHidden(HWND h) { return s_hidden.count(h) > 0; }

void Tick() {
    if (!s_ready) return;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastTick < 200) return;
    s_lastTick = now;
    if (s_screenMode) ScreenTick(); else VDTick();
    if (now < s_cooldownUntil) return;

    Collect c;
    EnumWindows(CollectWins, (LPARAM)&c);
    for (HWND w : c.wins) {
        bool z = IsZoomed(w) != FALSE && !IsIconic(w);
        auto prev = s_wasZoomed.find(w);
        (void)prev;
        // any maximised window you are using gets its own space (also ones maximised before Liquid started)
        if (z && GetForegroundWindow() == w) {
            Screen* sc;
            int idx;
            bool entered = false;
            if (s_screenMode && !ScreenOwns(w, &sc, &idx)) { ScreenEnter(w); entered = true; }
            else if (!s_screenMode && !s_managed.count(w)) { VDEnter(w); entered = true; }
            if (entered) s_cooldownUntil = now + 500;
        }
        s_wasZoomed[w] = z;
    }
    for (auto it = s_wasZoomed.begin(); it != s_wasZoomed.end();)
        it = IsWindow(it->first) ? std::next(it) : s_wasZoomed.erase(it);
}

// swipe on a screen: dir +1 = next space to the right, -1 = left
bool Switch(HMONITOR m, int dir) {
    if (!PerScreen()) return false;
    Screen& sc = ScreenFor(m);
    int target = std::clamp(sc.current + dir, 0, (int)sc.full.size());
    if (target == sc.current) return true;
    HideCurrent(sc);
    ShowIndex(sc, target);
    return true;
}

// a window hidden in another space was asked for (dock click): go to its space
void Reveal(HWND w) {
    for (auto& sc : s_screens) {
        for (size_t i = 0; i < sc.full.size(); i++)
            if (sc.full[i] == w) { HideCurrent(sc); ShowIndex(sc, (int)i + 1); return; }
        if (std::find(sc.parked.begin(), sc.parked.end(), w) != sc.parked.end()) {
            HideCurrent(sc);
            ShowIndex(sc, 0);
            ActivateWindow(w);
            return;
        }
    }
    Show(w, true);
}

void Shutdown() {
    if (!s_ready) return;
    if (s_screenMode) {
        for (HWND h : std::vector<HWND>(s_hidden.begin(), s_hidden.end())) Show(h, false);
        s_hidden.clear();
        DeleteFileW(HiddenFile().c_str());
    } else {
        std::vector<HWND> ws;
        for (auto& [w, m] : s_managed) ws.push_back(w);
        for (HWND w : ws) VDLeave(w, IsWindow(w) != FALSE);
    }
}
}  // namespace spaces
