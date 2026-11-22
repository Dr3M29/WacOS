#include "shell.h"
#include <shellscalingapi.h>
#include <cmath>
#include <algorithm>

// macOS Tahoe style volume HUD: a small glass bubble under the menu bar, top right.
// The volume keys are taken over (see gestures.cpp), so the Windows flyout never appears.

namespace {
Overlay s_ov;
float s_s = 1, s_appear = 0, s_appearV = 0, s_shown = 0, s_level = 0, s_levelV = 0;
double s_hideAt = 0;
bool s_muted = false, s_dirty = false;
ComPtr<ID2D1SolidColorBrush> s_br;
ComPtr<IDWriteTextFormat> s_glyph;
const float BW = 250, BH = 50, MARGIN = 30;

double Now() { return GetTickCount64() / 1000.0; }

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

bool Ensure() {
    if (s_ov.hwnd) return true;
    HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    s_s = dx / 96.f;
    float s = s_s;
    int ww = (int)std::ceil((BW + MARGIN * 2) * s), wh = (int)std::ceil((BH + MARGIN * 2) * s);
    int x = mi.rcMonitor.right - (int)((BW + 12 + MARGIN) * s);
    int y = mi.rcMonitor.top + (int)((cfg.menubarHeight + 8 - MARGIN) * s);
    if (!s_ov.Create(L"LiquidHUD", Proc, x, y, ww, wh, nullptr)) return false;
    s_ov.SetClickThrough(true);
    s_ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &s_br);
    s_glyph = MakeFont(L"Segoe Fluent Icons", 17 * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    s_glyph->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    s_glyph->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return true;
}

void Render() {
    float s = s_s, a = std::clamp(s_appear, 0.f, 1.f);
    float m = MARGIN * s, w = BW * s, h = BH * s;
    // drop in from slightly above, like the macOS HUD
    float dy = (1 - a) * -8 * s;
    D2D1_RECT_F box = D2D1::RectF(m, m + dy, m + w, m + h + dy);
    GlassShape gs{box.left, box.top, w, h};
    gs.radius = h / 2; gs.blur = 2.4f; gs.bezel = h * 0.42f; gs.thick = 2.2f;
    RECT behind{s_ov.x + (int)m, s_ov.y + (int)m, s_ov.x + (int)(m + w), s_ov.y + (int)(m + h)};
    bool light = s_ov.cap ? s_ov.cap->Luma(behind) > 0.62f : false;
    gs.tint = light ? 0.38f : -0.34f;
    gs.shadow = 0.2f; gs.highlight = 0.0f; gs.opacity = a;
    float tintLight[3] = {0.97f, 0.97f, 0.98f}, tintDark[3] = {0.13f, 0.13f, 0.15f};
    s_ov.Render(s, {gs}, light ? tintLight : tintDark, [&](ID2D1DeviceContext* dc) {
        D2D1_COLOR_F fg = light ? D2D1::ColorF(0, 0, 0, 0.85f * a) : D2D1::ColorF(1, 1, 1, 0.95f * a);
        D2D1_COLOR_F track = light ? D2D1::ColorF(0, 0, 0, 0.14f * a) : D2D1::ColorF(1, 1, 1, 0.2f * a);
        float lv = std::clamp(s_level, 0.f, 1.f);
        const wchar_t* g = s_muted || lv <= 0.001f ? L"" : lv < 0.34f ? L"" : lv < 0.67f ? L"" : L"";
        s_br->SetColor(fg);
        dc->DrawTextW(g, 1, s_glyph.Get(), D2D1::RectF(box.left + 10 * s, box.top, box.left + 44 * s, box.bottom), s_br.Get());
        float tl = box.left + 50 * s, tr = box.right - 20 * s, cy = (box.top + box.bottom) / 2, th = 6 * s;
        s_br->SetColor(track);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tl, cy - th / 2, tr, cy + th / 2), th / 2, th / 2), s_br.Get());
        if (!s_muted && lv > 0) {
            s_br->SetColor(fg);
            float fr = tl + std::max(th, (tr - tl) * lv);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tl, cy - th / 2, fr, cy + th / 2), th / 2, th / 2), s_br.Get());
        }
    });
}
}  // namespace

namespace osd {
void ShowVolume() {
    if (!Ensure()) return;
    s_muted = sys::Muted();
    s_shown = sys::Volume();
    s_hideAt = Now() + 1.5;
    s_ov.SetVisible(true);
    s_dirty = true;
}

// volume keys: macOS steps of 1/16, Shift+Alt for quarter steps
void VolumeKey(int vk) {
    if (vk == VK_VOLUME_MUTE) sys::SetMuted(!sys::Muted());
    else {
        float step = (GetKeyState(VK_SHIFT) < 0 && GetKeyState(VK_MENU) < 0) ? 1 / 64.f : 1 / 16.f;
        float v = sys::Volume() + (vk == VK_VOLUME_UP ? step : -step);
        v = std::round(std::clamp(v, 0.f, 1.f) / (1 / 64.f)) * (1 / 64.f);
        sys::SetVolume(v);
        if (sys::Muted()) sys::SetMuted(false);
    }
    ShowVolume();
}

void Tick(bool nf) {
    if (!s_ov.hwnd || !s_ov.visible) return;
    double now = Now();
    float target = now < s_hideAt ? 1.f : 0.f;
    float pa = s_appear, pl = s_level;
    Spring(s_appear, s_appearV, target, 1 / 60.f, 700.f);
    Spring(s_level, s_levelV, s_shown, 1 / 60.f, 900.f);
    if (target == 0 && s_appear < 0.01f && std::fabs(s_appearV) < 0.05f) {
        s_appear = 0;
        s_ov.SetVisible(false);
        return;
    }
    if (s_dirty || nf || pa != s_appear || pl != s_level || g.snapPending) { s_dirty = false; Render(); }
}

void Shutdown() { if (s_ov.hwnd) s_ov.Destroy(); }
}  // namespace osd
