// Liquid Keys, the extension's side of the page (isolated world): carries page.js's passkey
// requests to the background script and the answers back. Nothing shows in the page: Liquid's
// Dynamic Island, at the top of the screen, shows the Face ID check with the site's name.
//   making a passkey   saved in Liquid once Air Ring sees your face
//   signing in         Liquid answers if it has a passkey for the site
// Windows Hello's own box never shows (WINDOWS_HELLO = false, as asked on 2026-11-21: this PC
// holds no Windows Hello passkeys; the old ones live on a phone). Where Liquid has no passkey,
// the site hears "no passkey" at once, as if its box had been cancelled, and offers its other
// ways in (Google: "Try another way"). Autofill-style requests (mediation "conditional") just
// stay quiet. To use a phone's passkey through Windows' QR code, turn the extension off for a moment.
(() => {
  const WINDOWS_HELLO = false;
  const NO_PASSKEY = {ok: false, code: "NotAllowedError", error: "Liquid has no passkey for this site."};
  const pending = new Map();   // page request id -> {cancelled}

  window.addEventListener("message", e => {
    const d = e.data;
    if (e.source !== window || !d) return;
    if (d.__liquidKeys === "cancel" && pending.has(d.id)) { pending.get(d.id).cancelled = true; return; }
    if (d.__liquidKeys !== "req" || !d.req) return;
    pending.set(d.id, {cancelled: false});
    run(d.req).catch(err => ({ok: false, code: "NotAllowedError", error: String(err && err.message || err)}))
      .then(res => {
        const p = pending.get(d.id);
        pending.delete(d.id);
        if (p && !p.cancelled) window.postMessage({__liquidKeys: "res", id: d.id, res}, location.origin);
      });
  });

  const send = msg => chrome.runtime.sendMessage(msg);
  // Liquid can't do the check at all: let Windows Hello take the request.
  const unavailable = res => !res || res.noHost || res.code === "AirRingOff";

  async function run(req) {
    if (req.type === "get") {
      const has = await send({type: "has", rpId: req.rpId, allow: req.allow});
      if (!has || !has.has) {
        if (WINDOWS_HELLO) return {fallback: true};
        if (req.mediation === "conditional") return new Promise(() => {});   // autofill: nothing to offer, stay quiet
        return NO_PASSKEY;
      }
    }
    const res = await send(req);
    if (res && res.ok) return res;
    if (WINDOWS_HELLO && unavailable(res)) return {fallback: true};
    return {...(res || NO_PASSKEY), code: "NotAllowedError"};   // sites expect the standard error name
  }
})();
