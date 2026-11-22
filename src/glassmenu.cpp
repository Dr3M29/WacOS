#include "shell.h"
#include <shellscalingapi.h>
#include <cmath>
#include <algorithm>

// macOS Tahoe style liquid glass menu: frosted, lightly tinted refracting glass,
// rounded blue selection pill, grey shortcut column, cascading submenus.
// Runs a nested loop (the rest of the shell keeps animating) with low-level hooks so a
// click anywhere else, or Esc, dismisses it.

struct MenuState {
    Overlay ov;
    std::vector<MenuEntry> items;
    std::vector<D2D1_RECT_F> rects;
    D2D1_RECT_F box{};
    float s = 1, appear = 0, appearV = 0, margin = 0;
    int hover = -1, opener = -1;     // opener = parent item that opened this submenu
    bool light = false, done = false;
    int result = 0;
    POINT lastP{-1, -1};
    double hoverSince = 0;
    MenuState* parent = nullptr;
    ComPtr<IDWriteTextFormat> font, hint, glyph;
    ComPtr<ID2D1SolidColorBrush> br;
};
static MenuState* S = nullptr;     // innermost open menu
static HHOOK s_mouse = nullptr, s_kbd = nullptr;

bool GlassMenuOpen() { return S != nullptr; }

static bool SelectableIn(MenuState* m, int i) {
    return i >= 0 && i < (int)m->items.size() && m->items[i].id != 0 && m->items[i].enabled;
}
static bool Inside(MenuState* m, POINT p) {
    float x = (float)(p.x - m->ov.x), y = (float)(p.y - m->ov.y);
    return x >= m->box.left && x <= m->box.right && y >= m->box.top && y <= m->box.bottom;
}
static int HitItem(MenuState* m, POINT p) {
    float x = (float)(p.x - m->ov.x), y = (float)(p.y - m->ov.y);
    for (int i = 0; i < (int)m->rects.size(); i++) {
        auto& rc = m->rects[i];
        if (x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom && SelectableIn(m, i)) return i;
    }
    return -1;
}

static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && S) {
        auto* m = (MSLLHOOKSTRUCT*)lp;
        if (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN) {
            bool inAny = false;
            for (MenuState* q = S; q; q = q->parent) if (Inside(q, m->pt)) inAny = true;
            if (!inAny) {
                for (MenuState* q = S; q; q = q->parent) q->done = true;   // click elsewhere closes everything
            } else if (!Inside(S, m->pt)) {
                S->done = true;   // click on a parent menu closes this submenu; the parent handles the click
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && S && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        int n = (int)S->items.size();
        switch (k->vkCode) {
            case VK_ESCAPE: S->done = true; return 1;
            case VK_LEFT: if (S->parent) S->done = true; return 1;
            case VK_RIGHT:
                if (SelectableIn(S, S->hover) && !S->items[S->hover].sub.empty()) S->hoverSince = -1;   // open now
                return 1;
            case VK_DOWN:
            case VK_UP: {
                int dir = k->vkCode == VK_DOWN ? 1 : -1, i = S->hover;
                for (int step = 0; step < n; step++) {
                    i = (i + dir + n) % n;
                    if (SelectableIn(S, i)) break;
                }
                S->hover = i;
                return 1;
            }
            case VK_RETURN:
                if (SelectableIn(S, S->hover)) {
                    if (S->items[S->hover].sub.empty()) { S->result = S->items[S->hover].id; S->done = true; }
                    else S->hoverSince = -1;
                }
                return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK MenuProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            auto* m = (MenuState*)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (!m || m != S) return 0;
            POINT p;
            GetCursorPos(&p);
            int i = HitItem(m, p);
            if (i >= 0) {
                m->hover = i;
                if (m->items[i].sub.empty()) { m->result = m->items[i].id; m->done = true; }
                else m->hoverSince = -1;   // click on a submenu item opens it right away
            }
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void RenderMenu(MenuState& m) {
    float a = m.appear, s = m.s;
    std::vector<GlassShape> sh;
    GlassShape gs{m.box.left, m.box.top, m.box.right - m.box.left, m.box.bottom - m.box.top};
    // Tahoe menus: real refraction at the rim, frosted inside so text stays readable, soft shadow
    gs.radius = 13 * s; gs.blur = 2.4f; gs.bezel = 12 * s; gs.thick = 2.2f;
    gs.tint = m.light ? 0.38f : -0.34f;
    gs.shadow = 0.22f; gs.highlight = 0.0f; gs.opacity = a;
    sh.push_back(gs);
    float tintLight[3] = {0.97f, 0.97f, 0.98f}, tintDark[3] = {0.13f, 0.13f, 0.15f};
    m.ov.Render(s, sh, m.light ? tintLight : tintDark, [&](ID2D1DeviceContext* dc) {
        for (int i = 0; i < (int)m.items.size(); i++) {
            auto& it = m.items[i];
            D2D1_RECT_F r = m.rects[i];
            if (it.id == 0) {
                float cy = std::round((r.top + r.bottom) / 2);
                m.br->SetColor(m.light ? D2D1::ColorF(0, 0, 0, 0.10f * a) : D2D1::ColorF(1, 1, 1, 0.12f * a));
                dc->FillRectangle(D2D1::RectF(r.left + 14 * s, cy - 0.5f * s, r.right - 14 * s, cy + 0.5f * s), m.br.Get());
                continue;
            }
            bool hot = i == m.hover && it.enabled;
            if (hot) {
                m.br->SetColor(D2D1::ColorF(0.0f, 0.47f, 1.0f, 0.92f * a));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 5 * s, r.top, r.right - 5 * s, r.bottom),
                                                           8 * s, 8 * s), m.br.Get());
            }
            float main = it.enabled ? 1.f : 0.32f;
            D2D1_COLOR_F tc = hot ? D2D1::ColorF(1, 1, 1, a)
                            : m.light ? D2D1::ColorF(0, 0, 0, 0.86f * main * a)
                                      : D2D1::ColorF(1, 1, 1, 0.92f * main * a);
            D2D1_COLOR_F dim = hot ? D2D1::ColorF(1, 1, 1, 0.8f * a)
                             : m.light ? D2D1::ColorF(0, 0, 0, 0.42f * main * a)
                                       : D2D1::ColorF(1, 1, 1, 0.45f * main * a);
            m.br->SetColor(tc);
            if (it.checked)
                dc->DrawTextW(L"✓", 1, m.font.Get(), D2D1::RectF(r.left + 11 * s, r.top, r.left + 26 * s, r.bottom), m.br.Get());
            dc->DrawTextW(it.text.c_str(), (UINT32)it.text.size(), m.font.Get(),
                          D2D1::RectF(r.left + 27 * s, r.top, r.right - 30 * s, r.bottom), m.br.Get());
            m.br->SetColor(dim);
            if (!it.sub.empty()) {
                const wchar_t* chev = L"";   // ChevronRight (Segoe Fluent)
                dc->DrawTextW(chev, 1, m.glyph.Get(), D2D1::RectF(r.right - 26 * s, r.top, r.right - 12 * s, r.bottom), m.br.Get());
            } else if (!it.shortcut.empty()) {
                dc->DrawTextW(it.shortcut.c_str(), (UINT32)it.shortcut.size(), m.hint.Get(),
                              D2D1::RectF(r.left + 27 * s, r.top, r.right - 14 * s, r.bottom), m.br.Get());
            }
        }
    });
}

static double NowSec() { return GetTickCount64() / 1000.0; }
static int RunMenu(int x, int y, const std::vector<MenuEntry>& items, MenuState* parent, int opener, int alignRight);

// open the submenu of item i of menu m, next to it (flips left when there is no room)
static int OpenSub(MenuState& m, int i) {
    auto& r = m.rects[i];
    int right = m.ov.x + (int)(m.box.right - 4 * m.s);
    int left = m.ov.x + (int)(m.box.left + 4 * m.s);
    int top = m.ov.y + (int)(r.top - 5 * m.s);
    return RunMenu(right, top, m.items[i].sub, &m, i, left);
}

static int RunMenu(int x, int y, const std::vector<MenuEntry>& items, MenuState* parent, int opener, int alignRight) {
    if (items.empty()) return 0;
    MenuState st;
    st.parent = parent;
    st.opener = opener;
    st.items = items;
    HMONITOR hm = MonitorFromPoint({x, y}, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    st.s = dx / 96.f;
    float s = st.s;
    st.font = MakeFont(L"Segoe UI Variable Text", 13.5f * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    st.hint = MakeFont(L"Segoe UI Variable Text", 12.5f * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    st.hint->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    st.glyph = MakeFont(L"Segoe Fluent Icons", 9.5f * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    st.glyph->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    float itemH = 24 * s, sepH = 11 * s, pad = 5 * s, w = 200 * s;
    for (auto& it : items) {
        if (!it.id) continue;
        float extra = it.sub.empty() ? (it.shortcut.empty() ? 0 : TextWidth(st.hint.Get(), it.shortcut) + 24 * s) : 24 * s;
        w = std::max(w, TextWidth(st.font.Get(), it.text) + 46 * s + extra);
    }
    w = std::min(w, 480 * s);
    float h = pad * 2;
    for (auto& it : items) h += it.id ? itemH : sepH;
    st.margin = 30 * s;
    int ww = (int)std::ceil(w + st.margin * 2), wh = (int)std::ceil(h + st.margin * 2);

    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    int bx = x;
    if (bx + w > mi.rcWork.right - 4) bx = parent ? (int)(alignRight - w) : (int)(mi.rcWork.right - w - 4);
    bx = std::max(bx, (int)mi.rcWork.left + 4);
    int by = y;
    if (by + h > mi.rcWork.bottom - 4) by = parent ? (int)(mi.rcWork.bottom - h - 4) : (int)(y - h);   // flip above
    by = std::max(by, (int)mi.rcMonitor.top + 2);
    st.box = D2D1::RectF(st.margin, st.margin, st.margin + w, st.margin + h);
    float cy = st.margin + pad;
    for (auto& it : items) {
        float ih = it.id ? itemH : sepH;
        st.rects.push_back(D2D1::RectF(st.margin, cy, st.margin + w, cy + ih));
        cy += ih;
    }

    if (!st.ov.Create(L"LiquidMenu", MenuProc, bx - (int)st.margin, by - (int)st.margin, ww, wh, &st)) return 0;
    SetWindowLongPtrW(st.ov.hwnd, GWLP_USERDATA, (LONG_PTR)&st);
    st.ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &st.br);
    RECT behind{bx, by, bx + (int)w, by + (int)h};
    st.light = st.ov.cap ? st.ov.cap->Luma(behind) > 0.62f : false;

    MenuState* outer = S;
    S = &st;
    bool top = parent == nullptr;
    if (top) {
        s_mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
        s_kbd = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
    }
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -10000LL * 16;
    SetWaitableTimer(timer, &due, 16, nullptr, nullptr, FALSE);

    int lastHover = -2;
    double leaveParentSince = 0;
    while (!st.done && !g_quit) {
        DWORD r = MsgWaitForMultipleObjects(1, &timer, FALSE, 100, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = true; PostQuitMessage(0); }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (st.done) break;
        if (r != WAIT_OBJECT_0) continue;
        double now = NowSec();
        POINT p;
        GetCursorPos(&p);
        if (p.x != st.lastP.x || p.y != st.lastP.y) {
            st.lastP = p;
            if (Inside(&st, p)) {
                int hv = HitItem(&st, p);
                if (hv != st.hover) { st.hover = hv; st.hoverSince = now; }
            }
        }
        // submenu: moving onto another item of the parent closes this one (after a short grace period)
        if (parent && Inside(parent, p) && HitItem(parent, p) != opener) {
            if (!leaveParentSince) leaveParentSince = now;
            if (now - leaveParentSince > 0.18) { st.done = true; break; }
        } else leaveParentSince = 0;

        TickShell();
        float prevA = st.appear;
        Spring(st.appear, st.appearV, 1.f, 1 / 60.f, 900.f);
        if (st.appear != prevA || st.hover != lastHover || g.snapPending) {
            lastHover = st.hover;
            RenderMenu(st);
        } else {
            for (auto& c : g.caps) if (c.get() == st.ov.cap && c->fresh) { RenderMenu(st); break; }
        }

        // hover-open a submenu after 0.22 s (or at once when clicked / arrow-right)
        if (SelectableIn(&st, st.hover) && !st.items[st.hover].sub.empty() &&
            (st.hoverSince < 0 || now - st.hoverSince > 0.22)) {
            int pick = OpenSub(st, st.hover);
            S = &st;
            st.hoverSince = 1e300;   // don't reopen until the hover changes
            st.lastP = {-1, -1};
            lastHover = -2;
            if (pick) { st.result = pick; st.done = true; }
        }
    }
    if (top) {
        if (s_mouse) UnhookWindowsHookEx(s_mouse);
        if (s_kbd) UnhookWindowsHookEx(s_kbd);
        s_mouse = s_kbd = nullptr;
    }
    CloseHandle(timer);
    int result = st.result;
    st.ov.Destroy();
    S = outer;
    return result;
}

int GlassMenu(int x, int y, const std::vector<MenuEntry>& items) {
    if (S) return 0;
    return RunMenu(x, y, items, nullptr, -1, x);
}
