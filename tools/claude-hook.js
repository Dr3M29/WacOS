// Claude Code hook -> Liquid's Dynamic Island and Claude Code widget.
// Claude Code runs this on UserPromptSubmit / Stop / Notification / SessionEnd with the event as
// JSON on stdin. It writes one small file per session to %LOCALAPPDATA%\Liquid\claude\:
//   state (working / waiting / idle), the folder, your last prompt and the start of Claude's reply.
// It never blocks Claude: any error is swallowed and it always exits 0.
const fs = require("fs");
const path = require("path");

const clean = (s, n) => String(s || "").replace(/\s+/g, " ").replace(/"/g, "'").trim().slice(0, n);

// the last thing Claude said, from the end of the session transcript
function lastReply(transcript) {
  try {
    const st = fs.statSync(transcript);
    const len = Math.min(st.size, 256 * 1024);
    const fd = fs.openSync(transcript, "r");
    const buf = Buffer.alloc(len);
    fs.readSync(fd, buf, 0, len, st.size - len);
    fs.closeSync(fd);
    const lines = buf.toString("utf8").split("\n").reverse();
    for (const line of lines) {
      if (!line.includes('"assistant"')) continue;
      try {
        const j = JSON.parse(line);
        const content = j.message && j.message.content;
        if (!Array.isArray(content)) continue;
        const text = content.filter(c => c.type === "text").map(c => c.text).join(" ");
        if (text.trim()) return text;
      } catch {}
    }
  } catch {}
  return "";
}

let input = "";
process.stdin.on("data", d => (input += d));
process.stdin.on("end", () => {
  try {
    const ev = JSON.parse(input.replace(/^﻿/, "") || "{}");
    const id = String(ev.session_id || "unknown").replace(/[^a-zA-Z0-9_-]/g, "");
    const dir = path.join(process.env.LOCALAPPDATA || ".", "Liquid", "claude");
    fs.mkdirSync(dir, {recursive: true});
    const file = path.join(dir, id + ".json");
    const name = ev.hook_event_name || "";
    if (name === "SessionEnd") { try { fs.unlinkSync(file); } catch {} return; }
    let prev = {};
    try { prev = JSON.parse(fs.readFileSync(file, "utf8")); } catch {}
    const state = name === "UserPromptSubmit" ? "working" : name === "Notification" ? "waiting" : "idle";
    const out = {state, cwd: ev.cwd || prev.cwd || "", event: name, time: Date.now(),
                 prompt: prev.prompt || "", reply: prev.reply || ""};
    if (name === "UserPromptSubmit") { out.prompt = clean(ev.prompt, 160); out.reply = ""; }
    if (name === "Stop" && ev.transcript_path) out.reply = clean(lastReply(ev.transcript_path), 200);
    fs.writeFileSync(file, JSON.stringify(out));
  } catch {}
});
