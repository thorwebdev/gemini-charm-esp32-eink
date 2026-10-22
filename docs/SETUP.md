# Step-by-step setup and use

## 1. Build and upload

Complete [BUILD.md](BUILD.md). This firmware runs on the ESP32-S3, not as a desktop app.

## 2. Prepare Gemini access

Create your own Gemini API key in [Google AI Studio](https://aistudio.google.com/). Use a 2.4 GHz Wi-Fi network. The firmware uses `gemini-3.5-transcribe-live` with SMART transcription. See Google's [Live transcription guide](https://ai.google.dev/gemini-api/docs/live-api/live-transcribe).

## 3. Configure without saving credentials in source

The included helper asks for credentials interactively with hidden secret input. It sends newline-delimited JSON over USB serial and does not save credentials to a file. Close other serial monitors first.

```sh
python3 -m venv .venv
```

Activate it: `source .venv/bin/activate` on macOS/Linux, or `.venv\Scripts\activate` on Windows. Then:

```sh
python -m pip install -r requirements.txt
python tools/configure.py --port PORT --mode gemini
```

Enter Wi-Fi name, Wi-Fi password and API key when prompted. Expect `CONFIG_SAVED`. The device stores credentials in its local Preferences/NVS. Wait for Wi-Fi connection; press RESET if needed. Never put real credentials in a committed file or shell command.

## 4. Transcribe

- Short BOOT press: start/stop transcription; press BOOT to wake from idle light sleep.
- Speak after the display shows Listening.
- While you talk, the display shows the interim transcript under **LIVE ...**.
- When you pause, server-side voice activity detection finalizes the segment and the display switches to the cleaned-up **SMART TRANSCRIPT** (filler words removed, self-corrections resolved, punctuation and formatting applied).
- Language is detected automatically. Common accented Latin characters are shown; unsupported glyphs appear as `?`.
- Sessions end automatically after five minutes.

The firmware sleeps after about two idle minutes when no session is active. Serial diagnostics include `TRANSCRIPT_INTERIM:` and `TRANSCRIPT_SMART:` lines with the recognized text; keep logs private.

## 5. Optional: save backup Wi-Fi

```sh
python tools/configure.py --port PORT --mode backup-wifi
```

Hold BOOT for about three seconds to switch between primary and backup Wi-Fi. Use a 2.4 GHz compatible hotspot. To return explicitly to primary Wi-Fi, send `{"command":"select_primary_wifi"}` over a 115200-baud serial monitor with newline enabled.

## 6. Troubleshoot

- Upload fails: check the USB data cable, board port and BOOT/RESET sequence; close serial monitors.
- `CONFIG_INVALID`: Wi-Fi name cannot be empty; Gemini key must have at least 20 characters.
- Gemini connection fails: check network, API access/quota and that your key can use `gemini-3.5-transcribe-live`.
- Audio/display errors: confirm original hardware, pins and OPI PSRAM settings.

Microphone recognition needs a physical test on your board; successful compilation does not prove audio behavior.
