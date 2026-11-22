#include "shell.h"
#include <d3dcompiler.h>
#include <shobjidl.h>
#include <cmath>
#include <algorithm>

Gfx g;

// Liquid glass, modelled on Apple's material:
//  * a convex-squircle bezel, height h(x) = (1 - (1-x)^4)^(1/4), x = 0 at the rim, 1 where the flat top starts
//  * a vertical view ray refracted through that surface (Snell, n = 1.5); the lateral travel to the
//    backdrop is the lens displacement, pointing inward, so content bends and compresses at the rim
//  * a clear, undistorted centre with only light frost
//  * a thin specular rim lit from -60 degrees, with a weaker bounce on the opposite edge
// Output is premultiplied alpha.
static const char kHlsl[] = R"HLSL(
#define MAXS 48
Texture2D desk : register(t0);
Texture2D maskTex : register(t1);   // optional: text drawn as glass (lock screen clock)
SamplerState samp : register(s0);
cbuffer CB : register(b0) {
    float2 origin; float2 texSize;
    float2 winSize; float scale; int count;
    float4 tintColor;
    float4 light;          // xy cursor in window px, z = how near (0..1)
    float4 rects[MAXS];
    float4 params[MAXS];   // radius, blur, tint, bezel
    float4 params2[MAXS];  // shadow, thick, highlight, opacity
    float4 params3[MAXS];  // group (>0 melts with same group), melt radius
    float4 maskInfo;       // x refraction px (0 = no glass text), y height blur lod, z white amount, w opacity
};
struct VSOut { float4 pos : SV_Position; };
VSOut VS(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float sdBox(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + r;
    return length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
}
// polynomial smooth minimum: two glass blobs flow into one
float smin(float a, float b, float k) {
    float h = saturate(0.5 + 0.5 * (b - a) / k);
    return lerp(b, a, h) - k * h * (1 - h);
}
float shapeD(int k, float2 p, bool member) {
    float4 R = rects[k];
    float2 hb = R.zw * 0.5;
    float r = min(params[k].x, min(hb.x, hb.y));
    float d = sdBox(p - (R.xy + hb), hb, r);
    if (member) d += (1 - params2[k].w) * min(hb.x, hb.y) * 1.2;   // fading members shrink back into the leader
    return d;
}
float groupD(int k, float2 p) {
    float g = params3[k].x;
    float d = shapeD(k, p, false);
    if (g <= 0) return d;
    [loop] for (int j = k + 1; j < count; j++)
        if (params3[j].x == g) d = smin(d, shapeD(j, p, true), params3[k].y);
    return d;
}
float3 frost(float2 px, float lod) {
    float2 uv = (px + origin) / texSize;
    if (lod < 0.05) return desk.SampleLevel(samp, uv, 0).rgb;
    float2 o = exp2(lod) / texSize * 0.9;
    float3 c = desk.SampleLevel(samp, uv, lod).rgb * 0.36;
    c += desk.SampleLevel(samp, uv + float2( o.x,  o.y), lod).rgb * 0.16;
    c += desk.SampleLevel(samp, uv + float2(-o.x,  o.y), lod).rgb * 0.16;
    c += desk.SampleLevel(samp, uv + float2( o.x, -o.y), lod).rgb * 0.16;
    c += desk.SampleLevel(samp, uv + float2(-o.x, -o.y), lod).rgb * 0.16;
    return c;
}
// Snell refraction through a convex-squircle bezel: lateral travel in units of bezel width
float lens(float x, float thick) {
    x = clamp(x, 0.002, 1.0);
    float a = 1 - pow(1 - x, 4);
    float h = pow(a, 0.25);
    float dh = pow(1 - x, 3) * pow(max(a, 1e-5), -0.75);
    float alpha = atan(dh * thick);
    float theta2 = asin(sin(alpha) / 1.5);
    return h * thick * tan(alpha - theta2);
}
float4 PS(VSOut i) : SV_Target {
    float2 p = i.pos.xy;
    float4 acc = 0;
    // glass text: the glyph coverage is the shape, a blurred copy is the height field;
    // its slope bends the backdrop like a thick slab of glass with soft rounded edges
    if (maskInfo.x > 0) {
        float2 muv = p / winSize;
        float m = maskTex.SampleLevel(samp, muv, 0).a;
        if (m > 0.003) {
            float lod = maskInfo.y;
            float2 t = exp2(lod) / winSize;
            float hx = maskTex.SampleLevel(samp, muv + float2(t.x, 0), lod).a - maskTex.SampleLevel(samp, muv - float2(t.x, 0), lod).a;
            float hy = maskTex.SampleLevel(samp, muv + float2(0, t.y), lod).a - maskTex.SampleLevel(samp, muv - float2(0, t.y), lod).a;
            float hc = maskTex.SampleLevel(samp, muv, lod).a;
            float2 disp = -float2(hx, hy) * maskInfo.x;
            float3 col;
            col.r = frost(p + disp * 1.08, 0.5).r;
            col.g = frost(p + disp, 0.5).g;
            col.b = frost(p + disp * 0.92, 0.5).b;
            col = lerp(col, float3(1, 1, 1), maskInfo.z * (0.55 + 0.45 * (1 - hc)));
            acc = float4(col, 1) * m * maskInfo.w;
        }
    }
    [loop] for (int k = 0; k < count; k++) {
        float4 R = rects[k]; float4 P = params[k]; float4 Q = params2[k]; float4 G = params3[k];
        bool grouped = G.x > 0;
        if (grouped) {
            bool lead = true;
            for (int j = 0; j < k; j++) if (params3[j].x == G.x) { lead = false; break; }
            if (!lead) continue;   // drawn as part of its group's leader
        }
        float2 hb = R.zw * 0.5;
        float2 c = R.xy + hb;
        float2 lp = p - c;
        if (!grouped && any(abs(lp) > hb + 26 * scale)) continue;
        float d = groupD(k, p);
        if (d > 26 * scale) continue;
        float op = Q.w;
        float4 layer = 0;
        if (Q.x > 0 && d > -0.5) {
            float ds = groupD(k, p - float2(0, 5 * scale));
            float sa = Q.x * (1 - smoothstep(-8 * scale, 24 * scale, ds));
            sa *= saturate(d + 0.5);
            layer = float4(0, 0, 0, sa * op);
        }
        if (d < 0.5) {
            float cov = saturate(0.5 - d);
            float e = max(-d, 0);
            float2 n = float2(groupD(k, p + float2(1, 0)) - groupD(k, p - float2(1, 0)),
                              groupD(k, p + float2(0, 1)) - groupD(k, p - float2(0, 1)));
            n = n / max(length(n), 1e-4);
            float bezel = max(min(P.w, min(hb.x, hb.y)), 1);
            float x = e / bezel;
            float lod = P.y;
            // the slab magnifies; the curved rim refracts the backdrop inward
            float2 mag = 0.04 * Q.y * saturate(70 * scale / max(hb, 1));
            float2 base = c + lp * (1 - mag);
            float L1 = x < 1 ? min(lens(x, Q.y), 0.65) : 0;
            float2 disp = -n * L1 * bezel;
            float2 sp = base + disp;
            float ca = 0.045 * saturate(1 - x) + 0.008;
            float3 col;
            col.r = frost(sp - disp * ca, lod).r;
            col.g = frost(sp, lod).g;
            col.b = frost(sp + disp * ca, lod).b;
            // glass gathers light: brighter where rays converge, a whisper of glow in the body
            float extras = saturate(Q.z);   // 0 = pure refraction, nothing added
            col *= 1 + 0.22 * saturate(L1) * extras;
            col *= 1 - 0.025 * Q.y * exp(-pow((x - 1.05) * 5, 2)) * extras;
            float l = dot(col, float3(0.299, 0.587, 0.114));
            col = lerp(l.xxx, col, 1 + 0.12 * extras);
            col = lerp(col, P.z >= 0 ? tintColor.rgb : float3(0.06, 0.06, 0.08), abs(P.z));
            // light: a soft key from above-left, pulled toward the cursor when it's near
            float2 toL = light.xy - p;
            float ld = length(toL);
            float near = light.z * exp(-ld / (150 * scale));
            float2 Ldir = normalize(lerp(float2(-0.5, -0.866), toL / max(ld, 1), saturate(near * 1.6)));
            float facing = dot(n, Ldir);
            float spec = pow(saturate(facing), 2.5) + 0.3 * pow(saturate(-facing), 2.5);
            float rim = 1 - smoothstep(0.0, 1.4 * scale, e);
            float band = pow(saturate(1 - x), 2);
            col += Q.z * (rim * (0.04 + 0.62 * spec) + band * spec * 0.10) * (1 + 1.6 * near);
            float vy = saturate((p.y - R.y) / max(R.w, 1));
            col += Q.z * (0.03 + 0.05 * (1 - vy) * (1 - vy));   // body glow + top sheen
            // prism: a soft rainbow where the rim splits light, so glass reads even over pure black
            if (G.z > 0) {
                float ang = atan2(n.y, n.x);
                float3 hue = 0.5 + 0.5 * cos(ang * 1.0 + float3(0.0, 2.094, 4.189));
                float lit = 0.55 + 0.45 * saturate(dot(n, float2(-0.5, -0.866)) * 0.5 + 0.5);
                col += G.z * pow(saturate(1 - x), 3) * (0.35 + 0.65 * hue) * lit * 0.32;
                col += G.z * (1 - smoothstep(0.0, 1.2 * scale, e)) * hue * 0.10;
            }
            float4 gl = float4(saturate(col), 1) * cov * op;
            layer = gl + layer * (1 - gl.a);
        }
        acc = layer + acc * (1 - layer.a);
    }
    // backdrop mode (lock screen): the window shows its own picture under the glass
    if (light.w > 0) acc += float4(frost(p, 0) * light.w, light.w) * (1 - acc.a);
    return acc;
}
)HLSL";

static const int kMaxShapes = 48;
struct CBData {
    float origin[2]; float texSize[2];
    float winSize[2]; float scale; int count;
    float tint[4];
    float light[4];
    float rects[kMaxShapes][4];
    float params[kMaxShapes][4];
    float params2[kMaxShapes][4];
    float params3[kMaxShapes][4];
    float maskInfo[4];
};
static_assert(sizeof(CBData) % 16 == 0, "cb align");

static ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> b, e;
    HRESULT hr = D3DCompile(kHlsl, sizeof(kHlsl) - 1, "glass", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &e);
    if (FAILED(hr)) {
        if (e) Log(L"shader %S: %S", entry, (const char*)e->GetBufferPointer());
        return nullptr;
    }
    return b;
}

// ---------- Capture ----------
bool Capture::MakeTexture(UINT tw, UINT th) {
    if (tex && tw == w && th == h) return true;
    tex.Reset(); srv.Reset(); lumaStage.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = tw; td.Height = th; td.MipLevels = 0; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &tex))) return false;
    g.dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    mips = (UINT)std::floor(std::log2((double)std::max(tw, th))) + 1;
    lumaMip = std::min<UINT>(5, mips - 1);
    lumaW = std::max(1u, tw >> lumaMip);
    lumaH = std::max(1u, th >> lumaMip);
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = lumaW; sd.Height = lumaH; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g.dev->CreateTexture2D(&sd, nullptr, &lumaStage);
    w = tw; h = th;
    have = false;
    return true;
}

bool Capture::Start() {
    lastTry = GetTickCount64();
    dupl.Reset();
    if (!output) return false;
    HRESULT hr = output->DuplicateOutput(g.dev.Get(), &dupl);
    if (FAILED(hr)) { Log(L"DuplicateOutput failed 0x%08X", hr); dupl.Reset(); return false; }
    DXGI_OUTDUPL_DESC dd;
    dupl->GetDesc(&dd);
    if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        Log(L"rotated output, using wallpaper glass");
        dupl.Reset();
        live = false;
        return false;
    }
    if (!MakeTexture(dd.ModeDesc.Width, dd.ModeDesc.Height)) return false;
    first = true;
    Log(L"capturing %ux%u at (%d,%d)", w, h, rect.left, rect.top);
    return true;
}

bool Capture::Poll() {
    fresh = false;
    if (!live) return false;
    if (!dupl) {
        if (GetTickCount64() - lastTry > 1000) Start();
        if (!dupl) return false;
    }
    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = dupl->AcquireNextFrame(0, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) { dupl.Reset(); lastTry = GetTickCount64(); return false; }
    if (fi.LastPresentTime.QuadPart != 0 || !have || first) {
        ComPtr<ID3D11Texture2D> t;
        if (SUCCEEDED(res.As(&t))) {
            g.ctx->CopySubresourceRegion(tex.Get(), 0, 0, 0, 0, t.Get(), 0, nullptr);
            g.ctx->GenerateMips(srv.Get());
            have = true;
            fresh = true;
            first = false;
        }
    }
    dupl->ReleaseFrame();
    return fresh;
}

// The picture Windows shows on this screen (each monitor can have its own) and how it's placed.
std::wstring ScreenWallpaper(const RECT& rc, const std::wstring& fallback, int& pos) {
    pos = DWPOS_FILL;
    std::wstring out = fallback;
    ComPtr<IDesktopWallpaper> dw;
    if (FAILED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw)))) return out;
    DESKTOP_WALLPAPER_POSITION p;
    if (SUCCEEDED(dw->GetPosition(&p))) pos = p;
    UINT n = 0;
    dw->GetMonitorDevicePathCount(&n);
    for (UINT i = 0; i < n; i++) {
        LPWSTR id = nullptr, wp = nullptr;
        if (FAILED(dw->GetMonitorDevicePathAt(i, &id)) || !id) continue;
        RECT r;
        if (SUCCEEDED(dw->GetMonitorRECT(id, &r)) && EqualRect(&r, &rc) && SUCCEEDED(dw->GetWallpaper(id, &wp)) && wp && *wp)
            out = wp;
        if (wp) CoTaskMemFree(wp);
        CoTaskMemFree(id);
    }
    return out;
}

// Static backdrop (recording mode, or screens we can't capture): the wallpaper, cropped to fill.
bool Capture::LoadStatic(const std::wstring& fallback) {
    live = false;
    UINT mw = std::max<LONG>(1, rect.right - rect.left), mh = std::max<LONG>(1, rect.bottom - rect.top);
    std::vector<BYTE> px((size_t)mw * mh * 4);
    bool ok = false;
    int pos = DWPOS_FILL;
    std::wstring path = ScreenWallpaper(rect, fallback, pos);
    ComPtr<IWICBitmapDecoder> dec;
    if (!path.empty() && SUCCEEDED(g.wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                                     WICDecodeMetadataCacheOnLoad, &dec))) {
        ComPtr<IWICBitmapFrameDecode> fr;
        dec->GetFrame(0, &fr);
        UINT iw = 0, ih = 0;
        if (fr) fr->GetSize(&iw, &ih);
        if (iw && ih) {
            // Fill this screen, or with "Span" fill the box round all the screens and take this
            // screen's part of it, the way Windows draws it.
            bool span = pos == DWPOS_SPAN;
            LONG ax = rect.left, ay = rect.top;
            UINT aw = mw, ah = mh;
            if (span) {
                ax = GetSystemMetrics(SM_XVIRTUALSCREEN); ay = GetSystemMetrics(SM_YVIRTUALSCREEN);
                aw = (UINT)std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
                ah = (UINT)std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
            }
            double sc = std::max((double)aw / iw, (double)ah / ih);
            UINT sw = std::max(aw, (UINT)std::ceil(iw * sc)), sh = std::max(ah, (UINT)std::ceil(ih * sc));
            ComPtr<IWICBitmapScaler> scaler;
            ComPtr<IWICFormatConverter> conv;
            g.wic->CreateBitmapScaler(&scaler);
            g.wic->CreateFormatConverter(&conv);
            if (SUCCEEDED(scaler->Initialize(fr.Get(), sw, sh, WICBitmapInterpolationModeFant)) &&
                SUCCEEDED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                           nullptr, 0, WICBitmapPaletteTypeCustom))) {
                INT ox = (INT)((sw - aw) / 2) + (INT)(rect.left - ax), oy = (INT)((sh - ah) / 2) + (INT)(rect.top - ay);
                ox = std::clamp(ox, 0, (INT)sw - (INT)mw);
                oy = std::clamp(oy, 0, (INT)sh - (INT)mh);
                WICRect rc{ox, oy, (INT)mw, (INT)mh};
                ok = SUCCEEDED(conv->CopyPixels(&rc, mw * 4, (UINT)px.size(), px.data()));
            }
        }
    }
    if (!ok) for (size_t i = 0; i < px.size(); i += 4) { px[i] = 46; px[i + 1] = 40; px[i + 2] = 40; px[i + 3] = 255; }
    if (!MakeTexture(mw, mh)) return false;
    g.ctx->UpdateSubresource(tex.Get(), 0, nullptr, px.data(), mw * 4, 0);
    g.ctx->GenerateMips(srv.Get());
    have = true;
    fresh = true;
    return ok;
}

float Capture::Luma(const RECT& r) {
    if (!have || !lumaStage) return 0.3f;
    g.ctx->CopySubresourceRegion(lumaStage.Get(), 0, 0, 0, 0, tex.Get(), D3D11CalcSubresource(lumaMip, 0, mips), nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g.ctx->Map(lumaStage.Get(), 0, D3D11_MAP_READ, 0, &m))) return 0.3f;
    int x0 = std::clamp((int)((r.left - rect.left) >> lumaMip), 0, (int)lumaW - 1);
    int x1 = std::clamp((int)((r.right - rect.left) >> lumaMip), x0 + 1, (int)lumaW);
    int y0 = std::clamp((int)((r.top - rect.top) >> lumaMip), 0, (int)lumaH - 1);
    int y1 = std::clamp((int)((r.bottom - rect.top) >> lumaMip), y0 + 1, (int)lumaH);
    double sum = 0;
    int n = 0;
    for (int y = y0; y < y1; y++) {
        const BYTE* row = (const BYTE*)m.pData + y * m.RowPitch;
        for (int x = x0; x < x1; x++) {
            const BYTE* p = row + x * 4;
            sum += (0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]) / 255.0;
            n++;
        }
    }
    g.ctx->Unmap(lumaStage.Get(), 0);
    return n ? (float)(sum / n) : 0.3f;
}

// colour of one screen pixel (what's really on screen, without Liquid's own windows)
bool Capture::ColorAt(POINT pt, float rgb[3]) {
    if (!have || !tex || !live) return false;   // the wallpaper says nothing about what's on screen
    int x = pt.x - rect.left, y = pt.y - rect.top;
    if (x < 0 || y < 0 || x >= (int)w || y >= (int)h) return false;
    if (!pixStage) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        d.Width = d.Height = 1; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc = {1, 0};
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &pixStage))) return false;
    }
    D3D11_BOX box{(UINT)x, (UINT)y, 0, (UINT)x + 1, (UINT)y + 1, 1};
    g.ctx->CopySubresourceRegion(pixStage.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g.ctx->Map(pixStage.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    const BYTE* p = (const BYTE*)m.pData;
    rgb[0] = p[2] / 255.f; rgb[1] = p[1] / 255.f; rgb[2] = p[0] / 255.f;
    g.ctx->Unmap(pixStage.Get(), 0);
    return true;
}

// luminance of a short vertical run of screen pixels (finds a window's close-button glyph)
bool Capture::Column(int sx, int sy, int n, std::vector<float>& lum) {
    lum.clear();
    if (!have || !tex || !live || n <= 0) return false;
    int x = sx - rect.left, y = sy - rect.top;
    if (x < 0 || x >= (int)w || y < 0) return false;
    n = std::min(n, (int)h - y);
    if (n <= 0 || n > 256) return false;
    if (!colStage) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        d.Width = 1; d.Height = 256; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc = {1, 0};
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &colStage))) return false;
    }
    D3D11_BOX box{(UINT)x, (UINT)y, 0, (UINT)x + 1, (UINT)(y + n), 1};
    g.ctx->CopySubresourceRegion(colStage.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g.ctx->Map(colStage.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    for (int i = 0; i < n; i++) {
        const BYTE* p = (const BYTE*)m.pData + i * m.RowPitch;
        lum.push_back((0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2]) / 255.f);
    }
    g.ctx->Unmap(colStage.Get(), 0);
    return true;
}

// ---------- Gfx ----------
static BOOL CALLBACK AddMonitor(HMONITOR hm, HDC, LPRECT rc, LPARAM) {
    auto c = std::make_unique<Capture>();
    c->mon = hm;
    c->rect = *rc;
    g.caps.push_back(std::move(c));
    return TRUE;
}

bool Gfx::Init() {
    ComPtr<IDXGIFactory1> f1;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f1)))) return false;
    HMONITOR prim = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter1> ad;
    for (UINT i = 0; f1->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND && !adapter; i++) {
        ComPtr<IDXGIOutput> out;
        for (UINT j = 0; ad->EnumOutputs(j, &out) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC od;
            out->GetDesc(&od);
            if (od.Monitor == prim) adapter = ad;
            out.Reset();
        }
        ad.Reset();
    }
    if (!adapter) { Log(L"no adapter owns the primary monitor"); return false; }

    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { Log(L"D3D11CreateDevice 0x%08X", hr); return false; }
    dev.As(&dxgiDev);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    D2D1_FACTORY_OPTIONS o{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &o,
                                 (void**)d2dFactory.GetAddressOf()))) return false;
    if (FAILED(d2dFactory->CreateDevice(dxgiDev.Get(), &d2dDev))) return false;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dw.GetAddressOf());
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&dcomp)))) return false;

    auto vb = Compile("VS", "vs_5_0");
    auto pb = Compile("PS", "ps_5_0");
    if (!vb || !pb) return false;
    dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CBData);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, &cb);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &samp);

    // one capture per monitor; screens on another GPU (virtual displays) get wallpaper glass
    EnumDisplayMonitors(nullptr, nullptr, AddMonitor, 0);
    wchar_t wp[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0);
    for (auto& c : caps) {
        ComPtr<IDXGIOutput> out;
        for (UINT j = 0; adapter->EnumOutputs(j, &out) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC od;
            out->GetDesc(&od);
            if (od.Monitor == c->mon) { out.As(&c->output); break; }
            out.Reset();
        }
        if (!cfg.recordable && c->output && c->Start()) continue;
        c->LoadStatic(wp);
        Log(L"monitor at (%d,%d): wallpaper glass", c->rect.left, c->rect.top);
    }
    return true;
}

void Gfx::ApplyCaptureMode(HWND h) {
    if (std::find(ownWindows.begin(), ownWindows.end(), h) == ownWindows.end()) ownWindows.push_back(h);
    SetWindowDisplayAffinity(h, !cfg.recordable && !remoteView ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
}

void Gfx::SetRemoteView(bool on) {
    if (on == remoteView) return;
    remoteView = on;
    DWORD self = GetCurrentProcessId();
    std::erase_if(ownWindows, [self](HWND h) {
        DWORD pid = 0;
        return !IsWindow(h) || (GetWindowThreadProcessId(h, &pid), pid != self);
    });
    for (HWND h : ownWindows) SetWindowDisplayAffinity(h, !cfg.recordable && !on ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    if (cfg.recordable) return;   // already capturable, already on wallpaper glass
    // Capturable windows would show up in our own desktop duplication (glass bending itself), so
    // for the length of the stream live screens are rebuilt from per-window captures over the
    // wallpaper instead (streamglass.cpp; just the wallpaper if that can't start).
    wchar_t wp[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0);
    if (!on) streamglass::Stop();
    for (auto& c : caps) {
        if (on) {
            c->wasLive = c->live;
            if (c->live) { c->dupl.Reset(); c->LoadStatic(wp); }
        } else if (c->wasLive) {
            c->wasLive = false;
            c->live = true;
            c->Start();   // Poll() retries every second if the screen isn't ready yet
        }
    }
    if (on) streamglass::Start(caps);
}

Capture* Gfx::CapFor(HMONITOR m) {
    for (auto& c : caps) if (c->mon == m) return c.get();
    return caps.empty() ? nullptr : caps[0].get();
}

bool Gfx::PollAll() {
    bool any = false;
    if (remoteView) streamglass::Poll();   // sets the screens' fresh flag, picked up below
    for (auto& c : caps) {
        if (c->live) any |= c->Poll();
        else if (c->fresh) { any = true; c->fresh = false; }
    }
    return any;
}

void Gfx::DrawGlass(Capture* cap, ID3D11RenderTargetView* rtv, int wx, int wy, int ww, int wh, float scale,
                    const std::vector<GlassShape>& sh, const float tint[3]) {
    if (!cap || !cap->have) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    CBData* c = (CBData*)m.pData;
    memset(c, 0, sizeof(CBData));
    c->origin[0] = (float)(wx - cap->rect.left);
    c->origin[1] = (float)(wy - cap->rect.top);
    c->texSize[0] = (float)cap->w; c->texSize[1] = (float)cap->h;
    c->winSize[0] = (float)ww; c->winSize[1] = (float)wh;
    c->scale = scale;
    c->count = (int)std::min<size_t>(kMaxShapes, sh.size());
    for (int k = 0; k < c->count; k++) {
        const GlassShape& s = sh[k];
        c->rects[k][0] = s.x; c->rects[k][1] = s.y; c->rects[k][2] = s.w; c->rects[k][3] = s.h;
        c->params[k][0] = s.radius; c->params[k][1] = s.blur; c->params[k][2] = s.tint; c->params[k][3] = s.bezel;
        c->params2[k][0] = s.shadow; c->params2[k][1] = s.thick; c->params2[k][2] = s.highlight; c->params2[k][3] = s.opacity;
        c->params3[k][0] = (float)s.group; c->params3[k][1] = std::max(1.f, s.melt); c->params3[k][2] = s.prism;
    }
    // the cursor is the light: glints slide toward it when it comes near the glass
    POINT cp;
    GetCursorPos(&cp);
    float lx = (float)(cp.x - wx), ly = (float)(cp.y - wy);
    float ox = std::max({0.f, -lx, lx - ww}), oy = std::max({0.f, -ly, ly - wh});
    c->light[0] = lx; c->light[1] = ly;
    c->light[2] = std::exp(-std::sqrt(ox * ox + oy * oy) / (120.f * scale));
    c->light[3] = backdropOpacity;
    for (int k = 0; k < 4; k++) c->maskInfo[k] = maskSrv ? maskInfo[k] : 0.f;
    c->tint[0] = tint[0]; c->tint[1] = tint[1]; c->tint[2] = tint[2]; c->tint[3] = 1;
    ctx->Unmap(cb.Get(), 0);

    D3D11_VIEWPORT vp{0, 0, (float)ww, (float)wh, 0, 1};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
    ctx->PSSetShaderResources(0, 1, cap->srv.GetAddressOf());
    ctx->PSSetShaderResources(1, 1, &maskSrv);
    ctx->PSSetSamplers(0, 1, samp.GetAddressOf());
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nsrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nsrv);
    ctx->PSSetShaderResources(1, 1, &nsrv);
    ID3D11RenderTargetView* nrtv = nullptr;
    ctx->OMSetRenderTargets(1, &nrtv, nullptr);
}

void Gfx::GrabLayer(ID3D11Texture2D* src, int x, int y, int w, int h) {
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc = {1, 0};
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return;
    ctx->CopyResource(st.Get(), src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    Layer L{x, y, w, h};
    L.px.resize((size_t)w * h * 4);
    for (int r = 0; r < h; r++) memcpy(&L.px[(size_t)r * w * 4], (BYTE*)m.pData + (size_t)r * m.RowPitch, (size_t)w * 4);
    ctx->Unmap(st.Get(), 0);
    snapLayers.push_back(std::move(L));
}

// Snapshot of the primary screen: captured desktop + our own layers on top.
bool Gfx::SaveSnapshot(const std::wstring& path) {
    Capture* cap = CapFor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    if (!cap || !cap->have) { snapLayers.clear(); return false; }
    UINT W = cap->w, H = cap->h;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return false;
    ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, cap->tex.Get(), 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    std::vector<BYTE> buf((size_t)W * H * 4);
    for (UINT r = 0; r < H; r++) memcpy(&buf[(size_t)r * W * 4], (BYTE*)m.pData + (size_t)r * m.RowPitch, (size_t)W * 4);
    ctx->Unmap(st.Get(), 0);
    for (auto& L : snapLayers) {
        for (int yy = 0; yy < L.h; yy++) {
            int dy = L.y - cap->rect.top + yy;
            if (dy < 0 || dy >= (int)H) continue;
            for (int xx = 0; xx < L.w; xx++) {
                int dx = L.x - cap->rect.left + xx;
                if (dx < 0 || dx >= (int)W) continue;
                const BYTE* s = &L.px[((size_t)yy * L.w + xx) * 4];
                BYTE* t = &buf[((size_t)dy * W + dx) * 4];
                int a = s[3];
                for (int ch = 0; ch < 3; ch++) t[ch] = (BYTE)std::min(255, s[ch] + t[ch] * (255 - a) / 255);
            }
        }
    }
    for (size_t i = 3; i < buf.size(); i += 4) buf[i] = 255;
    snapLayers.clear();

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fr;
    if (FAILED(wic->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    enc->CreateNewFrame(&fr, nullptr);
    fr->Initialize(nullptr);
    fr->SetSize(W, H);
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    fr->SetPixelFormat(&pf);
    fr->WritePixels(H, W * 4, (UINT)buf.size(), buf.data());
    fr->Commit();
    enc->Commit();
    Log(L"snapshot saved: %s", path.c_str());
    return true;
}

// ---------- Overlay ----------
bool Overlay::Create(const wchar_t* cls, WNDPROC proc, int x_, int y_, int w_, int h_, void* self) {
    x = x_; y = y_; w = w_; h = h_;
    cap = g.CapFor(MonitorFromPoint({x + w / 2, y + h / 2}, MONITOR_DEFAULTTONEAREST));
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    DWORD ex = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED;
    hwnd = CreateWindowExW(ex, cls, L"Liquid", WS_POPUP, x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { Log(L"CreateWindow failed %u", GetLastError()); return false; }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    g.ApplyCaptureMode(hwnd);

    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = w; d.Height = h;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    HRESULT hr = g.factory->CreateSwapChainForComposition(g.dev.Get(), &d, nullptr, &sc);
    if (FAILED(hr)) { Log(L"CreateSwapChainForComposition 0x%08X", hr); return false; }
    g.dcomp->CreateTargetForHwnd(hwnd, TRUE, &target);
    g.dcomp->CreateVisual(&visual);
    visual->SetContent(sc.Get());
    target->SetRoot(visual.Get());
    g.dcomp->Commit();

    ComPtr<ID3D11Texture2D> bb;
    sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    g.dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
    g.d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
    ComPtr<IDXGISurface> surf;
    sc->GetBuffer(0, IID_PPV_ARGS(&surf));
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &d2dTarget);
    dc->SetTarget(d2dTarget.Get());
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return true;
}

void Overlay::Move(int nx, int ny) {
    if (nx == x && ny == y) return;
    x = nx; y = ny;
    cap = g.CapFor(MonitorFromPoint({x + w / 2, y + h / 2}, MONITOR_DEFAULTTONEAREST));
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

void Overlay::Render(float scale, const std::vector<GlassShape>& shapes, const float tint[3],
                     const std::function<void(ID2D1DeviceContext*)>& ui) {
    float clear[4] = {0, 0, 0, 0};
    g.ctx->ClearRenderTargetView(rtv.Get(), clear);
    g.backdropOpacity = backdrop;
    g.maskSrv = mask;
    for (int k = 0; k < 4; k++) g.maskInfo[k] = maskInfo[k];
    if (!shapes.empty() || backdrop > 0 || mask) g.DrawGlass(cap, rtv.Get(), x, y, w, h, scale, shapes, tint);
    g.backdropOpacity = 0;
    g.maskSrv = nullptr;
    dc->BeginDraw();
    ui(dc.Get());
    dc->EndDraw();
    if (g.snapPending && visible) {
        ComPtr<ID3D11Texture2D> bb;
        sc->GetBuffer(0, IID_PPV_ARGS(&bb));
        g.GrabLayer(bb.Get(), x, y, w, h);
    }
    sc->Present(0, 0);
}

void Overlay::SetClickThrough(bool ct) {
    if (ct == clickThrough) return;
    clickThrough = ct;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex = ct ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
}

void Overlay::SetVisible(bool v) {
    if (v == visible) return;
    visible = v;
    ShowWindow(hwnd, v ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void Overlay::Destroy() {
    if (hwnd) DestroyWindow(hwnd);
    hwnd = nullptr;
}
