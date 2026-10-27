# Step-by-step setup and use

## 1. Build and upload

Complete [BUILD.md](BUILD.md). This firmware runs on the ESP32-S3, not as a desktop app.

## 2. Prepare Gemini access

Create your own Gemini API key in [Google AI Studio](https://aistudio.google.com/). Use a 2.4 GHz Wi-Fi network. The firmware uses `gemini-3.5-transcribe-live` with SMART transcription. See Google's [Live transcription guide](https://ai.google.dev/gemini-api/docs/live-api/live-transcribe).

## 3. Set up from your phone

1. Power on the device. When it has no Wi-Fi or API key saved, it starts setup on its own: the screen shows **Setup – Step 1/2: Scan to connect** and a QR code.
2. Scan the QR code with your phone camera and join the `Gemini-XXXX` network. The hotspot password is printed under the QR code if you need to type it.
3. Once your phone has joined, the screen switches to **Step 2/2**. Most phones open the setup page on their own ("Sign in to network"). If yours doesn't, scan the second QR code or open `http://192.168.4.1`.
4. Pick your Wi-Fi from the list (or **Other network…**), enter its password and paste your Gemini API key. Tap **Save & connect**.
5. The device restarts and joins your Wi-Fi. Switch your phone back to its usual network.

Tip: copy your API key from AI Studio *before* joining the device hotspot, because the hotspot has no internet.

**Run setup again** (new Wi-Fi or key): hold BOOT for about 10 seconds. At 3 seconds the screen shows a hint; keep holding. A short BOOT press leaves setup again and keeps the saved settings. Unused setup returns to normal mode after 10 minutes. The API key field can stay empty to keep the current key.

If Wi-Fi fails after setup, the screen shows *Wi-Fi failed. Hold BOOT 10s for setup*. A rejected key shows *API error. Check key: hold BOOT 10s*.

### Advanced: USB setup

The serial helper still works, including while the setup QR code is shown. It asks for credentials interactively with hidden secret input. It sends newline-delimited JSON over USB serial and does not save credentials to a file. Close other serial monitors first.

```sh
python3 -m venv .venv
```

Activate it: `source .venv/bin/activate` on macOS/Linux, or `.venv\Scripts\activate` on Windows. Then:

```sh
python -m pip install -r requirements.txt
python tools/configure.py --port PORT --mode gemini
```

Enter Wi-Fi name, Wi-Fi password and API key when prompted. Expect `CONFIG_SAVED`. The device stores credentials in its local Preferences/NVS. Wait for Wi-Fi connection; press RESET if needed. Never put real credentials in a committed file or shell command.

To erase the saved Wi-Fi and key so the next boot starts phone setup, send `{"command":"forget_setup"}` over a 115200-baud serial monitor with newline enabled.

## 4. Transcribe

- Short BOOT press: start/stop transcription; press BOOT to wake from idle light sleep.
- Speak after the display shows Listening.
- While you talk, the display shows the interim transcript under **LIVE ...**.
- When you pause, server-side voice activity detection finalizes the segment and the display switches to the cleaned-up **SMART TRANSCRIPT** (filler words removed, self-corrections resolved, punctuation and formatting applied).
- Language is detected automatically. Common accented Latin characters are shown; unsupported glyphs appear as `?`.
- Sessions end automatically after five minutes.
- Stopping with BOOT shows the collected **PROMPT** (all smart segments of the session).

## 5. Create from the transcript

Press the side button (GPIO 18) while transcribing, or on the PROMPT/result screen. `gemini-3.5-flash-lite` reads the prompt and picks one action:

| Say something like | Result |
|---|---|
| "Draw a lighthouse in a storm" | `gemini-3.1-flash-lite-image` image, dithered to 200×200 black-and-white; stays on screen during sleep |
| "Tell me a fun fact about octopuses" | Short reply spoken by `gemini-3.8-flash-lite-tts` |
| "Make a ukulele song about coffee" | 30 s `lyria-3-clip-preview` song with lyrics on screen |
| "Create a gravelly old pirate voice" | Designs a voice (`/v1beta/voices`), plays a sample and uses it for later speech |
| "Go back to the normal voice" | Returns to the default `Kore` voice |

- BOOT during generation or playback cancels. Network requests already in flight finish first.
- Side button on a result screen runs the same prompt again; BOOT starts a new transcription.
- All model calls stream (SSE): speech starts playing while it is still being generated. Expect roughly 5–30 s for images and songs.
- Your API key needs access to all models above. A newly designed voice replaces (and deletes) the previous one.
- To test without speaking, send `{"command":"generate","text":"Draw a cat"}` over serial.

The firmware sleeps after about two idle minutes when no session or generation is active. Serial diagnostics include `TRANSCRIPT_INTERIM:`, `TRANSCRIPT_SMART:` and `ROUTER:` lines with the recognized text; keep logs private.

## 6. Optional: save backup Wi-Fi

```sh
python tools/configure.py --port PORT --mode backup-wifi
```

Hold BOOT for about three seconds, then release (before 10 seconds), to switch between primary and backup Wi-Fi. Use a 2.4 GHz compatible hotspot. To return explicitly to primary Wi-Fi, send `{"command":"select_primary_wifi"}` over a 115200-baud serial monitor with newline enabled.

## 7. Troubleshoot

- Upload fails: check the USB data cable, board port and BOOT/RESET sequence; close serial monitors.
- Setup page doesn't open: stay on the `Gemini-XXXX` network (some phones offer to leave a network without internet), turn off VPN / private DNS, then open `http://192.168.4.1`.
- Your network isn't listed: tap **Rescan**, or use **Other network…** for hidden networks. 5 GHz-only networks are not supported.
- `CONFIG_INVALID`: Wi-Fi name cannot be empty; Gemini key must have at least 20 characters.
- Gemini connection fails: check network, API access/quota and that your key can use `gemini-3.5-transcribe-live`. For create errors, look for `GEMINI_API_ERROR: code=...` in the serial log.
- Audio/display errors: confirm original hardware, pins and OPI PSRAM settings.

Microphone recognition needs a physical test on your board; successful compilation does not prove audio behavior.
