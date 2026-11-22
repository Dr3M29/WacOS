#include "shell.h"
#include <shellapi.h>
#include <powrprof.h>
#include <cmath>
#include <algorithm>

std::vector<std::unique_ptr<MenuBar>> menubars;

enum {
    ID_LOGO = 1, ID_APP = 2, ID_MENU0 = 10,
    ID_SEARCH = 30, ID_WIFI, ID_VOL, ID_CC, ID_CLOCK
};

#define WM_APPBAR (WM_APP + 1)

static std::wstring s_sharedApp = L"Finder";
static HWND s_sharedFg = nullptr;
static ULONGLONG s_lastStatus = 0;
static std::wstring s_wifiGlyph = L"\uE701", s_volGlyph = L"\uE767";

bool MenuBar::Init(RECT m, float scale, bool isPrimary) {
    mon = m;
    s = scale;
    primary = isPrimary;
    H = (int)std::lround(cfg.menubarHeight * s);
    if (!ov.Create(L"LiquidMenuBar", Proc, mon.left, mon.top, mon.right - mon.left, H, this)) return false;

    APPBARDATA a{sizeof(a)};
    a.hWnd = ov.hwnd;
    a.uCallbackMessage = WM_APPBAR;
    SHAppBarMessage(ABM_NEW, &a);
    a.uEdge = ABE_TOP;
    a.rc = {mon.left, mon.top, mon.right, mon.top + H};
    SHAppBarMessage(ABM_QUERYPOS, &a);
    a.rc.top = mon.top;
    a.rc.bottom = mon.top + H;
    SHAppBarMessage(ABM_SETPOS, &a);

    float fs = std::clamp(cfg.menubarHeight * 0.55f, 11.f, 16.f) * s;
    const wchar_t* ui = L"Segoe UI Variable Text";
    fBold = MakeFont(ui, fs, DWRITE_FONT_WEIGHT_BOLD, false);
    fReg = MakeFont(ui, fs, DWRITE_FONT_WEIGHT_NORMAL, false);
    fIcon = MakeFont(L"Segoe Fluent Icons", fs + 1 * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fLogo = MakeFont(L"Segoe UI Symbol", fs + 2 * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br);
    return true;
}

void MenuBar::Shutdown() {
    APPBARDATA a{sizeof(a)};
    a.hWnd = ov.hwnd;
    SHAppBarMessage(ABM_REMOVE, &a);
    ov.Destroy();
}

std::wstring MenuBar::AppNameFor(HWND w) {
    wchar_t cls[64];
    GetClassNameW(w, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return L"Finder";
    std::wstring path = ProcessPath(WindowPid(w));
    if (path.empty()) return s_sharedApp;
    std::wstring lp = Lower(path);
    if (lp.size() >= 13 && lp.compare(lp.size() - 13, 13, L"\\explorer.exe") == 0) return L"Finder";
    return FileDescription(path);
}

void MenuBar::Tick(bool newFrame) {
    ULONGLONG now = GetTickCount64();

    if (primary) {
        HWND fg = GetForegroundWindow();
        if (fg && fg != s_sharedFg) {
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            if (pid != g_selfPid) { s_sharedFg = fg; s_sharedApp = AppNameFor(fg); }
        }
        if (now - s_lastStatus > 2000) {
            s_lastStatus = now;
            s_wifiGlyph = sys::EthernetUp() ? L"\uE839" : L"\uE701";
            s_volGlyph = sys::Muted() ? L"\uE74F" : L"\uE767";
        }
    }
    // each screen's bar follows the app you're using on that screen: the foreground window if
    // it's here, otherwise the top app window on this screen
    if (now - lastTarget > 250) {
        lastTarget = now;
        HWND fg = s_sharedFg, pick = nullptr;
        HMONITOR me = MonitorFromPoint({mon.left + 1, mon.top + 1}, MONITOR_DEFAULTTONEAREST);
        if (fg && IsWindow(fg) && MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) == me) pick = fg;
        if (!pick) {
            for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
                if (!IsAppWindow(h) || IsIconic(h) || WindowPid(h) == g_selfPid || spaces::IsHidden(h)) continue;
                if (MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) == me) { pick = h; break; }
            }
        }
        if (pick != lastFg) {
            lastFg = pick;
            std::wstring a = pick ? (pick == s_sharedFg ? s_sharedApp : AppNameFor(pick)) : L"Finder";
            if (a != app) app = a;
            dirty = true;
        }
        if (lastFg != topsFor) { tops = appmenu::TopsFor(lastFg, app); topsFor = lastFg; dirty = true; }
    }
    if (wifiGlyph != s_wifiGlyph || volGlyph != s_volGlyph) { wifiGlyph = s_wifiGlyph; volGlyph = s_volGlyph; dirty = true; }

    SYSTEMTIME st;
    GetLocalTime(&st);
    if (st.wMinute != lastMinute) {
        lastMinute = st.wMinute;
        wchar_t d[64], t[64];
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd MMM d", d, 64, nullptr);
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"h:mm tt", t, 64);
        clock = std::wstring(d) + L"  " + t;
        dirty = true;
    }

    if (now - lastLuma > 400 && ov.cap) {
        lastLuma = now;
        RECT band{mon.left, mon.top, mon.right, mon.top + H};
        float l = ov.cap->Luma(band);
        bool dt = darkText ? l > 0.52f : l > 0.62f;
        if (dt != darkText) { darkText = dt; dirty = true; }
    }

    POINT p;
    GetCursorPos(&p);
    int hv = -1;
    if (p.y >= ov.y && p.y < ov.y + H && p.x >= ov.x && p.x < ov.x + ov.w) {
        float x = (float)(p.x - ov.x);
        for (auto& h : hits)
            if (x >= h.r.left && x < h.r.right) { hv = h.id; break; }
    }
    if (hv != hover) { hover = hv; dirty = true; }

    if (hidden) return;
    bool capFresh = ov.cap && ov.cap->fresh;
    if ((newFrame && capFresh) || dirty || g.snapPending) {
        Render();
        dirty = false;
    }
}

void MenuBar::Render() {
    struct T { IDWriteTextFormat* f; std::wstring str; float x, w; bool dim; };
    std::vector<T> ts;
    hits.clear();
    float pad = 7 * s;
    float x = 12 * s;
    auto addL = [&](IDWriteTextFormat* f, const std::wstring& t, int id) {
        float w = TextWidth(f, t);
        ts.push_back({f, t, x, w, false});
        hits.push_back({D2D1::RectF(x - pad, 0, x + w + pad, (float)H), id});
        x += w + 2 * pad;
    };
    addL(fLogo.Get(), cfg.logo, ID_LOGO);
    addL(fBold.Get(), app, ID_APP);
    for (size_t i = 0; i < tops.size() && i < 12; i++) addL(fReg.Get(), tops[i].title, ID_MENU0 + (int)i);

    float rx = (float)ov.w - 12 * s;
    auto addR = [&](IDWriteTextFormat* f, const std::wstring& t, int id) {
        float w = TextWidth(f, t);
        rx -= w;
        ts.push_back({f, t, rx, w, false});
        hits.push_back({D2D1::RectF(rx - pad, 0, rx + w + pad, (float)H), id});
        rx -= 2 * pad;
    };
    addR(fReg.Get(), clock, ID_CLOCK);
    addR(fIcon.Get(), L"\uE9E9", ID_CC);      // control centre (toggle switches)
    addR(fIcon.Get(), volGlyph, ID_VOL);
    addR(fIcon.Get(), wifiGlyph, ID_WIFI);
    addR(fIcon.Get(), L"\uE721", ID_SEARCH);

    std::vector<GlassShape> sh;
    if (cfg.menubarGlass) {
        GlassShape bar{-60.f, -60.f, (float)ov.w + 120, (float)H + 60};
        bar.blur = 0.0f; bar.tint = 0.0f; bar.bezel = H * 0.5f; bar.thick = 2.6f; bar.highlight = 0.0f;
        sh.push_back(bar);
    }
    int hv = pressed >= 0 ? pressed : hover;
    bool ccOpen = cc.open && cc.mon.left == mon.left && cc.mon.top == mon.top;
    for (auto& h : hits) {
        bool on = h.id == hv || (ccOpen && h.id == ID_CC);
        if (!on) continue;
        float ph = H - 4 * s;
        GlassShape pill{h.r.left, 2 * s, h.r.right - h.r.left, ph};
        pill.radius = ph / 2; pill.blur = 0.0f; pill.tint = 0.0f;
        pill.bezel = ph / 2; pill.thick = pressed >= 0 ? 3.2f : 2.6f; pill.highlight = 0.0f;
        sh.push_back(pill);
    }
    float tintLight[3] = {1, 1, 1}, tintDark[3] = {0.08f, 0.08f, 0.10f};
    ov.Render(s, sh, darkText ? tintLight : tintDark, [&](ID2D1DeviceContext* dc) {
        for (auto& t : ts) {
            if (!darkText) {
                br->SetColor(D2D1::ColorF(0, 0, 0, 0.30f));
                dc->DrawTextW(t.str.c_str(), (UINT32)t.str.size(), t.f,
                              D2D1::RectF(t.x, 1 * s, t.x + t.w + 50, H + 1 * s), br.Get());
            }
            float al = primary ? 1.f : 0.75f;
            br->SetColor(darkText ? D2D1::ColorF(0, 0, 0, 0.88f * al) : D2D1::ColorF(1, 1, 1, 0.97f * al));
            dc->DrawTextW(t.str.c_str(), (UINT32)t.str.size(), t.f,
                          D2D1::RectF(t.x, 0, t.x + t.w + 50, (float)H), br.Get());
        }
    });
}

static bool Confirm(const wchar_t* what) {
    return MessageBoxW(nullptr, what, L"Liquid", MB_OKCANCEL | MB_ICONQUESTION | MB_TOPMOST) == IDOK;
}

void MenuBar::Click(int id) {
    D2D1_RECT_F r{};
    for (auto& h : hits) if (h.id == id) r = h.r;
    int px = ov.x + (int)r.left, py = ov.y + H + (int)(4 * s);
    HWND target = lastFg;

    if (id == ID_LOGO) {
        // The Apple menu, item for item (App Store = Microsoft Store, Force Quit = Task Manager),
        // with Liquid's own two commands at the end.
        int c = GlassMenu(px, py, {
            {1, L"About This PC"}, {0},
            {2, L"System Settings..."}, {9, L"App Store..."}, {0},
            {11, L"Force Quit...", false, true, L"Ctrl+Shift+Esc"}, {0},
            {3, L"Sleep"}, {4, L"Restart..."}, {5, L"Shut Down..."}, {0},
            {6, L"Lock Screen", false, true, L"Win+L"}, {12, L"Log Out..."}, {0},
            {10, L"Reload Liquid", false, true, L"Ctrl+Alt+Shift+R"}, {7, L"Quit Liquid", false, true, L"Ctrl+Alt+Shift+Q"}});
        switch (c) {
            case 1: about::Open(); break;
            case 2: prefs::Open(prefs::General); break;
            case 9: Launch(L"ms-windows-store:"); break;
            case 11: Launch(L"taskmgr.exe"); break;
            case 3: SetSuspendState(FALSE, FALSE, FALSE); break;
            case 4: if (Confirm(L"Restart your computer now?")) Launch(L"shutdown.exe", L"/r /t 0", SW_HIDE); break;
            case 5: if (Confirm(L"Shut down your computer now?")) Launch(L"shutdown.exe", L"/s /t 0", SW_HIDE); break;
            case 6: lock::Lock(); break;
            case 12: if (Confirm(L"Log out of Windows now? Apps you have open will close.")) ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_FLAG_PLANNED); break;
            case 10: g_restart = true; g_quit = true; break;
            case 7: g_quit = true; break;
        }
    } else if (id == ID_APP) {
        int c = GlassMenu(px, py, {{4, L"About " + app}, {0}, {1, L"Hide " + app, false, true, L"Win+Down"}, {3, L"Hide Others"}, {5, L"Show All"},
                                   {0}, {2, L"Quit " + app, false, true, L"Alt+F4"}});
        if (c == 4 && target) {   // file properties of the app = its version, publisher...
            std::wstring path = ProcessPath(WindowPid(target));
            if (!path.empty()) { SHELLEXECUTEINFOW ei{sizeof(ei)}; ei.fMask = SEE_MASK_INVOKEIDLIST; ei.lpVerb = L"properties"; ei.lpFile = path.c_str(); ei.nShow = SW_SHOW; ShellExecuteExW(&ei); }
        } else if (c == 5) {
            EnumWindows([](HWND h, LPARAM) -> BOOL { if (IsAppWindow(h) && IsIconic(h)) ShowWindowAsync(h, SW_RESTORE); return TRUE; }, 0);
        }
        if (c == 1 && target) ShowWindow(target, SW_MINIMIZE);
        else if (c == 2 && target) PostMessageW(target, WM_CLOSE, 0, 0);
        else if (c == 3) {
            HWND keep = target;
            EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                if (h != (HWND)lp && IsAppWindow(h) && !IsIconic(h)) ShowWindowAsync(h, SW_MINIMIZE);
                return TRUE;
            }, (LPARAM)keep);
        }
    } else if (id >= ID_MENU0 && id < ID_MENU0 + (int)tops.size()) {
        appmenu::Open(target, tops[id - ID_MENU0], app, px, py);
    } else if (id == ID_SEARCH) {
        spotlight::Toggle();
    } else if (id == ID_WIFI) {
        nc::Close();
        cc.Toggle(mon, sys::WifiAvailable() ? 1 : 0);
    } else if (id == ID_VOL) {
        nc::Close();
        cc.Toggle(mon, 2);
    } else if (id == ID_CC) {
        nc::Close();
        cc.Toggle(mon, 0);
    } else if (id == ID_CLOCK) {
        nc::Toggle(mon);
    }
    dirty = true;
}

LRESULT CALLBACK MenuBar::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    MenuBar* mb = (MenuBar*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!mb) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONDOWN: mb->pressed = mb->hover; mb->dirty = true; return 0;
        case WM_LBUTTONUP: {
            int p = mb->pressed;
            mb->pressed = -1;
            mb->dirty = true;
            if (p >= 0 && p == mb->hover) mb->Click(p);
            return 0;
        }
        case WM_HOTKEY:
            if (wp == 1) g_quit = true;
            else if (wp == 2) g.snapPending = true;
            else if (wp == 3) { g_restart = true; g_quit = true; }
            return 0;
        case WM_APPBAR:
            if (wp == ABN_FULLSCREENAPP) {
                // a full-screen app: the menu bar stays on top (you can still switch apps and use
                // everything up there) and the dock tucks away, popping up at the bottom edge
                bool fs = lp != 0;
                SetWindowPos(mb->ov.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                if (dock.mon.left == mb->mon.left && dock.mon.top == mb->mon.top) {
                    dock.fsHide = fs;
                    SetWindowPos(dock.ov.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            g_restart = true;
            g_quit = true;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
