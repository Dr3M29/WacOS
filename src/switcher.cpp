#include "shell.h"
#include <shellscalingapi.h>
#include <cmath>
#include <algorithm>
#include <map>

// macOS Cmd+Tab style app switcher on Alt+Tab: one icon per app (not per window) in a liquid glass
// bar in the middle of the screen. Hold Alt, tap Tab to move (Shift+Tab / arrows go back), let go
// to switch. Esc cancels, Q quits the selected app. Windows hidden by Spaces are included, so
// switching to one brings its space back.

namespace {
struct App {
    std::wstring path, name;
    std::vector<HWND> wins;   // front to back
    IconImages icons;
    bool loaded = false;
};
Overlay s_ov;
float s_s = 1, s_anim = 0, s_animV = 0;
bool s_open = false, s_shown = false, s_dirty = false;
std::vector<App> s_apps;
int s_sel = 0, s_hover = -1;
double s_openedAt = 0;
std::map<std::wstring, IconImages> s_iconCache;
ComPtr<ID2D1SolidColorBrush> s_br;
ComPtr<IDWriteTextFormat> fName;
const float kIcon = 76, kPad = 16, kGap = 10, kM = 30;

double Now() { return GetTickCount64() / 1000.0; }

void Collect() {
    s_apps.clear();
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsAppWindow(h) || WindowPid(h) == g_selfPid) continue;
        std::wstring path = ProcessPath(WindowPid(h));
        if (path.empty()) continue;
        std::wstring key = Lower(path);
        auto it = std::find_if(s_apps.begin(), s_apps.end(), [&](const App& a) { return Lower(a.path) == key; });
        if (it == s_apps.end()) {
            App a;
            a.path = path;
            a.name = FileDescription(path);
            if (key.size() >= 13 && key.compare(key.size() - 13, 13, L"\\explorer.exe") == 0) a.name = L"Finder";
            a.wins.push_back(h);
            s_apps.push_back(a);
        } else it->wins.push_back(h);
    }
}

D2D1_RECT_F Bar() {
    float s = s_s, n = (float)std::max<size_t>(1, s_apps.size());
    float w = (kPad * 2 + n * kIcon + (n - 1) * kGap) * s, h = (kPad * 2 + kIcon + 26) * s;
    float cx = s_ov.w / 2.f, cy = s_ov.h / 2.f;
    return D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
}
D2D1_RECT_F IconRect(int i) {
    D2D1_RECT_F b = Bar();
    float s = s_s, x = b.left + (kPad + i * (kIcon + kGap)) * s, y = b.top + kPad * s;
    return D2D1::RectF(x, y, x + kIcon * s, y + kIcon * s);
}

void Render() {
    float s = s_s, a = std::clamp(s_anim, 0.f, 1.f);
    D2D1_RECT_F b = Bar();
    float pop = 0.94f + 0.06f * a, cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
    float w = (b.right - b.left) * pop, h = (b.bottom - b.top) * pop;
    GlassShape gs{cx - w / 2, cy - h / 2, w, h};
    gs.radius = 26 * s; gs.blur = 2.4f; gs.bezel = 20 * s; gs.thick = 2.4f; gs.tint = -0.45f; gs.shadow = 0.25f; gs.highlight = 0; gs.opacity = a;
    float tint[3] = {1, 1, 1};
    s_ov.Render(s, {gs}, tint, [&](ID2D1DeviceContext* dc) {
        for (int i = 0; i < (int)s_apps.size(); i++) {
            auto& ap = s_apps[i];
            if (!ap.loaded) {
                auto c = s_iconCache.find(Lower(ap.path));
                if (c != s_iconCache.end()) ap.icons = c->second;
                else { ap.icons = LoadIcons(dc, ap.path, (int)(kIcon * s * 2), IconOverride(ap.name, ap.path)); s_iconCache[Lower(ap.path)] = ap.icons; }
                ap.loaded = true;
            }
            D2D1_RECT_F r = IconRect(i);
            if (i == s_sel || i == s_hover) {
                s_br->SetColor(D2D1::ColorF(1, 1, 1, (i == s_sel ? 0.18f : 0.08f) * a));
                D2D1_RECT_F hl = D2D1::RectF(r.left - 5 * s, r.top - 5 * s, r.right + 5 * s, r.bottom + 5 * s);
                dc->FillRoundedRectangle(D2D1::RoundedRect(hl, 16 * s, 16 * s), s_br.Get());
            }
            ID2D1Bitmap1* bmp = ap.icons.custom ? ap.icons.color.Get() : ap.icons.color.Get();
            if (bmp) dc->DrawBitmap(bmp, &r, a, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            if (i == s_sel) {   // the app's name under the selected icon, like macOS
                D2D1_RECT_F nr = D2D1::RectF(r.left - 60 * s, r.bottom + 6 * s, r.right + 60 * s, r.bottom + 26 * s);
                s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.92f * a));
                dc->DrawTextW(ap.name.c_str(), (UINT32)ap.name.size(), fName.Get(), nr, s_br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    });
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_LBUTTONUP && s_hover >= 0) { s_sel = s_hover; switcher::Command(3); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

void Open() {
    Collect();
    if (s_apps.empty()) return;
    POINT p;
    GetCursorPos(&p);
    HMONITOR hm = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    s_s = dx / 96.f;
    int w = std::min<int>(mi.rcWork.right - mi.rcWork.left, (int)((kM * 2 + kPad * 2 + 16 * (kIcon + kGap)) * s_s));
    int h = (int)((kM * 2 + kPad * 2 + kIcon + 26) * s_s);
    int x = (mi.rcMonitor.left + mi.rcMonitor.right - w) / 2, y = (mi.rcMonitor.top + mi.rcMonitor.bottom - h) / 2;
    if (!s_ov.hwnd) {
        if (!s_ov.Create(L"LiquidSwitcher", Proc, x, y, w, h, nullptr)) return;
        s_ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &s_br);
        fName = MakeFont(L"Segoe UI Variable Text", 13 * s_s, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    } else s_ov.Move(x, y);
    s_ov.SetVisible(false);
    s_open = true;
    s_shown = false;
    s_anim = 0; s_animV = 0;
    s_openedAt = Now();
    s_sel = s_apps.size() > 1 ? 1 : 0;   // Alt+Tab goes to the app you used before this one
}

void Activate(App& ap) {
    if (ap.wins.empty()) return;
    HWND w = ap.wins.front();
    if (spaces::IsHidden(w)) spaces::Reveal(w);
    ActivateWindow(w);
}
}  // namespace

namespace switcher {
bool IsOpen() { return s_open; }

void Command(int c) {
    if (c == 1 || c == 2) {
        if (!s_open) { Open(); if (!s_open) return; if (c == 2) s_sel = (int)s_apps.size() - 1; }
        else {
            int n = (int)s_apps.size();
            s_sel = (s_sel + (c == 1 ? 1 : -1) + n) % n;
        }
        s_dirty = true;
        return;
    }
    if (!s_open) return;
    if (c == 5) {   // Q: quit the selected app (all its windows)
        if (s_sel >= 0 && s_sel < (int)s_apps.size())
            for (HWND w : s_apps[s_sel].wins) PostMessageW(w, WM_CLOSE, 0, 0);
        s_apps.erase(s_apps.begin() + s_sel);
        if (s_apps.empty()) c = 4;
        else { s_sel = std::min(s_sel, (int)s_apps.size() - 1); s_dirty = true; return; }
    }
    s_open = false;
    if (c == 3 && s_sel >= 0 && s_sel < (int)s_apps.size()) Activate(s_apps[s_sel]);
    s_dirty = true;
}

void Tick(bool) {
    if (!s_ov.hwnd) return;
    // a quick Alt+Tab just switches; the bar only appears if you keep holding Alt
    if (s_open && !s_shown && Now() - s_openedAt > 0.12) { s_shown = true; s_ov.SetVisible(true); s_dirty = true; }
    if (!s_shown) return;
    float pa = s_anim;
    Spring(s_anim, s_animV, s_open ? 1.f : 0.f, 1 / 60.f, 700.f);
    if (!s_open && s_anim < 0.01f) { s_anim = 0; s_shown = false; s_ov.SetVisible(false); return; }
    POINT p;
    GetCursorPos(&p);
    float x = (float)(p.x - s_ov.x), y = (float)(p.y - s_ov.y);
    int hv = -1;
    for (int i = 0; i < (int)s_apps.size(); i++) {
        D2D1_RECT_F r = IconRect(i);
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) hv = i;
    }
    if (hv != s_hover) { s_hover = hv; s_dirty = true; }
    D2D1_RECT_F b = Bar();
    s_ov.SetClickThrough(!(x >= b.left && x <= b.right && y >= b.top && y <= b.bottom));
    if (s_dirty || pa != s_anim || g.snapPending) { s_dirty = false; Render(); }
}

void Shutdown() { if (s_ov.hwnd) s_ov.Destroy(); }
}  // namespace switcher
