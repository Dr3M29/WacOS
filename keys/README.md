# Liquid Keys

Passkeys kept by Liquid on this PC. When a website asks for a passkey, Liquid answers instead of Windows Hello: Air Ring checks your face, the Dynamic Island shows the Face ID square, and you're signed in.

## What it can and can't do

Nothing shows inside the browser. The Dynamic Island at the top of the screen shows the Face ID mark, what's happening ("Save passkey" / "Sign in") and the site, then the check.

- New passkeys: when you make one on a site (its "Create a passkey" button), Liquid saves it once Air Ring sees your face.
- Signing in: if Liquid has a passkey for the site, it signs in after a face match, with no Windows Hello prompt.
- Windows Hello's box never shows (`WINDOWS_HELLO = false` in `extension\bridge.js`). Where Liquid has no passkey, the site is told "no passkey" at once and offers its other ways in (Google: "Try another way"). To use a passkey from your phone through Windows' QR code, turn the extension off for a moment.
- Air Ring or the helper not running, or your face not seen within 15 seconds: the site is told no, and you can try again.
- Existing Windows Hello passkeys can't be moved here. Windows has no way to export them. Add a Liquid passkey on each site once (usually in the site's security settings), then you can remove the old one.
- Sites that only take passwords can't be turned into passkey sites from the browser. Saving passwords and filling them in after a face match is the next phase.

## Pieces

| Part | Where | Job |
|---|---|---|
| Helper | `host\LiquidKeys.exe` | Liquid starts it at sign-in as a small service on `127.0.0.1:47811` (`--serve`) that only answers the Liquid Keys extension (checked by the browser's `Origin` header, plus an `X-Liquid-Keys` header and the host name); `server.log` shows refusals. Opera 140 never found it as a native messaging host, so that route is only the backup. Holds the passkeys and signs with them. Keys stay in Windows' key store for your account (`LiquidKeys-<id>`). The list of passkeys is in `%LOCALAPPDATA%\Liquid\keys\vault.bin`, encrypted for your Windows account. |
| Extension | `extension\` | `page.js` stands in for the page's `navigator.credentials`. `bridge.js` shows the banner. `background.js` checks the site belongs to the page and builds the signed data. |
| Face check | Air Ring `face_unlock.py` | Watches for your face while `Local\LiquidFaceVerifying` is set and answers with `Local\LiquidFaceVerified`. Same match level as the lock screen (0.45, twice in a row). |

## Set up

1. `build.bat` builds the helper.
2. `powershell -ExecutionPolicy Bypass -File install.ps1` registers it for your account (`-Remove` undoes it).
3. In your browser's extensions page (`opera://extensions`, `chrome://extensions`, `edge://extensions` or `brave://extensions`), turn on **Developer mode**, **Load unpacked**, and pick the `keys\extension` folder. The extension's id is `pgicggdhfghafaheccjhkpfkjkaiddoc`, which the helper checks.

## Test

- `node test\host-test.js` talks to the helper like the browser does. `--face` also makes a passkey for `localhost` and signs in with it, which needs your face.
- `host\LiquidKeys.exe --selftest | node test\verify.js` checks the crypto like a website's server would.
- `node test\serve.js`, then open http://localhost:8723 in the browser with the extension loaded.
- `host\LiquidKeys.exe --list` lists saved passkeys.
