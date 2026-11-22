#include "shell.h"
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>

// Mac-style window chrome for other apps:
//  * three frosted dots (minimize / zoom / close, Windows order) where the window's own buttons are,
//    on a patch of the title bar's colour that covers those buttons, so every app gets them
//  * Windows' own caption buttons hidden on classic windows (removing WS_SYSMENU), put back on quit;
//    apps that draw their own title bar (Chrome, Electron, Store apps) keep theirs
//  * frosted acrylic backdrop + dark title bars on apps that let DWM draw their frame

namespace {
Overlay s_ov;
HWND s_target = nullptr;
RECT s_trc{};
float s_s = 1;
int s_hover = -1, s_pressed = -1;
bool s_over = false, s_visible = false, s_dirty = true;
ComPtr<ID2D1SolidColorBrush> s_br;
ComPtr<IDWriteTextFormat> s_glyph;
std::map<HWND, LONG_PTR> s_styles;   // windows whose buttons we hid -> original style
std::set<HWND> s_frosted;
ULONGLONG s_lastScan = 0;
const float kD = 12, kGap = 9, kPadR = 16, kM = 6, kOvW = 190, kOvH = 104;   // dot size, gap, right inset, margin, overlay size
D2D1_RECT_F s_patch{};          // the area covering the app's own caption buttons (overlay coords)
D2D1_COLOR_F s_bar{0, 0, 0, 0};  // title bar colour under it
bool s_lightBar = false;

bool OwnTitleBar(const wchar_t* cls) {
    // these draw their own caption buttons; removing WS_SYSMENU does nothing useful (or breaks them)
    return !wcsncmp(cls, L"Chrome_WidgetWin", 16) || !wcscmp(cls, L"ApplicationFrameWindow") ||
           !wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") || !wcscmp(cls, L"WinUIDesktopWin32WindowClass") ||
           !wcsncmp(cls, L"Mozilla", 7) || !wcscmp(cls, L"Notepad") || !wcsncmp(cls, L"HwndWrapper", 11);
}

bool Eligible(HWND h) {
    if (!h || !IsWindow(h) || !IsAppWindow(h) || IsIconic(h) || WindowPid(h) == g_selfPid) return false;
    RECT r;
    GetWindowRect(h, &r);
    if (r.right - r.left < 240 || r.bottom - r.top < 120) return false;
    // real full screen (games, videos): no chrome
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    LONG st = GetWindowLongW(h, GWL_STYLE);
    if (!(st & WS_CAPTION) && !(st & WS_THICKFRAME) && r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
        r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom) return false;
    return true;
}

void HideButtons(HWND h) {
    if (!cfg.hideCaptionButtons || s_styles.count(h)) return;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (OwnTitleBar(cls)) return;
    LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    if ((st & WS_CAPTION) != WS_CAPTION || !(st & WS_SYSMENU)) return;
    s_styles[h] = st;
    SetWindowLongPtrW(h, GWL_STYLE, st & ~(LONG_PTR)WS_SYSMENU);
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void Frost(HWND h) {
    if (!cfg.frostWindows || s_frosted.count(h)) return;
    s_frosted.insert(h);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int backdrop = 3;   // DWMSBT_TRANSIENTWINDOW = acrylic (the frosted macOS look)
    DwmSetWindowAttribute(h, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop, sizeof(backdrop));
}

std::set<HWND> s_rounded;
void Round(HWND h) {   // every window gets Windows 11's rounded corners, even ones that opted out
    if (!cfg.roundCorners || s_rounded.count(h)) return;
    s_rounded.insert(h);
    int pref = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(h, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &pref, sizeof(pref));
}

// borderless full screen: apps on the list lose their title bar and frame and cover their whole
// screen (like a game's "windowed borderless"); Liquid's bar and dock step aside for them
struct Saved { LONG_PTR style, ex; RECT rc; };
std::map<HWND, Saved> s_borderless;
std::wstring ExeOf(HWND h) {
    std::wstring p = Lower(ProcessPath(WindowPid(h)));
    size_t sl = p.find_last_of(L'\\');
    return sl == std::wstring::npos ? p : p.substr(sl + 1);
}
bool ListedBorderless(HWND h) {
    std::wstring exe = ExeOf(h);
    if (exe.empty()) return false;
    std::wstring list = L"," + Lower(cfg.borderlessApps) + L",";
    return list.find(L"," + exe + L",") != std::wstring::npos;
}
std::set<HWND> s_exempt, s_forced;   // toggled by Ctrl+Win+F: windowed for now / borderless for now
void Unborderless(HWND h);
std::map<HWND, int> s_fights;   // windows that keep putting their frame back
bool Chromium(HWND h) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return !wcsncmp(cls, L"Chrome_WidgetWin", 16);
}
void Borderless(HWND h) {
    if (IsIconic(h) || s_exempt.count(h) || (!ListedBorderless(h) && !s_forced.count(h))) return;
    if (Chromium(h)) return;   // browsers / Electron draw their own frame and fight style changes (flashing)
    MONITORINFO mi{sizeof(mi)};
    HMONITOR hm = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(hm, &mi);
    RECT m = mi.rcMonitor, r;
    // fill the screen below the menu bar, so the bar (app menus, Control Center, clock) stays usable
    UINT dpx = 96, dpy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dpx, &dpy);
    m.top += (LONG)std::lround(cfg.menubarHeight * dpx / 96.f);
    GetWindowRect(h, &r);
    LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    bool framed = (st & (WS_CAPTION | WS_THICKFRAME)) != 0;
    if (!framed && r.left == m.left && r.top == m.top && r.right == m.right && r.bottom == m.bottom) return;
    if (s_borderless.count(h)) {
        // we already made it borderless and it put its frame back: don't fight it forever (flashing)
        if (++s_fights[h] >= 3) {
            Unborderless(h);
            s_exempt.insert(h);
            Log(L"borderless full screen: %s keeps restoring its frame, leaving it alone", ExeOf(h).c_str());
            return;
        }
    } else s_borderless[h] = {st, GetWindowLongPtrW(h, GWL_EXSTYLE), r};
    if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
    SetWindowLongPtrW(h, GWL_STYLE, st & ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME));
    SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) & ~(LONG_PTR)(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
    SetWindowPos(h, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    Log(L"borderless full screen: %s", ExeOf(h).c_str());
}
void Unborderless(HWND h) {
    auto it = s_borderless.find(h);
    if (it == s_borderless.end()) return;
    if (IsWindow(h)) {
        SetWindowLongPtrW(h, GWL_STYLE, it->second.style);
        SetWindowLongPtrW(h, GWL_EXSTYLE, it->second.ex);
        RECT r = it->second.rc;
        SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    s_borderless.erase(it);
}

void Scan() {
    // apps taken off the list go back to normal
    for (auto it = s_borderless.begin(); it != s_borderless.end();) {
        HWND h = it->first;
        ++it;
        if (!IsWindow(h)) s_borderless.erase(h);
        else if (!ListedBorderless(h) && !s_forced.count(h)) Unborderless(h);
    }
    EnumWindows([](HWND h, LPARAM) -> BOOL {
        if (IsAppWindow(h) && WindowPid(h) != g_selfPid) { HideButtons(h); Frost(h); Round(h); Borderless(h); }
        return TRUE;
    }, 0);
    for (auto it = s_styles.begin(); it != s_styles.end();) it = IsWindow(it->first) ? std::next(it) : s_styles.erase(it);
    for (auto it = s_frosted.begin(); it != s_frosted.end();) it = IsWindow(*it) ? std::next(it) : s_frosted.erase(it);
    for (auto it = s_rounded.begin(); it != s_rounded.end();) it = IsWindow(*it) ? std::next(it) : s_rounded.erase(it);
}

D2D1_POINT_2F DotCenter(int i) {   // 0 minimize, 1 zoom, 2 close (left to right, like Windows)
    float s = s_s;
    float cy = (s_patch.top + s_patch.bottom) / 2;
    float cx = s_patch.right - (kPadR + kD / 2) * s - (2 - i) * (kD + kGap) * s;
    return D2D1::Point2F(cx, cy);
}

int HitDot(POINT p) {
    float x = (float)(p.x - s_ov.x), y = (float)(p.y - s_ov.y);
    for (int i = 0; i < 3; i++) {
        auto c = DotCenter(i);
        float r = (kD / 2 + 3) * s_s;
        if ((x - c.x) * (x - c.x) + (y - c.y) * (y - c.y) <= r * r) return i;
    }
    return -1;
}

void Render() {
    float s = s_s;
    float tint[3] = {1, 1, 1};
    s_ov.Render(s, {}, tint, [&](ID2D1DeviceContext* dc) {
        // cover the app's own buttons with its title bar colour
        if (s_bar.a > 0) {
            s_br->SetColor(s_bar);
            float r = IsZoomed(s_target) ? 0.f : 8 * s;
            dc->FillRoundedRectangle(D2D1::RoundedRect(s_patch, r, r), s_br.Get());
        }
        const wchar_t* gl[3] = {L"", L"", L""};   // minimize, maximize, close
        for (int i = 0; i < 3; i++) {
            auto c = DotCenter(i);
            float r = kD / 2 * s;
            bool hot = i == s_hover;
            // frosted dot: soft milky fill, brighter rim, a glint on top
            D2D1_COLOR_F body = s_lightBar ? D2D1::ColorF(0, 0, 0, hot ? 0.24f : 0.13f) : D2D1::ColorF(1, 1, 1, hot ? 0.34f : 0.18f);
            D2D1_COLOR_F rim = s_lightBar ? D2D1::ColorF(0, 0, 0, 0.22f) : D2D1::ColorF(1, 1, 1, 0.38f);
            s_br->SetColor(body);
            dc->FillEllipse(D2D1::Ellipse(c, r, r), s_br.Get());
            s_br->SetColor(rim);
            dc->DrawEllipse(D2D1::Ellipse(c, r - 0.5f * s, r - 0.5f * s), s_br.Get(), 1.0f * s);
            s_br->SetColor(D2D1::ColorF(1, 1, 1, s_lightBar ? 0.5f : 0.28f));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x, c.y - r * 0.45f), r * 0.55f, r * 0.28f), s_br.Get());
            if (s_over) {
                s_br->SetColor(s_lightBar ? D2D1::ColorF(0, 0, 0, i == s_pressed ? 0.95f : 0.7f) : D2D1::ColorF(1, 1, 1, i == s_pressed ? 1.f : 0.85f));
                dc->DrawTextW(gl[i], 1, s_glyph.Get(), D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r), s_br.Get());
            }
        }
    });
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONDOWN: s_pressed = s_hover; s_dirty = true; return 0;
        case WM_LBUTTONUP: {
            int p = s_pressed;
            s_pressed = -1;
            s_dirty = true;
            if (p < 0 || p != s_hover || !IsWindow(s_target)) return 0;
            if (p == 2) PostMessageW(s_target, WM_SYSCOMMAND, SC_CLOSE, 0);
            else if (p == 0) ShowWindow(s_target, SW_MINIMIZE);
            else ShowWindow(s_target, IsZoomed(s_target) ? SW_RESTORE : SW_MAXIMIZE);   // zoom = its own space
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

namespace chrome {
bool IsBorderlessApp(HWND h) { return ListedBorderless(h); }

// Ctrl+Win+F: in and out of borderless full screen for this window right now (the app list stays as is)
void ToggleBorderlessWindow(HWND h) {
    h = h ? GetAncestor(h, GA_ROOT) : nullptr;
    if (!h || !IsAppWindow(h) || WindowPid(h) == g_selfPid) return;
    if (Chromium(h)) {   // Opera, Chrome, Discord...: their own full screen (F11) is smooth; ours flickers
        SetForegroundWindow(h);
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = VK_F11;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        return;
    }
    s_fights.erase(h);
    if (s_borderless.count(h)) {
        Unborderless(h);
        s_forced.erase(h);
        if (ListedBorderless(h)) s_exempt.insert(h);
        Log(L"borderless full screen off (window)");
    } else {
        s_exempt.erase(h);
        if (!ListedBorderless(h)) s_forced.insert(h);
        Borderless(h);
        Log(L"borderless full screen on (window)");
    }
}
void ToggleBorderlessApp(HWND h) {
    std::wstring exe = ExeOf(h);
    if (exe.empty()) return;
    std::vector<std::wstring> items;
    std::wstring cur = cfg.borderlessApps, tok;
    bool had = false;
    for (size_t i = 0; i <= cur.size(); i++) {
        if (i == cur.size() || cur[i] == L',') { if (!tok.empty()) { if (Lower(tok) == exe) had = true; else items.push_back(tok); } tok.clear(); }
        else if (cur[i] != L' ') tok += cur[i];
    }
    if (!had) items.push_back(exe);
    std::wstring out;
    for (auto& t : items) out += (out.empty() ? L"" : L",") + t;
    cfg.borderlessApps = out;
    SetConfigValue(L"borderless_apps", out);
    if (had) Unborderless(h); else Borderless(h);
}

bool Init() {
    if (!cfg.trafficLights && !cfg.hideCaptionButtons && !cfg.frostWindows && !cfg.roundCorners && cfg.borderlessApps.empty()) return true;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dx, &dy);
    s_s = dx / 96.f;
    if (cfg.trafficLights) {
        int w = (int)std::ceil(kOvW * s_s), h = (int)std::ceil(kOvH * s_s);
        if (!s_ov.Create(L"LiquidTrafficLights", Proc, -w - 100, 0, w, h, nullptr)) return false;
        s_ov.SetVisible(false);
        s_ov.SetClickThrough(true);
        s_ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &s_br);
        s_glyph = MakeFont(L"Segoe Fluent Icons", 6.0f * s_s, DWRITE_FONT_WEIGHT_NORMAL, true);
    }
    return true;
}

void Tick(bool) {
    ULONGLONG now = GetTickCount64();
    if (now - s_lastScan > 1000) { s_lastScan = now; Scan(); }
    if (!s_ov.hwnd) return;
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
    bool show = Eligible(root) && !lock::IsLocked() && !spotlight::IsOpen();
    if (show) {
        RECT r;
        GetWindowRect(root, &r);
        RECT ext{};   // the visible frame (without the invisible resize border)
        if (SUCCEEDED(DwmGetWindowAttribute(root, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext)))) r = ext;
        static ULONGLONG lastColor = 0;
        if (root != s_target || memcmp(&r, &s_trc, sizeof(r)) || now - lastColor > 600) {
            bool moved = root != s_target || memcmp(&r, &s_trc, sizeof(r));
            s_target = root;
            s_trc = r;
            lastColor = now;
            // where the app's own buttons are: DWM tells us for normal frames; apps that draw their
            // own title bar put them in the same spot, about 46 x 32 each
            float s = s_s, bw = 138 * s, bh = 32 * s;
            RECT cb{};
            if (SUCCEEDED(DwmGetWindowAttribute(root, 5 /*DWMWA_CAPTION_BUTTON_BOUNDS*/, &cb, sizeof(cb))) && cb.right - cb.left > 40 * s &&
                cb.bottom - cb.top > 16 * s) {
                bw = (float)(cb.right - cb.left);
                bh = (float)std::min<LONG>(cb.bottom - cb.top, (LONG)(48 * s));
            }
            // apps with a taller header centre their buttons in it (Spotify, Discord...): find the
            // close button's X glyph in the column through its middle and centre on that
            float top = 0;
            std::vector<float> lum;
            if (s_ov.cap && s_ov.cap->Column(r.right - (LONG)(23 * s), r.top + 1, (int)(96 * s), lum) && lum.size() > 8) {
                float bg = lum[1];
                int first = -1, last = -1;
                for (int i = 2; i < (int)lum.size(); i++)
                    if (std::fabs(lum[i] - bg) > 0.22f) { if (first < 0) first = i; last = i; if (i - first > 24 * s) break; }
                if (first >= 0 && last - first <= 20 * s) {
                    float cy = (first + last) / 2.f + 1;
                    bh = std::clamp(cy * 2, 24 * s, 88 * s);
                    top = std::max(0.f, cy - bh / 2);
                }
            }
            if (moved || s_patch.bottom - s_patch.top != bh) s_ov.Move(r.right - s_ov.w + (int)(kM * s), r.top - (int)(kM * s));
            s_patch = D2D1::RectF(s_ov.w - kM * s - bw, kM * s + top, s_ov.w - kM * s, kM * s + top + std::min(bh, kOvH * s - 2 * kM * s));
            float c[3];
            POINT sp{r.right - (LONG)bw - (LONG)(6 * s), r.top + (LONG)(top + bh / 2)};
            if (s_ov.cap && s_ov.cap->ColorAt(sp, c)) {
                s_bar = D2D1::ColorF(c[0], c[1], c[2], 1);
                s_lightBar = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2] > 0.6f;
            } else s_bar = D2D1::ColorF(0, 0, 0, 0);
            s_dirty = true;
        }
        // keep the dots just above the window they belong to
        SetWindowPos(s_ov.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (show != s_visible) { s_visible = show; s_ov.SetVisible(show); s_dirty = true; }
    if (!show) return;
    POINT p;
    GetCursorPos(&p);
    int hv = HitDot(p);
    float x = (float)(p.x - s_ov.x), y = (float)(p.y - s_ov.y);
    bool over = x >= s_patch.left && x <= s_patch.right && y >= s_patch.top && y <= s_patch.bottom;
    if (hv != s_hover || over != s_over) { s_hover = hv; s_over = over; s_dirty = true; }
    s_ov.SetClickThrough(!over);   // the whole patch is ours, so the hidden app buttons can't be hit
    if (s_dirty || g.snapPending) { s_dirty = false; Render(); }
}

void Shutdown() {
    while (!s_borderless.empty()) Unborderless(s_borderless.begin()->first);
    for (auto& [h, st] : s_styles) {
        if (!IsWindow(h)) continue;
        SetWindowLongPtrW(h, GWL_STYLE, st);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    s_styles.clear();
    for (HWND h : s_frosted) {
        if (!IsWindow(h)) continue;
        int backdrop = 0;   // back to automatic
        DwmSetWindowAttribute(h, 38, &backdrop, sizeof(backdrop));
    }
    s_frosted.clear();
    if (s_ov.hwnd) s_ov.Destroy();
}
}  // namespace chrome
