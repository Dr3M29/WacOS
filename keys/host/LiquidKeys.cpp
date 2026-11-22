// LiquidKeys.exe: the vault behind the Liquid Keys browser extension.
//
// The browser starts it (native messaging: 4-byte length + UTF-8 JSON on stdin/stdout) for each
// request. It keeps passkeys for websites and signs sign-in challenges with them, after Air Ring
// has seen your face.
//
//   keys      ECDSA P-256, one per passkey, in Windows' software key store (CNG, your user
//             account), named LiquidKeys-<credential id>. Never leave that store.
//   vault     %LOCALAPPDATA%\Liquid\keys\vault.bin: the list of passkeys (site, account name, key
//             name), encrypted with DPAPI for your Windows account.
//   face      sets Local\LiquidFaceVerifying and waits for Air Ring to set Local\LiquidFaceVerified
//             (face_unlock.py). The island shows its Face ID square meanwhile.
//
// Requests (the extension's background script has already checked that rpId belongs to the page):
//   {"cmd":"list","rpId":"example.com"}
//   {"cmd":"create","rpId","rpName","userId"(b64url),"userName","displayName","clientDataHash"(b64url),"exclude":[ids]}
//   {"cmd":"get","rpId","allow":[ids],"clientDataHash"}
//   {"cmd":"ping"}
// Replies: {"ok":true,...} or {"ok":false,"error":"...","code":"NotAllowedError"|...}
//
// Standalone: LiquidKeys.exe --list prints the vault; --selftest runs create + sign without a face.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>
#include <dpapi.h>
#include <shlobj.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include <utility>
#include <mutex>
#include <thread>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------- tiny JSON
struct Json {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json& operator[](const char* k) const {
        static Json none;
        for (auto& kv : o) if (kv.first == k) return kv.second;
        return none;
    }
    std::string str(const char* k) const { const Json& v = (*this)[k]; return v.t == Str ? v.s : ""; }
    static Json S(const std::string& v) { Json j; j.t = Str; j.s = v; return j; }
    static Json B(bool v) { Json j; j.t = Bool; j.b = v; return j; }
    static Json N(double v) { Json j; j.t = Num; j.n = v; return j; }
    static Json O() { Json j; j.t = Obj; return j; }
    static Json A() { Json j; j.t = Arr; return j; }
    Json& set(const std::string& k, Json v) {
        for (auto& kv : o) if (kv.first == k) { kv.second = std::move(v); return *this; }
        o.emplace_back(k, std::move(v));
        return *this;
    }
};

struct Parser {
    const std::string& t;
    size_t i = 0;
    bool bad = false;
    explicit Parser(const std::string& s) : t(s) {}
    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++; }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 63)); out += (char)(0x80 | (cp & 63)); }
        else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 63)); out += (char)(0x80 | (cp >> 6 & 63)); out += (char)(0x80 | (cp & 63)); }
    }
    std::string string() {
        std::string out;
        i++;   // opening quote
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= t.size()) break;
            char e = t[i++];
            switch (e) {
                case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                case 'u': {
                    if (i + 4 > t.size()) { bad = true; return out; }
                    unsigned cp = std::stoul(t.substr(i, 4), nullptr, 16); i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        unsigned lo = std::stoul(t.substr(i + 2, 4), nullptr, 16); i += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: out += e;
            }
        }
        i++;   // closing quote
        return out;
    }
    Json value() {
        ws();
        Json j;
        if (i >= t.size()) { bad = true; return j; }
        char c = t[i];
        if (c == '{') {
            j.t = Json::Obj; i++; ws();
            if (i < t.size() && t[i] == '}') { i++; return j; }
            while (i < t.size()) {
                ws();
                if (t[i] != '"') { bad = true; return j; }
                std::string k = string();
                ws();
                if (i >= t.size() || t[i] != ':') { bad = true; return j; }
                i++;
                j.o.emplace_back(k, value());
                ws();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == '}') { i++; return j; }
                bad = true; return j;
            }
        } else if (c == '[') {
            j.t = Json::Arr; i++; ws();
            if (i < t.size() && t[i] == ']') { i++; return j; }
            while (i < t.size()) {
                j.a.push_back(value());
                ws();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == ']') { i++; return j; }
                bad = true; return j;
            }
        } else if (c == '"') { j.t = Json::Str; j.s = string(); }
        else if (t.compare(i, 4, "true") == 0) { j.t = Json::Bool; j.b = true; i += 4; }
        else if (t.compare(i, 5, "false") == 0) { j.t = Json::Bool; i += 5; }
        else if (t.compare(i, 4, "null") == 0) { i += 4; }
        else {
            size_t e = i;
            while (e < t.size() && strchr("+-0123456789.eE", t[e])) e++;
            if (e == i) { bad = true; return j; }
            j.t = Json::Num; j.n = atof(t.substr(i, e - i).c_str()); i = e;
        }
        return j;
    }
};

static void Dump(const Json& j, std::string& out) {
    switch (j.t) {
        case Json::Null: out += "null"; break;
        case Json::Bool: out += j.b ? "true" : "false"; break;
        case Json::Num: { char b[32]; snprintf(b, sizeof b, "%.17g", j.n); out += b; break; }
        case Json::Str:
            out += '"';
            for (unsigned char c : j.s) {
                if (c == '"') out += "\\\""; else if (c == '\\') out += "\\\\";
                else if (c == '\n') out += "\\n"; else if (c == '\r') out += "\\r"; else if (c == '\t') out += "\\t";
                else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += (char)c;
            }
            out += '"';
            break;
        case Json::Arr:
            out += '[';
            for (size_t k = 0; k < j.a.size(); k++) { if (k) out += ','; Dump(j.a[k], out); }
            out += ']';
            break;
        case Json::Obj:
            out += '{';
            for (size_t k = 0; k < j.o.size(); k++) {
                if (k) out += ',';
                Dump(Json::S(j.o[k].first), out);
                out += ':';
                Dump(j.o[k].second, out);
            }
            out += '}';
            break;
    }
}

// ---------------------------------------------------------------- base64url, hex
static std::string B64u(const Bytes& d) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o;
    for (size_t i = 0; i < d.size(); i += 3) {
        unsigned v = d[i] << 16 | (i + 1 < d.size() ? d[i + 1] << 8 : 0) | (i + 2 < d.size() ? d[i + 2] : 0);
        o += T[v >> 18 & 63]; o += T[v >> 12 & 63];
        if (i + 1 < d.size()) o += T[v >> 6 & 63];
        if (i + 2 < d.size()) o += T[v & 63];
    }
    return o;
}
static Bytes UnB64u(const std::string& s) {
    Bytes o;
    unsigned v = 0; int bits = 0;
    for (char c : s) {
        int x = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : (c == '-' || c == '+') ? 62 : (c == '_' || c == '/') ? 63 : -1;
        if (x < 0) continue;
        v = v << 6 | x; bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back((uint8_t)(v >> bits & 0xFF)); }
    }
    return o;
}
static std::wstring Hex(const Bytes& d) {
    std::wstring o;
    wchar_t b[3];
    for (auto c : d) { swprintf(b, 3, L"%02x", c); o += b; }
    return o;
}

// ---------------------------------------------------------------- crypto
static Bytes Sha256(const Bytes& d) {
    Bytes h(32);
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)d.data(), (ULONG)d.size(), h.data(), 32);
    return h;
}
static Bytes Random(size_t n) {
    Bytes b(n);
    BCryptGenRandom(nullptr, b.data(), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return b;
}

struct Key {
    NCRYPT_PROV_HANDLE prov = 0;
    NCRYPT_KEY_HANDLE key = 0;
    ~Key() { if (key) NCryptFreeObject(key); if (prov) NCryptFreeObject(prov); }
};

static std::wstring KeyName(const Bytes& credId) { return L"LiquidKeys-" + Hex(credId); }

static bool CreateKey(const Bytes& credId, Bytes& x, Bytes& y) {
    Key k;
    if (NCryptOpenStorageProvider(&k.prov, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return false;
    if (NCryptCreatePersistedKey(k.prov, &k.key, NCRYPT_ECDSA_P256_ALGORITHM, KeyName(credId).c_str(), 0, 0) != ERROR_SUCCESS) return false;
    if (NCryptFinalizeKey(k.key, 0) != ERROR_SUCCESS) return false;
    BYTE blob[200]; DWORD n = 0;
    if (NCryptExportKey(k.key, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, blob, sizeof blob, &n, 0) != ERROR_SUCCESS) return false;
    auto* h = (BCRYPT_ECCKEY_BLOB*)blob;
    if (h->cbKey != 32) return false;
    x.assign(blob + sizeof(*h), blob + sizeof(*h) + 32);
    y.assign(blob + sizeof(*h) + 32, blob + sizeof(*h) + 64);
    return true;
}

static void DeleteKey(const Bytes& credId) {
    Key k;
    if (NCryptOpenStorageProvider(&k.prov, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return;
    if (NCryptOpenKey(k.prov, &k.key, KeyName(credId).c_str(), 0, 0) == ERROR_SUCCESS) { NCryptDeleteKey(k.key, 0); k.key = 0; }
}

// DER-encode one positive big-endian integer for an ECDSA signature.
static void DerInt(Bytes& o, const uint8_t* p, size_t n) {
    while (n > 1 && *p == 0) { p++; n--; }
    bool pad = (*p & 0x80) != 0;
    o.push_back(0x02);
    o.push_back((uint8_t)(n + (pad ? 1 : 0)));
    if (pad) o.push_back(0);
    o.insert(o.end(), p, p + n);
}

// ES256 signature (DER, as WebAuthn wants) over data.
static bool Sign(const Bytes& credId, const Bytes& data, Bytes& der) {
    Key k;
    if (NCryptOpenStorageProvider(&k.prov, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return false;
    if (NCryptOpenKey(k.prov, &k.key, KeyName(credId).c_str(), 0, 0) != ERROR_SUCCESS) return false;
    Bytes h = Sha256(data);
    BYTE rs[64]; DWORD n = 0;
    if (NCryptSignHash(k.key, nullptr, h.data(), 32, rs, sizeof rs, &n, 0) != ERROR_SUCCESS || n != 64) return false;
    Bytes body;
    DerInt(body, rs, 32);
    DerInt(body, rs + 32, 32);
    der = {0x30, (uint8_t)body.size()};
    der.insert(der.end(), body.begin(), body.end());
    return true;
}

// ---------------------------------------------------------------- CBOR (just what WebAuthn needs)
static void CborHead(Bytes& o, int major, uint64_t v) {
    uint8_t m = (uint8_t)(major << 5);
    if (v < 24) o.push_back(m | (uint8_t)v);
    else if (v < 256) { o.push_back(m | 24); o.push_back((uint8_t)v); }
    else if (v < 65536) { o.push_back(m | 25); o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v); }
    else { o.push_back(m | 26); for (int s = 24; s >= 0; s -= 8) o.push_back((uint8_t)(v >> s)); }
}
static void CborInt(Bytes& o, int64_t v) { if (v >= 0) CborHead(o, 0, (uint64_t)v); else CborHead(o, 1, (uint64_t)(-1 - v)); }
static void CborBytes(Bytes& o, const Bytes& b) { CborHead(o, 2, b.size()); o.insert(o.end(), b.begin(), b.end()); }
static void CborText(Bytes& o, const std::string& s) { CborHead(o, 3, s.size()); o.insert(o.end(), s.begin(), s.end()); }

static Bytes CoseKey(const Bytes& x, const Bytes& y) {
    Bytes o;
    CborHead(o, 5, 5);
    CborInt(o, 1); CborInt(o, 2);     // kty: EC2
    CborInt(o, 3); CborInt(o, -7);    // alg: ES256
    CborInt(o, -1); CborInt(o, 1);    // crv: P-256
    CborInt(o, -2); CborBytes(o, x);
    CborInt(o, -3); CborBytes(o, y);
    return o;
}

static Bytes Spki(const Bytes& x, const Bytes& y) {   // SubjectPublicKeyInfo DER for a P-256 key
    Bytes o = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86,
               0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04};
    o.insert(o.end(), x.begin(), x.end());
    o.insert(o.end(), y.begin(), y.end());
    return o;
}

// authenticatorData: rpIdHash | flags | signCount | [attested credential data]
static const uint8_t F_UP = 0x01, F_UV = 0x04, F_AT = 0x40;
static Bytes AuthData(const std::string& rpId, uint8_t flags, const Bytes* attested) {
    Bytes o = Sha256(Bytes(rpId.begin(), rpId.end()));
    o.push_back(flags);
    o.insert(o.end(), {0, 0, 0, 0});   // signature counter: always 0 (allowed; this vault isn't a counter device)
    if (attested) o.insert(o.end(), attested->begin(), attested->end());
    return o;
}

// ---------------------------------------------------------------- vault
static std::wstring VaultPath() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) r = p;
    if (p) CoTaskMemFree(p);
    std::wstring dir = r + L"\\Liquid";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\keys";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\vault.bin";
}

static Json LoadVault() {
    Json v = Json::O();
    v.set("creds", Json::A());
    HANDLE f = CreateFileW(VaultPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return v;
    DWORD size = GetFileSize(f, nullptr), rd = 0;
    Bytes enc(size);
    ReadFile(f, enc.data(), size, &rd, nullptr);
    CloseHandle(f);
    DATA_BLOB in{rd, enc.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return v;
    std::string text((char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    Parser p(text);
    Json j = p.value();
    if (!p.bad && j.t == Json::Obj && j["creds"].t == Json::Arr) v = j;
    return v;
}

static bool SaveVault(const Json& v) {
    std::string text;
    Dump(v, text);
    DATA_BLOB in{(DWORD)text.size(), (BYTE*)text.data()}, out{};
    if (!CryptProtectData(&in, L"Liquid Keys vault", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    std::wstring path = VaultPath(), tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    bool ok = f != INVALID_HANDLE_VALUE;
    DWORD wr = 0;
    if (ok) { ok = WriteFile(f, out.pbData, out.cbData, &wr, nullptr) && wr == out.cbData; CloseHandle(f); }
    LocalFree(out.pbData);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ---------------------------------------------------------------- face (Air Ring)
static thread_local std::string g_faceError;
// "AirRingOff": no camera check is possible, so the extension hands the site to Windows Hello.
// "NotAllowedError": the check ran and didn't see you in time; the site gets that and can retry.
static thread_local const char* g_faceCode = "NotAllowedError";

// What the Dynamic Island shows while it waits: "save" or "signin", and the site.
static void WritePending(const char* action, const std::string& site) {
    std::wstring p = VaultPath();
    p = p.substr(0, p.find_last_of(L'\\')) + L"\\pending.txt";
    FILE* f = _wfsopen(p.c_str(), L"wb", _SH_DENYNO);
    if (!f) return;
    fprintf(f, "%s\n%s\n", action, site.c_str());
    fclose(f);
}

static bool VerifyFace(DWORD timeoutMs, const char* action, const std::string& site) {
    HANDLE ring = OpenMutexW(SYNCHRONIZE, FALSE, L"AirRingSingleInstance");
    g_faceCode = "AirRingOff";
    if (!ring) { g_faceError = "Air Ring isn't running, so Liquid can't see your face. Start it from Liquid's Air Gestures settings."; return false; }
    CloseHandle(ring);
    HANDLE verifying = CreateEventW(nullptr, TRUE, FALSE, L"Local\\LiquidFaceVerifying");
    HANDLE ok = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidFaceVerified");
    if (!verifying || !ok) { g_faceError = "couldn't reach Air Ring"; return false; }
    g_faceCode = "NotAllowedError";
    ResetEvent(ok);
    WritePending(action, site);
    SetEvent(verifying);
    DWORD r = WaitForSingleObject(ok, timeoutMs);
    ResetEvent(verifying);
    CloseHandle(verifying);
    CloseHandle(ok);
    if (r != WAIT_OBJECT_0) g_faceError = "Face not recognized in time.";
    return r == WAIT_OBJECT_0;
}

// ---------------------------------------------------------------- requests
static Json Fail(const std::string& msg, const char* code = "NotAllowedError") {
    Json r = Json::O();
    r.set("ok", Json::B(false)).set("error", Json::S(msg)).set("code", Json::S(code));
    return r;
}

static std::mutex g_vaultMx;

static bool Listed(const Json& arr, const std::string& id) {
    for (auto& e : arr.a) if (e.t == Json::Str && e.s == id) return true;
    return false;
}

static Json Handle(const Json& req, bool faceCheck = true) {
    std::string cmd = req.str("cmd"), rpId = req.str("rpId");
    if (cmd == "ping") { Json r = Json::O(); r.set("ok", Json::B(true)).set("version", Json::S("1")); return r; }
    if (cmd != "list" && cmd != "create" && cmd != "get") return Fail("unknown command", "NotSupportedError");
    if (rpId.empty()) return Fail("no site", "SecurityError");
    // The service answers several requests at once: the vault file is read and written under a
    // lock, and the face wait (up to 15 s) happens with the lock released.
    std::unique_lock<std::mutex> lock(g_vaultMx);
    Json vault = LoadVault();
    Json creds = vault["creds"];

    if (cmd == "list") {
        Json out = Json::A();
        for (auto& c : creds.a) {
            if (c.str("rp") != rpId) continue;
            Json e = Json::O();
            e.set("id", Json::S(c.str("id"))).set("user", Json::S(c.str("user"))).set("name", Json::S(c.str("name")))
             .set("display", Json::S(c.str("display")));
            out.a.push_back(e);
        }
        Json r = Json::O();
        r.set("ok", Json::B(true)).set("creds", out);
        return r;
    }

    Bytes cdh = UnB64u(req.str("clientDataHash"));
    if (cdh.size() != 32) return Fail("bad client data hash", "SecurityError");

    if (cmd == "create") {
        const Json& exclude = req["exclude"];
        for (auto& c : creds.a)
            if (c.str("rp") == rpId && Listed(exclude, c.str("id"))) return Fail("Liquid already has a passkey for this account.", "InvalidStateError");
        lock.unlock();
        if (faceCheck && !VerifyFace(15000, "save", rpId)) return Fail(g_faceError, g_faceCode);
        lock.lock();
        vault = LoadVault();                                    // may have changed while we waited
        creds = vault["creds"];
        Bytes credId = Random(16), x, y;
        if (!CreateKey(credId, x, y)) return Fail("couldn't make a key", "UnknownError");
        Bytes attested(16, 0);                                  // AAGUID: none
        attested.push_back((uint8_t)(credId.size() >> 8));
        attested.push_back((uint8_t)credId.size());
        attested.insert(attested.end(), credId.begin(), credId.end());
        Bytes cose = CoseKey(x, y);
        attested.insert(attested.end(), cose.begin(), cose.end());
        Bytes auth = AuthData(rpId, F_UP | F_UV | F_AT, &attested);
        Bytes att;
        CborHead(att, 5, 3);
        CborText(att, "fmt"); CborText(att, "none");
        CborText(att, "attStmt"); CborHead(att, 5, 0);
        CborText(att, "authData"); CborBytes(att, auth);
        Json c = Json::O();
        c.set("id", Json::S(B64u(credId))).set("rp", Json::S(rpId)).set("rpName", Json::S(req.str("rpName")))
         .set("user", Json::S(req.str("userId"))).set("name", Json::S(req.str("userName")))
         .set("display", Json::S(req.str("displayName"))).set("created", Json::N((double)time(nullptr)));
        creds.a.push_back(c);
        vault.set("creds", creds);
        if (!SaveVault(vault)) { DeleteKey(credId); return Fail("couldn't save the vault", "UnknownError"); }
        Json r = Json::O();
        r.set("ok", Json::B(true)).set("id", Json::S(B64u(credId))).set("authenticatorData", Json::S(B64u(auth)))
         .set("attestationObject", Json::S(B64u(att))).set("publicKey", Json::S(B64u(Spki(x, y))))
         .set("publicKeyAlgorithm", Json::N(-7));
        return r;
    }

    // get
    const Json& allow = req["allow"];
    const Json* pick = nullptr;
    for (auto& c : creds.a) {
        if (c.str("rp") != rpId) continue;
        if (allow.t == Json::Arr && !allow.a.empty() && !Listed(allow, c.str("id"))) continue;
        if (!pick || c["created"].n > (*pick)["created"].n) pick = &c;   // newest
    }
    if (!pick) return Fail("Liquid has no passkey for this site.", "NotFoundError");
    std::string id = pick->str("id"), user = pick->str("user"), name = pick->str("name");
    lock.unlock();
    if (faceCheck && !VerifyFace(15000, "signin", rpId)) return Fail(g_faceError, g_faceCode);
    Bytes auth = AuthData(rpId, F_UP | F_UV, nullptr), data = auth, sig;
    data.insert(data.end(), cdh.begin(), cdh.end());
    if (!Sign(UnB64u(id), data, sig)) return Fail("couldn't sign: the key is missing", "UnknownError");
    Json r = Json::O();
    r.set("ok", Json::B(true)).set("id", Json::S(id)).set("authenticatorData", Json::S(B64u(auth)))
     .set("signature", Json::S(B64u(sig))).set("userHandle", Json::S(user)).set("userName", Json::S(name));
    return r;
}

// ---------------------------------------------------------------- native messaging
static bool ReadMsg(std::string& out) {
    uint32_t n = 0;
    if (fread(&n, 4, 1, stdin) != 1 || n > 1024 * 1024) return false;
    out.resize(n);
    return fread(out.data(), 1, n, stdin) == n;
}
static void WriteMsg(const Json& j) {
    std::string s;
    Dump(j, s);
    uint32_t n = (uint32_t)s.size();
    fwrite(&n, 4, 1, stdout);
    fwrite(s.data(), 1, n, stdout);
    fflush(stdout);
}

// ---------------------------------------------------------------- local service (--serve)
// The same requests over HTTP on 127.0.0.1, for browsers that don't find the native messaging
// registration (Opera 140 on 2026-11-21 said "not found" with every registry entry in place).
// Only the Liquid Keys extension is answered: browsers put the extension's origin in the Origin
// header of its requests, and a web page can't fake that. The request must also carry
// "X-Liquid-Keys: 1", which a page can't send to another origin without a CORS preflight, and
// preflights are only answered for the extension. Host must be 127.0.0.1 or localhost (no DNS
// rebinding). Liquid starts this at sign-in; one copy runs (mutex Local\LiquidKeysServer).
static const USHORT kPort = 47811;
static const char* kOrigins[] = {"chrome-extension://pgicggdhfghafaheccjhkpfkjkaiddoc"};

static void ServerLog(const char* fmt, const char* a = "", const char* b = "") {
    std::wstring path = VaultPath();                       // also creates %LOCALAPPDATA%\Liquid\keys
    path = path.substr(0, path.find_last_of(L'\\')) + L"\\server.log";
    FILE* f = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    fprintf(f, fmt, a, b);
    fputc('\n', f);
    fclose(f);
}

static std::string Header(const std::string& head, const char* name) {
    std::string lower = head;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)tolower(c); });
    std::string key = std::string("\r\n") + name + ":";
    size_t p = lower.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    size_t e = head.find("\r\n", p);
    std::string v = head.substr(p, e == std::string::npos ? std::string::npos : e - p);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    return v;
}

static void Reply(SOCKET c, int code, const char* status, const std::string& origin, const std::string& body) {
    std::string r = "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n";
    if (!origin.empty()) {
        r += "Access-Control-Allow-Origin: " + origin + "\r\nVary: Origin\r\n"
             "Access-Control-Allow-Methods: POST\r\nAccess-Control-Allow-Headers: content-type, x-liquid-keys\r\n";
    }
    r += "Content-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    send(c, r.data(), (int)r.size(), 0);
}

static void ServeClient(SOCKET c) {
    DWORD timeout = 5000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout);
    std::string buf;
    char tmp[4096];
    size_t he;
    while ((he = buf.find("\r\n\r\n")) == std::string::npos) {
        int n = recv(c, tmp, sizeof tmp, 0);
        if (n <= 0 || buf.size() > 16384) { closesocket(c); return; }
        buf.append(tmp, n);
    }
    std::string head = buf.substr(0, he + 2), body = buf.substr(he + 4);
    std::string method = head.substr(0, head.find(' '));
    size_t ps = head.find(' ') + 1;
    std::string path = head.substr(ps, head.find(' ', ps) - ps);
    std::string origin = Header(head, "origin"), host = Header(head, "host");
    bool allowed = false;
    for (auto* o : kOrigins) if (origin == o) allowed = true;
    std::string hostOk1 = "127.0.0.1:" + std::to_string(kPort), hostOk2 = "localhost:" + std::to_string(kPort);
    if (host != hostOk1 && host != hostOk2) allowed = false;
    if (!allowed) {
        ServerLog("refused %s from origin \"%s\"", method.c_str(), (origin + "\", host \"" + host).c_str());
        Reply(c, 403, "Forbidden", "", "{\"ok\":false,\"error\":\"not the Liquid Keys extension\",\"code\":\"SecurityError\"}");
        closesocket(c);
        return;
    }
    if (method == "OPTIONS") { Reply(c, 204, "No Content", origin, ""); closesocket(c); return; }
    std::string cl = Header(head, "content-length");
    size_t len = (size_t)atoi(cl.c_str());
    if (method == "POST" && cl.empty()) {                  // browsers always send it; chunked isn't supported
        Reply(c, 411, "Length Required", origin, "{\"ok\":false,\"error\":\"Content-Length needed\",\"code\":\"SyntaxError\"}");
        closesocket(c);
        return;
    }
    if (method != "POST" || path != "/v1" || Header(head, "x-liquid-keys") != "1" || len > 1024 * 1024) {
        Reply(c, 400, "Bad Request", origin, "{\"ok\":false,\"error\":\"bad request\",\"code\":\"SyntaxError\"}");
        closesocket(c);
        return;
    }
    while (body.size() < len) {
        int n = recv(c, tmp, sizeof tmp, 0);
        if (n <= 0) { closesocket(c); return; }
        body.append(tmp, n);
    }
    body.resize(len);
    Parser p(body);
    Json req = p.value();
    Json res = p.bad ? Fail("bad request", "SyntaxError") : Handle(req);
    std::string out;
    Dump(res, out);
    Reply(c, 200, "OK", origin, out);
    closesocket(c);
}

static int Serve() {
    HANDLE one = CreateMutexW(nullptr, TRUE, L"Local\\LiquidKeysServer");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return 1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kPort);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(s, (sockaddr*)&a, sizeof a) != 0 || listen(s, 16) != 0) {
        ServerLog("can't listen on 127.0.0.1:47811 (in use?)");
        return 1;
    }
    ServerLog("serving on 127.0.0.1:47811");
    for (;;) {
        SOCKET c = accept(s, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::thread(ServeClient, c).detach();
    }
    (void)one;
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && wcscmp(argv[1], L"--serve") == 0) return Serve();
    if (argc > 1 && wcscmp(argv[1], L"--list") == 0) {
        Json v = LoadVault();
        for (auto& c : v["creds"].a) printf("%s  %s  %s\n", c.str("rp").c_str(), c.str("name").c_str(), c.str("id").c_str());
        return 0;
    }
    if (argc > 1 && wcscmp(argv[1], L"--selftest") == 0) {
        // create + sign for a test site without a face check, print the results for test/verify.js, then delete
        Bytes cdh = Sha256(Bytes{'t', 'e', 's', 't'});
        Json c = Json::O();
        c.set("cmd", Json::S("create")).set("rpId", Json::S("selftest.liquid.invalid")).set("userId", Json::S("dQ"))
         .set("userName", Json::S("selftest")).set("clientDataHash", Json::S(B64u(cdh)));
        Json made = Handle(c, false);
        Json g = Json::O();
        g.set("cmd", Json::S("get")).set("rpId", Json::S("selftest.liquid.invalid")).set("clientDataHash", Json::S(B64u(cdh)));
        Json got = Handle(g, false);
        Json out = Json::O();
        out.set("create", made).set("get", got).set("clientDataHash", Json::S(B64u(cdh)));
        std::string s;
        Dump(out, s);
        printf("%s\n", s.c_str());
        // remove the test passkey again
        Json v = LoadVault();
        for (auto& kv : v.o) if (kv.first == "creds") {
            std::vector<Json> keep;
            for (auto& e : kv.second.a) if (e.str("rp") != "selftest.liquid.invalid") keep.push_back(e); else DeleteKey(UnB64u(e.str("id")));
            kv.second.a = keep;
        }
        SaveVault(v);
        return 0;
    }
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::string msg;
    while (ReadMsg(msg)) {
        Parser p(msg);
        Json req = p.value();
        WriteMsg(p.bad ? Fail("bad request", "SyntaxError") : Handle(req));
    }
    return 0;
}
