#include "shell.h"
#include <shellscalingapi.h>
#include <shlobj.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cmath>
#include <algorithm>
#include <memory>

// Desktop widgets, macOS style. Every widget is its own liquid glass piece on the desktop layer
// (above the wallpaper, below apps) and can sit anywhere on any screen.
//   Edit Widgets (right-click the desktop or a widget): drag them around, "-" removes one,
//   the gallery at the bottom adds more, Done saves. Positions live in layout.txt.
// Kinds: clock, calendar, music, system, claude, discord, launcher

namespace {
struct Kind { const wchar_t* type; const wchar_t* title; float w, h; };
const Kind kKinds[] = {
    {L"clock", L"Clock", 158, 158},     {L"calendar", L"Calendar", 158, 158}, {L"music", L"Music", 330, 158},
    {L"system", L"System", 330, 118},   {L"claude", L"Claude Code", 330, 176}, {L"discord", L"Discord", 330, 176},
    {L"launcher", L"Apps", 330, 96},
};
const Kind* KindOf(const std::wstring& t) {
    for (auto& k : kKinds) if (t == k.type) return &k;
    return nullptr;
}
const float kM = 26;   // window margin around a card (room for the refraction)

struct Widget;
std::vector<std::unique_ptr<Widget>> s_ws;
bool s_edit = false;
float s_s = 1;
ComPtr<IDWriteTextFormat> fTitle, fBody, fSmall, fTiny, fDow, fCalHead, fCalDay, fGlyph, fGlyphBig;
MediaInfo s_media;
float s_cpu = 0, s_mem = 0, s_gpu = 0;
FILETIME s_idle0{}, s_kern0{}, s_user0{};
PDH_HQUERY s_q = nullptr;
PDH_HCOUNTER s_gpuC = nullptr;
ULONGLONG s_lastPoll = 0;

struct Session { std::wstring name, state, prompt, reply; double ageMin = 0; };
std::vector<Session> s_sessions;
std::vector<Notif> s_discord;

ULONGLONG U64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

void PollSystem() {
    FILETIME i, k, u;
    if (GetSystemTimes(&i, &k, &u)) {
        ULONGLONG di = U64(i) - U64(s_idle0), dk = U64(k) - U64(s_kern0), du = U64(u) - U64(s_user0);
        if (dk + du > 0 && U64(s_kern0)) s_cpu = std::clamp(1.f - (float)di / (float)(dk + du), 0.f, 1.f);
        s_idle0 = i; s_kern0 = k; s_user0 = u;
    }
    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) s_mem = ms.dwMemoryLoad / 100.f;
    if (!s_q && PdhOpenQueryW(nullptr, 0, &s_q) == ERROR_SUCCESS)
        PdhAddEnglishCounterW(s_q, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &s_gpuC);
    if (s_q && s_gpuC && PdhCollectQueryData(s_q) == ERROR_SUCCESS) {
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(s_gpuC, PDH_FMT_DOUBLE, &size, &count, nullptr) == PDH_MORE_DATA && size) {
            std::vector<BYTE> buf(size);
            auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
            if (PdhGetFormattedCounterArrayW(s_gpuC, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
                double sum = 0;
                for (DWORD n = 0; n < count; n++) if (items[n].FmtValue.CStatus == ERROR_SUCCESS) sum += items[n].FmtValue.doubleValue;
                s_gpu = (float)std::clamp(sum / 100.0, 0.0, 1.0);
            }
        }
    }
}

std::wstring JsonField(const std::wstring& j, const wchar_t* key) {
    std::wstring k = std::wstring(L"\"") + key + L"\"";
    size_t p = j.find(k);
    if (p == std::wstring::npos) return L"";
    p = j.find(L'"', j.find(L':', p) + 1);
    if (p == std::wstring::npos) return L"";
    std::wstring out;
    for (size_t i = p + 1; i < j.size(); i++) {
        if (j[i] == L'\\' && i + 1 < j.size()) { wchar_t c = j[++i]; out += c == L'n' ? L' ' : c; continue; }
        if (j[i] == L'"') break;
        out += j[i];
    }
    return out;
}

void PollClaude() {
    s_sessions.clear();
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) return;
    std::wstring dir = std::wstring(p) + L"\\Liquid\\claude";
    CoTaskMemFree(p);
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        HANDLE f = CreateFileW((dir + L"\\" + fd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        char buf[4096] = {};
        DWORD rd = 0;
        ReadFile(f, buf, sizeof(buf) - 1, &rd, nullptr);
        CloseHandle(f);
        int n = MultiByteToWideChar(CP_UTF8, 0, buf, (int)rd, nullptr, 0);
        std::wstring j(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, buf, (int)rd, j.data(), n);
        Session s;
        s.state = JsonField(j, L"state");
        std::wstring cwd = JsonField(j, L"cwd");
        for (auto& c : cwd) if (c == L'/') c = L'\\';
        while (!cwd.empty() && cwd.back() == L'\\') cwd.pop_back();
        s.name = cwd.empty() ? L"Claude" : cwd.substr(cwd.find_last_of(L'\\') + 1);
        s.prompt = JsonField(j, L"prompt");
        s.reply = JsonField(j, L"reply");
        s.ageMin = (U64(nowFt) - U64(fd.ftLastWriteTime)) / 600000000.0;
        if (s.ageMin < 12 * 60) s_sessions.push_back(s);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(s_sessions.begin(), s_sessions.end(), [](const Session& a, const Session& b) {
        int ra = a.state == L"working" ? 0 : a.state == L"waiting" ? 1 : 2, rb = b.state == L"working" ? 0 : b.state == L"waiting" ? 1 : 2;
        return ra != rb ? ra < rb : a.ageMin < b.ageMin;
    });
}

void SinkToDesktop(HWND h) {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) SetWindowLongPtrW(h, GWLP_HWNDPARENT, (LONG_PTR)progman);
    SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void SaveLayout();
void OpenApp(const std::wstring& appNamePart) {
    struct C { std::wstring part; bool done = false; } c{Lower(appNamePart)};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* c = (C*)lp;
        if (IsAppWindow(w) && Lower(ProcessPath(WindowPid(w))).find(c->part) != std::wstring::npos) { ActivateWindow(w); c->done = true; return FALSE; }
        return TRUE;
    }, (LPARAM)&c);
    if (!c.done) { std::wstring t = spotlight::FindApp(appNamePart); if (!t.empty()) Launch(t); }
}

// ---------------------------------------------------------------- a widget
struct Widget {
    std::wstring type;
    const Kind* kind = nullptr;
    Overlay ov;
    ComPtr<ID2D1SolidColorBrush> br;
    struct Btn { D2D1_RECT_F r; int id; std::wstring arg; };
    std::vector<Btn> btns;
    int hit = -1;
    std::wstring hitArg;
    bool dirty = true, dragging = false;
    POINT dragOff{};
    ULONGLONG lastGlass = 0, lastBottom = 0;
    int lastSec = -1;
    // music: the blurred cover becomes the glass's own backdrop
    Capture coverCap;
    ComPtr<ID3D11Texture2D> coverRT;
    ComPtr<ID2D1Bitmap1> coverBmp, art;
    ComPtr<ID2D1Effect> blur;
    int artVer = -2;
    std::vector<IconImages> launchIcons;

    D2D1_RECT_F Card() const { float s = s_s; return D2D1::RectF(kM * s, kM * s, ov.w - kM * s, ov.h - kM * s); }

    bool Create(int x, int y) {
        kind = KindOf(type);
        if (!kind) return false;
        int w = (int)std::ceil((kind->w + 2 * kM) * s_s), h = (int)std::ceil((kind->h + 2 * kM) * s_s);
        if (!ov.Create(L"LiquidWidget", Proc, x - (int)(kM * s_s), y - (int)(kM * s_s), w, h, this)) return false;
        SinkToDesktop(ov.hwnd);
        ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br);
        return true;
    }

    POINT Pos() const { return {ov.x + (LONG)(kM * s_s), ov.y + (LONG)(kM * s_s)}; }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* w = (Widget*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!w) return DefWindowProcW(h, msg, wp, lp);
        switch (msg) {
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_WINDOWPOSCHANGING: {
                auto* p = (WINDOWPOS*)lp;
                if (!(p->flags & SWP_NOZORDER) && !s_edit) p->hwndInsertAfter = HWND_BOTTOM;
                break;
            }
            case WM_LBUTTONDOWN:
                if (s_edit && w->hit != 99) {
                    POINT pt; GetCursorPos(&pt);
                    w->dragging = true;
                    w->dragOff = {pt.x - w->ov.x, pt.y - w->ov.y};
                    SetCapture(h);
                }
                return 0;
            case WM_MOUSEMOVE:
                if (w->dragging) {
                    POINT pt; GetCursorPos(&pt);
                    int grid = (int)(8 * s_s);
                    int nx = pt.x - w->dragOff.x, ny = pt.y - w->dragOff.y;
                    nx = (nx / grid) * grid; ny = (ny / grid) * grid;
                    w->ov.Move(nx, ny);
                    w->dirty = true;
                }
                return 0;
            case WM_LBUTTONUP:
                if (w->dragging) { w->dragging = false; ReleaseCapture(); SaveLayout(); return 0; }
                if (s_edit) {
                    if (w->hit == 99) PostMessageW(h, WM_APP + 77, 0, 0);   // remove
                    return 0;
                }
                w->Click();
                return 0;
            case WM_APP + 77: {   // removal, done outside the window's own click handling
                for (size_t i = 0; i < s_ws.size(); i++)
                    if (s_ws[i].get() == w) { s_ws[i]->ov.Destroy(); s_ws.erase(s_ws.begin() + i); break; }
                SaveLayout();
                return 0;
            }
            case WM_RBUTTONUP: {
                POINT pt; GetCursorPos(&pt);
                int c = GlassMenu(pt.x, pt.y, {{1, s_edit ? L"Done Editing" : L"Edit Widgets..."}, {0}, {2, L"Remove Widget"}});
                if (c == 1) widgets::SetEditing(!s_edit);
                else if (c == 2) PostMessageW(h, WM_APP + 77, 0, 0);
                return 0;
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    void Click() {
        switch (hit) {
            case 1: sys::MediaCommand(0); break;
            case 2: sys::MediaCommand(1); break;
            case 3: sys::MediaCommand(2); break;
            case 4: if (!s_media.app.empty()) OpenApp(s_media.app); break;
            case 5: Launch(L"outlookcal:"); break;
            case 6: Launch(L"ms-clock:"); break;
            case 7: OpenApp(L"claude"); break;
            case 8: Launch(L"discord://"); break;
            case 9: if (!hitArg.empty()) Launch(hitArg); break;
        }
        dirty = true;
    }

    GlassShape CardGlass(float tint, float frost) const {
        D2D1_RECT_F c = Card();
        GlassShape g{c.left, c.top, c.right - c.left, c.bottom - c.top};
        g.radius = 24 * s_s; g.blur = frost; g.bezel = 18 * s_s; g.thick = 2.4f;
        g.tint = tint; g.shadow = 0.18f; g.highlight = 0.0f; g.opacity = 1;
        return g;
    }

    // the music widget's backdrop: the cover blurred over the whole window, which the glass then
    // frosts and bends at its rim (instead of the wallpaper)
    void BuildCoverBackdrop() {
        if (!art) return;
        UINT W = ov.w, H = ov.h;
        if (!coverRT) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &coverRT))) return;
            ComPtr<IDXGISurface> surf;
            coverRT.As(&surf);
            auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            ov.dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &coverBmp);
            coverCap.MakeTexture(W, H);
            coverCap.have = true;
            coverCap.live = false;
        }
        if (!blur) {
            ov.dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur);
            if (blur) blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        }
        if (!coverBmp || !blur) return;
        ComPtr<ID2D1Image> old;
        ov.dc->GetTarget(&old);
        ov.dc->SetTarget(coverBmp.Get());
        ov.dc->BeginDraw();
        ov.dc->Clear(D2D1::ColorF(0.05f, 0.05f, 0.06f, 1));
        blur->SetInput(0, art.Get());
        auto sz = art->GetSize();
        float k = std::max(W / sz.width, H / sz.height) * 1.15f;
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 14.0f / std::max(k, 0.01f) * s_s);
        D2D1_MATRIX_3X2_F m0;
        ov.dc->GetTransform(&m0);
        ov.dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(W / 2.f - sz.width * k / 2, H / 2.f - sz.height * k / 2));
        ComPtr<ID2D1Image> out;
        blur->GetOutput(&out);
        ov.dc->DrawImage(out.Get(), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        ov.dc->SetTransform(m0);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.30f));
        ov.dc->FillRectangle(D2D1::RectF(0, 0, (float)W, (float)H), br.Get());
        ov.dc->EndDraw();
        ov.dc->SetTarget(old.Get());
        g.ctx->CopySubresourceRegion(coverCap.tex.Get(), 0, 0, 0, 0, coverRT.Get(), 0, nullptr);
        g.ctx->GenerateMips(coverCap.srv.Get());
    }

    void Render() {
        float s = s_s;
        btns.clear();
        D2D1_RECT_F c = Card();
        std::vector<GlassShape> sh;
        bool music = type == L"music";
        Capture* desk = g.CapFor(MonitorFromWindow(ov.hwnd, MONITOR_DEFAULTTONEAREST));
        if (music) {
            if (s_media.artVersion != artVer) { artVer = s_media.artVersion; art = DecodeImage(ov.dc.Get(), s_media.art, (int)(220 * s)); BuildCoverBackdrop(); }
            if (art && s_media.present && coverCap.have) {   // glass made of the blurred cover itself
                coverCap.rect = {ov.x, ov.y, ov.x + ov.w, ov.y + ov.h};
                ov.cap = &coverCap;
                sh.push_back(CardGlass(-0.05f, 1.2f));
            } else {
                ov.cap = desk;
                sh.push_back(CardGlass(-0.34f, 2.2f));
            }
        } else {
            ov.cap = desk;
            sh.push_back(CardGlass(-0.34f, 2.2f));
        }
        float tint[3] = {1, 1, 1};
        SYSTEMTIME now;
        GetLocalTime(&now);
        ov.Render(s, sh, tint, [&](ID2D1DeviceContext* dc) {
            auto text = [&](IDWriteTextFormat* f, const std::wstring& t, D2D1_RECT_F r, D2D1_COLOR_F col) {
                br->SetColor(col);
                dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            };
            D2D1_COLOR_F fg = D2D1::ColorF(1, 1, 1, 0.95f), fg2 = D2D1::ColorF(1, 1, 1, 0.56f), red = D2D1::ColorF(1.0f, 0.27f, 0.23f);
            float p = 16 * s;
            if (type == L"clock") DrawClock(dc, c, now, fg, fg2);
            else if (type == L"calendar") DrawCalendar(dc, c, now, fg, fg2, red, text);
            else if (music) DrawMusic(dc, c, fg, fg2, text);
            else if (type == L"system") {
                float y = c.top + p - 2 * s;
                text(fTiny.Get(), L"SYSTEM", D2D1::RectF(c.left + p, y, c.right - p, y + 14 * s), fg2);
                struct Row { const wchar_t* name; float v; } rows[3] = {{L"CPU", s_cpu}, {L"Memory", s_mem}, {L"GPU", s_gpu}};
                y += 22 * s;
                for (auto& r : rows) {   // black and white: a white bar on a faint track
                    text(fSmall.Get(), r.name, D2D1::RectF(c.left + p, y - 2 * s, c.left + p + 70 * s, y + 16 * s), fg);
                    float bl = c.left + p + 72 * s, brt = c.right - p - 42 * s, cy = y + 7 * s, th = 5 * s;
                    br->SetColor(D2D1::ColorF(1, 1, 1, 0.12f));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bl, cy - th / 2, brt, cy + th / 2), th / 2, th / 2), br.Get());
                    br->SetColor(D2D1::ColorF(1, 1, 1, 0.92f));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bl, cy - th / 2, bl + std::max(th, (brt - bl) * r.v), cy + th / 2), th / 2, th / 2), br.Get());
                    text(fSmall.Get(), std::to_wstring((int)std::lround(r.v * 100)) + L"%", D2D1::RectF(brt + 6 * s, y - 2 * s, c.right - p + 4 * s, y + 16 * s), fg2);
                    y += 26 * s;
                }
            } else if (type == L"claude") {
                btns.push_back({c, 7});
                float y = c.top + p - 2 * s;
                text(fTiny.Get(), L"CLAUDE CODE", D2D1::RectF(c.left + p, y, c.right - p, y + 14 * s), fg2);
                y += 20 * s;
                if (s_sessions.empty()) text(fBody.Get(), L"No sessions yet. Claude's work shows up here.", D2D1::RectF(c.left + p, y, c.right - p, y + 40 * s), fg2);
                double t = GetTickCount64() / 1000.0;
                for (size_t i = 0; i < s_sessions.size() && i < 3; i++) {
                    auto& se = s_sessions[i];
                    bool working = se.state == L"working", waiting = se.state == L"waiting";
                    float a = working ? 0.55f + 0.45f * (float)std::sin(t * 5) : 1.f;
                    br->SetColor(working ? D2D1::ColorF(1, 1, 1, a) : waiting ? D2D1::ColorF(1, 1, 1, 0.9f) : D2D1::ColorF(1, 1, 1, 0.25f));
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.left + p + 4 * s, y + 9 * s), 3.5f * s, 3.5f * s), br.Get());
                    std::wstring head = se.name + (working ? L"  ·  working" : waiting ? L"  ·  needs you" : L"  ·  done");
                    text(fSmall.Get(), head, D2D1::RectF(c.left + p + 14 * s, y, c.right - p, y + 18 * s), fg);
                    std::wstring detail = working ? se.prompt : (!se.reply.empty() ? se.reply : se.prompt);
                    if (!detail.empty()) text(fSmall.Get(), detail, D2D1::RectF(c.left + p + 14 * s, y + 17 * s, c.right - p, y + 34 * s), fg2);
                    y += 40 * s;
                }
            } else if (type == L"discord") {
                float y = c.top + p - 2 * s;
                text(fTiny.Get(), L"DISCORD", D2D1::RectF(c.left + p, y, c.right - p, y + 14 * s), fg2);
                D2D1_RECT_F open = D2D1::RectF(c.right - p - 60 * s, y - 3 * s, c.right - p, y + 15 * s);
                btns.push_back({open, 8});
                br->SetColor(D2D1::ColorF(1, 1, 1, hit == 8 ? 0.22f : 0.12f));
                dc->FillRoundedRectangle(D2D1::RoundedRect(open, 9 * s, 9 * s), br.Get());
                fTiny->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                text(fTiny.Get(), L"Open", D2D1::RectF(open.left, open.top + 3 * s, open.right, open.bottom), fg);
                fTiny->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                y += 22 * s;
                if (s_discord.empty()) text(fBody.Get(), L"No new messages", D2D1::RectF(c.left + p, y, c.right - p, y + 20 * s), fg2);
                for (size_t i = 0; i < s_discord.size() && i < 3; i++) {
                    auto& n = s_discord[i];
                    D2D1_RECT_F row = D2D1::RectF(c.left + p - 6 * s, y - 3 * s, c.right - p + 6 * s, y + 36 * s);
                    btns.push_back({row, 8});
                    text(fSmall.Get(), n.title, D2D1::RectF(c.left + p, y, c.right - p, y + 17 * s), fg);
                    text(fSmall.Get(), n.body, D2D1::RectF(c.left + p, y + 16 * s, c.right - p, y + 33 * s), fg2);
                    y += 40 * s;
                }
            } else if (type == L"launcher") {
                size_t n = std::min<size_t>(layout::launch.size(), 6);
                if (launchIcons.size() != n) {
                    launchIcons.clear();
                    for (size_t i = 0; i < n; i++) launchIcons.push_back(LoadIcons(dc, layout::launch[i].target, (int)(96 * s), IconOverride(layout::launch[i].name, layout::launch[i].target)));
                }
                if (n == 0) text(fBody.Get(), L"Right-click an app in Spotlight to add it", D2D1::RectF(c.left + p, c.top + p, c.right - p, c.bottom - p), fg2);
                float cell = (c.right - c.left - 2 * p) / 6, ic = 44 * s, cy = (c.top + c.bottom) / 2;
                for (size_t i = 0; i < n; i++) {
                    float cx = c.left + p + cell * (i + 0.5f);
                    D2D1_RECT_F r = D2D1::RectF(cx - ic / 2, cy - ic / 2, cx + ic / 2, cy + ic / 2);
                    btns.push_back({r, 9, layout::launch[i].target});
                    auto& im = launchIcons[i];
                    ID2D1Bitmap1* bmp = im.custom ? im.color.Get() : im.silver ? im.silver.Get() : im.color.Get();
                    if (bmp) dc->DrawBitmap(bmp, &r, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                }
            }
            if (s_edit) {   // edit mode: a remove button on the corner
                float cr = 11 * s, cx = c.left + 4 * s, cy = c.top + 4 * s;
                D2D1_RECT_F xr = D2D1::RectF(cx - cr, cy - cr, cx + cr, cy + cr);
                btns.push_back({xr, 99});
                br->SetColor(D2D1::ColorF(0.22f, 0.22f, 0.24f, 1));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), cr, cr), br.Get());
                br->SetColor(D2D1::ColorF(1, 1, 1, 0.9f));
                dc->FillRectangle(D2D1::RectF(cx - 5 * s, cy - 1 * s, cx + 5 * s, cy + 1 * s), br.Get());
            }
        });
    }

    void DrawClock(ID2D1DeviceContext* dc, D2D1_RECT_F c, const SYSTEMTIME& now, D2D1_COLOR_F fg, D2D1_COLOR_F fg2) {
        float s = s_s;
        btns.push_back({c, 6});
        float cx = (c.left + c.right) / 2, cy = (c.top + c.bottom) / 2, rr = std::min(c.right - c.left, c.bottom - c.top) / 2 - 16 * s;
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.3f));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rr, rr), br.Get());
        for (int k = 0; k < 12; k++) {
            float an = k * 3.14159265f / 6, r1 = rr - 4 * s, r0 = rr - (k % 3 == 0 ? 12 : 7) * s;
            br->SetColor(k % 3 == 0 ? fg : fg2);
            dc->DrawLine(D2D1::Point2F(cx + std::sin(an) * r0, cy - std::cos(an) * r0), D2D1::Point2F(cx + std::sin(an) * r1, cy - std::cos(an) * r1),
                         br.Get(), (k % 3 == 0 ? 2.2f : 1.2f) * s);
        }
        float sec = now.wSecond + now.wMilliseconds / 1000.f, mn = now.wMinute + sec / 60, hr = (now.wHour % 12) + mn / 60;
        ComPtr<ID2D1StrokeStyle> round;
        D2D1_STROKE_STYLE_PROPERTIES sp{D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND};
        g.d2dFactory->CreateStrokeStyle(sp, nullptr, 0, &round);
        auto hand = [&](float frac, float len, float w, D2D1_COLOR_F col) {
            float an = frac * 2 * 3.14159265f;
            br->SetColor(col);
            dc->DrawLine(D2D1::Point2F(cx - std::sin(an) * len * 0.12f, cy + std::cos(an) * len * 0.12f),
                         D2D1::Point2F(cx + std::sin(an) * len, cy - std::cos(an) * len), br.Get(), w, round.Get());
        };
        hand(hr / 12, rr * 0.52f, 4 * s, fg);
        hand(mn / 60, rr * 0.8f, 2.8f * s, fg);
        hand(sec / 60, rr * 0.88f, 1.2f * s, D2D1::ColorF(1.0f, 0.6f, 0.0f));
        br->SetColor(D2D1::ColorF(1.0f, 0.6f, 0.0f));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3 * s, 3 * s), br.Get());
    }

    template <class T>
    void DrawCalendar(ID2D1DeviceContext* dc, D2D1_RECT_F c, const SYSTEMTIME& now, D2D1_COLOR_F fg, D2D1_COLOR_F fg2, D2D1_COLOR_F red, T& text) {
        float s = s_s, p = 14 * s;
        btns.push_back({c, 5});
        wchar_t mon[32];
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"MMMM", mon, 32, nullptr);
        std::wstring mt = mon;
        for (auto& ch : mt) ch = towupper(ch);
        text(fDow.Get(), mt, D2D1::RectF(c.left + p, c.top + p - 4 * s, c.right - p, c.top + p + 12 * s), red);
        const wchar_t* wd[7] = {L"S", L"M", L"T", L"W", L"T", L"F", L"S"};
        float gx = c.left + 10 * s, gw = (c.right - c.left - 20 * s) / 7, gy = c.top + 32 * s, rh = 17 * s;
        for (int k = 0; k < 7; k++) text(fCalHead.Get(), wd[k], D2D1::RectF(gx + k * gw, gy, gx + (k + 1) * gw, gy + 13 * s), fg2);
        SYSTEMTIME first = now;
        first.wDay = 1;
        FILETIME ft;
        SystemTimeToFileTime(&first, &ft);
        FileTimeToSystemTime(&ft, &first);
        static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int days = dim[now.wMonth - 1];
        if (now.wMonth == 2 && ((now.wYear % 4 == 0 && now.wYear % 100 != 0) || now.wYear % 400 == 0)) days = 29;
        for (int d = 1; d <= days; d++) {
            int cell = first.wDayOfWeek + d - 1, row = cell / 7, col = cell % 7;
            D2D1_RECT_F cr = D2D1::RectF(gx + col * gw, gy + 15 * s + row * rh, gx + (col + 1) * gw, gy + 15 * s + (row + 1) * rh);
            bool today = d == now.wDay;
            if (today) {
                br->SetColor(red);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2), 8.5f * s, 8.5f * s), br.Get());
            }
            text(fCalDay.Get(), std::to_wstring(d), cr, today ? D2D1::ColorF(1, 1, 1) : (col == 0 || col == 6 ? fg2 : fg));
        }
    }

    template <class T>
    void DrawMusic(ID2D1DeviceContext* dc, D2D1_RECT_F c, D2D1_COLOR_F fg, D2D1_COLOR_F fg2, T& text) {
        float s = s_s, p = 18 * s;
        D2D1_RECT_F ar = D2D1::RectF(c.left + p, c.top + p, c.left + p + 80 * s, c.top + p + 80 * s);
        if (art && s_media.present) {
            ComPtr<ID2D1RoundedRectangleGeometry> ag;
            g.d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(ar, 12 * s, 12 * s), &ag);
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), ag.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
            dc->DrawBitmap(art.Get(), &ar, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            dc->PopLayer();
        } else {
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.1f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(ar, 12 * s, 12 * s), br.Get());
            text(fGlyphBig.Get(), L"", ar, fg2);
        }
        float tx = ar.right + 14 * s;
        btns.push_back({D2D1::RectF(ar.left, ar.top, c.right - p, ar.bottom), 4});
        text(fTitle.Get(), s_media.present ? (s_media.title.empty() ? s_media.app : s_media.title) : L"Not Playing",
             D2D1::RectF(tx, ar.top + 12 * s, c.right - p, ar.top + 34 * s), s_media.present ? fg : fg2);
        text(fBody.Get(), s_media.present ? (s_media.artist.empty() ? s_media.app : s_media.artist) : L"",
             D2D1::RectF(tx, ar.top + 34 * s, c.right - p, ar.top + 54 * s), fg2);
        const wchar_t* gl[3] = {L"", s_media.playing ? L"" : L"", L""};
        float bw = 48 * s, by = c.bottom - p - 30 * s, bx = (c.left + c.right) / 2 - bw * 1.5f;
        for (int i = 0; i < 3; i++) {
            D2D1_RECT_F b = D2D1::RectF(bx + i * bw, by, bx + (i + 1) * bw, by + 30 * s);
            btns.push_back({b, 1 + i});
            if (hit == 1 + i) {
                br->SetColor(D2D1::ColorF(1, 1, 1, 0.16f));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((b.left + b.right) / 2, (b.top + b.bottom) / 2), 16 * s, 16 * s), br.Get());
            }
            text(i == 1 ? fGlyphBig.Get() : fGlyph.Get(), gl[i], b, s_media.present ? fg : fg2);
        }
    }

    void Tick(bool nf, bool dataChanged) {
        ULONGLONG now = GetTickCount64();
        if (!s_edit && now - lastBottom > 2000) {
            lastBottom = now;
            if (!GetWindowLongPtrW(ov.hwnd, GWLP_HWNDPARENT)) SinkToDesktop(ov.hwnd);   // desktop appeared after us
            SetWindowPos(ov.hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        POINT p;
        GetCursorPos(&p);
        float x = (float)(p.x - ov.x), y = (float)(p.y - ov.y);
        D2D1_RECT_F c = Card();
        bool over = x >= c.left - 12 * s_s && x <= c.right && y >= c.top - 12 * s_s && y <= c.bottom;
        int h = -1;
        std::wstring arg;
        if (over) for (auto& b : btns) if (x >= b.r.left && x <= b.r.right && y >= b.r.top && y <= b.r.bottom) { h = b.id; arg = b.arg; }
        if (h != hit || arg != hitArg) { hit = h; hitArg = arg; dirty = true; }
        ov.SetClickThrough(!over && !dragging);
        SYSTEMTIME st;
        GetLocalTime(&st);
        bool tick = (type == L"clock" || type == L"claude") && st.wSecond != lastSec;
        if (tick) lastSec = st.wSecond;
        if (type == L"claude" && !s_sessions.empty() && s_sessions[0].state == L"working") tick = true;   // pulse
        bool glass = nf && type != L"music" && ov.cap && ov.cap->fresh && now - lastGlass > 120;
        if (dirty || dataChanged || tick || glass || g.snapPending) { dirty = false; lastGlass = now; Render(); }
    }
};

// ---------------------------------------------------------------- the gallery (edit mode)
Overlay s_gal;
ComPtr<ID2D1SolidColorBrush> s_galBr;
int s_galHit = -1;
struct GBtn { D2D1_RECT_F r; int id; };
std::vector<GBtn> s_galBtns;

void SaveLayout() {
    layout::widgets.clear();
    for (auto& w : s_ws) { POINT p = w->Pos(); layout::widgets.push_back({w->type, p.x, p.y}); }
    layout::Save();
}

void AddWidget(const std::wstring& type, int x, int y) {
    auto w = std::make_unique<Widget>();
    w->type = type;
    if (w->Create(x, y)) s_ws.push_back(std::move(w));
}

void RenderGallery() {
    float s = s_s;
    s_galBtns.clear();
    float m = 30 * s;
    D2D1_RECT_F panel = D2D1::RectF(m, m, s_gal.w - m, s_gal.h - m);
    GlassShape gs{panel.left, panel.top, panel.right - panel.left, panel.bottom - panel.top};
    gs.radius = 26 * s; gs.blur = 2.4f; gs.bezel = 18 * s; gs.thick = 2.4f; gs.tint = -0.4f; gs.shadow = 0.25f; gs.highlight = 0;
    float tint[3] = {1, 1, 1};
    s_gal.Render(s, {gs}, tint, [&](ID2D1DeviceContext* dc) {
        auto text = [&](IDWriteTextFormat* f, const std::wstring& t, D2D1_RECT_F r, D2D1_COLOR_F c) {
            s_galBr->SetColor(c);
            dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, s_galBr.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        text(fTitle.Get(), L"Add Widgets  ·  drag widgets to move them", D2D1::RectF(panel.left + 20 * s, panel.top + 14 * s, panel.right - 110 * s, panel.top + 36 * s), D2D1::ColorF(1, 1, 1));
        D2D1_RECT_F done = D2D1::RectF(panel.right - 96 * s, panel.top + 12 * s, panel.right - 18 * s, panel.top + 38 * s);
        s_galBtns.push_back({done, 100});
        s_galBr->SetColor(D2D1::ColorF(1, 1, 1, s_galHit == 100 ? 1.f : 0.9f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(done, 13 * s, 13 * s), s_galBr.Get());
        fSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        text(fSmall.Get(), L"Done", D2D1::RectF(done.left, done.top + 5 * s, done.right, done.bottom), D2D1::ColorF(0, 0, 0));
        int n = (int)(sizeof(kKinds) / sizeof(kKinds[0]));
        float cw = (panel.right - panel.left - 40 * s) / n, y = panel.top + 52 * s;
        const wchar_t* gl[] = {L"", L"", L"", L"", L"", L"", L""};
        for (int i = 0; i < n; i++) {
            D2D1_RECT_F b = D2D1::RectF(panel.left + 20 * s + i * cw + 4 * s, y, panel.left + 20 * s + (i + 1) * cw - 4 * s, panel.bottom - 16 * s);
            s_galBtns.push_back({b, i});
            s_galBr->SetColor(D2D1::ColorF(1, 1, 1, s_galHit == i ? 0.16f : 0.07f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(b, 14 * s, 14 * s), s_galBr.Get());
            text(fGlyphBig.Get(), gl[i], D2D1::RectF(b.left, b.top + 10 * s, b.right, b.top + 44 * s), D2D1::ColorF(1, 1, 1, 0.9f));
            text(fSmall.Get(), kKinds[i].title, D2D1::RectF(b.left, b.top + 48 * s, b.right, b.top + 66 * s), D2D1::ColorF(1, 1, 1, 0.85f));
            text(fSmall.Get(), L"+ Add", D2D1::RectF(b.left, b.bottom - 24 * s, b.right, b.bottom - 6 * s), D2D1::ColorF(1, 1, 1, 0.5f));
        }
        fSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    });
}

LRESULT CALLBACK GalProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_LBUTTONUP) {
        if (s_galHit == 100) widgets::SetEditing(false);
        else if (s_galHit >= 0) {
            // the new widget lands in the middle of the main screen; drag it where you want
            HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(hm, &mi);
            auto& k = kKinds[s_galHit];
            AddWidget(k.type, (mi.rcWork.left + mi.rcWork.right) / 2 - (int)(k.w * s_s / 2), mi.rcWork.top + (int)(160 * s_s));
            SaveLayout();
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void DefaultLayout() {
    // right side of the main screen, a tidy column (icons live on the left)
    HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    float s = s_s;
    int gap = (int)(14 * s), right = mi.rcWork.right - (int)(24 * s), top = mi.rcWork.top + (int)(20 * s);
    int wide = (int)(330 * s), smallW = (int)(158 * s), x0 = right - wide;
    int y1 = top + smallW + gap, y2 = y1 + (int)(158 * s) + gap, y3 = y2 + (int)(176 * s) + gap;
    layout::widgets = {{L"clock", x0, top}, {L"calendar", x0 + smallW + gap, top}, {L"music", x0, y1}, {L"claude", x0, y2}, {L"system", x0, y3}};
}
}  // namespace

namespace widgets {
bool Editing() { return s_edit; }

void SetEditing(bool on) {
    if (on == s_edit) return;
    s_edit = on;
    if (on) {
        HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(hm, &mi);
        int w = (int)(860 * s_s), h = (int)(200 * s_s);
        int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = mi.rcWork.bottom - h - (int)(90 * s_s);
        if (!s_gal.hwnd) {
            s_gal.Create(L"LiquidWidgetGallery", GalProc, x, y, w, h, nullptr);
            s_gal.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &s_galBr);
        } else { s_gal.Move(x, y); s_gal.SetVisible(true); }
        for (auto& wd : s_ws) SetWindowPos(wd->ov.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        if (s_gal.hwnd) s_gal.SetVisible(false);
        for (auto& wd : s_ws) SetWindowPos(wd->ov.hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SaveLayout();
    }
    for (auto& wd : s_ws) wd->dirty = true;
}

bool Init() {
    if (!cfg.widgets) return true;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dx, &dy);
    s_s = dx / 96.f;
    float s = s_s;
    const wchar_t* ui = L"Segoe UI Variable Text";
    fTitle = MakeFont(ui, 15 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    fBody = MakeFont(ui, 13 * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fSmall = MakeFont(ui, 12 * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fTiny = MakeFont(ui, 10.5f * s, DWRITE_FONT_WEIGHT_BOLD, false);
    fDow = MakeFont(ui, 11 * s, DWRITE_FONT_WEIGHT_BOLD, false);
    fCalHead = MakeFont(ui, 9.5f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    fCalDay = MakeFont(ui, 10.5f * s, DWRITE_FONT_WEIGHT_MEDIUM, true);
    fGlyph = MakeFont(L"Segoe Fluent Icons", 15 * s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fGlyphBig = MakeFont(L"Segoe Fluent Icons", 22 * s, DWRITE_FONT_WEIGHT_NORMAL, true);
    if (layout::widgets.empty()) DefaultLayout();
    for (auto& wp : layout::widgets) AddWidget(wp.type, wp.x, wp.y);
    SaveLayout();
    PollSystem();
    return true;
}

void Tick(bool nf) {
    ULONGLONG now = GetTickCount64();
    bool changed = false;
    if (now - s_lastPoll > 1000) {
        s_lastPoll = now;
        PollSystem();
        PollClaude();
        s_media = sys::Media();
        s_discord.clear();
        for (auto& n : sys::Notifications()) if (Lower(n.app).find(L"discord") != std::wstring::npos) s_discord.push_back(n);
        changed = true;
    }
    for (auto& w : s_ws) w->Tick(nf, changed);
    if (s_edit && s_gal.hwnd) {
        POINT p;
        GetCursorPos(&p);
        float x = (float)(p.x - s_gal.x), y = (float)(p.y - s_gal.y);
        int h = -1;
        for (auto& b : s_galBtns) if (x >= b.r.left && x <= b.r.right && y >= b.r.top && y <= b.r.bottom) h = b.id;
        static int lastH = -2;
        static ULONGLONG lastG = 0;
        s_galHit = h;
        bool inside = x >= 30 * s_s && y >= 30 * s_s && x <= s_gal.w - 30 * s_s && y <= s_gal.h - 30 * s_s;
        s_gal.SetClickThrough(!inside);
        if (h != lastH || now - lastG > 120 || g.snapPending) { lastH = h; lastG = now; RenderGallery(); }
    }
}

void Shutdown() {
    if (s_edit) SaveLayout();
    for (auto& w : s_ws) w->ov.Destroy();
    s_ws.clear();
    if (s_gal.hwnd) s_gal.Destroy();
    if (s_q) PdhCloseQuery(s_q);
    s_q = nullptr;
}
}  // namespace widgets
