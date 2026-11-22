// Checks LiquidKeys --serve's door rules (node test\service-test.js):
// only the Liquid Keys extension's origin, with the X-Liquid-Keys header, on 127.0.0.1, gets answers.
const http = require("http");
const EXT = "chrome-extension://pgicggdhfghafaheccjhkpfkjkaiddoc";

function post(headers, body, method = "POST") {
  if (body) headers = {...headers, "content-length": Buffer.byteLength(body)};   // as browsers send it
  return new Promise(resolve => {
    const req = http.request({host: "127.0.0.1", port: 47811, path: "/v1", method, headers}, res => {
      let d = "";
      res.on("data", c => (d += c));
      res.on("end", () => resolve({status: res.statusCode, cors: res.headers["access-control-allow-origin"] || "", body: d}));
    });
    req.on("error", e => resolve({status: 0, body: String(e)}));
    if (body) req.write(body);
    req.end();
  });
}

(async () => {
  const ping = JSON.stringify({cmd: "ping"});
  const json = {"content-type": "application/json"};
  const cases = [
    ["a website", {...json, origin: "https://evil.example", "x-liquid-keys": "1"}, 403],
    ["no origin (another program)", {...json, "x-liquid-keys": "1"}, 403],
    ["a different extension", {...json, origin: "chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "x-liquid-keys": "1"}, 403],
    ["Liquid Keys without the header", {...json, origin: EXT}, 400],
    ["DNS-rebinding host name", {...json, origin: EXT, "x-liquid-keys": "1", host: "evil.example:47811"}, 403],
    ["Liquid Keys extension", {...json, origin: EXT, "x-liquid-keys": "1"}, 200],
  ];
  let bad = 0;
  for (const [name, h, want] of cases) {
    const r = await post(h, ping);
    const ok = r.status === want;
    if (!ok) bad++;
    console.log(`${ok ? "ok  " : "FAIL"} ${name}: ${r.status}${r.cors ? " (CORS " + r.cors + ")" : ""} ${r.status === 200 ? r.body : ""}`);
  }
  const pre = await post({origin: "https://evil.example", "access-control-request-method": "POST"}, null, "OPTIONS");
  console.log(`${pre.status === 403 ? "ok  " : "FAIL"} preflight from a website: ${pre.status}`);
  if (pre.status !== 403) bad++;
  const list = await post({...json, origin: EXT, "x-liquid-keys": "1"}, JSON.stringify({cmd: "list", rpId: "localhost"}));
  console.log(`list localhost via the service: ${list.body}`);
  console.log(bad ? `${bad} FAILED` : "ALL PASS");
})();
