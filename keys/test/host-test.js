// Talks to LiquidKeys.exe the way the browser does (native messaging: 4-byte length + JSON).
//   node test\host-test.js            ping + list (no face needed)
//   node test\host-test.js --face     also makes a passkey for localhost and signs in with it,
//                                     which needs Air Ring to see your face (twice), then deletes
//                                     nothing: the test passkey shows up as "liquid-keys test" for localhost.
const {spawn} = require("child_process");
const crypto = require("crypto");
const path = require("path");

const exe = path.join(__dirname, "..", "host", "LiquidKeys.exe");
const b64u = b => Buffer.from(b).toString("base64").replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
const unb64u = s => Buffer.from(s.replace(/-/g, "+").replace(/_/g, "/"), "base64");

function call(msg) {
  return new Promise((resolve, reject) => {
    const p = spawn(exe, ["chrome-extension://pgicggdhfghafaheccjhkpfkjkaiddoc/"]);
    let out = Buffer.alloc(0);
    p.stdout.on("data", d => {
      out = Buffer.concat([out, d]);
      if (out.length >= 4 && out.length >= 4 + out.readUInt32LE(0)) {
        resolve(JSON.parse(out.subarray(4, 4 + out.readUInt32LE(0)).toString("utf8")));
        p.stdin.end();
      }
    });
    p.on("error", reject);
    const body = Buffer.from(JSON.stringify(msg), "utf8");
    const head = Buffer.alloc(4); head.writeUInt32LE(body.length);
    p.stdin.write(Buffer.concat([head, body]));
  });
}

(async () => {
  console.log("ping ->", JSON.stringify(await call({cmd: "ping"})));
  console.log("list localhost ->", JSON.stringify(await call({cmd: "list", rpId: "localhost"})));
  console.log("bad request ->", JSON.stringify(await call({cmd: "get", rpId: ""})));
  if (!process.argv.includes("--face")) return;

  const cdj = JSON.stringify({type: "webauthn.create", challenge: b64u(crypto.randomBytes(16)), origin: "http://localhost:8723", crossOrigin: false});
  console.log("create (look at the camera)...");
  let t = Date.now();
  const made = await call({cmd: "create", rpId: "localhost", rpName: "Liquid Keys test", userId: b64u(Buffer.from("tester")),
                           userName: "liquid-keys test", displayName: "Liquid Keys test",
                           clientDataHash: b64u(crypto.createHash("sha256").update(cdj).digest())});
  console.log(`  -> ok ${made.ok} in ${((Date.now() - t) / 1000).toFixed(1)} s`, made.ok ? "" : made.error);
  if (!made.ok) return;
  const gcd = JSON.stringify({type: "webauthn.get", challenge: b64u(crypto.randomBytes(16)), origin: "http://localhost:8723", crossOrigin: false});
  const hash = crypto.createHash("sha256").update(gcd).digest();
  console.log("get (look at the camera again)...");
  t = Date.now();
  const got = await call({cmd: "get", rpId: "localhost", allow: [made.id], clientDataHash: b64u(hash)});
  console.log(`  -> ok ${got.ok} in ${((Date.now() - t) / 1000).toFixed(1)} s`, got.ok ? "" : got.error);
  if (!got.ok) return;
  const pub = crypto.createPublicKey({key: unb64u(made.publicKey), format: "der", type: "spki"});
  const good = crypto.verify("sha256", Buffer.concat([unb64u(got.authenticatorData), hash]), pub, unb64u(got.signature));
  console.log("  signature verifies:", good);
})();
