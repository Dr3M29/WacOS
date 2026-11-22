// Live glass while a remote stream is watching (see remote.cpp). Liquid has to be visible to screen
// capture then, so reading the whole screen would show the glass its own bars. Instead each app
// window behind the glass is captured on its own (Windows.Graphics.Capture sees one window and
// nothing in front of it) and the screen is rebuilt from those: the wallpaper, then the windows
// from the bottom up. Up to 6 windows per screen, stopping at one that covers the whole screen.
#include "shell.h"
#include <d3d11_4.h>
#include <dwmapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

namespace streamglass {
namespace {
struct Win {
    HWND hwnd = nullptr;
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 size{};
    ComPtr<ID3D11Texture2D> tex;   // the latest frame
    ComPtr<ID2D1Bitmap1> bmp;
    UINT w = 0, h = 0;
    bool fresh = false;
    ULONGLONG wanted = 0;
    void Close() {
        try { if (session) session.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
    }
};
struct Screen {
    Capture* cap = nullptr;
    ComPtr<ID3D11Texture2D> wallTex, compTex;
    ComPtr<ID2D1Bitmap1> wall, comp;
    std::vector<HWND> order;   // bottom to top
    std::vector<RECT> rects;   // where each one is on screen
    bool dirty = true;
};
std::vector<std::unique_ptr<Win>> s_wins;
std::vector<Screen> s_screens;
std::vector<std::pair<HWND, ULONGLONG>> s_failed;   // windows Windows won't capture: retry after 10 s
wd3d::IDirect3DDevice s_dev{nullptr};
ComPtr<ID2D1DeviceContext> s_dc;
ULONGLONG s_lastList = 0;
size_t s_logged = (size_t)-1;

ComPtr<ID3D11Texture2D> MakeTex(UINT w, UINT h, UINT bind) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    ComPtr<ID3D11Texture2D> t;
    g.dev->CreateTexture2D(&d, nullptr, &t);
    return t;
}

ComPtr<ID2D1Bitmap1> Bmp(ID3D11Texture2D* t, bool target) {
    ComPtr<IDXGISurface> s;
    if (!t || FAILED(t->QueryInterface(IID_PPV_ARGS(&s)))) return nullptr;
    D2D1_BITMAP_PROPERTIES1 p = D2D1::BitmapProperties1(
        target ? D2D1_BITMAP_OPTIONS_TARGET : D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ComPtr<ID2D1Bitmap1> b;
    s_dc->CreateBitmapFromDxgiSurface(s.Get(), &p, &b);
    return b;
}

bool InitDevice() {
    if (s_dev) return true;
    try {
        ComPtr<ID3D11Multithread> mt;   // the capture frame pools use the device from their own threads
        if (SUCCEEDED(g.ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
        winrt::com_ptr<::IInspectable> insp;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(g.dxgiDev.Get(), insp.put()));
        s_dev = insp.as<wd3d::IDirect3DDevice>();
        winrt::check_hresult(g.d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &s_dc));
        s_dc->SetDpi(96, 96);
        return true;
    } catch (winrt::hresult_error const& e) {
        Log(L"stream glass: no capture device (0x%08X), wallpaper glass", (unsigned)e.code().value);
    } catch (...) {
        Log(L"stream glass: no capture device, wallpaper glass");
    }
    s_dev = nullptr;
    return false;
}

Win* Find(HWND h) {
    for (auto& w : s_wins) if (w->hwnd == h) return w.get();
    return nullptr;
}

Win* Open(HWND h) {
    if (Win* w = Find(h)) return w;
    for (auto& f : s_failed) if (f.first == h) return nullptr;
    auto w = std::make_unique<Win>();
    w->hwnd = h;
    try {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(h, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(w->item)));
        w->size = w->item.Size();
        w->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(s_dev, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, w->size);
        w->session = w->pool.CreateCaptureSession(w->item);
        try { w->session.IsBorderRequired(false); } catch (...) {}   // no yellow frame round the window
        try { w->session.IsCursorCaptureEnabled(false); } catch (...) {}
        w->session.StartCapture();
    } catch (winrt::hresult_error const& e) {
        wchar_t cls[64] = {};
        GetClassNameW(h, cls, 64);
        Log(L"stream glass: can't capture a %s window (0x%08X)", cls, (unsigned)e.code().value);
        w->Close();
        s_failed.push_back({h, GetTickCount64()});
        return nullptr;
    } catch (...) {
        w->Close();
        s_failed.push_back({h, GetTickCount64()});
        return nullptr;
    }
    s_wins.push_back(std::move(w));
    return s_wins.back().get();
}

// newest frame of one window into its own texture
void Pull(Win& w) {
    wgc::Direct3D11CaptureFrame f{nullptr};
    try {
        for (auto n = w.pool.TryGetNextFrame(); n; n = w.pool.TryGetNextFrame()) {
            if (f) f.Close();
            f = n;
        }
    } catch (...) { return; }
    if (!f) return;
    auto cs = f.ContentSize();
    ComPtr<ID3D11Texture2D> src;
    try {
        auto access = f.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        access->GetInterface(IID_PPV_ARGS(&src));
    } catch (...) {}
    if (src) {
        D3D11_TEXTURE2D_DESC sd;
        src->GetDesc(&sd);
        UINT cw = std::min<UINT>((UINT)cs.Width, sd.Width), ch = std::min<UINT>((UINT)cs.Height, sd.Height);
        if (cw && ch) {
            if (!w.tex || w.w != cw || w.h != ch) {
                w.tex = MakeTex(cw, ch, D3D11_BIND_SHADER_RESOURCE);
                w.bmp = Bmp(w.tex.Get(), false);
                w.w = cw; w.h = ch;
            }
            D3D11_BOX box{0, 0, 0, cw, ch, 1};
            g.ctx->CopySubresourceRegion(w.tex.Get(), 0, 0, 0, 0, src.Get(), 0, &box);
            w.fresh = true;
        }
    }
    f.Close();
    if (cs.Width != w.size.Width || cs.Height != w.size.Height) {   // the window changed size
        w.size = cs;
        try { w.pool.Recreate(s_dev, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, cs); } catch (...) {}
    }
}

// App windows on this screen, bottom to top, that show behind Liquid.
std::vector<HWND> Behind(const RECT& mon, std::vector<RECT>& rects) {
    std::vector<HWND> out;
    rects.clear();
    for (HWND h = GetTopWindow(nullptr); h && out.size() < 6; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsWindowVisible(h) || IsIconic(h)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == g_selfPid) continue;
        // Overlays (NVIDIA GeForce Overlay, Game Bar, other apps' on-screen displays) are see-through on
        // screen but come back from window capture as solid black, which blacked out the glass.
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if (ex & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) continue;
        if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TOOLWINDOW)) continue;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) continue;
        wchar_t cls[64] = {};
        GetClassNameW(h, cls, 64);
        if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
            !wcscmp(cls, L"Shell_SecondaryTrayWnd")) continue;   // the desktop: the wallpaper stands in for it
        RECT r;
        if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
        RECT x;
        if (r.right - r.left < 48 || r.bottom - r.top < 32 || !IntersectRect(&x, &r, &mon)) continue;
        out.push_back(h);
        rects.push_back(r);
        if (r.left <= mon.left && r.top <= mon.top && r.right >= mon.right && r.bottom >= mon.bottom) break;
    }
    std::reverse(out.begin(), out.end());
    std::reverse(rects.begin(), rects.end());
    return out;
}

// "Liquid.exe --dump-backdrop": what the glass sees on each screen, to snapshots\backdrop-N.png
void Dump() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = self;
    dir = dir.substr(0, dir.find_last_of(L'\\')) + L"\\snapshots";
    CreateDirectoryW(dir.c_str(), nullptr);
    for (size_t i = 0; i < s_screens.size(); i++) {
        auto& sc = s_screens[i];
        D3D11_TEXTURE2D_DESC d;
        sc.compTex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &st))) continue;
        g.ctx->CopyResource(st.Get(), sc.compTex.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) continue;
        std::wstring path = dir + L"\\backdrop-" + std::to_wstring(i) + L".png";
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> fr;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(g.wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(g.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
            SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(d.Width, d.Height)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
            SUCCEEDED(fr->WritePixels(d.Height, m.RowPitch, m.RowPitch * d.Height, (BYTE*)m.pData))) {
            fr->Commit();
            enc->Commit();
            Log(L"stream glass: saved %s", path.c_str());
        }
        g.ctx->Unmap(st.Get(), 0);
    }
}

void Compose(Screen& sc) {
    sc.dirty = false;
    Capture* c = sc.cap;
    if (!sc.comp || !sc.wall || !c->tex) return;
    s_dc->SetTarget(sc.comp.Get());
    s_dc->BeginDraw();
    s_dc->DrawBitmap(sc.wall.Get());
    for (size_t i = 0; i < sc.order.size(); i++) {
        Win* w = Find(sc.order[i]);
        if (!w || !w->bmp) continue;
        float x = (float)(sc.rects[i].left - c->rect.left), y = (float)(sc.rects[i].top - c->rect.top);
        D2D1_RECT_F dr = D2D1::RectF(x, y, x + w->w, y + w->h);
        s_dc->DrawBitmap(w->bmp.Get(), &dr, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
    }
    s_dc->EndDraw();
    s_dc->SetTarget(nullptr);
    g.ctx->CopySubresourceRegion(c->tex.Get(), 0, 0, 0, 0, sc.compTex.Get(), 0, nullptr);
    g.ctx->GenerateMips(c->srv.Get());
    c->have = true;
    c->fresh = true;
}
}  // namespace

void Start(std::vector<std::unique_ptr<Capture>>& caps) {
    Stop();
    if (!InitDevice()) return;
    for (auto& c : caps) {
        if (!c->wasLive || !c->tex) continue;   // screens that never had live glass keep the wallpaper
        Screen sc;
        sc.cap = c.get();
        sc.wallTex = MakeTex(c->w, c->h, D3D11_BIND_SHADER_RESOURCE);
        sc.compTex = MakeTex(c->w, c->h, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        if (!sc.wallTex || !sc.compTex) continue;
        g.ctx->CopySubresourceRegion(sc.wallTex.Get(), 0, 0, 0, 0, c->tex.Get(), 0, nullptr);   // the wallpaper LoadStatic drew
        sc.wall = Bmp(sc.wallTex.Get(), false);
        sc.comp = Bmp(sc.compTex.Get(), true);
        if (sc.wall && sc.comp) s_screens.push_back(std::move(sc));
    }
    s_lastList = 0;
    Log(L"stream glass: rebuilding %zu screen(s) from their windows", s_screens.size());
}

void Stop() {
    for (auto& w : s_wins) w->Close();
    s_wins.clear();
    s_screens.clear();
    s_failed.clear();
    s_logged = (size_t)-1;
}

bool Poll() {
    if (s_screens.empty()) return false;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastList >= 150) {   // which windows are behind the glass, and where
        s_lastList = now;
        std::erase_if(s_failed, [now](auto& f) { return now - f.second > 10000; });
        size_t total = 0;
        for (auto& sc : s_screens) {
            std::vector<RECT> rects;
            auto order = Behind(sc.cap->rect, rects);
            bool same = order == sc.order && rects.size() == sc.rects.size();
            for (size_t i = 0; same && i < rects.size(); i++) same = EqualRect(&rects[i], &sc.rects[i]) != 0;
            if (!same) { sc.order = std::move(order); sc.rects = std::move(rects); sc.dirty = true; }
            for (HWND h : sc.order) if (Win* w = Open(h)) w->wanted = now;
            total += sc.order.size();
        }
        for (auto it = s_wins.begin(); it != s_wins.end();) {   // windows gone or covered up
            if (now - (*it)->wanted > 2000 || !IsWindow((*it)->hwnd)) { (*it)->Close(); it = s_wins.erase(it); }
            else ++it;
        }
        if (s_wins.size() != s_logged) {
            s_logged = s_wins.size();
            Log(L"stream glass: %zu window(s) captured behind the glass", s_logged);
        }
    }
    for (auto& w : s_wins) Pull(*w);
    bool any = false;
    for (auto& sc : s_screens) {
        bool d = sc.dirty;
        for (HWND h : sc.order)
            if (Win* w = Find(h); w && w->fresh) d = true;
        if (d) { Compose(sc); any = true; }
    }
    for (auto& w : s_wins) w->fresh = false;
    static HANDLE dump = CreateEventW(nullptr, FALSE, FALSE, L"LiquidShell.DumpBackdrop");
    if (dump && WaitForSingleObject(dump, 0) == WAIT_OBJECT_0) Dump();
    return any;
}
}  // namespace streamglass
