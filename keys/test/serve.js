// Serves passkey-test.html at http://localhost:8723 (passkeys need https or localhost).
//   node keys\test\serve.js
const http = require("http");
const fs = require("fs");
const path = require("path");
const page = path.join(__dirname, "passkey-test.html");
http.createServer((req, res) => {
  if (req.url !== "/" && req.url !== "/index.html") { res.writeHead(404); return res.end("not found"); }
  res.writeHead(200, {"content-type": "text/html; charset=utf-8", "cache-control": "no-store"});
  fs.createReadStream(page).pipe(res);
}).listen(8723, "127.0.0.1", () => console.log("Liquid Keys test page: http://localhost:8723"));
