// Checks LiquidKeys.exe --selftest output the way a website's server would:
//   node test\verify.js < selftest.json
// Decodes the attestation object (CBOR), checks the site hash, flags and credential id, pulls the
// COSE public key, checks it matches the SPKI key, and verifies the sign-in signature with Node's
// own crypto. Exits 1 on any mismatch.
const crypto = require("crypto");

const b64u = s => Buffer.from(s.replace(/-/g, "+").replace(/_/g, "/"), "base64");

function cbor(buf) {          // minimal CBOR decoder: ints, byte/text strings, arrays, maps
  let i = 0;
  function len(ai) {
    if (ai < 24) return ai;
    if (ai === 24) return buf[i++];
    if (ai === 25) { const v = buf.readUInt16BE(i); i += 2; return v; }
    if (ai === 26) { const v = buf.readUInt32BE(i); i += 4; return v; }
    throw new Error("cbor: length " + ai);
  }
  function item() {
    const b = buf[i++], major = b >> 5, n = len(b & 31);
    switch (major) {
      case 0: return n;
      case 1: return -1 - n;
      case 2: { const v = buf.subarray(i, i + n); i += n; return v; }
      case 3: { const v = buf.toString("utf8", i, i + n); i += n; return v; }
      case 4: return Array.from({length: n}, item);
      case 5: { const m = new Map(); for (let k = 0; k < n; k++) m.set(item(), item()); return m; }
      default: throw new Error("cbor: major " + major);
    }
  }
  const v = item();
  return {value: v, used: i};
}

let input = "";
process.stdin.on("data", d => (input += d));
process.stdin.on("end", () => {
  const t = JSON.parse(input.replace(/^﻿/, ""));   // PowerShell pipes add a byte-order mark
  const fail = m => { console.log("FAIL:", m); process.exit(1); };
  const ok = m => console.log("ok  ", m);
  if (!t.create.ok || !t.get.ok) fail("host returned an error: " + JSON.stringify(t));
  const rpHash = crypto.createHash("sha256").update("selftest.liquid.invalid").digest();

  const att = cbor(b64u(t.create.attestationObject)).value;
  if (att.get("fmt") !== "none") fail("fmt " + att.get("fmt"));
  ok("attestation object decodes, fmt none");
  const ad = att.get("authData");
  if (!ad.subarray(0, 32).equals(rpHash)) fail("rpIdHash");
  ok("rpIdHash = sha256(site)");
  const flags = ad[32];
  if ((flags & 0x45) !== 0x45) fail("flags " + flags.toString(16));
  ok("flags: user present, user verified, credential attached (0x" + flags.toString(16) + ")");
  const credLen = ad.readUInt16BE(53);
  const credId = ad.subarray(55, 55 + credLen);
  if (!credId.equals(b64u(t.create.id))) fail("credential id in authData differs from id");
  ok("credential id in authData matches (" + credLen + " bytes)");
  const cose = cbor(ad.subarray(55 + credLen)).value;
  if (cose.get(1) !== 2 || cose.get(3) !== -7 || cose.get(-1) !== 1) fail("COSE key header");
  const x = cose.get(-2), y = cose.get(-3);
  const spki = b64u(t.create.publicKey);
  if (!spki.subarray(27, 59).equals(x) || !spki.subarray(59, 91).equals(y)) fail("SPKI and COSE keys differ");
  ok("COSE key (EC2, ES256, P-256) matches getPublicKey() SPKI");

  const pub = crypto.createPublicKey({key: spki, format: "der", type: "spki"});
  const gad = b64u(t.get.authenticatorData);
  if (!gad.subarray(0, 32).equals(rpHash) || (gad[32] & 0x05) !== 0x05) fail("assertion authData");
  ok("assertion authData: site hash and flags");
  if (t.get.id !== t.create.id) fail("get used a different credential");
  const signed = Buffer.concat([gad, b64u(t.clientDataHash)]);
  if (!crypto.verify("sha256", signed, pub, b64u(t.get.signature))) fail("signature does not verify");
  ok("ES256 signature over authData || clientDataHash verifies");
  const tampered = Buffer.from(signed); tampered[40] ^= 1;
  if (crypto.verify("sha256", tampered, pub, b64u(t.get.signature))) fail("signature verified tampered data");
  ok("tampered data is rejected");
  console.log("ALL PASS");
});
