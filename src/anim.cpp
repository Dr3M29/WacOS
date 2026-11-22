#include "shell.h"
#include <dwmapi.h>
#include <cmath>
#include <algorithm>
#include <set>

// macOS-style window animations, drawn with live DWM thumbnails of the real window:
//  * minimize: the window shrinks down into its dock icon
//  * restore / an app opening: it grows up out of its dock icon into place
// Windows' own minimize/restore animation is switched off while Liquid runs (not saved; it comes
// back when Liquid quits). For opening/restoring, the real window waits just off-screen during the
// 0.25 s animation and is put back exactly where it was when the animation lands.

namespace {
HWND s_host = nullptr;            // invisible full-screen window the thumbnails are drawn on
HWINEVENTHOOK s_hookMin = nullptr, s_hookShow = nullptr;
std::set<HWND> s_seen;            // windows that already exist (only brand new ones get the open animation)
ANIMATIONINFO s_oldAnim{sizeof(ANIMATIONINFO), 0};
bool s_changedAnim = false;
RECT s_virt{};

struct Run {
    HWND w = nullptr;
    HTHUMBNAIL th = nullptr;
    RECT from{}, to{};
    double start = 0, dur = 0.26;
    bool growing = true;          // true = out of the dock, false = into it
    bool parked = false;          // real window moved off-screen during the animation
    RECT real{};                  // where to put it back
    float a0 = 1, a1 = 1;
};
std::vector<Run> s_runs;

double Now() { return GetTickCount64() / 1000.0; }

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, wp, lp);
}

bool Animatable(HWND h) {
    if (!h || !IsAppWindow(h) || WindowPid(h) == g_selfPid || lock::IsLocked()) return false;
    LONG st = GetWindowLongW(h, GWL_STYLE);
    if ((st & WS_CAPTION) != WS_CAPTION && !(st & WS_THICKFRAME)) return false;   // games / borderless: leave alone
    if (chrome::IsBorderlessApp(h)) return false;
    return true;
}

// the dock icon this window belongs to (or the middle of the dock)
RECT DockRectFor(HWND w) {
    std::wstring path = Lower(ProcessPath(WindowPid(w)));
    for (auto& it : dock.items) {
        bool mine = std::find(it.wins.begin(), it.wins.end(), w) != it.wins.end();
        if (!mine && !it.match.empty() && path.find(Lower(it.match)) != std::wstring::npos) mine = true;
        if (mine && it.size > 1) {
            float s = it.size;
            LONG x = dock.ov.x + (LONG)(it.cx - s / 2), y = dock.ov.y + (LONG)(dock.iconBottom - s);
            return {x, y, x + (LONG)s, y + (LONG)s};
        }
    }
    LONG cx = dock.ov.x + dock.ov.w / 2, b = dock.mon.bottom;
    return {cx - 24, b - 60, cx + 24, b - 12};
}

RECT VisibleRect(HWND w) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(w, &r);
    return r;
}

void Start(HWND w, RECT from, RECT to, bool growing, bool park, RECT real) {
    if (!s_host) return;
    for (auto& r : s_runs) if (r.w == w) return;   // already animating
    Run run;
    run.w = w; run.from = from; run.to = to; run.growing = growing; run.start = Now(); run.real = real;
    if (FAILED(DwmRegisterThumbnail(s_host, w, &run.th))) return;
    if (park) {   // keep the real window out of sight until the animation lands
        SetWindowPos(w, nullptr, s_virt.right + 200, real.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        run.parked = true;
    }
    s_runs.push_back(run);
    SetWindowPos(s_host, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Finish(Run& r) {
    if (r.th) DwmUnregisterThumbnail(r.th);
    r.th = nullptr;
    if (r.parked && IsWindow(r.w))
        SetWindowPos(r.w, nullptr, r.real.left, r.real.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    r.parked = false;
}

void CALLBACK OnEvent(HWINEVENTHOOK, DWORD ev, HWND w, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !w) return;
    if (ev == EVENT_SYSTEM_MINIMIZESTART) {
        if (!Animatable(w)) return;
        RECT from = VisibleRect(w);
        WINDOWPLACEMENT wp{sizeof(wp)};
        if (GetWindowPlacement(w, &wp) && (from.right - from.left < 50)) from = wp.rcNormalPosition;
        Start(w, from, DockRectFor(w), false, false, {});
    } else if (ev == EVENT_SYSTEM_MINIMIZEEND) {
        if (!Animatable(w) || IsZoomed(w)) return;
        RECT real;
        GetWindowRect(w, &real);
        Start(w, DockRectFor(w), VisibleRect(w), true, true, real);
    } else if (ev == EVENT_OBJECT_SHOW) {
        if (GetAncestor(w, GA_ROOT) != w || s_seen.count(w)) return;
        if (spaces::IsHidden(w)) return;
        if (!IsAppWindow(w)) return;
        s_seen.insert(w);
        if (!Animatable(w) || IsZoomed(w) || IsIconic(w)) return;
        RECT real;
        GetWindowRect(w, &real);
        Start(w, DockRectFor(w), VisibleRect(w), true, true, real);
    }
}

float Ease(float t, bool growing) {
    t = std::clamp(t, 0.f, 1.f);
    if (growing) { float u = 1 - t; return 1 - u * u * u; }   // out of the dock: fast start, soft landing
    return t * t * (3 - 2 * t);                                // into the dock: smooth
}
}  // namespace

namespace anim {
bool Init() {
    if (!cfg.windowAnimations) return true;
    GetSystemMetrics(SM_CXVIRTUALSCREEN);
    s_virt = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
              GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
              GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    WNDCLASSW wc{};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LiquidAnimHost";
    RegisterClassW(&wc);
    s_host = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                             wc.lpszClassName, L"", WS_POPUP, s_virt.left, s_virt.top, s_virt.right - s_virt.left,
                             s_virt.bottom - s_virt.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s_host) return false;
    SetLayeredWindowAttributes(s_host, 0, 255, LWA_ALPHA);
    g.ApplyCaptureMode(s_host);
    ShowWindow(s_host, SW_SHOWNOACTIVATE);
    // Windows' own minimize animation off while Liquid runs (runtime only, not saved)
    if (SystemParametersInfoW(SPI_GETANIMATION, sizeof(s_oldAnim), &s_oldAnim, 0) && s_oldAnim.iMinAnimate) {
        ANIMATIONINFO off{sizeof(off), 0};
        if (SystemParametersInfoW(SPI_SETANIMATION, sizeof(off), &off, 0)) s_changedAnim = true;
    }
    EnumWindows([](HWND h, LPARAM) -> BOOL { if (IsAppWindow(h)) s_seen.insert(h); return TRUE; }, 0);
    s_hookMin = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    s_hookShow = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    return true;
}

void Tick(bool) {
    if (s_runs.empty()) return;
    double now = Now();
    for (auto& r : s_runs) {
        float t = (float)((now - r.start) / r.dur), e = Ease(t, r.growing);
        auto lerp = [&](LONG a, LONG b) { return (LONG)std::lround(a + (b - a) * e); };
        RECT d{lerp(r.from.left, r.to.left), lerp(r.from.top, r.to.top), lerp(r.from.right, r.to.right), lerp(r.from.bottom, r.to.bottom)};
        OffsetRect(&d, -s_virt.left, -s_virt.top);
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcDestination = d;
        p.fVisible = TRUE;
        p.fSourceClientAreaOnly = FALSE;
        float op = r.growing ? std::min(1.f, 0.35f + e) : std::min(1.f, 1.25f - e);
        p.opacity = (BYTE)std::clamp((int)(op * 255), 0, 255);
        if (r.th) DwmUpdateThumbnailProperties(r.th, &p);
        if (t >= 1) Finish(r);
    }
    s_runs.erase(std::remove_if(s_runs.begin(), s_runs.end(), [](const Run& r) { return r.th == nullptr; }), s_runs.end());
    if (s_runs.empty()) DwmFlush();
}

void Shutdown() {
    for (auto& r : s_runs) Finish(r);
    s_runs.clear();
    if (s_hookMin) UnhookWinEvent(s_hookMin);
    if (s_hookShow) UnhookWinEvent(s_hookShow);
    s_hookMin = s_hookShow = nullptr;
    if (s_changedAnim) SystemParametersInfoW(SPI_SETANIMATION, sizeof(s_oldAnim), &s_oldAnim, 0);
    s_changedAnim = false;
    if (s_host) DestroyWindow(s_host);
    s_host = nullptr;
}
}  // namespace anim
