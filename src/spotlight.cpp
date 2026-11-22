#include "shell.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellscalingapi.h>
#include <powrprof.h>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <map>
#include <functional>

// macOS Tahoe style Spotlight: a liquid glass search bar with four glass shortcut
// buttons beside it (Applications, Files, Actions, Clipboard) that melt into the bar,
// and a glass results list. Alt+Space or the menu-bar magnifier opens it.

namespace {
enum Mode { M_ALL = 0, M_APPS, M_FILES, M_ACTIONS, M_CLIP };
enum Kind { K_APP, K_FILE, K_ACTION, K_CLIP, K_CALC, K_WEB };

struct AppItem { std::wstring name, lower, parsing; };
struct FileItem { std::wstring name, lower, path; ULONGLONG mtime = 0; bool dir = false; };
struct Action { std::wstring name, lower; const wchar_t* glyph; std::function<void()> run; };
struct Result { Kind kind; std::wstring title, sub, key; int idx = -1; int score = 0; const wchar_t* glyph = nullptr; };
struct Row { bool header; std::wstring text; int res = -1; D2D1_RECT_F r{}; };

const float kBarW = 620, kBarH = 52, kBtn = 46, kBtnGap = 10, kRowH = 40, kHeadH = 26, kMargin = 34, kMaxRows = 9;

std::atomic<bool> s_open{false};
Overlay s_ov;
float s_s = 1, s_anim = 0, s_animV = 0, s_listH = 0, s_listHV = 0;
std::wstring s_query;
int s_mode = M_ALL, s_sel = 0, s_hover = -1, s_btnHover = -1;
bool s_dirty = false, s_selectAll = false;
HWND s_prevFg = nullptr;
HHOOK s_mouse = nullptr;
std::vector<Result> s_results;
std::vector<Row> s_rows;
std::vector<D2D1_RECT_F> s_btnRects;
D2D1_RECT_F s_bar{}, s_list{};
ComPtr<ID2D1SolidColorBrush> s_br;
ComPtr<IDWriteTextFormat> fQuery, fRow, fSub, fHead, fGlyph, fGlyphBig, fBtn;
std::map<std::wstring, ComPtr<ID2D1Bitmap1>> s_icons;

std::mutex s_mx;
std::vector<AppItem> s_apps;
std::vector<FileItem> s_files;
std::vector<std::wstring> s_clip;   // newest first
std::vector<Action> s_actions;
std::thread s_indexer;
std::atomic<bool> s_stop{false};

// ---------- indexing (background) ----------
void IndexApps() {
    std::vector<AppItem> apps;
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder)))) {
        ComPtr<IEnumShellItems> en;
        if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
            ComPtr<IShellItem> it;
            while (!s_stop && en->Next(1, &it, nullptr) == S_OK) {
                LPWSTR name = nullptr, pn = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &pn))) {
                    std::wstring n = name, l = Lower(n);
                    bool junk = l.find(L"uninstall") != std::wstring::npos || l.find(L"readme") != std::wstring::npos ||
                                l.find(L"release notes") != std::wstring::npos || l == L"help" || l.find(L"website") != std::wstring::npos;
                    if (!junk) apps.push_back({n, l, pn});
                }
                if (name) CoTaskMemFree(name);
                if (pn) CoTaskMemFree(pn);
                it.Reset();
            }
        }
    }
    std::sort(apps.begin(), apps.end(), [](const AppItem& a, const AppItem& b) { return a.lower < b.lower; });
    apps.erase(std::unique(apps.begin(), apps.end(), [](const AppItem& a, const AppItem& b) { return a.lower == b.lower; }), apps.end());
    std::lock_guard<std::mutex> l(s_mx);
    s_apps = std::move(apps);
}

void ScanDir(const std::wstring& dir, int depth, std::vector<FileItem>& out) {
    if (depth > 6 || out.size() > 40000 || s_stop) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> sub;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (n[0] == L'.' || n == L"node_modules" || n == L"__pycache__" || n == L"AppData") continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        ULONGLONG mt = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        std::wstring path = dir + L"\\" + n;
        out.push_back({n, Lower(n), path, mt, isDir});
        if (isDir) sub.push_back(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (auto& d : sub) ScanDir(d, depth + 1, out);
}

void IndexFiles() {
    std::vector<FileItem> files;
    const KNOWNFOLDERID* ids[] = {&FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads, &FOLDERID_Pictures,
                                  &FOLDERID_Videos, &FOLDERID_Music};
    for (auto id : ids) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &p))) { ScanDir(p, 0, files); CoTaskMemFree(p); }
    }
    std::lock_guard<std::mutex> l(s_mx);
    s_files = std::move(files);
}

void Indexer() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    while (!s_stop) {
        IndexApps();
        IndexFiles();
        for (int i = 0; i < 600 && !s_stop; i++) Sleep(500);   // refresh every 5 minutes
    }
    CoUninitialize();
}

void RunAdmin(const std::wstring& target);

void BuildActions() {
    auto add = [](const wchar_t* n, const wchar_t* glyph, std::function<void()> f) {
        s_actions.push_back({n, Lower(n), glyph, std::move(f)});
    };
    add(L"Lock Screen", L"\uE72E", [] { lock::Lock(); });
    add(L"Sleep", L"", [] { SetSuspendState(FALSE, FALSE, FALSE); });
    add(L"Restart", L"", [] { Launch(L"shutdown.exe", L"/r /t 0", SW_HIDE); });
    add(L"Shut Down", L"", [] { Launch(L"shutdown.exe", L"/s /t 0", SW_HIDE); });
    add(L"Sign Out", L"", [] { ExitWindowsEx(EWX_LOGOFF, 0); });
    add(L"Empty Trash", L"", [] { SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI); });
    add(L"System Settings", L"", [] { Launch(L"ms-settings:"); });
    add(L"Wi-Fi Settings", L"", [] { Launch(L"ms-settings:network-wifi"); });
    add(L"Bluetooth Settings", L"", [] { Launch(L"ms-settings:bluetooth"); });
    add(L"Display Settings", L"", [] { Launch(L"ms-settings:display"); });
    add(L"Sound Settings", L"", [] { Launch(L"ms-settings:sound"); });
    add(L"Change Wallpaper", L"", [] { Launch(L"ms-settings:personalization-background"); });
    add(L"Task Manager", L"", [] { Launch(L"taskmgr.exe"); });
    add(L"Take Screenshot", L"", [] { TapKeys({VK_LWIN, VK_SHIFT, 'S'}); });
    add(L"Show Desktop", L"", [] { TapKeys({VK_LWIN, 'D'}); });
    add(L"Mission Control", L"", [] { TapKeys({VK_LWIN, VK_TAB}); });
    add(L"Terminal (Admin)", L"", [] { RunAdmin(L"wt.exe"); });
    add(L"PowerShell (Admin)", L"", [] { RunAdmin(L"powershell.exe"); });
    add(L"Command Prompt (Admin)", L"", [] { RunAdmin(L"cmd.exe"); });
    add(L"New Terminal Window", L"", [] { Launch(L"wt.exe"); });
    add(L"Open Downloads", L"", [] { Launch(L"shell:Downloads"); });
    add(L"Mute Sound", L"", [] { sys::SetMuted(!sys::Muted()); });
    add(L"Quit Liquid", L"", [] { g_quit = true; });
}

// ---------- matching ----------
int Score(const std::wstring& lower, const std::wstring& q) {
    if (q.empty()) return 1;
    if (lower.compare(0, q.size(), q) == 0) return 1000 - (int)lower.size();
    size_t pos = lower.find(q);
    if (pos != std::wstring::npos) {
        bool wordStart = pos > 0 && !iswalnum(lower[pos - 1]);
        return (wordStart ? 800 : 500) - (int)lower.size();
    }
    // word initials ("vsc" -> Visual Studio Code)
    std::wstring ini;
    for (size_t i = 0; i < lower.size(); i++) if (iswalnum(lower[i]) && (i == 0 || !iswalnum(lower[i - 1]))) ini += lower[i];
    return (q.size() >= 2 && ini.compare(0, q.size(), q) == 0) ? 300 - (int)lower.size() : 0;
}

// tiny calculator: + - * / ^ % and parentheses
struct Calc {
    const wchar_t* p;
    bool ok = true;
    void ws() { while (*p == L' ') p++; }
    double num() {
        ws();
        if (*p == L'(') { p++; double v = expr(); ws(); if (*p == L')') p++; else ok = false; return v; }
        if (*p == L'-') { p++; return -num(); }
        wchar_t* e = nullptr;
        double v = wcstod(p, &e);
        if (e == p) { ok = false; return 0; }
        p = e;
        return v;
    }
    double pw() { double b = num(); ws(); if (*p == L'^') { p++; return std::pow(b, pw()); } return b; }
    double term() {
        double v = pw();
        for (;;) {
            ws();
            if (*p == L'*' || *p == L'x') { p++; v *= pw(); }
            else if (*p == L'/') { p++; v /= pw(); }
            else if (*p == L'%') { p++; v = std::fmod(v, pw()); }
            else return v;
        }
    }
    double expr() {
        double v = term();
        for (;;) {
            ws();
            if (*p == L'+') { p++; v += term(); }
            else if (*p == L'-') { p++; v -= term(); }
            else return v;
        }
    }
};
bool Evaluate(const std::wstring& q, std::wstring& out) {
    bool digit = false, op = false;
    for (wchar_t c : q) {
        if (iswdigit(c)) digit = true;
        else if (wcschr(L"+-*/^%x()", c)) op = true;
        else if (c != L'.' && c != L' ') return false;
    }
    if (!digit || !op) return false;
    Calc c{q.c_str()};
    double v = c.expr();
    c.ws();
    if (!c.ok || *c.p || !std::isfinite(v)) return false;
    wchar_t b[64];
    swprintf_s(b, L"%.10g", v);
    out = b;
    return true;
}

void Search() {
    s_results.clear();
    std::wstring q = Lower(s_query);
    while (!q.empty() && q.back() == L' ') q.pop_back();
    std::lock_guard<std::mutex> l(s_mx);
    std::vector<Result> apps, files, acts, clip;
    if (s_mode == M_ALL || s_mode == M_APPS) {
        for (size_t i = 0; i < s_apps.size(); i++) {
            int sc = Score(s_apps[i].lower, q);
            if (sc > 0 && (!q.empty() || s_mode == M_APPS)) apps.push_back({K_APP, s_apps[i].name, L"Application", L"shell:AppsFolder\\" + s_apps[i].parsing, (int)i, sc});
        }
    }
    if ((s_mode == M_ALL && q.size() >= 2) || s_mode == M_FILES) {
        for (size_t i = 0; i < s_files.size(); i++) {
            int sc = q.empty() ? 1 : Score(s_files[i].lower, q);
            if (sc >= (q.empty() ? 1 : 400)) {
                auto& f = s_files[i];
                std::wstring folder = f.path.substr(0, f.path.find_last_of(L'\\'));
                size_t cut = folder.find_last_of(L'\\');
                Result r{K_FILE, f.name, cut == std::wstring::npos ? folder : folder.substr(cut + 1), f.path, (int)i, sc};
                r.glyph = f.dir ? L"" : nullptr;
                files.push_back(r);
            }
        }
        if (q.empty()) std::sort(files.begin(), files.end(), [](const Result& a, const Result& b) { return s_files[a.idx].mtime > s_files[b.idx].mtime; });
    }
    if (s_mode == M_ALL || s_mode == M_ACTIONS) {
        for (size_t i = 0; i < s_actions.size(); i++) {
            int sc = Score(s_actions[i].lower, q);
            if (sc > 0 && (!q.empty() || s_mode == M_ACTIONS)) acts.push_back({K_ACTION, s_actions[i].name, L"Action", L"", (int)i, sc, s_actions[i].glyph});
        }
    }
    if (s_mode == M_CLIP) {
        for (size_t i = 0; i < s_clip.size(); i++) {
            std::wstring one = s_clip[i];
            for (auto& c : one) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
            int sc = q.empty() ? 1 : (Lower(one).find(q) != std::wstring::npos ? 500 : 0);
            if (sc) clip.push_back({K_CLIP, one, L"Clipboard", L"", (int)i, sc, L""});
        }
    }
    auto bySc = [](const Result& a, const Result& b) { return a.score > b.score; };
    if (!q.empty()) {
        std::stable_sort(apps.begin(), apps.end(), bySc);
        std::stable_sort(files.begin(), files.end(), bySc);
        std::stable_sort(acts.begin(), acts.end(), bySc);
    }
    std::wstring calc;
    if (s_mode == M_ALL && Evaluate(s_query, calc)) s_results.push_back({K_CALC, calc, s_query + L" =", calc, -1, 5000, L""});
    size_t cap = s_mode == M_ALL ? 4 : 60;
    for (size_t i = 0; i < apps.size() && i < cap; i++) s_results.push_back(apps[i]);
    for (size_t i = 0; i < acts.size() && i < (s_mode == M_ALL ? 3 : 60); i++) s_results.push_back(acts[i]);
    for (size_t i = 0; i < files.size() && i < (s_mode == M_ALL ? 5 : 60); i++) s_results.push_back(files[i]);
    for (size_t i = 0; i < clip.size() && i < 60; i++) s_results.push_back(clip[i]);
    if (s_mode == M_ALL && !q.empty())
        s_results.push_back({K_WEB, L"Search the web for “" + s_query + L"”", L"Web", s_query, -1, 0, L""});
    s_sel = s_results.empty() ? -1 : 0;
    s_dirty = true;
}

std::wstring UrlEncode(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), u.data(), n, nullptr, nullptr);
    std::wstring o;
    wchar_t b[8];
    for (unsigned char c : u) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (wchar_t)c;
        else if (c == ' ') o += L'+';
        else { swprintf_s(b, L"%%%02X", c); o += b; }
    }
    return o;
}

void SetClipboardText(const std::wstring& t) {
    if (!OpenClipboard(s_ov.hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (h) {
        memcpy(GlobalLock(h), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

std::wstring ClipboardText() {
    std::wstring r;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr)) return r;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) { auto* p = (const wchar_t*)GlobalLock(h); if (p) r = p; GlobalUnlock(h); }
    CloseClipboard();
    return r;
}

void Close();

// "Run as administrator" for an app or file (Windows asks with its UAC prompt)
void RunAdmin(const std::wstring& target) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    SHELLEXECUTEINFOW ei{sizeof(ei)};
    ei.lpVerb = L"runas";
    ei.nShow = SW_SHOWNORMAL;
    std::wstring home = HomeDir();
    ei.lpDirectory = home.c_str();
    if (SUCCEEDED(SHParseDisplayName(target.c_str(), nullptr, &pidl, 0, nullptr))) {
        ei.fMask = SEE_MASK_INVOKEIDLIST;
        ei.lpIDList = pidl;
    } else ei.lpFile = target.c_str();
    if (!ShellExecuteExW(&ei) && GetLastError() != ERROR_CANCELLED) {
        // apps from the Store don't have a "run as admin" verb; just start them normally
        Launch(target);
    }
    if (pidl) CoTaskMemFree(pidl);
}

void Run(int i, bool admin = false) {
    if (i < 0 || i >= (int)s_results.size()) return;
    Result r = s_results[i];
    HWND back = s_prevFg;
    Close();
    switch (r.kind) {
        case K_APP:
        case K_FILE: if (admin) RunAdmin(r.key); else Launch(r.key); break;
        case K_ACTION: if (r.idx >= 0 && r.idx < (int)s_actions.size()) s_actions[r.idx].run(); break;
        case K_CALC: SetClipboardText(r.key); break;
        case K_WEB: Launch(L"https://www.google.com/search?q=" + UrlEncode(r.key)); break;
        case K_CLIP: {
            std::wstring t;
            { std::lock_guard<std::mutex> l(s_mx); if (r.idx < (int)s_clip.size()) t = s_clip[r.idx]; }
            SetClipboardText(t);
            if (back) { SetForegroundWindow(back); Sleep(60); TapKeys({VK_CONTROL, 'V'}); }   // paste where you were
            break;
        }
    }
}

ComPtr<ID2D1Bitmap1> IconFor(const Result& r) {
    if (r.kind != K_APP && r.kind != K_FILE) return nullptr;
    if (r.glyph) return nullptr;
    auto it = s_icons.find(r.key);
    if (it != s_icons.end()) return it->second;
    auto ic = LoadIcons(s_ov.dc.Get(), r.key, (int)(32 * s_s));
    s_icons[r.key] = ic.color;
    if (s_icons.size() > 400) s_icons.clear();
    return ic.color;
}

LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && s_open && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN)) {
        auto* m = (MSLLHOOKSTRUCT*)lp;
        float x = (float)(m->pt.x - s_ov.x), y = (float)(m->pt.y - s_ov.y);
        auto in = [&](D2D1_RECT_F r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; };
        bool hit = in(s_bar) || (s_listH > 2 && in(s_list));
        for (auto& b : s_btnRects) if (in(b)) hit = true;
        if (!hit) Close();
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_CLIPBOARDUPDATE: {
            std::wstring t = ClipboardText();
            if (!t.empty() && t.size() < 20000) {
                std::lock_guard<std::mutex> l(s_mx);
                s_clip.erase(std::remove(s_clip.begin(), s_clip.end(), t), s_clip.end());
                s_clip.insert(s_clip.begin(), t);
                if (s_clip.size() > 40) s_clip.resize(40);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (s_btnHover >= 0) {
                int m = s_btnHover + 1;
                s_mode = s_mode == m ? M_ALL : m;
                Search();
            } else if (s_hover >= 0) Run(s_hover);
            return 0;
        }
        case WM_RBUTTONUP: {   // right-click a result: Open / Run as administrator / Show in folder / Copy path
            if (s_hover < 0 || s_hover >= (int)s_results.size()) return 0;
            int i = s_hover;
            Result r = s_results[i];
            std::vector<MenuEntry> m;
            if (r.kind == K_APP || r.kind == K_FILE) {
                m.push_back({1, L"Open", false, true, L"Enter"});
                m.push_back({2, L"Run as Administrator", false, true, L"Ctrl+Shift+Enter"});
                if (r.kind == K_FILE) { m.push_back({0}); m.push_back({3, L"Show in Folder"}); m.push_back({4, L"Copy Path"}); }
                m.push_back({0});
                m.push_back({5, L"Add to Desktop"});
            } else m.push_back({1, L"Open", false, true, L"Enter"});
            POINT pt;
            GetCursorPos(&pt);
            int c = GlassMenu(pt.x, pt.y, m);
            if (c == 1) Run(i);
            else if (c == 2) Run(i, true);
            else if (c == 3) { Close(); Launch(L"explorer.exe", L"/select,\"" + r.key + L"\""); }
            else if (c == 4) { SetClipboardText(r.key); Close(); }
            else if (c == 5) { deskicons::Add(r.title, r.key); Close(); }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int n = (int)s_results.size();
            if (n) { s_sel = std::clamp(s_sel - GET_WHEEL_DELTA_WPARAM(wp) / 120, 0, n - 1); s_dirty = true; }
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Layout() {
    s_rows.clear();
    float s = s_s, m = kMargin * s;
    float totalW = (kBarW + 4 * (kBtn + kBtnGap)) * s;
    float left = m + ((s_ov.w - 2 * m) - totalW) / 2;
    s_bar = D2D1::RectF(left, m, left + kBarW * s, m + kBarH * s);
    s_btnRects.clear();
    float bx = s_bar.right + kBtnGap * s, by = s_bar.top + (kBarH - kBtn) / 2 * s;
    for (int i = 0; i < 4; i++) {
        s_btnRects.push_back(D2D1::RectF(bx, by, bx + kBtn * s, by + kBtn * s));
        bx += (kBtn + kBtnGap) * s;
    }
    // rows with section headers
    float y = 8 * s;
    int lastKind = -1, count = 0;
    int first = std::max(0, std::min(s_sel - (int)kMaxRows + 2, (int)s_results.size() - (int)kMaxRows));
    if (s_sel < (int)kMaxRows - 1) first = 0;
    for (int i = first; i < (int)s_results.size() && count < (int)kMaxRows; i++, count++) {
        auto& r = s_results[i];
        int kg = r.kind == K_WEB ? K_WEB : r.kind;
        if (kg != lastKind && s_mode == M_ALL) {
            const wchar_t* names[] = {L"Applications", L"Files", L"Actions", L"Clipboard", L"Calculator", L"Web"};
            s_rows.push_back({true, names[kg], -1, D2D1::RectF(0, y, 0, y + kHeadH * s)});
            y += kHeadH * s;
            lastKind = kg;
        }
        s_rows.push_back({false, L"", i, D2D1::RectF(0, y, 0, y + kRowH * s)});
        y += kRowH * s;
    }
    float listH = s_results.empty() ? 0 : y + 8 * s;
    if (!s_query.empty() && s_results.empty()) listH = 54 * s;
    s_list = D2D1::RectF(s_bar.left, s_bar.bottom + 10 * s, s_bar.right, s_bar.bottom + 10 * s + s_listH);
    for (auto& rw : s_rows) { rw.r.left = s_list.left + 8 * s; rw.r.right = s_list.right - 8 * s; rw.r.top += s_list.top; rw.r.bottom += s_list.top; }
    (void)listH;
}

float ListTarget() {
    float s = s_s;
    if (s_results.empty()) return (!s_query.empty()) ? 54 * s : 0;
    float y = 8 * s;
    int lastKind = -1, count = 0;
    int first = std::max(0, std::min(s_sel - (int)kMaxRows + 2, (int)s_results.size() - (int)kMaxRows));
    if (s_sel < (int)kMaxRows - 1) first = 0;
    for (int i = first; i < (int)s_results.size() && count < (int)kMaxRows; i++, count++) {
        int kg = s_results[i].kind;
        if (kg != lastKind && s_mode == M_ALL) { y += kHeadH * s; lastKind = kg; }
        y += kRowH * s;
    }
    return y + 8 * s;
}

void Render() {
    Layout();
    float s = s_s, a = std::clamp(s_anim, 0.f, 1.f);
    RECT behind{s_ov.x + (int)s_bar.left, s_ov.y + (int)s_bar.top, s_ov.x + (int)s_bar.right, s_ov.y + (int)s_bar.bottom + 200};
    bool light = s_ov.cap ? s_ov.cap->Luma(behind) > 0.62f : false;
    D2D1_COLOR_F fg = light ? D2D1::ColorF(0, 0, 0, 0.88f * a) : D2D1::ColorF(1, 1, 1, 0.95f * a);
    D2D1_COLOR_F fg2 = light ? D2D1::ColorF(0, 0, 0, 0.45f * a) : D2D1::ColorF(1, 1, 1, 0.5f * a);
    D2D1_COLOR_F accent = D2D1::ColorF(0.0f, 0.47f, 1.0f, 0.92f * a);

    std::vector<GlassShape> sh;
    float pop = 0.94f + 0.06f * a;   // a little scale-in
    auto shape = [&](D2D1_RECT_F r, float radius, int group, float opacity) {
        float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, w = (r.right - r.left) * pop, h = (r.bottom - r.top) * pop;
        GlassShape gs{cx - w / 2, cy - h / 2, w, h};
        gs.radius = radius * pop; gs.blur = 2.4f; gs.bezel = std::min(h * 0.45f, 18 * s); gs.thick = 2.2f;
        gs.tint = light ? 0.38f : -0.34f; gs.shadow = 0.2f; gs.highlight = 0.0f; gs.opacity = opacity;
        gs.group = group; gs.melt = 14 * s;
        sh.push_back(gs);
    };
    shape(s_bar, kBarH / 2 * s, 1, a);
    // shortcut droplets pop out of the bar one after another
    for (int i = 0; i < 4; i++) {
        float t = std::clamp(a * 1.6f - i * 0.15f, 0.f, 1.f);
        D2D1_RECT_F b = s_btnRects[i];
        float slide = (1 - t) * (b.left - s_bar.right + kBtn * s);
        b.left -= slide; b.right -= slide;
        s_btnRects[i] = b;
        shape(b, kBtn / 2 * s, 1, t * a);
    }
    if (s_listH > 2) shape(D2D1::RectF(s_list.left, s_list.top, s_list.right, s_list.top + s_listH), 18 * s, 0, a);
    float tintLight[3] = {0.97f, 0.97f, 0.98f}, tintDark[3] = {0.13f, 0.13f, 0.15f};

    s_ov.Render(s, sh, light ? tintLight : tintDark, [&](ID2D1DeviceContext* dc) {
        auto text = [&](IDWriteTextFormat* f, const std::wstring& t, D2D1_RECT_F r, D2D1_COLOR_F c) {
            s_br->SetColor(c);
            dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, s_br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        // search field
        D2D1_RECT_F mg = D2D1::RectF(s_bar.left + 12 * s, s_bar.top, s_bar.left + 46 * s, s_bar.bottom);
        text(fGlyphBig.Get(), L"", mg, fg2);
        D2D1_RECT_F tr = D2D1::RectF(mg.right + 4 * s, s_bar.top, s_bar.right - 20 * s, s_bar.bottom);
        const wchar_t* modeNames[] = {L"Spotlight Search", L"Search Applications", L"Search Files", L"Search Actions", L"Search Clipboard"};
        if (s_query.empty()) text(fQuery.Get(), modeNames[s_mode], tr, fg2);
        else {
            if (s_selectAll) {
                float w = TextWidth(fQuery.Get(), s_query);
                s_br->SetColor(D2D1::ColorF(0.0f, 0.47f, 1.0f, 0.45f * a));
                dc->FillRectangle(D2D1::RectF(tr.left, s_bar.top + 14 * s, tr.left + w, s_bar.bottom - 14 * s), s_br.Get());
            }
            text(fQuery.Get(), s_query, tr, fg);
        }
        // caret
        if ((GetTickCount64() / 530) % 2 == 0 && a > 0.5f) {
            float cx = tr.left + (s_query.empty() ? 0 : TextWidth(fQuery.Get(), s_query)) + 1 * s;
            s_br->SetColor(D2D1::ColorF(0.0f, 0.47f, 1.0f, a));
            dc->FillRectangle(D2D1::RectF(cx, s_bar.top + 15 * s, cx + 2 * s, s_bar.bottom - 15 * s), s_br.Get());
        }
        // shortcut buttons
        const wchar_t* bg[4] = {L"", L"", L"", L""};
        for (int i = 0; i < 4; i++) {
            auto b = s_btnRects[i];
            bool on = s_mode == i + 1, hot = s_btnHover == i;
            if (on || hot) {
                s_br->SetColor(on ? accent : (light ? D2D1::ColorF(0, 0, 0, 0.08f * a) : D2D1::ColorF(1, 1, 1, 0.12f * a)));
                float r = (b.right - b.left) / 2 - 3 * s;
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((b.left + b.right) / 2, (b.top + b.bottom) / 2), r, r), s_br.Get());
            }
            text(fBtn.Get(), bg[i], b, on ? D2D1::ColorF(1, 1, 1, a) : fg);
        }
        if (s_listH <= 2) return;
        D2D1_RECT_F clip = D2D1::RectF(s_list.left, s_list.top, s_list.right, s_list.top + s_listH);
        dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (s_results.empty())
            text(fRow.Get(), L"No Results", D2D1::RectF(s_list.left + 22 * s, s_list.top, s_list.right, s_list.top + 54 * s), fg2);
        for (auto& rw : s_rows) {
            if (rw.header) {
                text(fHead.Get(), rw.text, D2D1::RectF(rw.r.left + 12 * s, rw.r.top + 4 * s, rw.r.right, rw.r.bottom), fg2);
                continue;
            }
            auto& r = s_results[rw.res];
            bool sel = rw.res == s_sel;
            if (sel) {
                s_br->SetColor(accent);
                dc->FillRoundedRectangle(D2D1::RoundedRect(rw.r, 10 * s, 10 * s), s_br.Get());
            } else if (rw.res == s_hover) {
                s_br->SetColor(light ? D2D1::ColorF(0, 0, 0, 0.06f * a) : D2D1::ColorF(1, 1, 1, 0.07f * a));
                dc->FillRoundedRectangle(D2D1::RoundedRect(rw.r, 10 * s, 10 * s), s_br.Get());
            }
            D2D1_COLOR_F c1 = sel ? D2D1::ColorF(1, 1, 1, a) : fg, c2 = sel ? D2D1::ColorF(1, 1, 1, 0.75f * a) : fg2;
            D2D1_RECT_F ic = D2D1::RectF(rw.r.left + 10 * s, rw.r.top + 6 * s, rw.r.left + 38 * s, rw.r.bottom - 6 * s);
            auto bmp = IconFor(r);
            if (bmp) dc->DrawBitmap(bmp.Get(), &ic, a, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            else {
                const wchar_t* gl = r.glyph ? r.glyph : L"";
                text(fGlyph.Get(), gl, ic, c1);
            }
            std::wstring sub = (sel && r.kind == K_APP) ? L"Ctrl+Shift+Enter: Admin" : r.sub;
            float subW = TextWidth(fSub.Get(), sub) + 8 * s;
            subW = std::min(subW, 200 * s);
            text(fRow.Get(), r.title, D2D1::RectF(ic.right + 12 * s, rw.r.top, rw.r.right - subW - 16 * s, rw.r.bottom), c1);
            fSub->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            text(fSub.Get(), sub, D2D1::RectF(rw.r.right - subW - 12 * s, rw.r.top, rw.r.right - 12 * s, rw.r.bottom), c2);
            fSub->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        dc->PopAxisAlignedClip();
    });
}

void Open() {
    if (!s_ov.hwnd) return;
    if (nc::IsOpen()) nc::Close();
    if (cc.open) cc.Close();
    POINT p;
    GetCursorPos(&p);
    HMONITOR hm = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    int x = mi.rcMonitor.left + ((mi.rcMonitor.right - mi.rcMonitor.left) - s_ov.w) / 2;
    int y = mi.rcMonitor.top + (int)((mi.rcMonitor.bottom - mi.rcMonitor.top) * 0.2f) - (int)(kMargin * s_s);
    s_ov.Move(x, y);
    s_prevFg = GetForegroundWindow();
    s_query.clear();
    s_mode = M_ALL;
    s_selectAll = false;
    s_listH = 0; s_listHV = 0;
    Search();
    s_ov.SetVisible(true);
    s_open = true;
    s_mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
    s_dirty = true;
}

void Close() {
    if (!s_open) return;
    s_open = false;
    if (s_mouse) UnhookWindowsHookEx(s_mouse);
    s_mouse = nullptr;
    s_dirty = true;
}
}  // namespace

namespace spotlight {
bool Init() {
    HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    s_s = dx / 96.f;
    float s = s_s;
    int w = (int)std::ceil((kBarW + 4 * (kBtn + kBtnGap) + kMargin * 2) * s);
    int h = (int)std::ceil((kBarH + 10 + 16 + kMaxRows * (kRowH + kHeadH) + kMargin * 2) * s);
    if (!s_ov.Create(L"LiquidSpotlight", Proc, -w - 100, 0, w, h, nullptr)) return false;
    s_ov.SetVisible(false);
    s_ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &s_br);
    AddClipboardFormatListener(s_ov.hwnd);
    const wchar_t* ui = L"Segoe UI Variable Text";
    fQuery = MakeFont(L"Segoe UI Variable Display", 21 * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fRow = MakeFont(ui, 14 * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fSub = MakeFont(ui, 12.5f * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fHead = MakeFont(ui, 11.5f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    fGlyph = MakeFont(L"Segoe Fluent Icons", 17 * s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fGlyphBig = MakeFont(L"Segoe Fluent Icons", 19 * s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fBtn = MakeFont(L"Segoe Fluent Icons", 17 * s, DWRITE_FONT_WEIGHT_NORMAL, true);
    BuildActions();
    s_indexer = std::thread(Indexer);
    std::wstring now = ClipboardText();
    if (!now.empty()) s_clip.push_back(now);
    return true;
}

bool IsOpen() { return s_open; }

bool AppsReady() { std::lock_guard<std::mutex> l(s_mx); return !s_apps.empty(); }

// best installed app for a name ("Spotify", "FL Studio"...): shell:AppsFolder\<id>, or empty
std::wstring FindApp(const std::wstring& name, std::wstring* display) {
    std::lock_guard<std::mutex> l(s_mx);
    std::wstring q = Lower(name);
    int best = 0;
    const AppItem* hit = nullptr;
    for (auto& a : s_apps) {
        int sc = Score(a.lower, q);
        if (sc > best && sc >= 500) { best = sc; hit = &a; }
    }
    if (!hit) return L"";
    if (display) *display = hit->name;
    return L"shell:AppsFolder\\" + hit->parsing;
}

void Toggle() { if (s_open) Close(); else Open(); }

void Key(int vk, wchar_t ch) {
    if (!s_open) return;
    bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
    int n = (int)s_results.size();
    switch (vk) {
        case VK_ESCAPE:
            if (!s_query.empty() || s_mode != M_ALL) { s_query.clear(); s_mode = M_ALL; Search(); }
            else Close();
            return;
        case VK_RETURN: Run(s_sel, ctrl && GetAsyncKeyState(VK_SHIFT) < 0); return;   // Ctrl+Shift+Enter = as admin
        case VK_DOWN: if (n) { s_sel = (s_sel + 1) % n; s_dirty = true; } return;
        case VK_UP: if (n) { s_sel = (s_sel - 1 + n) % n; s_dirty = true; } return;
        case VK_BACK:
            if (s_selectAll) { s_query.clear(); s_selectAll = false; }
            else if (ctrl) {
                while (!s_query.empty() && s_query.back() == L' ') s_query.pop_back();
                while (!s_query.empty() && s_query.back() != L' ') s_query.pop_back();
            } else if (!s_query.empty()) s_query.pop_back();
            else if (s_mode != M_ALL) s_mode = M_ALL;
            Search();
            return;
        case VK_TAB: if (n) { s_sel = (s_sel + 1) % n; s_dirty = true; } return;
    }
    if (ctrl) {
        if (vk >= '1' && vk <= '4') { int m = vk - '0'; s_mode = s_mode == m ? M_ALL : m; Search(); }
        else if (vk == 'A') { s_selectAll = !s_query.empty(); s_dirty = true; }
        else if (vk == 'V') {
            std::wstring t = ClipboardText();
            for (auto& c : t) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
            if (s_selectAll) { s_query.clear(); s_selectAll = false; }
            s_query += t.substr(0, 200);
            Search();
        } else if (vk == 'C' && s_sel >= 0 && s_sel < n) {
            SetClipboardText(s_results[s_sel].kind == K_FILE || s_results[s_sel].kind == K_CALC ? s_results[s_sel].key : s_results[s_sel].title);
        }
        return;
    }
    if (ch >= 32) {
        if (s_selectAll) { s_query.clear(); s_selectAll = false; }
        if (s_query.size() < 200) s_query += ch;
        Search();
    }
}

void Tick(bool nf) {
    if (!s_ov.hwnd) return;
    float pa = s_anim, ph = s_listH;
    Spring(s_anim, s_animV, s_open ? 1.f : 0.f, 1 / 60.f, 620.f);
    if (!s_open && s_anim < 0.003f) {
        s_anim = 0;
        if (s_ov.visible) s_ov.SetVisible(false);
        return;
    }
    Spring(s_listH, s_listHV, s_open ? ListTarget() : 0.f, 1 / 60.f, 700.f);
    if (s_listH < 0) s_listH = 0;
    POINT p;
    GetCursorPos(&p);
    float x = (float)(p.x - s_ov.x), y = (float)(p.y - s_ov.y);
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; };
    int hv = -1, bh = -1;
    bool over = in(s_bar) || (s_listH > 2 && in(s_list));
    for (int i = 0; i < (int)s_btnRects.size(); i++) if (in(s_btnRects[i])) { bh = i; over = true; }
    if (s_listH > 2) for (auto& rw : s_rows) if (!rw.header && in(rw.r)) hv = rw.res;
    s_ov.SetClickThrough(!over || !s_open);
    if (hv != s_hover || bh != s_btnHover) { s_hover = hv; s_btnHover = bh; s_dirty = true; }
    static ULONGLONG lastBlink = 0;
    ULONGLONG t = GetTickCount64() / 530;
    bool blink = t != lastBlink;
    lastBlink = t;
    if (s_dirty || nf || blink || pa != s_anim || ph != s_listH || g.snapPending) { s_dirty = false; Render(); }
}

void Shutdown() {
    s_stop = true;
    if (s_ov.hwnd) RemoveClipboardFormatListener(s_ov.hwnd);
    if (s_indexer.joinable()) s_indexer.detach();   // don't wait for a file scan to finish; the process is ending
    if (s_ov.hwnd) s_ov.Destroy();
}
}  // namespace spotlight
