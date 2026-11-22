#include "shell.h"
#include <shellapi.h>
#include <functional>
#include <algorithm>

// The menu bar's File / Edit / View / Window / Help, as Liquid glass menus.
//  * Classic Win32 apps with a real menu bar (GetMenu): their own menus are read and shown
//    in glass, and choosing an item sends the app that exact command.
//  * Everything else (Electron, Chromium, modern apps): macOS-style standard menus whose
//    items are the universal shortcuts (Undo, Copy, Zoom In, New Tab...), sent to the app the
//    menu bar belongs to, plus "<App>'s own ... menu" to open the app's native one.

namespace {
enum { B_APPMENU = 1, B_FILE, B_EDIT, B_VIEW, B_GO, B_WINDOW, B_HELP };

std::vector<std::function<void()>> s_actions;

void SendKeys(const std::vector<WORD>& keys) {
    std::vector<INPUT> in;
    for (WORD k : keys) { INPUT i{INPUT_KEYBOARD}; i.ki.wVk = k; in.push_back(i); }
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        INPUT i{INPUT_KEYBOARD}; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

void Focus(HWND w) {
    if (!w || !IsWindow(w)) return;
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    SetForegroundWindow(w);
    Sleep(40);
}

// a menu entry that sends a shortcut to the app
MenuEntry Key(HWND w, const std::wstring& text, std::vector<WORD> keys, const std::wstring& hint) {
    s_actions.push_back([w, keys] { Focus(w); SendKeys(keys); });
    return {(int)s_actions.size(), text, false, true, hint};
}
MenuEntry Act(const std::wstring& text, std::function<void()> f, const std::wstring& hint = L"", bool checked = false) {
    s_actions.push_back(std::move(f));
    return {(int)s_actions.size(), text, checked, true, hint};
}

std::wstring Clean(const std::wstring& raw, std::wstring* hint) {
    std::wstring t = raw;
    size_t tab = t.find(L'\t');
    if (tab != std::wstring::npos) { if (hint) *hint = t.substr(tab + 1); t = t.substr(0, tab); }
    std::wstring o;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == L'&') { if (i + 1 < t.size() && t[i + 1] == L'&') { o += L'&'; i++; } continue; }
        o += t[i];
    }
    return o;
}

// read an app's real (Win32) menu into glass entries; choosing one posts WM_COMMAND to it
void ReadHMenu(HWND w, HMENU hm, std::vector<MenuEntry>& out, int depth) {
    DWORD_PTR r = 0;
    SendMessageTimeoutW(w, WM_INITMENUPOPUP, (WPARAM)hm, 0, SMTO_ABORTIFHUNG, 150, &r);   // apps fill menus lazily
    int n = GetMenuItemCount(hm);
    for (int i = 0; i < n && i < 80; i++) {
        wchar_t buf[256] = {};
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        mi.dwTypeData = buf;
        mi.cch = 255;
        if (!GetMenuItemInfoW(hm, i, TRUE, &mi)) continue;
        if (mi.fType & MFT_SEPARATOR) { if (!out.empty() && out.back().id != 0) out.push_back({0}); continue; }
        MenuEntry e;
        e.text = Clean(buf, &e.shortcut);
        if (e.text.empty()) continue;
        e.checked = (mi.fState & MFS_CHECKED) != 0;
        e.enabled = !(mi.fState & (MFS_DISABLED | MFS_GRAYED));
        if (mi.hSubMenu) {
            if (depth > 3) continue;
            ReadHMenu(w, mi.hSubMenu, e.sub, depth + 1);
            if (e.sub.empty()) continue;
            e.id = -1;
            e.shortcut.clear();
        } else {
            UINT cmd = mi.wID;
            s_actions.push_back([w, cmd] { Focus(w); PostMessageW(w, WM_COMMAND, MAKEWPARAM(cmd, 0), 0); });
            e.id = (int)s_actions.size();
        }
        out.push_back(std::move(e));
    }
    while (!out.empty() && out.back().id == 0) out.pop_back();
}

bool IsFinder(HWND w, const std::wstring& app) { return app == L"Finder"; }

// windows that belong to the same app (same process), for the Window menu
std::vector<HWND> AppWindows(HWND w) {
    struct C { DWORD pid; std::vector<HWND> v; } c{WindowPid(w), {}};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = (C*)lp;
        if (WindowPid(h) == c->pid && IsAppWindow(h)) c->v.push_back(h);
        return TRUE;
    }, (LPARAM)&c);
    return c.v;
}

void NativeMenu(HWND w, WORD letter) { Focus(w); SendKeys({VK_MENU, letter}); }

std::vector<MenuEntry> Builtin(HWND w, int which, const std::wstring& app) {
    std::vector<MenuEntry> m;
    bool finder = IsFinder(w, app);
    auto own = [&](const wchar_t* name, WORD letter) {
        if (finder || !w) return;
        m.push_back({0});
        m.push_back(Act(app + L"'s own " + name + L" menu", [w, letter] { NativeMenu(w, letter); }));
    };
    switch (which) {
        case B_FILE:
            if (finder) {
                m.push_back(Act(L"New Finder Window", [] { Launch(L"explorer.exe"); }, L"Ctrl+N"));
                m.push_back(Key(w, L"New Folder", {VK_CONTROL, VK_SHIFT, 'N'}, L"Ctrl+Shift+N"));
                m.push_back(Key(w, L"New Tab", {VK_CONTROL, 'T'}, L"Ctrl+T"));
                m.push_back({0});
                m.push_back(Key(w, L"Open", {VK_RETURN}, L"Enter"));
                m.push_back(Key(w, L"Rename", {VK_F2}, L"F2"));
                m.push_back(Key(w, L"Get Info", {VK_MENU, VK_RETURN}, L"Alt+Enter"));
                m.push_back({0});
                m.push_back(Key(w, L"Move to Trash", {VK_DELETE}, L"Del"));
                m.push_back(Key(w, L"Close Window", {VK_CONTROL, 'W'}, L"Ctrl+W"));
            } else {
                m.push_back(Key(w, L"New Window", {VK_CONTROL, 'N'}, L"Ctrl+N"));
                m.push_back(Key(w, L"New Tab", {VK_CONTROL, 'T'}, L"Ctrl+T"));
                m.push_back(Key(w, L"Open...", {VK_CONTROL, 'O'}, L"Ctrl+O"));
                m.push_back({0});
                m.push_back(Key(w, L"Save", {VK_CONTROL, 'S'}, L"Ctrl+S"));
                m.push_back(Key(w, L"Save As...", {VK_CONTROL, VK_SHIFT, 'S'}, L"Ctrl+Shift+S"));
                m.push_back({0});
                m.push_back(Key(w, L"Print...", {VK_CONTROL, 'P'}, L"Ctrl+P"));
                m.push_back({0});
                m.push_back(Key(w, L"Close Tab", {VK_CONTROL, 'W'}, L"Ctrl+W"));
                m.push_back(Act(L"Close Window", [w] { PostMessageW(w, WM_CLOSE, 0, 0); }, L"Alt+F4"));
            }
            own(L"File", 'F');
            break;
        case B_EDIT:
            m.push_back(Key(w, L"Undo", {VK_CONTROL, 'Z'}, L"Ctrl+Z"));
            m.push_back(Key(w, L"Redo", {VK_CONTROL, 'Y'}, L"Ctrl+Y"));
            m.push_back({0});
            m.push_back(Key(w, L"Cut", {VK_CONTROL, 'X'}, L"Ctrl+X"));
            m.push_back(Key(w, L"Copy", {VK_CONTROL, 'C'}, L"Ctrl+C"));
            m.push_back(Key(w, L"Paste", {VK_CONTROL, 'V'}, L"Ctrl+V"));
            m.push_back(Key(w, L"Paste and Match Style", {VK_CONTROL, VK_SHIFT, 'V'}, L"Ctrl+Shift+V"));
            m.push_back(Key(w, L"Select All", {VK_CONTROL, 'A'}, L"Ctrl+A"));
            m.push_back({0});
            m.push_back(Key(w, L"Find...", {VK_CONTROL, 'F'}, L"Ctrl+F"));
            m.push_back({0});
            m.push_back(Key(w, L"Clipboard History", {VK_LWIN, 'V'}, L"Win+V"));
            m.push_back(Key(w, L"Emoji & Symbols", {VK_LWIN, VK_OEM_PERIOD}, L"Win+."));
            own(L"Edit", 'E');
            break;
        case B_VIEW:
            if (finder) {
                m.push_back(Key(w, L"as Icons", {VK_CONTROL, VK_SHIFT, '2'}, L"Ctrl+Shift+2"));
                m.push_back(Key(w, L"as List", {VK_CONTROL, VK_SHIFT, '5'}, L"Ctrl+Shift+5"));
                m.push_back(Key(w, L"as Details", {VK_CONTROL, VK_SHIFT, '6'}, L"Ctrl+Shift+6"));
                m.push_back({0});
                m.push_back(Key(w, L"Show Preview", {VK_MENU, 'P'}, L"Alt+P"));
                m.push_back(Key(w, L"Refresh", {VK_F5}, L"F5"));
            } else {
                m.push_back(Key(w, L"Reload", {VK_F5}, L"F5"));
                m.push_back({0});
                m.push_back(Key(w, L"Zoom In", {VK_CONTROL, VK_OEM_PLUS}, L"Ctrl+="));
                m.push_back(Key(w, L"Zoom Out", {VK_CONTROL, VK_OEM_MINUS}, L"Ctrl+-"));
                m.push_back(Key(w, L"Actual Size", {VK_CONTROL, '0'}, L"Ctrl+0"));
                m.push_back({0});
                m.push_back(Key(w, L"Enter Full Screen", {VK_F11}, L"F11"));
            }
            own(L"View", 'V');
            break;
        case B_GO:
            m.push_back(Key(w, L"Back", {VK_MENU, VK_LEFT}, L"Alt+Left"));
            m.push_back(Key(w, L"Forward", {VK_MENU, VK_RIGHT}, L"Alt+Right"));
            m.push_back(Key(w, L"Enclosing Folder", {VK_MENU, VK_UP}, L"Alt+Up"));
            m.push_back({0});
            m.push_back(Act(L"Home", [] { Launch(L"shell:Profile"); }));
            m.push_back(Act(L"Desktop", [] { Launch(L"shell:Desktop"); }));
            m.push_back(Act(L"Documents", [] { Launch(L"shell:Personal"); }));
            m.push_back(Act(L"Downloads", [] { Launch(L"shell:Downloads"); }));
            m.push_back(Act(L"Applications", [] { Launch(L"shell:AppsFolder"); }));
            m.push_back(Act(L"Computer", [] { Launch(L"shell:MyComputerFolder"); }));
            m.push_back(Act(L"Trash", [] { Launch(L"shell:RecycleBinFolder"); }));
            m.push_back({0});
            m.push_back(Key(w, L"Go to Folder...", {VK_CONTROL, 'L'}, L"Ctrl+L"));
            break;
        case B_WINDOW: {
            m.push_back(Act(L"Minimize", [w] { ShowWindow(w, SW_MINIMIZE); }, L"Win+Down"));
            m.push_back(Act(L"Zoom", [w] { ShowWindow(w, IsZoomed(w) ? SW_RESTORE : SW_MAXIMIZE); }, L"Win+Up"));
            m.push_back(Act(L"Center", [w] {
                RECT r; GetWindowRect(w, &r);
                MONITORINFO mi{sizeof(mi)};
                GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
                if (IsZoomed(w)) ShowWindow(w, SW_RESTORE);
                int ww = r.right - r.left, hh = r.bottom - r.top;
                SetWindowPos(w, nullptr, (mi.rcWork.left + mi.rcWork.right - ww) / 2, (mi.rcWork.top + mi.rcWork.bottom - hh) / 2, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }));
            m.push_back({0});
            m.push_back(Key(w, L"Tile Window to Left of Screen", {VK_LWIN, VK_LEFT}, L"Win+Left"));
            m.push_back(Key(w, L"Tile Window to Right of Screen", {VK_LWIN, VK_RIGHT}, L"Win+Right"));
            m.push_back(Key(w, L"Move to Next Display", {VK_LWIN, VK_SHIFT, VK_RIGHT}, L"Win+Shift+Right"));
            m.push_back(Act(L"Borderless Full Screen", [w] { chrome::ToggleBorderlessApp(w); }, L"", chrome::IsBorderlessApp(w)));
            m.push_back({0});
            m.push_back(Act(L"Bring All to Front", [w] {
                for (HWND h : AppWindows(w)) { if (IsIconic(h)) ShowWindow(h, SW_RESTORE); SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE); }
                Focus(w);
            }));
            auto wins = AppWindows(w);
            if (!wins.empty()) m.push_back({0});
            for (HWND h : wins) {
                wchar_t t[200] = {};
                GetWindowTextW(h, t, 200);
                std::wstring title = t[0] ? t : app;
                if (title.size() > 60) title = title.substr(0, 57) + L"...";
                m.push_back(Act(title, [h] { ActivateWindow(h); }, L"", h == w));
            }
            break;
        }
        case B_HELP:
            m.push_back(Key(w, app + L" Help", {VK_F1}, L"F1"));
            m.push_back(Act(L"Search the Web for " + app + L" Help", [app] {
                std::wstring q;
                for (wchar_t c : app + L" help") q += c == L' ' ? L'+' : c;
                Launch(L"https://www.google.com/search?q=" + q);
            }));
            m.push_back({0});
            m.push_back(Act(L"Keyboard Shortcuts...", [] { Launch(L"https://support.microsoft.com/windows/keyboard-shortcuts-in-windows-dcc61a57-8ff0-cffe-9796-cb9706c75eec"); }));
            own(L"Help", 'H');
            break;
    }
    return m;
}
}  // namespace

namespace appmenu {
std::vector<Top> TopsFor(HWND w, const std::wstring& app) {
    std::vector<Top> tops;
    HMENU hm = (w && IsWindow(w)) ? GetMenu(w) : nullptr;
    if (hm && IsMenu(hm)) {
        int n = GetMenuItemCount(hm);
        for (int i = 0; i < n && tops.size() < 12; i++) {
            wchar_t buf[128] = {};
            MENUITEMINFOW mi{sizeof(mi)};
            mi.fMask = MIIM_FTYPE | MIIM_SUBMENU | MIIM_STRING | MIIM_STATE;
            mi.dwTypeData = buf;
            mi.cch = 127;
            if (!GetMenuItemInfoW(hm, i, TRUE, &mi) || !mi.hSubMenu || (mi.fType & MFT_SEPARATOR)) continue;
            std::wstring t = Clean(buf, nullptr);
            if (!t.empty()) tops.push_back({t, mi.hSubMenu, -1});
        }
        if (!tops.empty()) {
            // macOS order: the app's own menus, then Window (if it has none) and Help last
            bool hasWindow = false;
            for (auto& t : tops) if (Lower(t.title) == L"window") hasWindow = true;
            if (!hasWindow) tops.insert(tops.end() - (Lower(tops.back().title) == L"help" ? 1 : 0), {L"Window", nullptr, B_WINDOW});
            return tops;
        }
    }
    bool finder = app == L"Finder";
    tops.push_back({L"File", nullptr, B_FILE});
    tops.push_back({L"Edit", nullptr, B_EDIT});
    tops.push_back({L"View", nullptr, B_VIEW});
    if (finder) tops.push_back({L"Go", nullptr, B_GO});
    tops.push_back({L"Window", nullptr, B_WINDOW});
    tops.push_back({L"Help", nullptr, B_HELP});
    return tops;
}

void Open(HWND w, const Top& t, const std::wstring& app, int x, int y) {
    s_actions.clear();
    std::vector<MenuEntry> m;
    if (t.sub) ReadHMenu(w, t.sub, m, 0);
    else m = Builtin(w, t.builtin, app);
    if (m.empty()) m.push_back({-2, L"(empty)", false, false});
    int pick = GlassMenu(x, y, m);
    if (pick > 0 && pick <= (int)s_actions.size()) {
        auto f = s_actions[pick - 1];
        f();
    }
    s_actions.clear();
}
}  // namespace appmenu
