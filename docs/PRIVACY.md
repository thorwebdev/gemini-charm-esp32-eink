# Export and privacy

Included: application source, required drivers/vendor source and licenses, public CA certificates, build settings, setup helper and documentation.

Excluded: compiled firmware and full-flash images, NVS/device snapshots, logs and transcripts, personal paths/board identity, real credentials and API keys.

Credentials are provided at runtime over USB serial; they are stored on the board, not in this repository. Public CA certificates are trust anchors, not private keys. Upstream copyright attribution is retained.

Microphone audio is streamed to the Gemini API only while a transcription session is active (after pressing BOOT, until stopped or the session ends).

Pressing the side button sends the session's transcript text to the Gemini API to generate an image, speech, a song or a voice. Designed voices are stored in your Gemini API project (the previous one is deleted when a new one is created); the voice ID is kept in NVS.

Keep build output, serial logs and device backups private and outside version control. NVS is not encrypted by this application. Anyone with physical access to a board/flash backup may be able to recover credentials. Serial output can contain Wi-Fi names and transcript text.
