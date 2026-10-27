#pragma once
#include <stdint.h>
#include <stddef.h>
// Kept in a header so Arduino's auto-generated prototypes can reference these types.
constexpr size_t TRANSCRIPT_CAP=640;
enum TranscriptKind:uint8_t{TRANSCRIPT_NONE=0,TRANSCRIPT_INTERIM=1,TRANSCRIPT_SMART=2};
// Screen layouts. HOME renders the status text (ready, connecting, errors, sleep).
enum ScreenMode:uint8_t{
  MODE_HOME=0,   // icon + status text
  MODE_LISTEN,   // listening face, or caption label + live/smart transcript
  MODE_PROMPT,   // collected prompt waiting for the side button
  MODE_BUSY,     // generation in progress: caption + small body line
  MODE_IMAGE,    // full-screen 1-bit image from imageBitmap
  MODE_PANEL,    // caption label + wrapped body (speech reply, lyrics, voice)
  MODE_QR,       // setup: caption title + QR code of `text` + `status` hint line
};
struct ScreenMsg {
  char status[96];
  uint8_t mode;
  bool filledLabel;            // inverted caption pill (final results)
  char caption[48];
  char footer[40];
  char text[TRANSCRIPT_CAP];
};
// What the buttons do depends on this (see loop()).
enum AppState:uint8_t{APP_READY=0,APP_TRANSCRIBING,APP_PROMPT,APP_GENERATING,APP_PLAYING,APP_RESULT};
constexpr int IMAGE_SIZE=200;
constexpr size_t IMAGE_BYTES=IMAGE_SIZE*IMAGE_SIZE/8; // 1 = black pixel, row-major, MSB first
