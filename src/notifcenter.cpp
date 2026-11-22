#include "shell.h"
#include <shellscalingapi.h>
#include <cmath>
#include <algorithm>
#include <map>
#include <set>

// macOS Tahoe style Notification Center: opens from the menu-bar clock, slides in from the
// right as separate floating glass cards: your Windows notifications on top, then widgets
// (date, analog clock, month calendar).

namespace {
enum { N_CLEAR = 1, N_CAL = 2, N_CLOCK = 3, N_DATE = 4, N_NOTIF0 = 100, N_CLOSE0 = 300 };
const float kW = 344, kMargin = 30, kGap = 10;

Overlay s_ov;
float s_s = 1, s_anim = 0, s_animV = 0;
bool s_open = false, s_dirty = false;
RECT s_mon{};
int s_hover = -1, s_pressed = -1;
ULONGLONG s_lastPoll = 0;
std::vector<Notif> s_list;
int s_lastSec = -1;
struct Hit { D2D1_RECT_F r; int id; };
std::vector<Hit> s_hits;
std::vector<D2D1_RECT_F> s_cards;
HHOOK s_mouse = nullptr, s_kbd = nullptr;
ComPtr<ID2D1SolidColorBrush> s_br;
ComPtr<IDWriteTextFormat> fApp, fTitle, fBody, fSmall, fIcon, fCalHead, fCalDay, fBigDay, fDow, fHead;

std::map<std::wstring, ComPtr<ID2D1Bitmap1>> s_appIcons;
ComPtr<ID2D1Bitmap1> AppIcon(const std::wstring& aumid) {
    if (aumid.empty()) return nullptr;
    auto it = s_appIcons.find(aumid);
    if (it != s_appIcons.end()) return it->second;
    auto ic = LoadIcons(s_ov.dc.Get(), L"shell:AppsFolder\\" + aumid, (int)(30 * s_s * 1.5f));
    s_appIcons[aumid] = ic.color;
    return ic.color;
}

bool InCard(POINT p) {
    float x = (float)(p.x - s_ov.x), y = (float)(p.y - s_ov.y);
    for (auto& c : s_cards) if (x >= c.left && x <= c.right && y >= c.top && y <= c.bottom) return true;
    return false;
}

LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && s_open && !GlassMenuOpen() && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN)) {
        auto* m = (MSLLHOOKSTRUCT*)lp;
        bool onBar = false;
        for (auto& mb : menubars) {
            RECT r{mb->ov.x, mb->ov.y, mb->ov.x + mb->ov.w, mb->ov.y + mb->H};
            if (PtInRect(&r, m->pt)) onBar = true;
        }
        if (!InCard(m->pt) && !onBar) nc::Close();
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && s_open && wp == WM_KEYDOWN && ((KBDLLHOOKSTRUCT*)lp)->vkCode == VK_ESCAPE) {
        nc::Close();
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

std::wstring Ago(long long t) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    long long now = ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    long long sec = (now - t) / 10000000LL;
    if (sec < 60) return L"now";
    if (sec < 3600) return std::to_wstring(sec / 60) + L"m ago";
    if (sec < 86400) return std::to_wstring(sec / 3600) + L"h ago";
    FILETIME f{(DWORD)t, (DWORD)(t >> 32)}, lf;
    FileTimeToLocalFileTime(&f, &lf);
    SYSTEMTIME st;
    FileTimeToSystemTime(&lf, &st);
    wchar_t b[32];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"MMM d", b, 32, nullptr);
    return b;
}

void OnClick(int id) {
    if (id == N_CLEAR) { sys::ClearNotifications(); s_list.clear(); }
    else if (id == N_CAL || id == N_DATE) { Launch(L"outlookcal:"); nc::Close(); }
    else if (id == N_CLOCK) { Launch(L"ms-clock:"); nc::Close(); }
    else if (id >= N_CLOSE0 && id < N_CLOSE0 + (int)s_list.size()) {
        sys::DismissNotification(s_list[id - N_CLOSE0].id);
        s_list.erase(s_list.begin() + (id - N_CLOSE0));
    } else if (id >= N_NOTIF0 && id < N_NOTIF0 + (int)s_list.size()) {
        auto n = s_list[id - N_NOTIF0];
        if (!n.aumid.empty()) Launch(L"shell:AppsFolder\\" + n.aumid);
        sys::DismissNotification(n.id);
        nc::Close();
    }
    s_dirty = true;
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONDOWN: s_pressed = s_hover; return 0;
        case WM_LBUTTONUP: {
            int p = s_pressed;
            s_pressed = -1;
            if (p >= 0 && p == s_hover) OnClick(p);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Render() {
    s_hits.clear();
    s_cards.clear();
    float s = s_s, a = std::clamp(s_anim, 0.f, 1.f), m = kMargin * s, W = kW * s, gap = kGap * s;
    float xoff = (1 - a) * 60 * s;   // slides in from the right edge
    float left = m + xoff, right = left + W;
    float y = m;

    // each card picks light or dark glass from what is behind it (like macOS)
    auto lightAt = [&](D2D1_RECT_F r) {
        RECT b{s_ov.x + (int)(r.left - xoff), s_ov.y + (int)r.top, s_ov.x + (int)(r.right - xoff), s_ov.y + (int)r.bottom};
        return s_ov.cap ? s_ov.cap->Luma(b) > 0.6f : false;
    };
    bool light = false;
    D2D1_COLOR_F fg{}, fg2{};
    auto setLight = [&](bool l) {
        light = l;
        fg = l ? D2D1::ColorF(0, 0, 0, 0.88f * a) : D2D1::ColorF(1, 1, 1, 0.95f * a);
        fg2 = l ? D2D1::ColorF(0, 0, 0, 0.5f * a) : D2D1::ColorF(1, 1, 1, 0.58f * a);
    };
    D2D1_COLOR_F red = D2D1::ColorF(1.0f, 0.27f, 0.23f, a);   // macOS calendar red

    struct Card { D2D1_RECT_F r; int kind; int idx; bool light = false; };
    std::vector<Card> cards;
    // header
    bool anyN = !s_list.empty();
    float headH = anyN ? 28 * s : 0;
    D2D1_RECT_F clearR = D2D1::RectF(right - 84 * s, y, right, y + 26 * s);
    if (anyN) cards.push_back({clearR, 5, 0});
    y += headH + (anyN ? 6 * s : 0);
    int access = sys::NotificationAccess();
    size_t shown = std::min<size_t>(s_list.size(), 5);
    for (size_t i = 0; i < shown; i++) {
        float ch = (s_list[i].body.empty() ? 58 : 76) * s;
        cards.push_back({D2D1::RectF(left, y, right, y + ch), 0, (int)i});
        y += ch + gap;
    }
    if (!anyN) { cards.push_back({D2D1::RectF(left, y, right, y + 46 * s), 1, 0}); y += 46 * s + gap; }
    y += 8 * s;
    // widgets: two small squares, then the month calendar
    float half = (W - gap) / 2, sq = 150 * s;
    cards.push_back({D2D1::RectF(left, y, left + half, y + sq), 2, 0});
    cards.push_back({D2D1::RectF(left + half + gap, y, right, y + sq), 3, 0});
    y += sq + gap;
    cards.push_back({D2D1::RectF(left, y, right, y + 196 * s), 4, 0});

    std::vector<GlassShape> sh;
    for (auto& c : cards) {
        c.light = lightAt(c.r);
        s_cards.push_back(c.r);
        GlassShape gs{c.r.left, c.r.top, c.r.right - c.r.left, c.r.bottom - c.r.top};
        float h = c.r.bottom - c.r.top;
        gs.radius = c.kind == 5 ? h / 2 : (c.kind >= 2 ? 22 : 18) * s; gs.blur = 2.4f; gs.bezel = std::min(14 * s, h * 0.45f); gs.thick = 2.2f;
        gs.tint = c.light ? 0.42f : -0.36f; gs.shadow = 0.18f; gs.highlight = 0.0f; gs.opacity = a;
        if (sh.size() < 30) sh.push_back(gs);
    }
    float tintLight[3] = {0.97f, 0.97f, 0.98f}, tintDark[3] = {0.13f, 0.13f, 0.15f};

    SYSTEMTIME now;
    GetLocalTime(&now);

    (void)tintDark;
    s_ov.Render(s, sh, tintLight, [&](ID2D1DeviceContext* dc) {
        auto text = [&](IDWriteTextFormat* f, const std::wstring& t, D2D1_RECT_F r, D2D1_COLOR_F c) {
            s_br->SetColor(c);
            dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, s_br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        for (auto& c : cards) {
            D2D1_RECT_F r = c.r;
            float p = 14 * s;
            setLight(c.light);
            if (c.kind == 5) {
                s_hits.push_back({r, N_CLEAR});
                if (s_hover == N_CLEAR) {
                    s_br->SetColor(light ? D2D1::ColorF(0, 0, 0, 0.08f * a) : D2D1::ColorF(1, 1, 1, 0.12f * a));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(r, (r.bottom - r.top) / 2, (r.bottom - r.top) / 2), s_br.Get());
                }
                fSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                text(fSmall.Get(), L"Clear All", r, fg);
                fSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            } else if (c.kind == 0) {
                auto& n = s_list[c.idx];
                int id = N_NOTIF0 + c.idx;
                s_hits.push_back({r, id});
                // app glyph tile
                D2D1_RECT_F ic = D2D1::RectF(r.left + p, r.top + p, r.left + p + 30 * s, r.top + p + 30 * s);
                auto bmp = AppIcon(n.aumid);
                if (bmp) dc->DrawBitmap(bmp.Get(), &ic, a, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                else {
                    s_br->SetColor(light ? D2D1::ColorF(0, 0, 0, 0.08f * a) : D2D1::ColorF(1, 1, 1, 0.12f * a));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(ic, 7 * s, 7 * s), s_br.Get());
                    std::wstring ini = n.app.empty() ? L"•" : n.app.substr(0, 1);
                    fTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    text(fTitle.Get(), ini, ic, fg);
                    fTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                }
                float tx = ic.right + 10 * s;
                std::wstring when = Ago(n.time);
                float ww = TextWidth(fSmall.Get(), when) + 4 * s;
                text(fTitle.Get(), n.title.empty() ? n.app : n.title, D2D1::RectF(tx, r.top + 10 * s, r.right - p - ww - 6 * s, r.top + 30 * s), fg);
                text(fSmall.Get(), when, D2D1::RectF(r.right - p - ww, r.top + 11 * s, r.right - p, r.top + 29 * s), fg2);
                if (!n.body.empty())
                    text(fBody.Get(), n.body, D2D1::RectF(tx, r.top + 30 * s, r.right - p, r.bottom - 8 * s), fg);
                else
                    text(fBody.Get(), n.app, D2D1::RectF(tx, r.top + 30 * s, r.right - p, r.bottom - 8 * s), fg2);
                // hover: close button on the top-left corner, like macOS
                bool hot = s_hover == id || s_hover == N_CLOSE0 + c.idx;
                if (hot) {
                    float cr = 10 * s, cx = r.left + 4 * s, cy = r.top + 4 * s;
                    D2D1_RECT_F xr = D2D1::RectF(cx - cr, cy - cr, cx + cr, cy + cr);
                    s_hits.push_back({xr, N_CLOSE0 + c.idx});
                    s_cards.push_back(xr);
                    s_br->SetColor(light ? D2D1::ColorF(0.92f, 0.92f, 0.93f, a) : D2D1::ColorF(0.28f, 0.28f, 0.30f, a));
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), cr, cr), s_br.Get());
                    text(fIcon.Get(), L"", xr, fg);
                }
            } else if (c.kind == 1) {
                std::wstring msg = access == 0 ? L"Notifications are off for Liquid" : L"No Notifications";
                fBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fBody->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                text(fBody.Get(), msg, r, fg2);
                fBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                fBody->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            } else if (c.kind == 2) {
                // date widget: weekday in red, big day number
                s_hits.push_back({r, N_DATE});
                wchar_t dow[32], mon[32];
                GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"dddd", dow, 32, nullptr);
                GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"MMMM", mon, 32, nullptr);
                std::wstring d = dow;
                for (auto& ch : d) ch = towupper(ch);
                text(fDow.Get(), d, D2D1::RectF(r.left + p, r.top + p, r.right - p, r.top + p + 18 * s), red);
                text(fBigDay.Get(), std::to_wstring(now.wDay), D2D1::RectF(r.left + p - 2 * s, r.top + 30 * s, r.right - p, r.top + 100 * s), fg);
                text(fBody.Get(), mon, D2D1::RectF(r.left + p, r.bottom - p - 20 * s, r.right - p, r.bottom - p), fg2);
            } else if (c.kind == 3) {
                // analog clock
                s_hits.push_back({r, N_CLOCK});
                float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, R = std::min(r.right - r.left, r.bottom - r.top) / 2 - 16 * s;
                s_br->SetColor(light ? D2D1::ColorF(1, 1, 1, 0.55f * a) : D2D1::ColorF(0, 0, 0, 0.28f * a));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), R, R), s_br.Get());
                for (int k = 0; k < 12; k++) {
                    float an = k * 3.14159265f / 6;
                    float r1 = R - 4 * s, r0 = R - (k % 3 == 0 ? 11 : 7) * s;
                    s_br->SetColor(k % 3 == 0 ? fg : fg2);
                    dc->DrawLine(D2D1::Point2F(cx + std::sin(an) * r0, cy - std::cos(an) * r0),
                                 D2D1::Point2F(cx + std::sin(an) * r1, cy - std::cos(an) * r1), s_br.Get(), (k % 3 == 0 ? 2.f : 1.2f) * s);
                }
                float sec = now.wSecond + now.wMilliseconds / 1000.f;
                float mn = now.wMinute + sec / 60, hr = (now.wHour % 12) + mn / 60;
                auto hand = [&](float frac, float len, float wdt, D2D1_COLOR_F col) {
                    float an = frac * 2 * 3.14159265f;
                    ComPtr<ID2D1StrokeStyle> round;
                    D2D1_STROKE_STYLE_PROPERTIES sp{D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND};
                    g.d2dFactory->CreateStrokeStyle(sp, nullptr, 0, &round);
                    s_br->SetColor(col);
                    dc->DrawLine(D2D1::Point2F(cx - std::sin(an) * len * 0.12f, cy + std::cos(an) * len * 0.12f),
                                 D2D1::Point2F(cx + std::sin(an) * len, cy - std::cos(an) * len), s_br.Get(), wdt, round.Get());
                };
                hand(hr / 12, R * 0.52f, 3.6f * s, fg);
                hand(mn / 60, R * 0.78f, 2.6f * s, fg);
                hand(sec / 60, R * 0.86f, 1.2f * s, D2D1::ColorF(1.0f, 0.6f, 0.0f, a));
                s_br->SetColor(D2D1::ColorF(1.0f, 0.6f, 0.0f, a));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3 * s, 3 * s), s_br.Get());
            } else if (c.kind == 4) {
                // month calendar
                s_hits.push_back({r, N_CAL});
                wchar_t mon[32];
                GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"MMMM", mon, 32, nullptr);
                std::wstring mt = mon;
                for (auto& ch : mt) ch = towupper(ch);
                text(fDow.Get(), mt, D2D1::RectF(r.left + p + 4 * s, r.top + p - 2 * s, r.right - p, r.top + p + 16 * s), red);
                const wchar_t* wd[7] = {L"S", L"M", L"T", L"W", L"T", L"F", L"S"};
                float gx = r.left + p, gw = (r.right - r.left - 2 * p) / 7, gy = r.top + 40 * s, rh = 23 * s;
                for (int k = 0; k < 7; k++)
                    text(fCalHead.Get(), wd[k], D2D1::RectF(gx + k * gw, gy, gx + (k + 1) * gw, gy + 16 * s), fg2);
                SYSTEMTIME first = now;
                first.wDay = 1;
                FILETIME ft;
                SystemTimeToFileTime(&first, &ft);
                FileTimeToSystemTime(&ft, &first);   // fills wDayOfWeek
                int startCol = first.wDayOfWeek;
                static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                int days = dim[now.wMonth - 1];
                if (now.wMonth == 2 && ((now.wYear % 4 == 0 && now.wYear % 100 != 0) || now.wYear % 400 == 0)) days = 29;
                for (int d = 1; d <= days; d++) {
                    int cell = startCol + d - 1, row = cell / 7, col = cell % 7;
                    D2D1_RECT_F cr = D2D1::RectF(gx + col * gw, gy + 20 * s + row * rh, gx + (col + 1) * gw, gy + 20 * s + (row + 1) * rh);
                    bool today = d == now.wDay;
                    if (today) {
                        s_br->SetColor(red);
                        float cxx = (cr.left + cr.right) / 2, cyy = (cr.top + cr.bottom) / 2;
                        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cxx, cyy), 11 * s, 11 * s), s_br.Get());
                    }
                    text(fCalDay.Get(), std::to_wstring(d), cr, today ? D2D1::ColorF(1, 1, 1, a) : (col == 0 || col == 6 ? fg2 : fg));
                }
            }
        }
    });
}

// ---------- Mac-style notification banners ----------
// New notifications slide in as a glass card under the menu bar (top right), stay ~5 s
// (longer while hovered), click to open the app. Windows' own banners are switched off
// per app (ShowBanner = 0) while Liquid runs, and put back when it quits.
Overlay s_bov;
std::set<uint32_t> s_seen;
bool s_seenInit = false;
std::vector<Notif> s_queue;
Notif s_cur;
bool s_hasCur = false, s_bHover = false, s_bCloseHover = false;
float s_bAnim = 0, s_bAnimV = 0;
double s_bUntil = 0;
ULONGLONG s_bPoll = 0;
D2D1_RECT_F s_bCard{}, s_bClose{};
const float kBH = 76, kBMargin = 30;

double NowS() { return GetTickCount64() / 1000.0; }

// ---- Windows banners off (remembering what each app had) ----
const wchar_t* kNotifKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings";
std::map<std::wstring, DWORD> s_bannerWas;   // aumid -> previous ShowBanner (2 = value didn't exist)
std::wstring BannerFile() { return ExeDir() + L"\\banner-settings.txt"; }

void SaveBannerFile() {
    std::wstring t;
    for (auto& [k, v] : s_bannerWas) t += k + L"=" + std::to_wstring(v) + L"\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), u.data(), n, nullptr, nullptr);
    HANDLE f = CreateFileW(BannerFile().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(f);
}

void LoadBannerFile() {   // left over from a crash: keep the original values
    HANDLE f = CreateFileW(BannerFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string u(64 * 1024, 0);
    DWORD rd = 0;
    ReadFile(f, u.data(), (DWORD)u.size(), &rd, nullptr);
    CloseHandle(f);
    u.resize(rd);
    int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0);
    std::wstring t(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), t.data(), n);
    size_t pos = 0;
    while (pos < t.size()) {
        size_t nl = t.find(L'\n', pos);
        std::wstring line = t.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? t.size() : nl + 1;
        size_t eq = line.find_last_of(L'=');
        if (eq != std::wstring::npos) s_bannerWas[line.substr(0, eq)] = (DWORD)_wtoi(line.substr(eq + 1).c_str());
    }
}

void QuietApp(const std::wstring& aumid) {
    if (aumid.empty() || s_bannerWas.count(aumid)) return;
    std::wstring key = std::wstring(kNotifKey) + L"\\" + aumid;
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return;
    DWORD v = 2, cb = sizeof(DWORD), type = 0, cur = 0;
    if (RegQueryValueExW(h, L"ShowBanner", nullptr, &type, (BYTE*)&cur, &cb) == ERROR_SUCCESS && type == REG_DWORD) v = cur;
    DWORD zero = 0;
    RegSetValueExW(h, L"ShowBanner", 0, REG_DWORD, (BYTE*)&zero, sizeof(zero));
    RegCloseKey(h);
    s_bannerWas[aumid] = v;
}

void QuietAllApps() {
    LoadBannerFile();
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kNotifKey, 0, KEY_READ, &h) == ERROR_SUCCESS) {
        wchar_t name[512];
        for (DWORD i = 0;; i++) {
            DWORD n = 512;
            if (RegEnumKeyExW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            QuietApp(name);
        }
        RegCloseKey(h);
    }
    SaveBannerFile();
    Log(L"notifications: Windows banners off for %d apps (Liquid shows its own)", (int)s_bannerWas.size());
}

void RestoreBanners() {
    for (auto& [aumid, v] : s_bannerWas) {
        std::wstring key = std::wstring(kNotifKey) + L"\\" + aumid;
        HKEY h;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_WRITE, &h) != ERROR_SUCCESS) continue;
        if (v == 2) RegDeleteValueW(h, L"ShowBanner");
        else RegSetValueExW(h, L"ShowBanner", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
        RegCloseKey(h);
    }
    s_bannerWas.clear();
    DeleteFileW(BannerFile().c_str());
}

// Windows' own toast pop-up ("New notification" window of ShellExperienceHost): hide it the
// moment it shows. The notification itself still arrives (Liquid's banner + Notification Center).
HWINEVENTHOOK s_toastHook = nullptr;
void CALLBACK ToastShown(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !h) return;
    wchar_t cls[64] = {}, title[64] = {};
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow")) return;
    GetWindowTextW(h, title, 64);
    if (wcscmp(title, L"New notification")) return;
    ShowWindow(h, SW_HIDE);
}

LRESULT CALLBACK BannerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONUP:
            if (s_hasCur) {
                if (s_bCloseHover) sys::DismissNotification(s_cur.id);
                else {
                    if (!s_cur.aumid.empty()) Launch(L"shell:AppsFolder\\" + s_cur.aumid);
                    sys::DismissNotification(s_cur.id);
                }
                s_bUntil = 0;   // slide away now
            }
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void RenderBanner() {
    float s = s_s, a = std::clamp(s_bAnim, 0.f, 1.f), m = kBMargin * s, W = kW * s, H = kBH * s;
    float xoff = (1 - a) * (W + m);   // slides in from the right edge
    s_bCard = D2D1::RectF(m + xoff, m, m + xoff + W, m + H);
    RECT behind{s_bov.x + (int)(m), s_bov.y + (int)m, s_bov.x + (int)(m + W), s_bov.y + (int)(m + H)};
    bool light = s_bov.cap ? s_bov.cap->Luma(behind) > 0.6f : false;
    D2D1_COLOR_F fg = light ? D2D1::ColorF(0, 0, 0, 0.88f) : D2D1::ColorF(1, 1, 1, 0.95f);
    D2D1_COLOR_F fg2 = light ? D2D1::ColorF(0, 0, 0, 0.5f) : D2D1::ColorF(1, 1, 1, 0.58f);
    GlassShape gs{s_bCard.left, s_bCard.top, W, H};
    gs.radius = 18 * s; gs.blur = 2.4f; gs.bezel = 14 * s; gs.thick = 2.2f;
    gs.tint = light ? 0.42f : -0.36f; gs.shadow = 0.18f; gs.highlight = 0.0f; gs.opacity = std::min(1.f, a * 1.5f);
    float tintLight[3] = {0.97f, 0.97f, 0.98f};
    s_bov.Render(s, {gs}, tintLight, [&](ID2D1DeviceContext* dc) {
        auto text = [&](IDWriteTextFormat* f, const std::wstring& t, D2D1_RECT_F r, D2D1_COLOR_F c) {
            s_br->SetColor(c);
            dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, s_br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        D2D1_RECT_F r = s_bCard;
        float p = 14 * s;
        D2D1_RECT_F ic = D2D1::RectF(r.left + p, r.top + p, r.left + p + 32 * s, r.top + p + 32 * s);
        auto bmp = AppIcon(s_cur.aumid);
        if (bmp) dc->DrawBitmap(bmp.Get(), &ic, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        float tx = ic.right + 10 * s;
        text(fTitle.Get(), s_cur.title.empty() ? s_cur.app : s_cur.title, D2D1::RectF(tx, r.top + 10 * s, r.right - p - 40 * s, r.top + 30 * s), fg);
        text(fSmall.Get(), L"now", D2D1::RectF(r.right - p - 30 * s, r.top + 11 * s, r.right - p, r.top + 29 * s), fg2);
        text(fBody.Get(), s_cur.body.empty() ? s_cur.app : s_cur.body, D2D1::RectF(tx, r.top + 30 * s, r.right - p, r.bottom - 8 * s),
             s_cur.body.empty() ? fg2 : fg);
        if (s_bHover) {
            float cr = 10 * s, cx = r.left + 4 * s, cy = r.top + 4 * s;
            s_bClose = D2D1::RectF(cx - cr, cy - cr, cx + cr, cy + cr);
            s_br->SetColor(light ? D2D1::ColorF(0.92f, 0.92f, 0.93f, 1) : D2D1::ColorF(0.28f, 0.28f, 0.30f, 1));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), cr, cr), s_br.Get());
            text(fIcon.Get(), L"", s_bClose, fg);
        } else s_bClose = {};
    });
}

void TickBanner(bool nf) {
    if (!cfg.macNotifications && !cfg.islandNotifications) return;
    ULONGLONG t = GetTickCount64();
    if (t - s_bPoll > 700) {
        s_bPoll = t;
        auto list = sys::Notifications();
        if (!s_seenInit && sys::NotificationAccess() == 1) {
            for (auto& n : list) s_seen.insert(n.id);   // don't replay what was already there
            s_seenInit = true;
        } else if (s_seenInit) {
            for (auto it = list.rbegin(); it != list.rend(); ++it) {
                if (s_seen.count(it->id)) continue;
                s_seen.insert(it->id);
                if (s_open || lock::IsLocked()) continue;
                if (cfg.macNotifications && s_bov.hwnd) s_queue.push_back(*it);   // banner at the top right
                if (cfg.islandNotifications) island::Notify(*it);                // and in the Dynamic Island
            }
        }
    }
    if (!cfg.macNotifications || !s_bov.hwnd) return;
    double now = NowS();
    if (!s_hasCur && !s_queue.empty()) {
        s_cur = s_queue.front();
        s_queue.erase(s_queue.begin());
        s_hasCur = true;
        s_bUntil = now + 5.0;
        s_bAnim = 0; s_bAnimV = 0;
        HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(hm, &mi);
        s_bov.Move(mi.rcMonitor.right - (int)((kW + kBMargin + 8) * s_s), mi.rcMonitor.top + (int)(cfg.menubarHeight * s_s) + 6 - (int)(kBMargin * s_s));
        s_bov.SetVisible(true);
    }
    if (!s_hasCur) return;
    POINT p;
    GetCursorPos(&p);
    float x = (float)(p.x - s_bov.x), y = (float)(p.y - s_bov.y);
    bool in = x >= s_bCard.left - 12 * s_s && x <= s_bCard.right && y >= s_bCard.top - 12 * s_s && y <= s_bCard.bottom;
    bool inClose = in && x >= s_bClose.left && x <= s_bClose.right && y >= s_bClose.top && y <= s_bClose.bottom;
    bool dirty = in != s_bHover || inClose != s_bCloseHover;
    s_bHover = in; s_bCloseHover = inClose;
    s_bov.SetClickThrough(!in);
    if (in && s_bUntil > 0) s_bUntil = std::max(s_bUntil, now + 1.5);   // hovering keeps it up
    if (s_open || lock::IsLocked()) s_bUntil = 0;
    float target = now < s_bUntil ? 1.f : 0.f, pa = s_bAnim;
    Spring(s_bAnim, s_bAnimV, target, 1 / 60.f, 380.f);
    if (target == 0 && s_bAnim < 0.004f) {
        s_hasCur = false;
        s_bov.SetVisible(false);
        s_bov.SetClickThrough(true);
        return;
    }
    if (dirty || nf || pa != s_bAnim || g.snapPending) RenderBanner();
}

}  // namespace

namespace nc {
bool Init() {
    HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    s_s = dx / 96.f;
    float s = s_s;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    int w = (int)std::ceil((kW + kMargin * 2 + 60) * s), h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (!s_ov.Create(L"LiquidNotificationCenter", Proc, -w - 100, 0, w, h, nullptr)) return false;
    s_ov.SetVisible(false);
    s_ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &s_br);
    const wchar_t* ui = L"Segoe UI Variable Text";
    const wchar_t* disp = L"Segoe UI Variable Display";
    fHead = MakeFont(disp, 15 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    fTitle = MakeFont(ui, 13.5f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    fBody = MakeFont(ui, 13 * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fBody->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fBody->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fSmall = MakeFont(ui, 11.5f * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fIcon = MakeFont(L"Segoe Fluent Icons", 8 * s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fCalHead = MakeFont(ui, 10.5f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    fCalDay = MakeFont(ui, 12 * s, DWRITE_FONT_WEIGHT_MEDIUM, true);
    fDow = MakeFont(ui, 12 * s, DWRITE_FONT_WEIGHT_BOLD, false);
    fBigDay = MakeFont(disp, 56 * s, DWRITE_FONT_WEIGHT_LIGHT, false);
    if (cfg.macNotifications) {
        int bw = (int)std::ceil((kW + kBMargin * 2) * s), bh = (int)std::ceil((kBH + kBMargin * 2) * s);
        if (s_bov.Create(L"LiquidBanner", BannerProc, -bw - 100, 0, bw, bh, nullptr)) {
            s_bov.SetVisible(false);
            s_bov.SetClickThrough(true);
        }
    }
    if (cfg.macNotifications || cfg.islandNotifications) {   // either way Liquid shows them, not Windows
        LoadBannerFile();   // undo the older registry approach if it left anything behind
        RestoreBanners();
        s_toastHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, ToastShown, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        Log(L"notifications: Windows banners hidden, Liquid shows its own");
    }
    return true;
}

bool IsOpen() { return s_open; }

void Toggle(RECT mon) {
    if (s_open) { Close(); return; }
    if (!s_ov.hwnd) return;
    if (cc.open) cc.Close();
    s_mon = mon;
    int x = mon.right - (int)((kW + kMargin + 8) * s_s);
    int y = mon.top + (int)(cfg.menubarHeight * s_s) + 6 - (int)(kMargin * s_s);
    s_ov.Move(x, y);
    s_ov.SetVisible(true);
    s_open = true;
    s_list = sys::Notifications();
    s_lastPoll = GetTickCount64();
    s_mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
    s_kbd = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
    s_dirty = true;
}

void Close() {
    if (!s_open) return;
    s_open = false;
    if (s_mouse) UnhookWindowsHookEx(s_mouse);
    if (s_kbd) UnhookWindowsHookEx(s_kbd);
    s_mouse = s_kbd = nullptr;
    s_dirty = true;
}

void Tick(bool nf) {
    TickBanner(nf);
    if (!s_ov.hwnd) return;
    float pa = s_anim;
    Spring(s_anim, s_animV, s_open ? 1.f : 0.f, 1 / 60.f, 420.f);
    if (!s_open && s_anim < 0.003f) {
        s_anim = 0;
        if (s_ov.visible) s_ov.SetVisible(false);
        return;
    }
    ULONGLONG t = GetTickCount64();
    if (s_open && t - s_lastPoll > 1500) {
        s_lastPoll = t;
        auto l = sys::Notifications();
        bool changed = l.size() != s_list.size();
        for (size_t i = 0; !changed && i < l.size(); i++) changed = l[i].id != s_list[i].id;
        if (changed) { s_list = std::move(l); s_dirty = true; }
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    bool tickClock = st.wSecond != s_lastSec;
    if (tickClock) s_lastSec = st.wSecond;

    POINT p;
    GetCursorPos(&p);
    float x = (float)(p.x - s_ov.x), y = (float)(p.y - s_ov.y);
    bool in = s_open && InCard(p);
    s_ov.SetClickThrough(!in);
    int hv = -1;
    if (in) for (auto& h : s_hits) if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) hv = h.id;
    if (hv != s_hover) { s_hover = hv; s_dirty = true; }
    if (s_dirty || nf || tickClock || pa != s_anim || g.snapPending) { s_dirty = false; Render(); }
}

void Shutdown() {
    Close();
    if (s_toastHook) UnhookWinEvent(s_toastHook);   // Windows banners come back when Liquid isn't running
    s_toastHook = nullptr;
    if (s_bov.hwnd) s_bov.Destroy();
    if (s_ov.hwnd) s_ov.Destroy();
}
}  // namespace nc
