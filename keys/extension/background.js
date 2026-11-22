// Liquid Keys background: the trusted side. For each passkey request from a page it
//   1. takes the page's origin from the browser (sender), never from the page,
//   2. checks the site (rpId) belongs to that origin, so evil.example can't ask for bank.example's passkey,
//   3. builds clientDataJSON itself (type, challenge, origin) and hashes it,
//   4. asks LiquidKeys.exe to check your face and sign: through its local service on
//      127.0.0.1:47811 (started by Liquid), or, if that isn't running, as the native messaging
//      host com.liquid.keys. Opera 140 didn't find the native host, so the service comes first.
const HOST = "com.liquid.keys";
const SERVICE = "http://127.0.0.1:47811/v1";

const enc = new TextEncoder();
const b64u = buf => {
  let s = "";
  for (const c of new Uint8Array(buf)) s += String.fromCharCode(c);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
};

function siteOf(sender) {
  const origin = sender.origin || (sender.url && new URL(sender.url).origin);
  if (!origin) return null;
  const u = new URL(origin);
  const secure = u.protocol === "https:" || (u.protocol === "http:" && u.hostname === "localhost");
  return secure ? {origin, host: u.hostname} : null;
}

// rpId must be the page's host or a parent domain of it, and look like a real domain
// (a dot in it, not ending in one), or be localhost.
function rpIdOk(rpId, host) {
  rpId = String(rpId || "").toLowerCase();
  if (!rpId) return false;
  if (rpId === "localhost") return host === "localhost";
  if (!rpId.includes(".") || rpId.startsWith(".") || rpId.endsWith(".")) return false;
  return host === rpId || host.endsWith("." + rpId);
}

async function native(msg) {
  try {
    const r = await fetch(SERVICE, {method: "POST", headers: {"content-type": "application/json", "x-liquid-keys": "1"},
                                    body: JSON.stringify(msg)});
    if (r.status === 200) return await r.json();
  } catch (e) { /* service not running: try native messaging */ }
  return new Promise(resolve => {
    chrome.runtime.sendNativeMessage(HOST, msg, res => {
      if (chrome.runtime.lastError || !res)
        resolve({ok: false, code: "NotAllowedError", noHost: true,
                 error: "Liquid's key helper isn't running. Start Liquid (it starts the helper), or run keys\\host\\LiquidKeys.exe --serve."});
      else resolve(res);
    });
  });
}

async function clientData(type, challenge, origin, crossOrigin) {
  // property order as browsers write it: type, challenge, origin, crossOrigin
  const json = JSON.stringify({type, challenge, origin, crossOrigin});
  const hash = await crypto.subtle.digest("SHA-256", enc.encode(json));
  return {clientDataJSON: b64u(enc.encode(json)), clientDataHash: b64u(hash)};
}

async function handle(req, sender) {
  const site = siteOf(sender);
  if (!site) return {ok: false, code: "SecurityError", error: "Passkeys need a secure (https) page."};
  if (!rpIdOk(req.rpId, site.host)) return {ok: false, code: "SecurityError", error: `${req.rpId} isn't part of ${site.host}.`};
  const crossOrigin = sender.frameId !== 0;

  if (req.type === "has") {
    const r = await native({cmd: "list", rpId: req.rpId});
    if (!r.ok) return {has: false};
    const allow = req.allow || [];
    return {has: r.creds.some(c => !allow.length || allow.includes(c.id))};
  }
  if (req.type === "create") {
    const cd = await clientData("webauthn.create", req.challenge, site.origin, crossOrigin);
    const r = await native({cmd: "create", rpId: req.rpId, rpName: req.rpName, userId: req.userId, userName: req.userName,
                            displayName: req.displayName, exclude: req.exclude || [], clientDataHash: cd.clientDataHash});
    return r.ok ? {...r, clientDataJSON: cd.clientDataJSON} : r;
  }
  if (req.type === "get") {
    const cd = await clientData("webauthn.get", req.challenge, site.origin, crossOrigin);
    const r = await native({cmd: "get", rpId: req.rpId, allow: req.allow || [], clientDataHash: cd.clientDataHash});
    return r.ok ? {...r, clientDataJSON: cd.clientDataJSON} : r;
  }
  return {ok: false, code: "NotSupportedError", error: "unknown request"};
}

chrome.runtime.onMessage.addListener((req, sender, reply) => {
  handle(req, sender).then(reply, err => reply({ok: false, code: "NotAllowedError", error: String(err)}));
  return true;   // reply comes later
});
