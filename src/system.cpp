// System bridge: Core Audio, WLAN, and (on a background MTA thread) WinRT radios + media sessions.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.ApplicationModel.h>
#undef GetCurrentTime
#include "shell.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wlanapi.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <map>
#include <algorithm>
#include <functional>
#include <cmath>

// ---------- default audio device switching (undocumented but stable since Vista) ----------
interface DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
class DECLSPEC_UUID("870af99c-171d-4f9e-af0d-e63df40c2bc9") CPolicyConfigClient;

namespace {
ComPtr<IMMDeviceEnumerator> s_enum;
ComPtr<IAudioEndpointVolume> s_vol;
std::wstring s_volDev;
ULONGLONG s_volCheck = 0;

HANDLE s_wlan = nullptr;
GUID s_if{};
bool s_hasIf = false;

std::thread s_thr;
std::atomic<bool> s_run{false};
std::mutex s_mx;
MediaInfo s_media;
int s_wifiR = -1, s_btR = -1;
std::vector<int> s_cmds;
std::vector<Notif> s_notifs;
int s_notifAccess = -1;
std::vector<uint32_t> s_removeIds;
bool s_clearAll = false;
HANDLE s_wake = nullptr;
enum { C_PREV, C_PLAY, C_NEXT, C_WIFI_ON, C_WIFI_OFF, C_BT_ON, C_BT_OFF };

IAudioEndpointVolume* Vol() {
    if (!s_enum) return nullptr;
    ULONGLONG now = GetTickCount64();
    if (s_vol && now - s_volCheck < 1000) return s_vol.Get();
    s_volCheck = now;
    ComPtr<IMMDevice> d;
    if (FAILED(s_enum->GetDefaultAudioEndpoint(eRender, eConsole, &d))) return nullptr;
    LPWSTR id = nullptr;
    d->GetId(&id);
    if (!s_vol || s_volDev != id) {
        s_vol.Reset();
        d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)s_vol.GetAddressOf());
        s_volDev = id;
    }
    CoTaskMemFree(id);
    return s_vol.Get();
}

std::wstring Ssid(const DOT11_SSID& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, (const char*)s.ucSSID, (int)s.uSSIDLength, nullptr, 0);
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, (const char*)s.ucSSID, (int)s.uSSIDLength, r.data(), n);
    return r;
}

std::wstring PrettyApp(std::wstring id) {
    size_t bang = id.find(L'!');
    if (bang != std::wstring::npos) id = id.substr(bang + 1);
    if (id.size() > 4 && Lower(id.substr(id.size() - 4)) == L".exe") id = id.substr(0, id.size() - 4);
    size_t dot = id.find_last_of(L'.');
    if (dot != std::wstring::npos && dot + 1 < id.size()) id = id.substr(dot + 1);
    if (!id.empty()) id[0] = towupper(id[0]);
    return id;
}

void Worker() {
    using namespace winrt;
    using namespace winrt::Windows::Media::Control;
    using namespace winrt::Windows::Devices::Radios;
    using namespace winrt::Windows::Storage::Streams;
    init_apartment(apartment_type::multi_threaded);
    GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
    try { mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get(); } catch (...) {}
    Radio wifi{nullptr}, bt{nullptr};
    try {
        Radio::RequestAccessAsync().get();
        for (auto r : Radio::GetRadiosAsync().get()) {
            if (r.Kind() == RadioKind::WiFi && !wifi) wifi = r;
            if (r.Kind() == RadioKind::Bluetooth && !bt) bt = r;
        }
    } catch (...) {}
    using namespace winrt::Windows::UI::Notifications;
    using namespace winrt::Windows::UI::Notifications::Management;
    UserNotificationListener listener{nullptr};
    int access = 0;
    try {
        listener = UserNotificationListener::Current();
        auto st = listener.GetAccessStatus();
        if (st == UserNotificationListenerAccessStatus::Unspecified) st = listener.RequestAccessAsync().get();
        access = st == UserNotificationListenerAccessStatus::Allowed ? 1 : 0;
    } catch (...) { access = 0; listener = nullptr; }
    Log(L"notifications: listener access %s", access ? L"allowed" : L"not available");
    ULONGLONG lastNotif = 0;
    std::wstring lastKey;
    std::vector<BYTE> art;
    int artVer = 0;
    while (s_run) {
        std::vector<int> cmds;
        { std::lock_guard<std::mutex> l(s_mx); cmds.swap(s_cmds); }
        for (int c : cmds) {
            try {
                if (c <= C_NEXT && mgr) {
                    auto ses = mgr.GetCurrentSession();
                    if (ses) {
                        if (c == C_PREV) ses.TrySkipPreviousAsync().get();
                        if (c == C_PLAY) ses.TryTogglePlayPauseAsync().get();
                        if (c == C_NEXT) ses.TrySkipNextAsync().get();
                    }
                }
                if ((c == C_WIFI_ON || c == C_WIFI_OFF) && wifi) wifi.SetStateAsync(c == C_WIFI_ON ? RadioState::On : RadioState::Off).get();
                if ((c == C_BT_ON || c == C_BT_OFF) && bt) bt.SetStateAsync(c == C_BT_ON ? RadioState::On : RadioState::Off).get();
            } catch (...) {}
        }
        if (access == 1 && listener) {
            std::vector<uint32_t> rm;
            bool clr = false;
            { std::lock_guard<std::mutex> l(s_mx); rm.swap(s_removeIds); clr = s_clearAll; s_clearAll = false; }
            try {
                if (clr) listener.ClearNotifications();
                for (auto id : rm) listener.RemoveNotification(id);
            } catch (...) {}
            if (clr || !rm.empty() || GetTickCount64() - lastNotif > 800) {
                lastNotif = GetTickCount64();
                std::vector<Notif> list;
                try {
                    auto all = listener.GetNotificationsAsync(NotificationKinds::Toast).get();
                    for (auto n : all) {
                        Notif x;
                        x.id = n.Id();
                        try { x.app = n.AppInfo().DisplayInfo().DisplayName().c_str(); x.aumid = n.AppInfo().AppUserModelId().c_str(); } catch (...) {}
                        x.time = n.CreationTime().time_since_epoch().count();   // 100ns ticks since 1601
                        try {
                            auto b = n.Notification().Visual().GetBinding(KnownNotificationBindings::ToastGeneric());
                            if (b) {
                                int k = 0;
                                for (auto t : b.GetTextElements()) {
                                    std::wstring tx = t.Text().c_str();
                                    if (k == 0) x.title = tx; else { if (!x.body.empty()) x.body += L" "; x.body += tx; }
                                    k++;
                                }
                            }
                        } catch (...) {}
                        list.push_back(std::move(x));
                    }
                } catch (...) {}
                std::sort(list.begin(), list.end(), [](const Notif& a, const Notif& b) { return a.time > b.time; });
                std::lock_guard<std::mutex> l(s_mx);
                s_notifs = std::move(list);
            }
        }
        { std::lock_guard<std::mutex> l(s_mx); s_notifAccess = access; }
        MediaInfo mi;
        try {
            auto ses = mgr ? mgr.GetCurrentSession() : nullptr;
            if (ses) {
                auto props = ses.TryGetMediaPropertiesAsync().get();
                auto pb = ses.GetPlaybackInfo();
                mi.present = true;
                mi.title = props.Title().c_str();
                mi.artist = props.Artist().c_str();
                mi.app = PrettyApp(ses.SourceAppUserModelId().c_str());
                mi.playing = pb.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                std::wstring key = mi.title + L"|" + mi.artist;
                if (key != lastKey) {
                    lastKey = key;
                    art.clear();
                    auto th = props.Thumbnail();
                    if (th) {
                        auto stream = th.OpenReadAsync().get();
                        uint32_t size = (uint32_t)stream.Size();
                        if (size && size < 8 * 1024 * 1024) {
                            Buffer buf(size);
                            auto res = stream.ReadAsync(buf, size, InputStreamOptions::None).get();
                            art.assign(res.data(), res.data() + res.Length());
                        }
                    }
                    artVer++;
                }
            } else if (!lastKey.empty()) {
                lastKey.clear(); art.clear(); artVer++;
            }
        } catch (...) {}
        mi.art = art;
        mi.artVersion = artVer;
        int wr = -1, br = -1;
        try { if (wifi) wr = wifi.State() == RadioState::On ? 1 : 0; } catch (...) {}
        try { if (bt) br = bt.State() == RadioState::On ? 1 : 0; } catch (...) {}
        {
            std::lock_guard<std::mutex> l(s_mx);
            s_media = std::move(mi);
            s_wifiR = wr;
            s_btR = br;
        }
        WaitForSingleObject(s_wake, 500);
    }
}

void Queue(int c) {
    { std::lock_guard<std::mutex> l(s_mx); s_cmds.push_back(c); }
    SetEvent(s_wake);
}
}  // namespace

namespace sys {
void Init() {
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&s_enum));
    DWORD ver = 0;
    if (WlanOpenHandle(2, nullptr, &ver, &s_wlan) == ERROR_SUCCESS) {
        PWLAN_INTERFACE_INFO_LIST list = nullptr;
        if (WlanEnumInterfaces(s_wlan, nullptr, &list) == ERROR_SUCCESS && list->dwNumberOfItems > 0) {
            s_if = list->InterfaceInfo[0].InterfaceGuid;
            s_hasIf = true;
        }
        if (list) WlanFreeMemory(list);
    }
    s_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s_run = true;
    s_thr = std::thread(Worker);
}

void Shutdown() {
    s_run = false;
    if (s_wake) SetEvent(s_wake);
    if (s_thr.joinable()) s_thr.join();
    if (s_wlan) WlanCloseHandle(s_wlan, nullptr);
}

// ---- volume ----
// When AudioRelay is streaming this PC's sound elsewhere, it captures the audio before Windows'
// master volume, so the master slider changes nothing you can hear. In that case Liquid's volume
// sets every app's own level (the per-app mixer), which happens before the capture.
static bool s_relay = false;
static ULONGLONG s_relayCheck = 0, s_sessApply = 0;
static float s_appVol = -1;
static bool s_appMute = false;

static bool RelayRunning() {
    ULONGLONG now = GetTickCount64();
    if (now - s_relayCheck < 3000) return s_relay;
    s_relayCheck = now;
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (!_wcsicmp(pe.szExeFile, L"audiorelay-backend.exe") || !_wcsicmp(pe.szExeFile, L"AudioRelay.exe")) { found = true; break; }
        CloseHandle(snap);
    }
    if (found && !s_relay) Log(L"volume: AudioRelay is running, Liquid's volume now sets each app's level");
    s_relay = found;
    return s_relay;
}

static void ForEachSession(const std::function<void(ISimpleAudioVolume*)>& f) {
    if (!s_enum) return;
    ComPtr<IMMDevice> d;
    if (FAILED(s_enum->GetDefaultAudioEndpoint(eRender, eConsole, &d))) return;
    ComPtr<IAudioSessionManager2> mgr;
    if (FAILED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)mgr.GetAddressOf()))) return;
    ComPtr<IAudioSessionEnumerator> en;
    if (FAILED(mgr->GetSessionEnumerator(&en))) return;
    int n = 0;
    en->GetCount(&n);
    for (int i = 0; i < n; i++) {
        ComPtr<IAudioSessionControl> c;
        if (FAILED(en->GetSession(i, &c))) continue;
        ComPtr<IAudioSessionControl2> c2;
        if (SUCCEEDED(c.As(&c2))) {
            DWORD pid = 0;
            c2->GetProcessId(&pid);
            std::wstring path = pid ? Lower(ProcessPath(pid)) : L"";
            if (path.find(L"audiorelay") != std::wstring::npos) continue;   // never turn the relay itself down
        }
        ComPtr<ISimpleAudioVolume> sv;
        if (SUCCEEDED(c.As(&sv))) f(sv.Get());
    }
}

static void LoadAppVol() {
    if (s_appVol >= 0) return;
    s_appVol = 0.6f;
    ForEachSession([](ISimpleAudioVolume* v) { float x; if (SUCCEEDED(v->GetMasterVolume(&x))) s_appVol = x; });
}

float Volume() {
    if (RelayRunning()) { LoadAppVol(); return s_appVol; }
    float v = 0;
    if (auto e = Vol()) e->GetMasterVolumeLevelScalar(&v);
    return v;
}
void SetVolume(float v) {
    v = std::clamp(v, 0.f, 1.f);
    if (RelayRunning()) {
        s_appVol = v;
        if (v > 0) s_appMute = false;
        ForEachSession([v](ISimpleAudioVolume* s) { s->SetMasterVolume(v, nullptr); if (v > 0) s->SetMute(FALSE, nullptr); });
        return;
    }
    if (auto e = Vol()) { e->SetMasterVolumeLevelScalar(v, nullptr); if (v > 0) e->SetMute(FALSE, nullptr); }
}
bool Muted() {
    if (RelayRunning()) return s_appMute;
    BOOL m = FALSE;
    if (auto e = Vol()) e->GetMute(&m);
    return m != FALSE;
}
void SetMuted(bool m) {
    if (RelayRunning()) {
        s_appMute = m;
        ForEachSession([m](ISimpleAudioVolume* s) { s->SetMute(m, nullptr); });
        return;
    }
    if (auto e = Vol()) e->SetMute(m, nullptr);
}
// apps that start playing later get the same level
void AudioTick() {
    ULONGLONG now = GetTickCount64();
    if (now - s_sessApply < 1500 || !RelayRunning() || s_appVol < 0) return;
    s_sessApply = now;
    float v = s_appVol;
    bool m = s_appMute;
    ForEachSession([v, m](ISimpleAudioVolume* s) {
        float cur = 0; BOOL mu = FALSE;
        s->GetMasterVolume(&cur); s->GetMute(&mu);
        if (std::fabs(cur - v) > 0.004f) s->SetMasterVolume(v, nullptr);
        if ((mu != FALSE) != m) s->SetMute(m, nullptr);
    });
}

std::vector<AudioDevice> OutputDevices() {
    std::vector<AudioDevice> out;
    if (!s_enum) return out;
    std::wstring def;
    ComPtr<IMMDevice> d;
    if (SUCCEEDED(s_enum->GetDefaultAudioEndpoint(eRender, eConsole, &d))) {
        LPWSTR id = nullptr;
        d->GetId(&id);
        def = id;
        CoTaskMemFree(id);
    }
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(s_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        ComPtr<IMMDevice> dev;
        col->Item(i, &dev);
        LPWSTR id = nullptr;
        dev->GetId(&id);
        AudioDevice ad;
        ad.id = id;
        CoTaskMemFree(id);
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) ad.name = pv.pwszVal;
            PropVariantClear(&pv);
        }
        ad.isDefault = ad.id == def;
        out.push_back(ad);
    }
    return out;
}

void SetDefaultOutput(const std::wstring& id) {
    ComPtr<IPolicyConfig> pc;
    if (FAILED(CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), (void**)pc.GetAddressOf()))) return;
    pc->SetDefaultEndpoint(id.c_str(), eConsole);
    pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
    pc->SetDefaultEndpoint(id.c_str(), eCommunications);
    s_volCheck = 0;
}

bool WifiAvailable() { return s_hasIf; }

std::wstring WifiConnectedSsid() {
    if (!s_hasIf) return L"";
    DWORD size = 0;
    PWLAN_CONNECTION_ATTRIBUTES ca = nullptr;
    WLAN_OPCODE_VALUE_TYPE vt;
    std::wstring r;
    if (WlanQueryInterface(s_wlan, &s_if, wlan_intf_opcode_current_connection, nullptr, &size, (PVOID*)&ca, &vt) == ERROR_SUCCESS) {
        if (ca->isState == wlan_interface_state_connected) r = Ssid(ca->wlanAssociationAttributes.dot11Ssid);
        WlanFreeMemory(ca);
    }
    return r;
}

std::vector<WifiNet> WifiNetworks() {
    std::vector<WifiNet> out;
    if (!s_hasIf) return out;
    PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
    if (WlanGetAvailableNetworkList(s_wlan, &s_if, 0, nullptr, &list) != ERROR_SUCCESS) return out;
    std::map<std::wstring, WifiNet> best;
    for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
        auto& n = list->Network[i];
        std::wstring ssid = Ssid(n.dot11Ssid);
        if (ssid.empty()) continue;
        WifiNet w;
        w.ssid = ssid;
        if (n.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) w.profile = n.strProfileName;
        w.signal = (int)n.wlanSignalQuality;
        w.secure = n.bSecurityEnabled != FALSE;
        w.connected = (n.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
        auto it = best.find(ssid);
        if (it == best.end() || w.connected || (!it->second.connected && w.signal > it->second.signal)) {
            if (it != best.end() && w.profile.empty()) w.profile = it->second.profile;
            best[ssid] = w;
        }
    }
    WlanFreeMemory(list);
    for (auto& [k, v] : best) out.push_back(v);
    std::sort(out.begin(), out.end(), [](const WifiNet& a, const WifiNet& b) {
        if (a.connected != b.connected) return a.connected;
        return a.signal > b.signal;
    });
    return out;
}

void WifiScan() {
    if (s_hasIf) WlanScan(s_wlan, &s_if, nullptr, nullptr, nullptr);
}

void WifiConnect(const std::wstring& profile) {
    if (!s_hasIf || profile.empty()) return;
    WLAN_CONNECTION_PARAMETERS p{};
    p.wlanConnectionMode = wlan_connection_mode_profile;
    p.strProfile = profile.c_str();
    p.dot11BssType = dot11_BSS_type_any;
    WlanConnect(s_wlan, &s_if, &p, nullptr);
}

bool EthernetUp() {
    ULONG size = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, nullptr, nullptr, &size);
    std::vector<BYTE> buf(size);
    auto* a = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, nullptr, a, &size) != NO_ERROR)
        return false;
    for (; a; a = a->Next) {
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD || a->OperStatus != IfOperStatusUp) continue;
        std::wstring d = Lower(a->Description ? a->Description : L"");
        if (d.find(L"virtual") != std::wstring::npos || d.find(L"hyper-v") != std::wstring::npos ||
            d.find(L"vmware") != std::wstring::npos || d.find(L"tap") != std::wstring::npos ||
            d.find(L"loopback") != std::wstring::npos || d.find(L"bluetooth") != std::wstring::npos)
            continue;
        if (a->FirstGatewayAddress) return true;
    }
    return false;
}

int WifiRadio() { std::lock_guard<std::mutex> l(s_mx); return s_wifiR; }
int BluetoothRadio() { std::lock_guard<std::mutex> l(s_mx); return s_btR; }
void SetWifiRadio(bool on) { Queue(on ? C_WIFI_ON : C_WIFI_OFF); }
void SetBluetoothRadio(bool on) { Queue(on ? C_BT_ON : C_BT_OFF); }
MediaInfo Media() { std::lock_guard<std::mutex> l(s_mx); return s_media; }
std::vector<Notif> Notifications() { std::lock_guard<std::mutex> l(s_mx); return s_notifs; }
int NotificationAccess() { std::lock_guard<std::mutex> l(s_mx); return s_notifAccess; }
void DismissNotification(uint32_t id) {
    { std::lock_guard<std::mutex> l(s_mx); s_removeIds.push_back(id);
      s_notifs.erase(std::remove_if(s_notifs.begin(), s_notifs.end(), [&](const Notif& n) { return n.id == id; }), s_notifs.end()); }
    SetEvent(s_wake);
}
void ClearNotifications() {
    { std::lock_guard<std::mutex> l(s_mx); s_clearAll = true; s_notifs.clear(); }
    SetEvent(s_wake);
}
void MediaCommand(int cmd) { Queue(cmd == 0 ? C_PREV : cmd == 1 ? C_PLAY : C_NEXT); }
}  // namespace sys
