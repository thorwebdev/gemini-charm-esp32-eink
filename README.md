![Gemini Live Transcribe — tiny wireless transcription device](assets/gemini-live-agent-banner-v4.png)

# Gemini Live Transcribe — ESP32-S3

Real-time speech-to-text on a pocket-sized e-paper device using the Gemini Live API with `gemini-3.5-transcribe-live` and SMART transcription.

## How it works

Press BOOT and start talking. While you speak, the display shows the interim transcript as it is recognized. When you pause, server-side voice activity detection finalizes the segment and the display switches to the cleaned-up smart transcript: filler words and false starts removed, self-corrections resolved, punctuation, casing and formatting applied. Press BOOT again to stop.

Then press the side button to turn what you said into something: Gemini Flash-Lite routes the prompt to an image (Nano Banana Lite, dithered for e-paper), spoken audio (Gemini 3.8 Flash-Lite TTS), a 30-second song with lyrics (Lyria 3 Clip), or a newly designed voice that is used for later speech. See [SETUP](docs/SETUP.md#5-create-from-the-transcript).

Language is detected automatically. Includes e-paper display, battery indicator, light sleep and backup Wi-Fi.

During idle light sleep, Wi-Fi and audio turn off; BOOT wakes the device. Battery life has not yet been measured.

On battery, hold PWR (the side button) for about 2 seconds to turn on; hold it for 3 seconds to power off. A short press of PWR is "create" and fires when released. On USB, power off falls back to deep sleep and PWR wakes it.

## Build and setup

- [Build guide](docs/BUILD.md)
- [Step-by-step setup and use](docs/SETUP.md)
- [Privacy and export details](docs/PRIVACY.md)

This is device firmware; it runs on the matching ESP32-S3 hardware. Included vendor sources retain their original licenses. No new license is granted for the application code.

Reference: [Live transcription with Gemini Live API](https://ai.google.dev/gemini-api/docs/live-api/live-transcribe).

## Disclaimer

This repository and its contents are provided for illustration and educational purposes only as example code. This is not an official Google product or officially supported Google Cloud project. This code is provided as-is for demonstration purposes and is NOT intended or supported for production workloads. The views, code, and opinions expressed in this repository are those of the author(s) and do not necessarily reflect the position, opinions, or official policy of Google LLC or Google Cloud Platform.
