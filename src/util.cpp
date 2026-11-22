#include "shell.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <exdisp.h>
#include <shldisp.h>
#include <dwmapi.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <cmath>
#include <map>
#include <algorithm>

Config cfg;
DWORD g_selfPid = GetCurrentProcessId();

std::wstring ExeDir() {
    wchar_t b[MAX_PATH];
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    PathRemoveFileSpecW(b);
    return b;
}

void Log(const wchar_t* fmt, ...) {
    static FILE* f = nullptr;
    if (!f) {
        std::wstring p = ExeDir() + L"\\liquid.log";
        // keep the last run's log (so a crash can still be read after Liquid comes back)
        MoveFileExW(p.c_str(), (ExeDir() + L"\\liquid.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
        f = _wfsopen(p.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);   // readable while Liquid runs
        if (!f) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(f, L"[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list a;
    va_start(a, fmt);
    vfwprintf(f, fmt, a);
    va_end(a);
    fputwc(L'\n', f);
    fflush(f);
}

// Start a program as if double-clicked on the desktop: Explorer becomes its parent, so it is not
// tied to whatever started Liquid (Task Scheduler or a terminal inside another app closes
// everything it started when it finishes or updates). False if Explorer's desktop isn't up yet.
bool LaunchViaExplorer(const std::wstring& exe, const std::wstring& args) {
    ComPtr<IShellWindows> sw;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sw)))) return false;
    VARIANT loc{}, empty{};
    loc.vt = VT_I4; loc.lVal = CSIDL_DESKTOP;
    long hwnd = 0;
    ComPtr<IDispatch> disp;
    if (sw->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) != S_OK || !disp) return false;
    ComPtr<IServiceProvider> sp;
    ComPtr<IShellBrowser> sb;
    ComPtr<IShellView> sv;
    ComPtr<IDispatch> bg, app;
    ComPtr<IShellFolderViewDual> fvd;
    ComPtr<IShellDispatch2> sd;
    if (FAILED(disp.As(&sp)) || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) ||
        FAILED(sb->QueryActiveShellView(&sv)) || FAILED(sv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&bg))) ||
        FAILED(bg.As(&fvd)) || FAILED(fvd->get_Application(&app)) || FAILED(app.As(&sd)))
        return false;
    BSTR file = SysAllocString(exe.c_str());
    VARIANT vArgs{}, vDir{}, vOp{}, vShow{};
    vArgs.vt = VT_BSTR; vArgs.bstrVal = SysAllocString(args.c_str());
    vShow.vt = VT_I4; vShow.lVal = SW_SHOWNORMAL;
#pragma push_macro("ShellExecute")
#undef ShellExecute
    HRESULT hr = sd->ShellExecute(file, vArgs, vDir, vOp, vShow);
#pragma pop_macro("ShellExecute")
    SysFreeString(file);
    VariantClear(&vArgs);
    return SUCCEEDED(hr);
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::wstring FileStem(const std::wstring& path) {
    size_t a = path.find_last_of(L"\\/");
    std::wstring n = a == std::wstring::npos ? path : path.substr(a + 1);
    size_t d = n.find_last_of(L'.');
    if (d != std::wstring::npos && d > 0) n = n.substr(0, d);
    return n;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

float Spring(float& x, float& v, float target, float dt, float k) {
    float c = 2 * std::sqrt(k);
    v += (k * (target - x) - c * v) * dt;
    x += v * dt;
    if (std::fabs(target - x) < 0.0005f && std::fabs(v) < 0.01f) { x = target; v = 0; }
    return x;
}

// ---------- config ----------
static std::wstring ConfigPath() { return ExeDir() + L"\\config.ini"; }

static std::wstring ReadUtf8(const std::wstring& path) {
    HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return L"";
    DWORD size = GetFileSize(fh, nullptr), got = 0;
    std::string bytes(size, '\0');
    ReadFile(fh, bytes.data(), size, &got, nullptr);
    CloseHandle(fh);
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF) bytes = bytes.substr(3);
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), text.data(), n);
    return text;
}

static std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    return lines;
}

void LoadConfig() {
    for (auto& raw : SplitLines(ReadUtf8(ConfigPath()))) {
        std::wstring line = Trim(raw);
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = Lower(Trim(line.substr(0, eq))), v = Trim(line.substr(eq + 1));
        std::wstring lv = Lower(v);
        bool on = lv == L"on" || lv == L"true" || lv == L"yes" || lv == L"1";
        if (k == L"logo") cfg.logo = v;
        else if (k == L"menubar") cfg.menubarGlass = lv != L"clear";
        else if (k == L"menubar_height") cfg.menubarHeight = std::clamp((float)_wtof(v.c_str()), 18.f, 40.f);
        else if (k == L"icon_size") cfg.iconSize = std::clamp((float)_wtof(v.c_str()), 32.f, 96.f);
        else if (k == L"magnify") cfg.magnify = std::clamp((float)_wtof(v.c_str()), 1.f, 2.2f);
        else if (k == L"finder") cfg.finder = on;
        else if (k == L"trash") cfg.trash = on;
        else if (k == L"icon_style") cfg.iconStyle = lv;
        else if (k == L"autohide") cfg.autohide = on;
        else if (k == L"dock_overlap") cfg.dockOverlap = on;
        else if (k == L"recordable") cfg.recordable = on;
        else if (k == L"gestures") cfg.gestures = on;
        else if (k == L"gestures_everywhere") cfg.gesturesEverywhere = on;
        else if (k == L"desktop_menu") cfg.desktopMenu = on;
        else if (k == L"volume_hud") cfg.volumeHud = on;
        else if (k == L"spotlight") cfg.spotlight = on;
        else if (k == L"windows_key_spotlight") cfg.winKeySpotlight = on;
        else if (k == L"lock_screen") cfg.lockScreen = on;
        else if (k == L"mac_notifications") cfg.macNotifications = on;
        else if (k == L"island_notifications") cfg.islandNotifications = on;
        else if (k == L"air_ring") cfg.airRing = on;
        else if (k == L"traffic_lights") cfg.trafficLights = on;
        else if (k == L"hide_caption_buttons") cfg.hideCaptionButtons = on;
        else if (k == L"frost_windows") cfg.frostWindows = on;
        else if (k == L"round_corners") cfg.roundCorners = on;
        else if (k == L"dynamic_island") cfg.dynamicIsland = on;
        else if (k == L"widgets") cfg.widgets = on;
        else if (k == L"app_switcher") cfg.appSwitcher = on;
        else if (k == L"window_animations") cfg.windowAnimations = on;
        else if (k == L"desktop_icons") cfg.desktopIcons = on;
        else if (k == L"borderless_apps") cfg.borderlessApps = v;
        else if (k == L"lock_on_start") cfg.lockOnStart = on;
        else if (k == L"lock_on_wake") cfg.lockOnWake = on;
        else if (k == L"away_pause_seconds") cfg.awayPauseSeconds = std::clamp((float)_wtof(v.c_str()), 0.f, 3600.f);
        else if (k == L"away_lock_seconds") cfg.awayLockSeconds = std::clamp((float)_wtof(v.c_str()), 0.f, 3600.f);
        else if (k == L"lock_idle_minutes") cfg.lockIdleMinutes = std::clamp((float)_wtof(v.c_str()), 0.f, 240.f);
        else if (k == L"lock_pin_hash") cfg.lockPinHash = lv;
        else if (k == L"lock_pin_salt") cfg.lockPinSalt = lv;
        else if (k == L"lock_wallpaper") cfg.lockWallpaper = v;
        else if (k == L"fullscreen_spaces") cfg.fullscreenSpaces = on;
        else if (k == L"spaces_mode") cfg.spacesMode = lv;
        else if (k == L"pin" && !v.empty()) cfg.pins.push_back(v);
    }
    if (cfg.pins.empty()) {
        wchar_t appdata[MAX_PATH];
        GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        std::wstring dir = std::wstring(appdata) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar\\";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"*.lnk").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = Lower(fd.cFileName);
                if (cfg.finder && n.find(L"file explorer") != std::wstring::npos) continue;
                cfg.pins.push_back(dir + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(cfg.pins.begin(), cfg.pins.end());
    }
}

void SetConfigValue(const std::wstring& key, const std::wstring& value) {
    auto lines = SplitLines(ReadUtf8(ConfigPath()));
    bool done = false;
    for (auto& l : lines) {
        std::wstring t = Trim(l);
        size_t eq = t.find(L'=');
        if (t.empty() || t[0] == L'#' || eq == std::wstring::npos) continue;
        if (Lower(Trim(t.substr(0, eq))) == key) { l = key + L"=" + value; done = true; break; }
    }
    if (!done) lines.push_back(key + L"=" + value);
    std::wstring text;
    for (size_t i = 0; i < lines.size(); i++) { text += lines[i]; if (i + 1 < lines.size()) text += L"\r\n"; }
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string bytes(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), bytes.data(), n, nullptr, nullptr);
    HANDLE fh = CreateFileW(ConfigPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(fh, bytes.data(), (DWORD)bytes.size(), &wr, nullptr);
    CloseHandle(fh);
}

// ---------- autostart ----------
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool IsAutostart() {
    wchar_t b[MAX_PATH * 2];
    DWORD sz = sizeof(b);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Liquid", RRF_RT_REG_SZ, nullptr, b, &sz) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring v = std::wstring(L"\"") + self + L"\"";
        RegSetValueExW(k, L"Liquid", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"Liquid");
    }
    RegCloseKey(k);
}

// ---------- processes / windows ----------
std::wstring ProcessPath(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t b[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring r;
    if (QueryFullProcessImageNameW(p, 0, b, &n)) r.assign(b, n);
    CloseHandle(p);
    return r;
}

std::wstring FileDescription(const std::wstring& path) {
    static std::map<std::wstring, std::wstring> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    std::wstring result;
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size) {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
            struct LangCp { WORD lang, cp; } *tr = nullptr;
            UINT len = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && len >= sizeof(LangCp)) {
                auto query = [&](const wchar_t* nm) -> std::wstring {
                    wchar_t key[128];
                    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\%s", tr[0].lang, tr[0].cp, nm);
                    wchar_t* val = nullptr;
                    UINT vl = 0;
                    if (VerQueryValueW(data.data(), key, (void**)&val, &vl) && vl > 1) return Trim(val);
                    return L"";
                };
                std::wstring desc = query(L"FileDescription"), prod = query(L"ProductName");
                std::wstring ld = Lower(desc);
                bool helper = ld.find(L"helper") != std::wstring::npos || ld.find(L"host") != std::wstring::npos ||
                              ld.find(L"launcher") != std::wstring::npos || ld.find(L"bootstrap") != std::wstring::npos;
                result = (desc.empty() || (helper && !prod.empty())) ? prod : desc;
            }
        }
    }
    if (result.empty()) {
        result = FileStem(path);
        if (!result.empty()) result[0] = towupper(result[0]);
    }
    cache[path] = result;
    return result;
}

std::wstring ResolveLnk(const std::wstring& lnk) {
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return L"";
    ComPtr<IPersistFile> pf;
    if (FAILED(sl.As(&pf)) || FAILED(pf->Load(lnk.c_str(), STGM_READ))) return L"";
    wchar_t b[MAX_PATH] = {};
    if (FAILED(sl->GetPath(b, MAX_PATH, nullptr, 0))) return L"";
    wchar_t ex[MAX_PATH];
    ExpandEnvironmentStringsW(b, ex, MAX_PATH);
    return ex;
}

std::wstring LnkArguments(const std::wstring& lnk) {
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return L"";
    ComPtr<IPersistFile> pf;
    if (FAILED(sl.As(&pf)) || FAILED(pf->Load(lnk.c_str(), STGM_READ))) return L"";
    wchar_t b[1024] = {};
    if (FAILED(sl->GetArguments(b, 1024))) return L"";
    return b;
}

struct ChildPidSearch { DWORD hostPid; DWORD found; };
static BOOL CALLBACK FindChildPid(HWND c, LPARAM lp) {
    auto* s = (ChildPidSearch*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(c, &pid);
    if (pid != s->hostPid) { s->found = pid; return FALSE; }
    return TRUE;
}

DWORD WindowPid(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"ApplicationFrameWindow")) {
        ChildPidSearch s{pid, 0};
        EnumChildWindows(h, FindChildPid, (LPARAM)&s);
        if (s.found) return s.found;
    }
    return pid;
}

bool IsAppWindow(HWND h) {
    if (!IsWindowVisible(h) && !spaces::IsHidden(h)) return false;   // windows parked in another space still count
    if (GetWindow(h, GW_OWNER)) return false;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    if ((ex & WS_EX_NOACTIVATE) && !(ex & WS_EX_APPWINDOW)) return false;
    int cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd") || !wcscmp(cls, L"Windows.UI.Core.CoreWindow"))
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid != g_selfPid;
}

void TapKeys(std::initializer_list<WORD> keys) {
    std::vector<INPUT> in;
    for (WORD k : keys) {
        INPUT i{INPUT_KEYBOARD};
        i.ki.wVk = k;
        in.push_back(i);
    }
    for (auto it = keys.end(); it != keys.begin();) {
        --it;
        INPUT i{INPUT_KEYBOARD};
        i.ki.wVk = *it;
        i.ki.dwFlags = KEYEVENTF_KEYUP;
        in.push_back(i);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

std::wstring HomeDir() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p))) r = p;
    if (p) CoTaskMemFree(p);
    return r;
}

// things Liquid starts open in your home folder, not in Liquid's own folder
void Launch(const std::wstring& target, const std::wstring& args, int show) {
    std::wstring home = HomeDir();
    ShellExecuteW(nullptr, L"open", target.c_str(), args.empty() ? nullptr : args.c_str(), home.empty() ? nullptr : home.c_str(), show);
}

void ActivateWindow(HWND w) {
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    if (!SetForegroundWindow(w)) {
        TapKeys({VK_MENU});
        SetForegroundWindow(w);
    }
}

// ---------- icons ----------
static ComPtr<ID2D1Bitmap1> MakeBitmap(ID2D1DeviceContext* dc, const BYTE* px, int w, int h, int stride) {
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> b;
    dc->CreateBitmap(D2D1::SizeU(w, h), px, stride, bp, &b);
    return b;
}

// Turn any icon into a glyph like the user's hand-made ones (Discord, Spotify, Claude...):
// a crisp, solid white logo, centred at a consistent size, with the app's own background removed.
//  * "badge" icons (a tile or disc with a logo on it): the logo is whatever differs from the
//    tile's dominant colour, so a dark M on a green disc still becomes a white M
//  * "shape" icons (the art is the logo): the whole silhouette becomes the glyph
static const int kGlyphN = 256;
static const float kGlyphFill = 0.64f;   // logo's longest side as a fraction of the tile

// out: white mask glyph; colored: the same glyph keeping the logo's own colours (dark logos borrow the badge colour)
static std::vector<BYTE> MonoGlyph(const std::vector<BYTE>& c, int n, std::vector<BYTE>& colored) {
    std::vector<float> a(n * n);
    for (int i = 0; i < n * n; i++) a[i] = c[i * 4 + 3] / 255.f;
    auto col = [&](int i, int ch) { float al = std::max(a[i], 1e-3f); return c[i * 4 + ch] / 255.f / al; };

    // boundary band of the silhouette: opaque pixels within ~7% of a transparent pixel or the canvas edge
    int band = std::max(2, n * 7 / 100);
    std::vector<int> distEdge(n * n, 1 << 20);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int i = y * n + x;
            if (a[i] < 0.5f) { distEdge[i] = 0; continue; }
            int d = std::min({x, y, n - 1 - x, n - 1 - y}) + 1;
            if (x > 0) d = std::min(d, distEdge[i - 1] + 1);
            if (y > 0) d = std::min(d, distEdge[i - n] + 1);
            distEdge[i] = d;
        }
    for (int y = n - 1; y >= 0; y--)
        for (int x = n - 1; x >= 0; x--) {
            int i = y * n + x;
            if (x < n - 1) distEdge[i] = std::min(distEdge[i], distEdge[i + 1] + 1);
            if (y < n - 1) distEdge[i] = std::min(distEdge[i], distEdge[i + n] + 1);
        }
    (void)band;
    // is the art a filled container (tile / disc)? solid silhouettes fill most of their bounding box
    int sx0 = n, sy0 = n, sx1 = -1, sy1 = -1, silhouette = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (a[y * n + x] >= 0.5f) {
                silhouette++;
                sx0 = std::min(sx0, x); sx1 = std::max(sx1, x); sy0 = std::min(sy0, y); sy1 = std::max(sy1, y);
            }
    float solidity = sx1 >= 0 ? silhouette / (float)((sx1 - sx0 + 1) * (sy1 - sy0 + 1)) : 0;

    // split the container's pixels by brightness (Otsu); the logo is the smaller class
    auto lum = [&](int i) { return std::clamp(0.299f * col(i, 2) + 0.587f * col(i, 1) + 0.114f * col(i, 0), 0.f, 1.f); };
    int hist[256] = {};
    int inner = 0;
    for (int i = 0; i < n * n; i++)
        if (a[i] >= 0.5f && distEdge[i] > 2) { hist[(int)(lum(i) * 255)]++; inner++; }
    double total = 0;
    for (int k = 0; k < 256; k++) total += k * (double)hist[k];
    double sumB = 0, best = -1;
    int wB = 0, T = 128;
    for (int k = 0; k < 256; k++) {
        wB += hist[k];
        if (!wB || wB == inner) continue;
        sumB += k * (double)hist[k];
        double mB = sumB / wB, mF = (total - sumB) / (inner - wB);
        double between = (double)wB * (inner - wB) * (mB - mF) * (mB - mF);
        if (between > best) { best = between; T = k; }
    }
    int dark = 0;
    double sumDark = 0, sumLight = 0;
    for (int k = 0; k < 256; k++) {
        if (k <= T) { dark += hist[k]; sumDark += k * (double)hist[k]; }
        else sumLight += k * (double)hist[k];
    }
    int light = inner - dark;
    float contrast = (dark && light) ? (float)((sumLight / light - sumDark / dark) / 255.0) : 0;
    bool badge = solidity >= 0.66f && contrast >= 0.16f && inner > 0;
    bool logoIsLight = light <= dark;
    float thr = (T + 0.5f) / 255.f, soft = std::max(0.04f, contrast * 0.25f);

    std::vector<float> m(n * n, 0.f);
    for (int i = 0; i < n * n; i++) {
        if (a[i] <= 0) continue;
        if (!badge) { m[i] = a[i]; continue; }
        if (distEdge[i] <= 2) continue;   // drop the container's own rim
        float t = std::clamp((lum(i) - thr) / soft * 0.5f + 0.5f, 0.f, 1.f);
        if (!logoIsLight) t = 1 - t;
        m[i] = a[i] * t * t * (3 - 2 * t);
    }

    // a dark logo on a coloured badge (Modrinth's M on green) is drawn in the badge colour instead
    float badgeCol[3] = {1, 1, 1};
    // (macOS dark mode does the same for white logos on colourful badges: Mail goes blue, Messages green)
    bool useBadgeCol = false;
    if (badge) {
        double s3[3] = {0, 0, 0}; int cnt = 0;
        for (int i = 0; i < n * n; i++)
            if (a[i] >= 0.5f && distEdge[i] > 2 && ((lum(i) > thr) != logoIsLight)) { for (int ch = 0; ch < 3; ch++) s3[ch] += col(i, ch); cnt++; }
        if (cnt) for (int ch = 0; ch < 3; ch++) badgeCol[ch] = (float)(s3[ch] / cnt);
        float mx = std::max({badgeCol[0], badgeCol[1], badgeCol[2]}), mn = std::min({badgeCol[0], badgeCol[1], badgeCol[2]});
        float sat = mx > 0.02f ? (mx - mn) / mx : 0;
        useBadgeCol = cnt && (sat > 0.25f || !logoIsLight);
        if (useBadgeCol && sat > 0.25f) {   // make it vivid
            for (int ch = 0; ch < 3; ch++) badgeCol[ch] = mx > mn ? (badgeCol[ch] - mn) / (mx - mn) * 0.75f + 0.25f : 1;
        }
    }

    // fit the logo into a consistent box in the middle of the tile
    int x0 = n, y0 = n, x1 = -1, y1 = -1;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (m[y * n + x] > 0.35f) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    std::vector<BYTE> out((size_t)kGlyphN * kGlyphN * 4, 0);
    colored.assign(out.size(), 0);
    if (x1 < 0) return out;
    float bw = (float)(x1 - x0 + 1), bh = (float)(y1 - y0 + 1);
    float sc = kGlyphFill * kGlyphN / std::max(bw, bh);
    float ox = (kGlyphN - bw * sc) / 2, oy = (kGlyphN - bh * sc) / 2;
    for (int y = 0; y < kGlyphN; y++) {
        for (int x = 0; x < kGlyphN; x++) {
            float sx = (x + 0.5f - ox) / sc + x0 - 0.5f, sy = (y + 0.5f - oy) / sc + y0 - 0.5f;
            if (sx < x0 - 1 || sy < y0 - 1 || sx > x1 + 1 || sy > y1 + 1) continue;
            int ix = (int)std::floor(sx), iy = (int)std::floor(sy);
            float fx = sx - ix, fy = sy - iy, v = 0;
            float cc[3] = {0, 0, 0}, cw = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    int px = std::clamp(ix + dx, 0, n - 1), py = std::clamp(iy + dy, 0, n - 1);
                    float wgt = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                    int si = py * n + px;
                    v += m[si] * wgt;
                    float cwt = wgt * (m[si] + 1e-3f);
                    for (int ch = 0; ch < 3; ch++) cc[ch] += col(si, ch) * cwt;
                    cw += cwt;
                }
            // sharpen the edge so the logo reads crisp, not washed out
            v = std::clamp((v - 0.15f) / 0.7f, 0.f, 1.f);
            BYTE b = (BYTE)std::lround(v * 0.97f * 255);
            BYTE* o = &out[((size_t)y * kGlyphN + x) * 4];
            o[0] = o[1] = o[2] = o[3] = b;
            // coloured version, lifted so it glows on a dark tile
            float rgb[3];
            for (int ch = 0; ch < 3; ch++) rgb[ch] = useBadgeCol ? badgeCol[ch] : (cw > 0 ? cc[ch] / cw : 1);
            float L = 0.114f * rgb[0] + 0.587f * rgb[1] + 0.299f * rgb[2];
            if (L < 0.55f) {
                float k = 0.55f / std::max(L, 0.05f);
                for (int ch = 0; ch < 3; ch++) rgb[ch] = std::min(1.f, rgb[ch] * k);
                float L2 = 0.114f * rgb[0] + 0.587f * rgb[1] + 0.299f * rgb[2];
                if (L2 < 0.5f) for (int ch = 0; ch < 3; ch++) rgb[ch] += (1 - rgb[ch]) * (0.5f - L2);
            }
            BYTE* q = &colored[((size_t)y * kGlyphN + x) * 4];
            float al = v * 0.97f;
            for (int ch = 0; ch < 3; ch++) q[ch] = (BYTE)std::lround(std::clamp(rgb[ch], 0.f, 1.f) * al * 255);
            q[3] = b;
        }
    }
    return out;
}

// macOS Tahoe glyph finish: silver gradient (bright top, soft grey bottom) plus a dark drop copy for depth
static void MakeSilver(ID2D1DeviceContext* dc, const std::vector<BYTE>& glyph, IconImages& r) {
    std::vector<BYTE> silver(glyph.size()), shadow(glyph.size());
    for (int y = 0; y < kGlyphN; y++) {
        float t = (float)y / kGlyphN;
        float u = std::clamp((t - 0.18f) / 0.70f, 0.f, 1.f);
        float v = 1.0f - 0.40f * u * u * (3 - 2 * u);
        for (int x = 0; x < kGlyphN; x++) {
            size_t i = ((size_t)y * kGlyphN + x) * 4;
            float a = glyph[i + 3] / 255.f;
            BYTE c = (BYTE)std::lround(v * a * 255);
            silver[i] = (BYTE)std::lround(v * 0.985f * a * 255); silver[i + 1] = c; silver[i + 2] = c; silver[i + 3] = glyph[i + 3];
            shadow[i] = shadow[i + 1] = shadow[i + 2] = 0; shadow[i + 3] = (BYTE)std::lround(a * 0.45f * 255);
        }
    }
    r.silver = MakeBitmap(dc, silver.data(), kGlyphN, kGlyphN, kGlyphN * 4);
    r.shadow = MakeBitmap(dc, shadow.data(), kGlyphN, kGlyphN, kGlyphN * 4);
}

// pixels of an image file (custom icon override), premultiplied BGRA, square-padded
static bool ImagePixels(const std::wstring& path, std::vector<BYTE>& px, int& w, int& h) {
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(g.wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr))) return false;
    UINT iw, ih;
    fr->GetSize(&iw, &ih);
    double sc = 256.0 / std::max(iw, ih);
    UINT sw = std::max(1u, (UINT)(iw * sc)), sh = std::max(1u, (UINT)(ih * sc));
    ComPtr<IWICBitmapScaler> scaler;
    g.wic->CreateBitmapScaler(&scaler);
    scaler->Initialize(fr.Get(), sw, sh, WICBitmapInterpolationModeFant);
    ComPtr<IWICFormatConverter> conv;
    g.wic->CreateFormatConverter(&conv);
    if (FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    w = h = 256;
    px.assign((size_t)w * h * 4, 0);
    std::vector<BYTE> tmp((size_t)sw * sh * 4);
    if (FAILED(conv->CopyPixels(nullptr, sw * 4, (UINT)tmp.size(), tmp.data()))) return false;
    int ox = (w - (int)sw) / 2, oy = (h - (int)sh) / 2;
    for (UINT y = 0; y < sh; y++) memcpy(&px[((size_t)(y + oy) * w + ox) * 4], &tmp[(size_t)y * sw * 4], (size_t)sw * 4);
    return true;
}

IconImages LoadIcons(ID2D1DeviceContext* dc, const std::wstring& path, int size, const std::wstring& overrideImage) {
    IconImages r;
    int w = 0, h = 0;
    std::vector<BYTE> px;
    bool straight = false, anyAlpha = true;
    if (!overrideImage.empty() && ImagePixels(overrideImage, px, w, h)) {
        // a designed icon: keep it exactly as drawn. "<name>.glyph.png" is only a better source
        // image: it gets the same glyph + tile treatment as every other dock icon
        r.custom = Lower(overrideImage).find(L".glyph.") == std::wstring::npos;
    } else {
    ComPtr<IShellItemImageFactory> f;
    if (path == L"known:recyclebin") {
        if (FAILED(SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&f)))) return r;
    } else if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return r;
    HBITMAP hb = nullptr;
    if (FAILED(f->GetImage(SIZE{size, size}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb)) || !hb) return r;
    BITMAP bm{};
    GetObjectW(hb, sizeof(bm), &bm);
    w = bm.bmWidth; h = bm.bmHeight;
    px.resize((size_t)w * h * 4);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC hdc = GetDC(nullptr);
    GetDIBits(hdc, hb, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    DeleteObject(hb);
    anyAlpha = false;
    for (size_t i = 0; i < px.size(); i += 4) {
        BYTE a = px[i + 3];
        if (a) anyAlpha = true;
        if (px[i] > a || px[i + 1] > a || px[i + 2] > a) straight = true;
    }
    for (size_t i = 0; i < px.size(); i += 4) {
        if (!anyAlpha) px[i + 3] = 255;
        else if (straight) {
            BYTE a = px[i + 3];
            px[i] = (BYTE)(px[i] * a / 255);
            px[i + 1] = (BYTE)(px[i + 1] * a / 255);
            px[i + 2] = (BYTE)(px[i + 2] * a / 255);
        }
    }
    }
    // crop to the visible art (small icons come back centred in a big empty canvas), square
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            if (px[((size_t)yy * w + xx) * 4 + 3] > 96) {
                x0 = std::min(x0, xx); x1 = std::max(x1, xx);
                y0 = std::min(y0, yy); y1 = std::max(y1, yy);
            }
    int cx = 0, cy = 0, side = std::min(w, h);
    if (x1 >= 0) {
        int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        side = std::max(bw, bh);
        if (side < w * 0.6) side = std::min(w, (int)(side * 1.12f));
        side = std::min(side, std::min(w, h));
        cx = std::clamp((x0 + x1 + 1) / 2 - side / 2, 0, w - side);
        cy = std::clamp((y0 + y1 + 1) / 2 - side / 2, 0, h - side);
    }
    std::vector<BYTE> crop((size_t)side * side * 4);
    for (int yy = 0; yy < side; yy++)
        memcpy(&crop[(size_t)yy * side * 4], &px[((size_t)(cy + yy) * w + cx) * 4], (size_t)side * 4);
    r.color = MakeBitmap(dc, crop.data(), side, side, side * 4);
    std::vector<BYTE> colored;
    auto glyph = MonoGlyph(crop, side, colored);
    r.clear = MakeBitmap(dc, glyph.data(), kGlyphN, kGlyphN, kGlyphN * 4);
    r.brand = MakeBitmap(dc, colored.data(), kGlyphN, kGlyphN, kGlyphN * 4);
    MakeSilver(dc, glyph, r);
    r.tile = true;
    return r;
}

// A crisp glyph from the Segoe Fluent icon font (used for Finder and Trash, like macOS's own glyph icons)
IconImages GlyphIcon(ID2D1DeviceContext* dc, const wchar_t* glyph) {
    IconImages r;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    auto f = MakeFont(L"Segoe Fluent Icons", kGlyphN * kGlyphFill * 0.98f, DWRITE_FONT_WEIGHT_NORMAL, true);
    ComPtr<ID2D1Image> old;
    dc->GetTarget(&old);
    auto paint = [&](ID2D1Brush* brush) {
        ComPtr<ID2D1Bitmap1> bmp;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(kGlyphN, kGlyphN), nullptr, 0, bp, &bmp))) return bmp;
        dc->SetTarget(bmp.Get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawTextW(glyph, 1, f.Get(), D2D1::RectF(0, 0, (float)kGlyphN, (float)kGlyphN), brush);
        dc->EndDraw();
        return bmp;
    };
    ComPtr<ID2D1SolidColorBrush> white, black;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.97f), &white);
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.45f), &black);
    D2D1_GRADIENT_STOP st[2] = {{0.2f, D2D1::ColorF(1.0f, 1.0f, 1.0f)}, {0.88f, D2D1::ColorF(0.60f, 0.60f, 0.62f)}};
    ComPtr<ID2D1GradientStopCollection> sc;
    dc->CreateGradientStopCollection(st, 2, &sc);
    ComPtr<ID2D1LinearGradientBrush> silver;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, (float)kGlyphN)), sc.Get(), &silver);
    r.clear = paint(white.Get());
    r.silver = paint(silver.Get());
    r.shadow = paint(black.Get());
    dc->SetTarget(old.Get());
    r.color = r.clear;
    r.brand = r.clear;
    r.tile = true;
    return r;
}

ComPtr<ID2D1Bitmap1> DecodeImage(ID2D1DeviceContext* dc, const std::vector<BYTE>& bytes, int maxSide) {
    if (bytes.empty()) return nullptr;
    ComPtr<IWICStream> st;
    if (FAILED(g.wic->CreateStream(&st))) return nullptr;
    if (FAILED(st->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size()))) return nullptr;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(g.wic->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec))) return nullptr;
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr))) return nullptr;
    UINT iw, ih;
    fr->GetSize(&iw, &ih);
    double sc = std::min(1.0, (double)maxSide / std::max(iw, ih));
    ComPtr<IWICBitmapScaler> scaler;
    g.wic->CreateBitmapScaler(&scaler);
    scaler->Initialize(fr.Get(), std::max(1u, (UINT)(iw * sc)), std::max(1u, (UINT)(ih * sc)), WICBitmapInterpolationModeFant);
    ComPtr<IWICFormatConverter> conv;
    g.wic->CreateFormatConverter(&conv);
    if (FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom))) return nullptr;
    ComPtr<ID2D1Bitmap1> b;
    dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &b);
    return b;
}

// ---------- text ----------
ComPtr<IDWriteTextFormat> MakeFont(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, bool center) {
    ComPtr<IDWriteTextFormat> f;
    g.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                           size, L"en-us", &f);
    if (!f) return f;
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (center) f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ell;
    g.dw->CreateEllipsisTrimmingSign(f.Get(), &ell);
    f->SetTrimming(&tr, ell.Get());
    return f;
}

float TextWidth(IDWriteTextFormat* f, const std::wstring& s) {
    ComPtr<IDWriteTextLayout> l;
    if (!f || FAILED(g.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000, 1000, &l))) return 0;
    DWRITE_TEXT_METRICS m;
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

// Custom icon override: drop  icons\<App Name>.png  (or <exe name>.png / .ico) next to Liquid.exe.
std::wstring IconOverride(const std::wstring& name, const std::wstring& exePath) {
    std::wstring dir = ExeDir() + L"\\icons\\";
    std::vector<std::wstring> stems = {name};
    if (!exePath.empty()) stems.push_back(FileStem(exePath));
    for (auto& s : stems) {
        if (s.empty()) continue;
        for (const wchar_t* ext : {L".png", L".ico", L".glyph.png"}) {
            std::wstring p = dir + s + ext;
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        }
    }
    return L"";
}