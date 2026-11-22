#include "shell.h"
#include <shellscalingapi.h>
#include <bcrypt.h>
#include <sddl.h>
#define SECURITY_WIN32
#include <security.h>
#include <powrprof.h>
#include <wtsapi32.h>
#include <cmath>
#include <algorithm>
#include <atomic>

// macOS Tahoe style lock screen: wallpaper on every screen, date + big clock at the top,
// your picture, name and a frosted PIN field at the bottom. Unlocks with a Liquid PIN
// (PBKDF2-hashed in config.ini). Windows' own lock screen can't be replaced by an app, so
// this sits in front of Windows; Ctrl+Alt+Del still reaches Windows' secure screen.

namespace {
struct Screen {
    Overlay ov;
    Capture wall;   // the lock wallpaper, scaled to this screen; the glass refracts it
    ComPtr<ID3D11Texture2D> maskRT, maskMip;      // clock text drawn by D2D, then mip-mapped (blur = height)
    ComPtr<ID3D11ShaderResourceView> maskSrv;
    ComPtr<ID2D1Bitmap1> maskBmp;
    std::wstring maskText;
    float maskY = -1;
    RECT rc{};
    bool primary = false;
    float s = 1;
    ComPtr<ID2D1SolidColorBrush> br;
};
std::vector<Screen*> s_screens;
std::atomic<bool> s_locked{false};
bool s_closing = false, s_dirty = false;
float s_anim = 0, s_animV = 0, s_shake = 0, s_shakeV = 0;
int s_mode = 0;   // 0 enter PIN, 1 choose a new PIN, 2 confirm it
std::wstring s_entry, s_newPin, s_msg;
int s_lastMin = -1, s_hover = -1;
ULONGLONG s_lastTop = 0;
HWND s_msgWnd = nullptr;
// Face unlock (Air Ring does the webcam work): Liquid keeps "LiquidLocked" set while the PIN
// screen is waiting, so Air Ring only runs face recognition then; Air Ring sets
// "LiquidFaceUnlock" when the enrolled face matches.
HANDLE s_evLocked = nullptr, s_evFace = nullptr, s_evEnroll = nullptr, s_evForget = nullptr;
HANDLE s_evSeen = nullptr, s_evNoMatch = nullptr;   // Air Ring: "a face is in view", "that face isn't you"
bool s_evLockedOn = false;
double s_faceSeen = -10, s_faceOk = 0, s_faceFail = -10;   // seconds (GetTickCount64), drive the island's Face ID square
double NowS() { return GetTickCount64() / 1000.0; }
// Walk-away: Air Ring reports "someone at the desk" / "desk empty" about once a second (face or
// body). No one there and no keyboard/mouse input -> pause media, then lock. A PIN unlock with
// nobody at the desk (remote desktop) turns this off until someone is seen at the desk again.
HANDLE s_evPresent = nullptr, s_evAbsent = nullptr;
double s_lastReport = -100, s_lastPresent = 0;
bool s_awayPaused = false, s_pinUnlocked = false;
void ResumeIfAwayPaused() {
    if (s_awayPaused && !sys::Media().playing) sys::MediaCommand(1);
    s_awayPaused = false;
}
std::wstring s_name;
ComPtr<ID2D1Bitmap1> s_avatar;
ComPtr<IDWriteTextFormat> fClock, fTime, fDate, fName, fField, fHint, fGlyph, fDots;
struct Btn { D2D1_RECT_F r; int id; };
std::vector<Btn> s_btns;
enum { B_SLEEP = 1, B_RESTART, B_SHUTDOWN, B_GO, B_POWER };
bool s_powerOpen = false;
float s_powerT = 0, s_powerV = 0;

std::wstring Hex(const BYTE* b, size_t n) {
    static const wchar_t* d = L"0123456789abcdef";
    std::wstring o;
    for (size_t i = 0; i < n; i++) { o += d[b[i] >> 4]; o += d[b[i] & 15]; }
    return o;
}
std::vector<BYTE> Unhex(const std::wstring& h) {
    std::vector<BYTE> o;
    for (size_t i = 0; i + 1 < h.size(); i += 2) o.push_back((BYTE)wcstoul(h.substr(i, 2).c_str(), nullptr, 16));
    return o;
}

std::wstring HashPin(const std::wstring& pin, const std::vector<BYTE>& salt) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return L"";
    int n = WideCharToMultiByte(CP_UTF8, 0, pin.c_str(), (int)pin.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, pin.c_str(), (int)pin.size(), u.data(), n, nullptr, nullptr);
    BYTE out[32] = {};
    NTSTATUS st = BCryptDeriveKeyPBKDF2(alg, (PUCHAR)u.data(), (ULONG)u.size(), (PUCHAR)salt.data(), (ULONG)salt.size(),
                                        120000, out, sizeof(out), 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    SecureZeroMemory(u.data(), u.size());
    return st == 0 ? Hex(out, sizeof(out)) : L"";
}

bool CheckPin(const std::wstring& pin) {
    if (cfg.lockPinHash.empty()) return false;
    std::wstring h = HashPin(pin, Unhex(cfg.lockPinSalt));
    if (h.size() != cfg.lockPinHash.size()) return false;
    int diff = 0;
    for (size_t i = 0; i < h.size(); i++) diff |= h[i] ^ cfg.lockPinHash[i];
    return diff == 0;
}

void SavePin(const std::wstring& pin) {
    BYTE salt[16];
    BCryptGenRandom(nullptr, salt, sizeof(salt), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::vector<BYTE> sv(salt, salt + 16);
    cfg.lockPinSalt = Hex(salt, 16);
    cfg.lockPinHash = HashPin(pin, sv);
    SetConfigValue(L"lock_pin_salt", cfg.lockPinSalt);
    SetConfigValue(L"lock_pin_hash", cfg.lockPinHash);
    Log(L"lock: new PIN saved");
}

std::vector<BYTE> ReadFileBytes(const std::wstring& path) {
    std::vector<BYTE> b;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return b;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    if (sz.QuadPart > 0 && sz.QuadPart < 64 * 1024 * 1024) {
        b.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        ReadFile(f, b.data(), (DWORD)b.size(), &rd, nullptr);
        b.resize(rd);
    }
    CloseHandle(f);
    return b;
}

std::wstring AvatarPath() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return L"";
    BYTE buf[256];
    DWORD len = 0;
    std::wstring path;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len)) {
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &sid)) {
            std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\" + std::wstring(sid);
            const wchar_t* vals[] = {L"Image448", L"Image424", L"Image240", L"Image192", L"Image96"};
            for (auto v : vals) {
                wchar_t p[MAX_PATH] = {};
                DWORD cb = sizeof(p);
                if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), v, RRF_RT_REG_SZ, nullptr, p, &cb) == ERROR_SUCCESS &&
                    GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) { path = p; break; }
            }
            LocalFree(sid);
        }
    }
    CloseHandle(tok);
    return path;
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONUP:
            if (s_hover == B_POWER) { s_powerOpen = !s_powerOpen; s_dirty = true; }
            else if (s_hover == B_SLEEP) SetSuspendState(FALSE, FALSE, FALSE);
            else if (s_hover == B_RESTART) Launch(L"shutdown.exe", L"/r /t 0", SW_HIDE);
            else if (s_hover == B_SHUTDOWN) Launch(L"shutdown.exe", L"/s /t 0", SW_HIDE);
            else if (s_hover == B_GO) lock::Key(VK_RETURN, 0);
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// hidden window that hears about sleep / session changes
LRESULT CALLBACK MsgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_POWERBROADCAST && wp == PBT_APMSUSPEND && cfg.lockOnWake) {
        lock::Lock();   // lock before sleeping so it is already up when the PC wakes
        return TRUE;
    }
    if (msg == WM_POWERBROADCAST && (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) && cfg.lockOnWake) {
        lock::Lock();
        return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Draws the clock text into a texture once a minute (or while it animates); the shader turns
// its coverage into glass and a blurred copy into the height field that bends the wallpaper.
void UpdateClockMask(Screen& sc, const std::wstring& text, float y, float h) {
    UINT W = (UINT)(sc.rc.right - sc.rc.left), H = (UINT)(sc.rc.bottom - sc.rc.top);
    if (!sc.maskRT) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &sc.maskRT))) return;
        d.MipLevels = 0;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &sc.maskMip))) return;
        g.dev->CreateShaderResourceView(sc.maskMip.Get(), nullptr, &sc.maskSrv);
        ComPtr<IDXGISurface> surf;
        sc.maskRT.As(&surf);
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        sc.ov.dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &sc.maskBmp);
    }
    if (!sc.maskBmp || (text == sc.maskText && std::fabs(y - sc.maskY) < 0.5f)) return;
    sc.maskText = text;
    sc.maskY = y;
    ComPtr<ID2D1Image> old;
    sc.ov.dc->GetTarget(&old);
    sc.ov.dc->SetTarget(sc.maskBmp.Get());
    sc.ov.dc->BeginDraw();
    sc.ov.dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    sc.br->SetColor(D2D1::ColorF(1, 1, 1, 1));
    sc.ov.dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    sc.ov.dc->DrawTextW(text.c_str(), (UINT32)text.size(), fClock.Get(), D2D1::RectF(0, y, (float)W, y + h), sc.br.Get());
    sc.ov.dc->EndDraw();
    sc.ov.dc->SetTarget(old.Get());
    g.ctx->CopySubresourceRegion(sc.maskMip.Get(), 0, 0, 0, 0, sc.maskRT.Get(), 0, nullptr);
    g.ctx->GenerateMips(sc.maskSrv.Get());
}

GlassShape Pill(D2D1_RECT_F r, float tint, float blur, float a) {
    float h = r.bottom - r.top;
    GlassShape g{r.left, r.top, r.right - r.left, h};
    g.radius = h / 2; g.blur = blur; g.bezel = h * 0.45f; g.thick = 2.2f; g.tint = tint;
    g.shadow = 0; g.highlight = 0; g.opacity = a;
    return g;
}

void RenderScreen(Screen& sc) {
    float s = sc.s, a = std::clamp(s_anim, 0.f, 1.f);
    float W = (float)(sc.rc.right - sc.rc.left), H = (float)(sc.rc.bottom - sc.rc.top);
    float tint[3] = {1, 1, 1};
    std::vector<GlassShape> sh;
    sc.ov.backdrop = a;
    if (!sc.primary) { sc.ov.Render(s, sh, tint, [](ID2D1DeviceContext*) {}); return; }
    s_btns.clear();

    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t date[64];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"dddd, MMMM d", date, 64, nullptr);
    int hr = now.wHour % 12;
    if (hr == 0) hr = 12;
    float rise = (1 - a) * -30 * s, fall = (1 - a) * 30 * s, cx = W / 2;

    // liquid glass clock (real font, the glass bends the wallpaper behind it)
    wchar_t tm[16];
    swprintf_s(tm, L"%d:%02d", hr, now.wMinute);
    UpdateClockMask(sc, tm, H * 0.075f + 22 * s + rise, 180 * s);
    sc.ov.mask = sc.maskSrv.Get();
    sc.ov.maskInfo[0] = 42 * s; sc.ov.maskInfo[1] = 3.2f; sc.ov.maskInfo[2] = 0.22f; sc.ov.maskInfo[3] = a;

    // user block
    float fieldY = H - 132 * s + fall, ar = 34 * s, avY = fieldY - 92 * s;
    float fw = 200 * s, fh = 32 * s, shakeX = s_shake * 14 * s;
    D2D1_RECT_F fr = D2D1::RectF(cx - fw / 2 + shakeX, fieldY - fh / 2, cx + fw / 2 + shakeX, fieldY + fh / 2);
    GlassShape field = Pill(fr, 0.14f, 2.2f, a);
    field.group = 2; field.melt = 10 * s;
    sh.push_back(field);
    D2D1_RECT_F go = D2D1::RectF(fr.right + 8 * s, fr.top, fr.right + 8 * s + fh, fr.bottom);
    if (!s_entry.empty()) {
        GlassShape gb = Pill(go, 0.14f, 2.2f, a);   // the arrow droplet melts out of the field
        gb.group = 2; gb.melt = 10 * s;
        sh.push_back(gb);
        s_btns.push_back({go, B_GO});
    }
    const wchar_t* gl[3] = {L"\uE708", L"\uE777", L"\uE7E8"};
    const wchar_t* lb[3] = {L"Sleep", L"Restart", L"Shut Down"};
    float by = H - 58 * s + fall, br = 17 * s, pt = std::clamp(s_powerT, 0.f, 1.2f);
    D2D1_RECT_F pw = D2D1::RectF(cx - br, by - br, cx + br, by + br);
    GlassShape pb = Pill(pw, s_hover == B_POWER || s_powerOpen ? 0.2f : 0.12f, 1.6f, a);
    pb.group = 3; pb.melt = 16 * s;
    sh.push_back(pb);
    s_btns.push_back({pw, B_POWER});
    float optX[3], optY = by - 62 * s * pt;
    for (int i = 0; i < 3; i++) {
        optX[i] = cx + (i - 1) * 84 * s * pt;
        if (pt < 0.05f) continue;
        float r = br * std::min(1.f, pt) + (s_hover == B_SLEEP + i ? 2 * s : 0);
        GlassShape o = Pill(D2D1::RectF(optX[i] - r, optY - r, optX[i] + r, optY + r), s_hover == B_SLEEP + i ? 0.22f : 0.13f, 1.6f, a * std::min(1.f, pt));
        o.group = 3; o.melt = 16 * s;
        sh.push_back(o);
        if (s_powerOpen) s_btns.push_back({D2D1::RectF(optX[i] - 40 * s, optY - br - 4 * s, optX[i] + 40 * s, optY + br + 20 * s), B_SLEEP + i});
    }

    sc.ov.Render(s, sh, tint, [&](ID2D1DeviceContext* dc) {
        auto text = [&](IDWriteTextFormat* f, const std::wstring& tx, D2D1_RECT_F r, D2D1_COLOR_F c) {
            sc.br->SetColor(c);
            dc->DrawTextW(tx.c_str(), (UINT32)tx.size(), f, r, sc.br.Get());
        };
        text(fDate.Get(), date, D2D1::RectF(0, H * 0.075f + rise, W, H * 0.075f + 34 * s + rise), D2D1::ColorF(1, 1, 1, 0.9f * a));

        D2D1_ELLIPSE av = D2D1::Ellipse(D2D1::Point2F(cx, avY), ar, ar);
        if (s_avatar) {
            ComPtr<ID2D1EllipseGeometry> geo;
            g.d2dFactory->CreateEllipseGeometry(av, &geo);
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), a), nullptr);
            D2D1_RECT_F ir = D2D1::RectF(cx - ar, avY - ar, cx + ar, avY + ar);
            dc->DrawBitmap(s_avatar.Get(), &ir, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            dc->PopLayer();
        } else {
            sc.br->SetColor(D2D1::ColorF(1, 1, 1, 0.22f * a));
            dc->FillEllipse(av, sc.br.Get());
            text(fName.Get(), s_name.empty() ? L"?" : s_name.substr(0, 1), D2D1::RectF(cx - ar, avY - ar, cx + ar, avY + ar), D2D1::ColorF(1, 1, 1, a));
        }
        text(fName.Get(), s_name, D2D1::RectF(0, avY + ar + 6 * s, W, avY + ar + 30 * s), D2D1::ColorF(1, 1, 1, 0.95f * a));

        if (s_entry.empty()) {
            const wchar_t* ph = s_mode == 1 ? L"Choose a PIN" : s_mode == 2 ? L"Confirm PIN" : L"Enter PIN";
            text(fField.Get(), ph, fr, D2D1::ColorF(1, 1, 1, 0.62f * a));
        } else {
            std::wstring dots;
            for (size_t i = 0; i < s_entry.size() && i < 16; i++) dots += L"●";
            text(fDots.Get(), dots, D2D1::RectF(fr.left + 14 * s, fr.top, fr.right - 10 * s, fr.bottom), D2D1::ColorF(1, 1, 1, 0.95f * a));
            text(fGlyph.Get(), L"", go, D2D1::ColorF(1, 1, 1, a));
        }
        std::wstring hint = !s_msg.empty() ? s_msg
                          : s_mode == 1 ? L"Pick a PIN or password for unlocking Liquid (4+ characters)"
                          : s_mode == 2 ? L"Type it again to confirm" : L"";
        if (!hint.empty()) text(fHint.Get(), hint, D2D1::RectF(0, fr.bottom + 10 * s, W, fr.bottom + 30 * s), D2D1::ColorF(1, 1, 1, 0.75f * a));
        text(fGlyph.Get(), L"\uE7E8", pw, D2D1::ColorF(1, 1, 1, 0.92f * a));
        float oa = std::clamp((pt - 0.4f) / 0.6f, 0.f, 1.f) * a;
        if (oa > 0.01f)
            for (int i = 0; i < 3; i++) {
                text(fGlyph.Get(), gl[i], D2D1::RectF(optX[i] - br, optY - br, optX[i] + br, optY + br), D2D1::ColorF(1, 1, 1, 0.92f * oa));
                text(fHint.Get(), lb[i], D2D1::RectF(optX[i] - 42 * s, optY + br + 3 * s, optX[i] + 42 * s, optY + br + 20 * s), D2D1::ColorF(1, 1, 1, 0.75f * oa));
            }
    });
}

std::wstring LockWallpaperPath() {
    auto desktop = [] {
        wchar_t wp[MAX_PATH] = {};
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0);
        return std::wstring(wp);
    };
    if (Lower(cfg.lockWallpaper) == L"desktop") return desktop();
    std::wstring p = !cfg.lockWallpaper.empty() ? cfg.lockWallpaper : ExeDir() + L"\\wallpapers\\lock.png";
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES ? p : desktop();   // a fresh install has no lock.png
}

void LoadArt(ID2D1DeviceContext* dc) {
    std::wstring ap = AvatarPath();
    auto ab = ap.empty() ? std::vector<BYTE>{} : ReadFileBytes(ap);
    s_avatar = ab.empty() ? nullptr : DecodeImage(dc, ab, 256);
}

void Destroy() {
    for (auto* sc : s_screens) { sc->ov.Destroy(); delete sc; }
    s_screens.clear();
    s_avatar.Reset();
}

BOOL CALLBACK AddMon(HMONITOR hm, HDC, LPRECT, LPARAM) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    auto* sc = new Screen;
    sc->rc = mi.rcMonitor;
    sc->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    sc->s = dx / 96.f;
    if (!sc->ov.Create(L"LiquidLock", Proc, sc->rc.left, sc->rc.top, sc->rc.right - sc->rc.left, sc->rc.bottom - sc->rc.top, nullptr)) {
        delete sc;
        return TRUE;
    }
    sc->ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &sc->br);
    sc->wall.mon = hm;
    sc->wall.rect = sc->rc;
    std::wstring wp = LockWallpaperPath();
    size_t dot = wp.find_last_of(L'.');
    if (dot != std::wstring::npos) {   // a copy cut to this screen's exact size wins: never stretched
        std::wstring exact = wp.substr(0, dot) + L"-" + std::to_wstring(sc->rc.right - sc->rc.left) + L"x" +
                             std::to_wstring(sc->rc.bottom - sc->rc.top) + wp.substr(dot);
        if (GetFileAttributesW(exact.c_str()) != INVALID_FILE_ATTRIBUTES) wp = exact;
    }
    sc->wall.LoadStatic(wp);
    sc->ov.cap = &sc->wall;
    s_screens.push_back(sc);
    return TRUE;
}
}  // namespace

namespace lock {
bool Init() {
    wchar_t nm[256] = {};
    ULONG n = 256;
    if (GetUserNameExW(NameDisplay, nm, &n) && nm[0]) s_name = nm;
    else { DWORD m = 256; GetUserNameW(nm, &m); s_name = nm; }
    HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    float s = dx / 96.f;
    const wchar_t* disp = L"Segoe UI Variable Display";
    fTime = MakeFont(disp, 128 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    fClock = MakeFont(disp, 150 * s, DWRITE_FONT_WEIGHT_BOLD, true);
    fDate = MakeFont(disp, 22 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    fName = MakeFont(disp, 15 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    fField = MakeFont(L"Segoe UI Variable Text", 13 * s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fDots = MakeFont(L"Segoe UI Variable Text", 9 * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fHint = MakeFont(L"Segoe UI Variable Text", 12 * s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fGlyph = MakeFont(L"Segoe Fluent Icons", 13 * s, DWRITE_FONT_WEIGHT_NORMAL, true);
    WNDCLASSW wc{};
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LiquidLockListener";
    RegisterClassW(&wc);
    s_msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    s_evLocked = CreateEventW(nullptr, TRUE, FALSE, L"Local\\LiquidLocked");
    s_evFace = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidFaceUnlock");
    s_evEnroll = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidFaceEnroll");
    s_evForget = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidFaceForget");
    if (s_evLocked) ResetEvent(s_evLocked);
    if (s_evFace) ResetEvent(s_evFace);
    s_evSeen = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidFaceSeen");
    s_evNoMatch = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidFaceNoMatch");
    s_evPresent = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidPresent");
    s_evAbsent = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidAbsent");
    s_lastPresent = NowS();
    return true;
}

bool IsLocked() { return s_locked; }

void FaceTimes(double& seen, double& ok, double& fail) { seen = s_faceSeen; ok = s_faceOk; fail = s_faceFail; }

void FaceSetup(bool forget) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"AirRingSingleInstance");
    if (!m) {
        MessageBoxW(nullptr, L"Face ID uses the webcam through Air Ring, and Air Ring isn't running. "
                             L"Start it in System Settings > Air Gestures, then try again.",
                    L"Liquid", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return;
    }
    CloseHandle(m);
    if (!forget && NowS() - s_lastReport > 5) {   // Air Ring reports about once a second while its camera loop runs
        MessageBoxW(nullptr, L"Air Ring is open but isn't using the camera right now (paused, no camera, or stuck). "
                             L"Turn the camera on in System Settings > Air Gestures, then try again.",
                    L"Liquid", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return;
    }
    if (cfg.lockPinHash.empty() && !forget) {
        MessageBoxW(nullptr, L"Set a lock PIN first (Change Lock PIN...). The PIN stays as the backup for face unlock.",
                    L"Liquid", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return;
    }
    SetEvent(forget ? s_evForget : s_evEnroll);
    Log(L"lock: asked Air Ring to %s", forget ? L"forget the face" : L"set up face unlock");
}

void Lock(bool setPin, bool instant) {
    if (!cfg.lockScreen) { LockWorkStation(); return; }
    if (s_locked && !s_closing) return;
    if (spotlight::IsOpen()) spotlight::Toggle();
    if (nc::IsOpen()) nc::Close();
    if (cc.open) cc.Close();
    if (s_screens.empty()) {
        EnumDisplayMonitors(nullptr, nullptr, AddMon, 0);
        if (s_screens.empty()) { LockWorkStation(); return; }
        LoadArt(s_screens[0]->ov.dc.Get());
    }
    s_mode = setPin || cfg.lockPinHash.empty() ? 1 : 0;
    s_entry.clear(); s_newPin.clear(); s_msg.clear();
    s_closing = false;
    s_powerOpen = false;
    s_locked = true;
    s_lastTop = 0;
    s_faceOk = 0;
    if (instant) { s_anim = 1; s_animV = 0; }
    for (auto* sc : s_screens) { sc->ov.SetVisible(true); sc->ov.SetClickThrough(false); }
    s_dirty = true;
    Log(L"lock: locked%s", s_mode == 1 ? L" (choose a PIN)" : L"");
}

void Key(int vk, wchar_t ch) {
    if (!s_locked || s_closing) return;
    s_msg.clear();
    if (vk == VK_ESCAPE) { s_entry.clear(); s_powerOpen = false; s_dirty = true; return; }
    if (vk == VK_BACK) { if (!s_entry.empty()) s_entry.pop_back(); s_dirty = true; return; }
    if (vk == VK_RETURN) {
        if (s_entry.empty()) return;
        bool bad = false;
        if (s_mode == 0) {
            if (CheckPin(s_entry)) { s_closing = true; s_pinUnlocked = true; Log(L"lock: unlocked"); }
            else bad = true;
        } else if (s_mode == 1) {
            if (s_entry.size() < 4) { s_msg = L"Use at least 4 characters"; bad = true; }
            else { s_newPin = s_entry; s_mode = 2; }
        } else {
            if (s_entry == s_newPin) { SavePin(s_newPin); s_closing = true; }
            else { s_msg = L"Those didn't match. Choose a PIN again."; s_mode = 1; bad = true; }
            SecureZeroMemory(s_newPin.data(), s_newPin.size() * sizeof(wchar_t));
            s_newPin.clear();
        }
        SecureZeroMemory(s_entry.data(), s_entry.size() * sizeof(wchar_t));
        s_entry.clear();
        if (bad) s_shakeV = 60.f;   // macOS-style head shake
        s_dirty = true;
        return;
    }
    if (ch >= 32 && s_entry.size() < 64) { s_entry += ch; s_dirty = true; }
}

void Tick(bool nf) {
    // idle lock (not while Air Ring sees you at the desk: using air gestures means no keyboard or
    // mouse input for minutes, and walking away is covered by the walk-away lock below)
    // Not in the first 20 seconds after Liquid starts: Air Ring hasn't said yet whether you're at the
    // desk, and a reload while you sat still reading would otherwise lock you out at once.
    static double s_started = NowS();
    if (!s_locked && cfg.lockScreen && cfg.lockIdleMinutes > 0 && NowS() - s_started > 20) {
        LASTINPUTINFO li{sizeof(li)};
        double t = NowS();
        bool seen = t - s_lastReport < 5 && t - s_lastPresent < 10;
        if (!seen && GetLastInputInfo(&li) && (GetTickCount() - li.dwTime) > (DWORD)(cfg.lockIdleMinutes * 60000)) {
            bool media = sys::Media().playing;
            if (!media) { Log(L"lock: no input for %.0f min, locking", cfg.lockIdleMinutes); Lock(); }
        }
    }
    // walk-away
    {
        double t = NowS();
        // Reports coming back after a gap (Air Ring restarting, camera hiccup): the gap says nothing
        // about whether you were there, so start the away clock over instead of counting it.
        bool gap = t - s_lastReport > 5;
        if (s_evAbsent && WaitForSingleObject(s_evAbsent, 0) == WAIT_OBJECT_0) {
            if (gap) s_lastPresent = t;
            s_lastReport = t;
        }
        if (s_evPresent && WaitForSingleObject(s_evPresent, 0) == WAIT_OBJECT_0) {
            s_lastReport = s_lastPresent = t;
            if (!s_locked) {
                if (s_pinUnlocked) { s_pinUnlocked = false; Log(L"lock: someone at the desk again, walk-away lock back on"); }
                if (s_awayPaused) { Log(L"lock: back at the desk, resuming media"); ResumeIfAwayPaused(); }
            }
        }
        // not while streaming this screen to another device: the desk is empty because you're
        // using it from somewhere else
        if (!s_locked && cfg.lockScreen && !cfg.lockPinHash.empty() && !s_pinUnlocked && !g.remoteView &&
            t - s_lastReport < 5) {
            LASTINPUTINFO li{sizeof(li)};
            double idle = GetLastInputInfo(&li) ? (GetTickCount() - li.dwTime) / 1000.0 : 0;
            double away = std::min(t - s_lastPresent, idle);
            if (cfg.awayPauseSeconds > 0 && away >= cfg.awayPauseSeconds && !s_awayPaused && sys::Media().playing) {
                sys::MediaCommand(1);
                s_awayPaused = true;
                Log(L"lock: nobody at the desk for %.0f s, paused media", away);
            }
            if (cfg.awayLockSeconds > 0 && away >= cfg.awayLockSeconds) {
                // locking always pauses the music too; a face unlock picks it back up
                if (!s_awayPaused && sys::Media().playing) { sys::MediaCommand(1); s_awayPaused = true; }
                Log(L"lock: nobody at the desk for %.0f s, locking%s", away, s_awayPaused ? L" (media paused)" : L"");
                Lock();
            }
        }
    }
    // face unlock: only while the normal PIN screen waits (never while choosing a first PIN)
    bool want = s_locked && !s_closing && s_mode == 0 && !cfg.lockPinHash.empty();
    if (s_evLocked && want != s_evLockedOn) {
        s_evLockedOn = want;
        if (want) { ResetEvent(s_evFace); SetEvent(s_evLocked); } else ResetEvent(s_evLocked);
    }
    if (want) {
        double t = NowS();
        if (s_evSeen && WaitForSingleObject(s_evSeen, 0) == WAIT_OBJECT_0) s_faceSeen = t;
        if (s_evNoMatch && WaitForSingleObject(s_evNoMatch, 0) == WAIT_OBJECT_0) s_faceFail = t;
        if (!s_faceOk && s_evFace && WaitForSingleObject(s_evFace, 0) == WAIT_OBJECT_0) {
            s_faceOk = t;   // the island draws its check first, then we open
            s_faceSeen = t;
            Log(L"lock: face matched (Air Ring)");
        }
    }
    if (want && s_faceOk && NowS() - s_faceOk > 0.55) {
        SecureZeroMemory(s_entry.data(), s_entry.size() * sizeof(wchar_t));
        s_entry.clear();
        s_msg.clear();
        s_powerOpen = false;
        s_closing = true;
        s_dirty = true;
        Log(L"lock: unlocked by face (Air Ring)");
        ResumeIfAwayPaused();
    }
    if (s_screens.empty()) return;
    float pa = s_anim, ps = s_shake;
    Spring(s_anim, s_animV, (s_locked && !s_closing) ? 1.f : 0.f, 1 / 60.f, s_closing ? 260.f : 400.f);
    Spring(s_shake, s_shakeV, 0.f, 1 / 60.f, 900.f);
    float pp = s_powerT;
    Spring(s_powerT, s_powerV, s_powerOpen ? 1.f : 0.f, 1 / 60.f, 300.f);
    if (pp != s_powerT) s_dirty = true;
    if (s_closing && s_anim < 0.004f) {
        s_locked = false;
        s_closing = false;
        Destroy();
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (s_locked && now - s_lastTop > 400) {   // stay above everything
        s_lastTop = now;
        for (auto* sc : s_screens) SetWindowPos(sc->ov.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        island::Raise();   // its Face ID square shows on top of the lock screen
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    bool minute = st.wMinute != s_lastMin;
    s_lastMin = st.wMinute;
    POINT p;
    GetCursorPos(&p);
    int hv = -1;
    for (auto* sc : s_screens) if (sc->primary) {
        float x = (float)(p.x - sc->ov.x), y = (float)(p.y - sc->ov.y);
        for (auto& b : s_btns) if (x >= b.r.left && x <= b.r.right && y >= b.r.top && y <= b.r.bottom) hv = b.id;
    }
    if (hv != s_hover) { s_hover = hv; s_dirty = true; }
    bool anim = pa != s_anim || ps != s_shake;
    if (s_dirty || minute || anim || g.snapPending) {
        s_dirty = false;
        for (auto* sc : s_screens) if (sc->primary || anim || minute || g.snapPending || nf) RenderScreen(*sc);
        // the other screens only need a redraw when fading
    }
}

void Shutdown() {
    s_locked = false;
    if (s_evLocked) { ResetEvent(s_evLocked); CloseHandle(s_evLocked); s_evLocked = nullptr; }
    if (s_evFace) { CloseHandle(s_evFace); s_evFace = nullptr; }
    if (s_evSeen) { CloseHandle(s_evSeen); s_evSeen = nullptr; }
    if (s_evNoMatch) { CloseHandle(s_evNoMatch); s_evNoMatch = nullptr; }
    if (s_evPresent) { CloseHandle(s_evPresent); s_evPresent = nullptr; }
    if (s_evAbsent) { CloseHandle(s_evAbsent); s_evAbsent = nullptr; }
    if (s_evEnroll) { CloseHandle(s_evEnroll); s_evEnroll = nullptr; }
    if (s_evForget) { CloseHandle(s_evForget); s_evForget = nullptr; }
    Destroy();
    if (s_msgWnd) DestroyWindow(s_msgWnd);
}
}  // namespace lock
