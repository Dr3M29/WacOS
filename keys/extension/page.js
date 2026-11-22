// Liquid Keys, in the page itself (MAIN world): stands in for navigator.credentials so a site's
// passkey requests come to Liquid. Liquid answers with its own passkeys after Air Ring sees your
// face. When Liquid has no passkey for the site (or you pick Windows Hello), the request goes
// to the browser's own implementation unchanged, so existing Windows Hello passkeys keep working.
//
// It talks to bridge.js (the extension's isolated content script) with window messages; the
// background script checks the site and builds the signed data, so nothing here is trusted.
(() => {
  if (window.__liquidKeys || !navigator.credentials || !window.PublicKeyCredential) return;
  Object.defineProperty(window, "__liquidKeys", {value: true});

  const creds = navigator.credentials;
  const nativeCreate = creds.create.bind(creds);
  const nativeGet = creds.get.bind(creds);

  const bytes = v => v instanceof ArrayBuffer ? new Uint8Array(v)
    : ArrayBuffer.isView(v) ? new Uint8Array(v.buffer, v.byteOffset, v.byteLength) : new Uint8Array(0);
  const b64u = v => {
    let s = "";
    for (const c of bytes(v)) s += String.fromCharCode(c);
    return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  };
  const unb64u = s => {
    const b = atob((s || "").replace(/-/g, "+").replace(/_/g, "/"));
    const out = new Uint8Array(b.length);
    for (let i = 0; i < b.length; i++) out[i] = b.charCodeAt(i);
    return out.buffer;
  };

  let seq = 0;
  const waiting = new Map();
  window.addEventListener("message", e => {
    const d = e.data;
    if (e.source !== window || !d || d.__liquidKeys !== "res" || !waiting.has(d.id)) return;
    waiting.get(d.id)(d.res);
    waiting.delete(d.id);
  });

  function ask(req, signal) {
    return new Promise((resolve, reject) => {
      if (signal && signal.aborted) return reject(new DOMException("The operation was aborted.", "AbortError"));
      const id = ++seq;
      waiting.set(id, resolve);
      if (signal) signal.addEventListener("abort", () => {
        if (!waiting.delete(id)) return;
        window.postMessage({__liquidKeys: "cancel", id}, location.origin);
        reject(new DOMException("The operation was aborted.", "AbortError"));
      }, {once: true});
      window.postMessage({__liquidKeys: "req", id, req}, location.origin);
    });
  }

  // Objects that look and behave like the browser's own (instanceof works, toJSON works).
  function attestation(r, pk) {
    const clientDataJSON = unb64u(r.clientDataJSON), attestationObject = unb64u(r.attestationObject);
    const authData = unb64u(r.authenticatorData), spki = unb64u(r.publicKey);
    const response = Object.create(AuthenticatorAttestationResponse.prototype, {
      clientDataJSON: {value: clientDataJSON, enumerable: true},
      attestationObject: {value: attestationObject, enumerable: true},
      getTransports: {value: () => ["internal"]},
      getAuthenticatorData: {value: () => authData},
      getPublicKey: {value: () => spki},
      getPublicKeyAlgorithm: {value: () => -7},
    });
    const ext = pk.extensions && pk.extensions.credProps ? {credProps: {rk: true}} : {};
    return credential(r.id, response, ext, () => ({
      clientDataJSON: r.clientDataJSON, attestationObject: r.attestationObject, authenticatorData: r.authenticatorData,
      transports: ["internal"], publicKey: r.publicKey, publicKeyAlgorithm: -7,
    }));
  }

  function assertion(r) {
    const response = Object.create(AuthenticatorAssertionResponse.prototype, {
      clientDataJSON: {value: unb64u(r.clientDataJSON), enumerable: true},
      authenticatorData: {value: unb64u(r.authenticatorData), enumerable: true},
      signature: {value: unb64u(r.signature), enumerable: true},
      userHandle: {value: r.userHandle ? unb64u(r.userHandle) : null, enumerable: true},
    });
    return credential(r.id, response, {}, () => ({
      clientDataJSON: r.clientDataJSON, authenticatorData: r.authenticatorData, signature: r.signature,
      userHandle: r.userHandle || undefined,
    }));
  }

  function credential(id, response, ext, responseJSON) {
    return Object.create(PublicKeyCredential.prototype, {
      id: {value: id, enumerable: true},
      rawId: {value: unb64u(id), enumerable: true},
      type: {value: "public-key", enumerable: true},
      authenticatorAttachment: {value: "platform", enumerable: true},
      response: {value: response, enumerable: true},
      getClientExtensionResults: {value: () => ext},
      toJSON: {value: () => ({id, rawId: id, type: "public-key", authenticatorAttachment: "platform",
                              response: responseJSON(), clientExtensionResults: ext})},
    });
  }

  function fail(r) {
    throw new DOMException(r.error || "Liquid couldn't finish signing in.", r.code || "NotAllowedError");
  }

  creds.create = function (options) {
    const pk = options && options.publicKey;
    // Liquid makes ES256 platform passkeys; anything else (a security key only, other
    // algorithms) goes to the browser.
    const sel = (pk && pk.authenticatorSelection) || {};
    if (!pk || !(pk.pubKeyCredParams || []).some(p => p.alg === -7) || sel.authenticatorAttachment === "cross-platform")
      return nativeCreate(options);
    return ask({
      type: "create",
      rpId: (pk.rp && pk.rp.id) || location.hostname,
      rpName: (pk.rp && pk.rp.name) || "",
      userId: b64u(pk.user && pk.user.id),
      userName: (pk.user && pk.user.name) || "",
      displayName: (pk.user && pk.user.displayName) || "",
      challenge: b64u(pk.challenge),
      exclude: (pk.excludeCredentials || []).map(c => b64u(c.id)),
    }, options.signal).then(r => r.fallback ? nativeCreate(options) : r.ok ? attestation(r, pk) : fail(r));
  };

  creds.get = function (options) {
    const pk = options && options.publicKey;
    if (!pk) return nativeGet(options);
    return ask({
      type: "get",
      rpId: pk.rpId || location.hostname,
      allow: (pk.allowCredentials || []).map(c => b64u(c.id)),
      challenge: b64u(pk.challenge),
      mediation: options.mediation || "",
    }, options.signal).then(r => r.fallback ? nativeGet(options) : r.ok ? assertion(r) : fail(r));
  };

  // Sites ask these before offering passkeys: there is one here now, on this PC.
  Object.defineProperty(PublicKeyCredential, "isUserVerifyingPlatformAuthenticatorAvailable",
                        {value: () => Promise.resolve(true), configurable: true});
  Object.defineProperty(PublicKeyCredential, "isConditionalMediationAvailable",
                        {value: () => Promise.resolve(true), configurable: true});
})();
