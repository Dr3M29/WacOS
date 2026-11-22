#include "shell.h"
#include <shellscalingapi.h>
#include <shlobj.h>
#include <cmath>
#include <algorithm>
#include <map>

// Dynamic Island: a black pill in the middle of the menu bar.
//  * Claude working  -> a pulsing Claude spark + "Working" (from Claude Code hooks, see
//    tools\claude-hook.js: each session writes its state to %LOCALAPPDATA%\Liquid\claude\)
//  * music playing   -> the album art and a little equalizer
//  * Claude finished -> the island lights up: an orange glow pulses round it, with "Done" and
//    the project folder
// Hover it and it opens up: now playing with controls and the Claude sessions.
//  * Face ID      -> on the lock screen, when Air Ring sees a face, the island grows into a
//    black square with the Face ID glyph: a check when it's you (then Liquid unlocks), a shake if not.

namespace {
Overlay s_ov;
float s_s = 1, s_w = 0, s_wv = 0, s_h = 0, s_hv = 0, s_open = 0, s_openV = 0;
bool s_hover = false, s_dirty = true;
int s_working = 0, s_waiting = 0;
std::vector<std::wstring> s_sessions;   // short names of working sessions
double s_doneUntil = 0;
const double kDoneSecs = 5.0;
std::wstring s_doneName;                 // folder of the session that just finished
std::map<std::wstring, std::wstring> s_lastState;   // session file -> state at the previous scan
std::wstring s_pkAction, s_pkSite;       // passkey check in progress (keys\host\LiquidKeys.cpp writes pending.txt)
ULONGLONG s_lastScan = 0;
int s_hit = -1;
struct Btn { D2D1_RECT_F r; int id; };
std::vector<Btn> s_btns;
ComPtr<ID2D1SolidColorBrush> s_br;
ComPtr<IDWriteTextFormat> fSmall, fTitle, fBody, fGlyph, fGlyphBig, fSpark;
ComPtr<ID2D1Bitmap1> s_art;
ComPtr<ID2D1Effect> s_blur;
int s_artVer = -1;
MediaInfo s_media;
const float kOvW = 440, kOvH = 170;
bool s_face = false;                       // Face ID square showing
double s_fSeen = -10, s_fOk = 0, s_fFail = -10;
ComPtr<ID2D1StrokeStyle> s_round;
// A new notification drops into the island for a few seconds (Notification Center passes it on).
Notif s_note;
double s_noteUntil = 0;
std::vector<Notif> s_noteQueue;
std::map<std::wstring, ComPtr<ID2D1Bitmap1>> s_noteIcons;   // aumid -> app icon
const double kNoteSecs = 5.0;
double Now();
bool NoteShowing() { return Now() < s_noteUntil; }
ComPtr<ID2D1Bitmap1> NoteIcon(const std::wstring& aumid) {
    if (aumid.empty()) return nullptr;
    auto it = s_noteIcons.find(aumid);
    if (it != s_noteIcons.end()) return it->second;
    auto ic = LoadIcons(s_ov.dc.Get(), L"shell:AppsFolder\\" + aumid, (int)(38 * s_s * 1.5f));
    s_noteIcons[aumid] = ic.color;
    return ic.color;
}

// Face ID glyph in a 2x2 box around (cx, cy): corner brackets, eyes, nose, smile. ok (0..1)
// swaps the face for a check that draws itself in. Also drawn by the Lock & Face ID settings
// window, which brings its own brush.
void DrawFaceIdWith(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, float cx, float cy, float half, float a, float ok) {
    ComPtr<ID2D1SolidColorBrush> s_br(brush);   // the caller's brush, not the island's
    if (!s_round) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        g.d2dFactory->CreateStrokeStyle(sp, nullptr, 0, &s_round);
    }
    auto P = [&](float x, float y) { return D2D1::Point2F(cx + x * half, cy + y * half); };
    float sw = 0.085f * half;
    ComPtr<ID2D1PathGeometry> path;
    g.d2dFactory->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sk;
    path->Open(&sk);
    const float L = 0.42f, rc = 0.3f;
    for (int i = 0; i < 4; i++) {
        float sx = (i & 1) ? 1.f : -1.f, sy = (i & 2) ? 1.f : -1.f;
        sk->BeginFigure(P(sx, sy * (1 - L)), D2D1_FIGURE_BEGIN_HOLLOW);
        sk->AddLine(P(sx, sy * (1 - rc)));
        sk->AddQuadraticBezier(D2D1::QuadraticBezierSegment(P(sx, sy), P(sx * (1 - rc), sy)));
        sk->AddLine(P(sx * (1 - L), sy));
        sk->EndFigure(D2D1_FIGURE_END_OPEN);
    }
    sk->Close();
    s_br->SetColor(D2D1::ColorF(1, 1, 1, a));
    dc->DrawGeometry(path.Get(), s_br.Get(), sw, s_round.Get());
    float fa = a * std::clamp(1 - ok * 3, 0.f, 1.f);   // face fades out fast once matched
    if (fa > 0.01f) {
        s_br->SetColor(D2D1::ColorF(1, 1, 1, fa));
        dc->DrawLine(P(-0.36f, -0.4f), P(-0.36f, -0.18f), s_br.Get(), sw, s_round.Get());
        dc->DrawLine(P(0.36f, -0.4f), P(0.36f, -0.18f), s_br.Get(), sw, s_round.Get());
        dc->DrawLine(P(0.03f, -0.38f), P(0.03f, 0.1f), s_br.Get(), sw, s_round.Get());
        dc->DrawLine(P(0.03f, 0.1f), P(-0.1f, 0.1f), s_br.Get(), sw, s_round.Get());
        ComPtr<ID2D1PathGeometry> m;
        g.d2dFactory->CreatePathGeometry(&m);
        ComPtr<ID2D1GeometrySink> ms;
        m->Open(&ms);
        ms->BeginFigure(P(-0.38f, 0.34f), D2D1_FIGURE_BEGIN_HOLLOW);
        ms->AddQuadraticBezier(D2D1::QuadraticBezierSegment(P(0, 0.62f), P(0.38f, 0.34f)));
        ms->EndFigure(D2D1_FIGURE_END_OPEN);
        ms->Close();
        dc->DrawGeometry(m.Get(), s_br.Get(), sw, s_round.Get());
    }
    float cp = std::clamp((ok - 0.25f) / 0.5f, 0.f, 1.f);   // the check draws in after the face is gone
    if (cp > 0) {
        D2D1_POINT_2F a0 = P(-0.4f, 0.02f), a1 = P(-0.1f, 0.32f), a2 = P(0.44f, -0.3f);
        float l1 = std::hypot(a1.x - a0.x, a1.y - a0.y), l2 = std::hypot(a2.x - a1.x, a2.y - a1.y), d = cp * (l1 + l2);
        s_br->SetColor(D2D1::ColorF(1, 1, 1, a));
        float k1 = std::min(d / l1, 1.f);
        dc->DrawLine(a0, D2D1::Point2F(a0.x + (a1.x - a0.x) * k1, a0.y + (a1.y - a0.y) * k1), s_br.Get(), sw * 1.2f, s_round.Get());
        if (d > l1) {
            float k2 = (d - l1) / l2;
            dc->DrawLine(a1, D2D1::Point2F(a1.x + (a2.x - a1.x) * k2, a1.y + (a2.y - a1.y) * k2), s_br.Get(), sw * 1.2f, s_round.Get());
        }
    }
}

void DrawFaceId(ID2D1DeviceContext* dc, float cx, float cy, float half, float a, float ok) {
    DrawFaceIdWith(dc, s_br.Get(), cx, cy, half, a, ok);
}

double Now() { return GetTickCount64() / 1000.0; }

std::wstring LocalAppData() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) r = p;
    if (p) CoTaskMemFree(p);
    return r;
}

std::wstring StatusDir() {
    std::wstring base = LocalAppData();
    return base.empty() ? base : base + L"\\Liquid\\claude";
}

// Every folder Claude Code hooks may have written to. The Claude desktop app is a packaged
// (MSIX) app, so what its hooks write under AppData\Local really lands in the package's
// LocalCache, where nothing outside the package sees it. Claude Code run from a normal
// terminal writes to the real folder. Re-checked every 10 s.
const std::vector<std::wstring>& StatusDirs() {
    static std::vector<std::wstring> dirs;
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (!dirs.empty() && now - last < 10000) return dirs;
    last = now;
    dirs.clear();
    std::wstring base = LocalAppData();
    if (base.empty()) return dirs;
    dirs.push_back(base + L"\\Liquid\\claude");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"\\Packages\\Claude_*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring d = base + L"\\Packages\\" + fd.cFileName + L"\\LocalCache\\Local\\Liquid\\claude";
            DWORD a = GetFileAttributesW(d.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) dirs.push_back(d);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return dirs;
}

std::wstring ReadSmall(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    char buf[2048] = {};
    DWORD rd = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(f);
    int n = MultiByteToWideChar(CP_UTF8, 0, buf, (int)rd, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, buf, (int)rd, w.data(), n);
    return w;
}

std::wstring Field(const std::wstring& json, const wchar_t* key) {   // tiny "key":"value" reader
    std::wstring k = std::wstring(L"\"") + key + L"\"";
    size_t p = json.find(k);
    if (p == std::wstring::npos) return L"";
    p = json.find(L'"', json.find(L':', p) + 1);
    if (p == std::wstring::npos) return L"";
    size_t e = json.find(L'"', p + 1);
    return e == std::wstring::npos ? L"" : json.substr(p + 1, e - p - 1);
}

std::wstring FolderName(std::wstring cwd) {
    for (auto& c : cwd) if (c == L'/') c = L'\\';
    while (!cwd.empty() && cwd.back() == L'\\') cwd.pop_back();
    size_t sl = cwd.find_last_of(L'\\');
    std::wstring name = cwd.substr(sl == std::wstring::npos ? 0 : sl + 1);
    return name.empty() ? cwd : name;
}

void ScanClaude() {
    int working = 0, waiting = 0;
    std::vector<std::wstring> names;
    std::map<std::wstring, std::wstring> states;
    std::wstring finished;
    bool anyFinished = false;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULONGLONG now = ((ULONGLONG)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
    for (const std::wstring& dir : StatusDirs()) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            double ageMin = (now - t) / 600000000.0;
            std::wstring path = dir + L"\\" + fd.cFileName;
            if (ageMin > 24 * 60) { DeleteFileW(path.c_str()); continue; }   // old sessions
            std::wstring j = ReadSmall(path);
            std::wstring st = Field(j, L"state");
            states[path] = st;
            // Finished = this session's Stop hook ran since the last scan (it was working, or
            // waiting on a permission prompt, and is now idle).
            auto prev = s_lastState.find(path);
            if (prev != s_lastState.end() && prev->second != L"idle" && st == L"idle" && Field(j, L"event") == L"Stop") {
                anyFinished = true;
                std::wstring cwd = Field(j, L"cwd");
                if (!cwd.empty()) finished = FolderName(cwd);
            }
            if (st == L"working" && ageMin < 45) {
                working++;
                std::wstring cwd = Field(j, L"cwd");
                names.push_back(cwd.empty() ? L"Claude" : FolderName(cwd));
            } else if (st == L"waiting" && ageMin < 45) waiting++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    s_lastState = std::move(states);
    if (anyFinished) {
        s_doneUntil = Now() + kDoneSecs;
        s_doneName = finished;
        s_dirty = true;
        Log(L"island: Claude finished%s%s", finished.empty() ? L"" : L" in ", finished.c_str());
    }
    if (working != s_working || waiting != s_waiting) Log(L"island: Claude %d working, %d waiting", working, waiting);
    if (working != s_working || waiting != s_waiting || names != s_sessions) s_dirty = true;
    s_working = working;
    s_waiting = waiting;
    s_sessions = names;
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONUP:
            if (NoteShowing()) {   // click a notification: open its app, like a banner
                if (!s_note.aumid.empty()) Launch(L"shell:AppsFolder\\" + s_note.aumid);
                s_noteUntil = 0;
                s_dirty = true;
                return 0;
            }
            if (s_hit == 1) sys::MediaCommand(0);
            else if (s_hit == 2) sys::MediaCommand(1);
            else if (s_hit == 3) sys::MediaCommand(2);
            else if (s_hit == 4 && !s_media.app.empty()) {   // tap the song: bring the player up
                EnumWindows([](HWND w, LPARAM lp) -> BOOL {
                    auto* app = (std::wstring*)lp;
                    wchar_t t[256];
                    GetWindowTextW(w, t, 256);
                    if (IsAppWindow(w) && Lower(ProcessPath(WindowPid(w))).find(Lower(*app)) != std::wstring::npos) { ActivateWindow(w); return FALSE; }
                    return TRUE;
                }, (LPARAM)&s_media.app);
            }
            s_dirty = true;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Render() {
    float s = s_s, W = s_w, H = s_h, cx = s_ov.w / 2.f;
    D2D1_RECT_F pill = D2D1::RectF(cx - W / 2, 2 * s, cx + W / 2, 2 * s + H);
    float r = std::min(H / 2, 22 * s + (H - 20 * s) * 0.15f);
    float open = std::clamp(s_open, 0.f, 1.f);
    s_btns.clear();
    float tint[3] = {1, 1, 1};
    double t = Now();
    s_ov.Render(s, {}, tint, [&](ID2D1DeviceContext* dc) {
        if (W < 2) return;
        D2D1_COLOR_F claude = D2D1::ColorF(0.85f, 0.47f, 0.34f, 1);   // Claude's orange
        bool done = t < s_doneUntil;
        // Claude finished: an orange glow round the pill, three pulses, then a steady glow that
        // fades out as the island closes.
        float glow = 0;
        if (done && !s_face) {
            double since = t - (s_doneUntil - kDoneSecs);
            double p = since < 2.7 ? std::sin(3.14159265 * since / 0.9) : 0.0;
            glow = (float)((0.45 + 0.55 * p * p) * std::clamp((s_doneUntil - t) / 0.6, 0.0, 1.0));
            for (int i = 7; i >= 1; i--) {
                float gd = i * 1.5f * s;
                D2D1_RECT_F gr = D2D1::RectF(pill.left - gd, pill.top - gd, pill.right + gd, pill.bottom + gd);
                s_br->SetColor(D2D1::ColorF(claude.r, claude.g, claude.b, glow * 0.2f * (8 - i) / 7.f));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(gr, r + gd, r + gd), s_br.Get(), 1.6f * s);
            }
        }
        s_br->SetColor(D2D1::ColorF(0, 0, 0, 1));
        dc->FillRoundedRectangle(D2D1::RoundedRect(pill, r, r), s_br.Get());
        if (glow > 0) {   // a lit rim on the pill itself
            s_br->SetColor(D2D1::ColorF(claude.r, claude.g, claude.b, glow));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(pill, r, r), s_br.Get(), 1.5f * s);
        }
        auto text = [&](IDWriteTextFormat* f, const std::wstring& str, D2D1_RECT_F rc, D2D1_COLOR_F c) {
            s_br->SetColor(c);
            dc->DrawTextW(str.c_str(), (UINT32)str.size(), f, rc, s_br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        if (s_face) {
            float ok = s_fOk ? (float)std::clamp((t - s_fOk) / 0.6, 0.0, 1.0) : 0.f;
            double ft = t - s_fFail;
            float shake = ft < 0.6 ? (float)(std::sin(ft * 42) * std::exp(-ft * 6) * 9 * s) : 0.f;
            float breathe = ok > 0 ? 1.f : 1 + 0.035f * (float)std::sin(t * 4.5);
            float half = std::min(W, H) * 0.27f * breathe;
            float ga = std::clamp((std::min(W, H) - 60 * s) / (40 * s), 0.f, 1.f);   // glyph fades in as the square grows
            bool passkey = !s_pkSite.empty() && !lock::IsLocked();
            // a passkey: the Face ID mark sits in the square at the left, what's happening beside it
            float gx = passkey ? pill.left + H / 2 : (pill.left + pill.right) / 2;
            if (ga > 0.01f) DrawFaceId(dc, gx + shake, (pill.top + pill.bottom) / 2, half, ga, ok);
            float ta = passkey ? std::clamp((W - H - 60 * s) / (80 * s), 0.f, 1.f) : 0.f;   // text fades in as it widens
            if (ta > 0.01f) {
                bool save = s_pkAction == L"save";
                std::wstring what = ok > 0 ? (save ? L"Passkey saved" : L"Signed in") : (save ? L"Save passkey" : L"Sign in");
                float tx = pill.left + H - 6 * s, cy = (pill.top + pill.bottom) / 2;
                text(fTitle.Get(), what, D2D1::RectF(tx + shake, cy - 22 * s, pill.right - 18 * s, cy), D2D1::ColorF(1, 1, 1, ta));
                text(fBody.Get(), s_pkSite, D2D1::RectF(tx + shake, cy + 1 * s, pill.right - 18 * s, cy + 21 * s), D2D1::ColorF(1, 1, 1, 0.68f * ta));
            }
            return;
        }
        if (NoteShowing()) {
            // a notification: app icon, then app name, title and the first line of the text, which
            // fade in as the pill widens
            float na = std::clamp((W - 200 * s) / (140 * s), 0.f, 1.f);
            if (na < 0.01f) return;
            float p = 12 * s, ic = 38 * s, cy = (pill.top + pill.bottom) / 2;
            D2D1_RECT_F ir = D2D1::RectF(pill.left + p, cy - ic / 2, pill.left + p + ic, cy + ic / 2);
            if (auto icon = NoteIcon(s_note.aumid)) {
                ComPtr<ID2D1RoundedRectangleGeometry> geo;
                g.d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(ir, 9 * s, 9 * s), &geo);
                dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), na), nullptr);
                dc->DrawBitmap(icon.Get(), &ir, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                dc->PopLayer();
            } else {
                s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.14f * na));
                dc->FillRoundedRectangle(D2D1::RoundedRect(ir, 9 * s, 9 * s), s_br.Get());
                text(fGlyphBig.Get(), L"", ir, D2D1::ColorF(1, 1, 1, 0.8f * na));   // bell
            }
            float tx = ir.right + 12 * s, tr = pill.right - 18 * s;
            bool hasBody = !s_note.body.empty();
            float ty = hasBody ? pill.top + 9 * s : cy - 18 * s;
            std::wstring app = s_note.app.empty() ? L"Notification" : s_note.app;
            text(fSmall.Get(), app, D2D1::RectF(tx, ty, tr - 34 * s, ty + 16 * s), D2D1::ColorF(1, 1, 1, 0.55f * na));
            text(fSmall.Get(), L"now", D2D1::RectF(tr - 32 * s, ty, tr, ty + 16 * s), D2D1::ColorF(1, 1, 1, 0.45f * na));
            text(fTitle.Get(), s_note.title.empty() ? app : s_note.title, D2D1::RectF(tx, ty + 16 * s, tr, ty + 35 * s), D2D1::ColorF(1, 1, 1, na));
            if (hasBody) {
                std::wstring b = s_note.body;
                for (auto& c : b) if (c == L'\n' || c == L'\r') c = L' ';
                text(fBody.Get(), b, D2D1::RectF(tx, ty + 35 * s, tr, ty + 53 * s), D2D1::ColorF(1, 1, 1, 0.72f * na));
            }
            return;
        }
        float ca = 1 - open;   // compact content fades as it opens
        if (ca > 0.02f) {
            float y0 = pill.top, y1 = pill.top + 22 * s;
            // left: done check (a session just finished, even while others still work) /
            // Claude spark (pulsing while working) / album art
            if (done) {
                text(fGlyph.Get(), L"", D2D1::RectF(pill.left + 8 * s, y0, pill.left + 26 * s, y1), D2D1::ColorF(0.3f, 0.85f, 0.45f, ca));
                text(fSmall.Get(), s_doneName.empty() ? L"Claude is done" : L"Done · " + s_doneName,
                     D2D1::RectF(pill.left + 28 * s, y0, pill.right - 30 * s, y1), D2D1::ColorF(1, 1, 1, 0.9f * ca));
            } else if (s_working > 0) {
                float pulse = 0.6f + 0.4f * (float)std::sin(t * 5.0);
                text(fSpark.Get(), L"✳", D2D1::RectF(pill.left + 8 * s, y0, pill.left + 26 * s, y1), D2D1::ColorF(claude.r, claude.g, claude.b, pulse * ca));
                std::wstring lbl = s_working > 1 ? std::to_wstring(s_working) + L" working" : L"Working";
                text(fSmall.Get(), lbl, D2D1::RectF(pill.left + 28 * s, y0, pill.right - 30 * s, y1), D2D1::ColorF(1, 1, 1, 0.9f * ca));
            } else if (s_waiting > 0) {
                text(fSpark.Get(), L"✳", D2D1::RectF(pill.left + 8 * s, y0, pill.left + 26 * s, y1), D2D1::ColorF(claude.r, claude.g, claude.b, ca));
                text(fSmall.Get(), L"Needs you", D2D1::RectF(pill.left + 28 * s, y0, pill.right - 30 * s, y1), D2D1::ColorF(1, 1, 1, 0.9f * ca));
            } else if (s_media.present && s_art) {
                D2D1_RECT_F ar = D2D1::RectF(pill.left + 7 * s, y0 + 3 * s, pill.left + 23 * s, y0 + 19 * s);
                dc->DrawBitmap(s_art.Get(), &ar, ca, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            }
            // right: equalizer while music plays
            if (s_media.present && s_media.playing) {
                for (int i = 0; i < 4; i++) {
                    float hgt = (5 + 7 * (0.5f + 0.5f * (float)std::sin(t * (6 + i * 1.7) + i))) * s;
                    float bx = pill.right - (12 + (3 - i) * 4.5f) * s, cy = (y0 + y1) / 2;
                    s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.85f * ca));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, cy - hgt / 2, bx + 2.6f * s, cy + hgt / 2), 1.3f * s, 1.3f * s), s_br.Get());
                }
            }
        }
        if (open > 0.02f && s_art) {
            // expanded: the album cover, blurred, fills the island behind everything
            if (!s_blur) {
                dc->CreateEffect(CLSID_D2D1GaussianBlur, &s_blur);
                if (s_blur) s_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            }
            if (s_blur) {
                s_blur->SetInput(0, s_art.Get());
                auto sz = s_art->GetSize();
                float k = std::max((pill.right - pill.left) / sz.width, (pill.bottom - pill.top) / sz.height) * 1.15f;
                s_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 9.0f / std::max(k, 0.01f) * s);
                ComPtr<ID2D1RoundedRectangleGeometry> geo;
                g.d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(pill, r, r), &geo);
                dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), open), nullptr);
                D2D1_MATRIX_3X2_F old;
                dc->GetTransform(&old);
                float cx2 = (pill.left + pill.right) / 2, cy2 = (pill.top + pill.bottom) / 2;
                dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(cx2 - sz.width * k / 2, cy2 - sz.height * k / 2) * old);
                ComPtr<ID2D1Image> out;
                s_blur->GetOutput(&out);
                dc->DrawImage(out.Get(), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                dc->SetTransform(old);
                s_br->SetColor(D2D1::ColorF(0, 0, 0, 0.42f));   // keeps the text readable
                dc->FillRectangle(pill, s_br.Get());
                dc->PopLayer();
            }
        }
        if (open > 0.02f) {
            float a = open;
            float p = 14 * s, y = pill.top + p;
            // now playing
            if (s_media.present) {
                D2D1_RECT_F ar = D2D1::RectF(pill.left + p, y, pill.left + p + 52 * s, y + 52 * s);
                if (s_art) {
                    ComPtr<ID2D1RoundedRectangleGeometry> geo;
                    g.d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(ar, 10 * s, 10 * s), &geo);
                    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
                    dc->DrawBitmap(s_art.Get(), &ar, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                    dc->PopLayer();
                }
                float tx = ar.right + 12 * s, bw = 34 * s, bx = pill.right - p - bw * 3;
                D2D1_RECT_F song = D2D1::RectF(tx, y + 4 * s, bx - 6 * s, y + 50 * s);
                s_btns.push_back({song, 4});
                text(fTitle.Get(), s_media.title.empty() ? s_media.app : s_media.title, D2D1::RectF(tx, y + 6 * s, bx - 6 * s, y + 26 * s), D2D1::ColorF(1, 1, 1, a));
                text(fBody.Get(), s_media.artist.empty() ? s_media.app : s_media.artist, D2D1::RectF(tx, y + 27 * s, bx - 6 * s, y + 46 * s), D2D1::ColorF(1, 1, 1, 0.6f * a));
                const wchar_t* gl[3] = {L"", s_media.playing ? L"" : L"", L""};
                for (int i = 0; i < 3; i++) {
                    D2D1_RECT_F b = D2D1::RectF(bx + i * bw, y + 10 * s, bx + (i + 1) * bw, y + 42 * s);
                    s_btns.push_back({b, i + 1});
                    if (s_hit == i + 1) {
                        s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.12f * a));
                        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((b.left + b.right) / 2, (b.top + b.bottom) / 2), 15 * s, 15 * s), s_br.Get());
                    }
                    text(i == 1 ? fGlyphBig.Get() : fGlyph.Get(), gl[i], b, D2D1::ColorF(1, 1, 1, a));
                }
                y += 64 * s;
            }
            // Claude
            std::wstring line;
            D2D1_COLOR_F dot = D2D1::ColorF(1, 1, 1, 0.4f * a);
            if (s_working > 0) {
                line = s_working == 1 ? L"Claude is working" : L"Claude is working in " + std::to_wstring(s_working) + L" sessions";
                if (!s_sessions.empty()) line += L"  ·  " + s_sessions[0] + (s_sessions.size() > 1 ? L" +" + std::to_wstring(s_sessions.size() - 1) : L"");
                dot = D2D1::ColorF(claude.r, claude.g, claude.b, (0.6f + 0.4f * (float)std::sin(t * 5.0)) * a);
            } else if (s_waiting > 0) {
                line = L"Claude needs your answer";
                dot = D2D1::ColorF(claude.r, claude.g, claude.b, a);
            } else line = !done ? L"Claude is idle" : s_doneName.empty() ? L"Claude finished" : L"Claude finished in " + s_doneName;
            text(fSpark.Get(), L"✳", D2D1::RectF(pill.left + p, y, pill.left + p + 18 * s, y + 20 * s), dot);
            text(fBody.Get(), line, D2D1::RectF(pill.left + p + 24 * s, y, pill.right - p, y + 20 * s), D2D1::ColorF(1, 1, 1, 0.85f * a));
        }
    });
}
}  // namespace

namespace island {
bool Init() {
    if (!cfg.dynamicIsland) return true;
    HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    s_s = dx / 96.f;
    int w = (int)(kOvW * s_s), h = (int)(kOvH * s_s);
    int x = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - w) / 2;
    if (!s_ov.Create(L"LiquidIsland", Proc, x, mi.rcMonitor.top, w, h, nullptr)) return false;
    s_ov.SetClickThrough(true);
    s_ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &s_br);
    const wchar_t* ui = L"Segoe UI Variable Text";
    fSmall = MakeFont(ui, 11.5f * s_s, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    fTitle = MakeFont(ui, 13.5f * s_s, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    fBody = MakeFont(ui, 12.5f * s_s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fGlyph = MakeFont(L"Segoe Fluent Icons", 13 * s_s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fGlyphBig = MakeFont(L"Segoe Fluent Icons", 18 * s_s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fSpark = MakeFont(L"Segoe UI Symbol", 13 * s_s, DWRITE_FONT_WEIGHT_NORMAL, true);   // has the Claude-like spark
    CreateDirectoryW((StatusDir().substr(0, StatusDir().find_last_of(L'\\'))).c_str(), nullptr);
    CreateDirectoryW(StatusDir().c_str(), nullptr);
    Log(L"island: on, Claude status from %s", StatusDir().c_str());
    return true;
}

void Tick(bool) {
    if (!s_ov.hwnd) return;
    ULONGLONG now = GetTickCount64();
    // the island lives in the menu bar: keep it above the bar (the bar re-asserts itself on top
    // when a full-screen app appears)
    static ULONGLONG lastTop = 0;
    if (now - lastTop > 500 && !lock::IsLocked()) {
        lastTop = now;
        SetWindowPos(s_ov.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (now - s_lastScan > 500) {
        s_lastScan = now;
        ScanClaude();
        MediaInfo m = sys::Media();
        if (m.title != s_media.title || m.playing != s_media.playing || m.present != s_media.present) s_dirty = true;
        s_media = m;
        if (m.artVersion != s_artVer) { s_artVer = m.artVersion; s_art = DecodeImage(s_ov.dc.Get(), m.art, (int)(104 * s_s)); s_dirty = true; }
    }
    bool locked = lock::IsLocked();
    double tn = Now();
    // A passkey sign-in (LiquidKeys.exe) is waiting for Air Ring to see your face: show the
    // Face ID square for the whole wait, not just once a face is in view. The lock screen only
    // takes Air Ring's face events while locked, so here they're ours.
    static HANDLE evVerifying = CreateEventW(nullptr, TRUE, FALSE, L"Local\\LiquidFaceVerifying");
    static HANDLE evSeen = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidFaceSeen");
    static HANDLE evNoMatch = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidFaceNoMatch");
    static HANDLE evOk = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidFaceVerifiedUi");
    static bool wasVerifying = false;
    static double vSeen = -10, vOk = 0, vFail = -10;
    bool verifying = !locked && evVerifying && WaitForSingleObject(evVerifying, 0) == WAIT_OBJECT_0;
    if (verifying && !wasVerifying) {   // a new passkey check: which site, saving or signing in
        vSeen = tn; vOk = 0; vFail = -10;
        std::wstring p = ReadSmall(LocalAppData() + L"\\Liquid\\keys\\pending.txt");
        size_t nl = p.find(L'\n');
        s_pkAction = nl == std::wstring::npos ? L"" : p.substr(0, nl);
        s_pkSite = nl == std::wstring::npos ? L"" : p.substr(nl + 1);
        while (!s_pkSite.empty() && (s_pkSite.back() == L'\n' || s_pkSite.back() == L'\r')) s_pkSite.pop_back();
        s_dirty = true;
    }
    wasVerifying = verifying;
    if (!locked) {
        if (evSeen && WaitForSingleObject(evSeen, 0) == WAIT_OBJECT_0) vSeen = tn;
        if (evNoMatch && WaitForSingleObject(evNoMatch, 0) == WAIT_OBJECT_0) vFail = tn;
        if (evOk && WaitForSingleObject(evOk, 0) == WAIT_OBJECT_0) { vOk = tn; vSeen = tn; }
        s_fSeen = vSeen; s_fOk = vOk; s_fFail = vFail;
    } else {
        lock::FaceTimes(s_fSeen, s_fOk, s_fFail);
    }
    bool face = locked ? (tn - s_fSeen < 1.6 || (s_fOk && tn - s_fOk < 1.2))
                       : (verifying || (s_fOk && tn - s_fOk < 1.2));
    if (face != s_face) {
        s_face = face;
        s_dirty = true;
        if (face) SetWindowPos(s_ov.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (locked) s_noteUntil = 0;
    if (!NoteShowing() && !s_noteQueue.empty() && !locked && !face) {
        s_note = s_noteQueue.front();
        s_noteQueue.erase(s_noteQueue.begin());
        s_noteUntil = Now() + kNoteSecs;
        s_dirty = true;
        SetWindowPos(s_ov.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    bool note = NoteShowing();
    bool hasStuff = note || s_working > 0 || s_waiting > 0 || Now() < s_doneUntil || s_media.present;
    POINT p;
    GetCursorPos(&p);
    float x = (float)(p.x - s_ov.x), y = (float)(p.y - s_ov.y), cx = s_ov.w / 2.f;
    bool in = hasStuff && !locked && x >= cx - s_w / 2 - 4 * s_s && x <= cx + s_w / 2 + 4 * s_s && y >= 0 && y <= 2 * s_s + s_h + 4 * s_s;
    if (in != s_hover) { s_hover = in; s_dirty = true; }
    if (in && note) s_noteUntil = std::max(s_noteUntil, Now() + 1.5);   // hovering keeps it up
    float tw = 0, th = 0;
    if (hasStuff && !locked) {
        if (note) { tw = 370 * s_s; th = (s_note.body.empty() ? 58.f : 72.f) * s_s; }
        else if (s_hover) { tw = 380 * s_s; th = (s_media.present ? 116.f : 50.f) * s_s; }
        else {
            bool claudeTxt = s_working > 0 || s_waiting > 0 || Now() < s_doneUntil;
            tw = (claudeTxt ? 210.f : 170.f) * s_s;
            th = std::max(20.f, cfg.menubarHeight - 4) * s_s;
        }
    }
    if (!s_face && !verifying) s_pkSite.clear();   // the passkey check is over
    if (s_face) {
        th = 112 * s_s;
        tw = (!s_pkSite.empty() && !locked) ? 330 * s_s : 112 * s_s;
    }
    float pw = s_w, ph = s_h, po = s_open;
    Spring(s_w, s_wv, tw, 1 / 60.f, 420.f);
    Spring(s_h, s_hv, th, 1 / 60.f, 420.f);
    Spring(s_open, s_openV, s_hover && !note ? 1.f : 0.f, 1 / 60.f, 300.f);
    if (s_w < 1 && tw == 0) { s_w = 0; }
    int hit = -1;
    for (auto& b : s_btns) if (x >= b.r.left && x <= b.r.right && y >= b.r.top && y <= b.r.bottom) hit = b.id;
    if (hit != s_hit) { s_hit = hit; s_dirty = true; }
    s_ov.SetClickThrough(!in);
    bool animating = pw != s_w || ph != s_h || po != s_open || s_working > 0 || (s_media.playing && s_w > 1) || s_face ||
                     Now() < s_doneUntil;
    if (s_dirty || animating || g.snapPending) { s_dirty = false; Render(); }
}

void Raise() {
    if (s_ov.hwnd && s_face) SetWindowPos(s_ov.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Notify(const Notif& n) {
    if (!s_ov.hwnd || lock::IsLocked()) return;
    if (s_noteQueue.size() < 4) s_noteQueue.push_back(n);   // a burst shows the first few, not a minute of them
    s_dirty = true;
}

void DrawFaceIdGlyph(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, float cx, float cy, float half, float alpha) {
    DrawFaceIdWith(dc, br, cx, cy, half, alpha, 0);
}

void Shutdown() { if (s_ov.hwnd) s_ov.Destroy(); }
}  // namespace island
