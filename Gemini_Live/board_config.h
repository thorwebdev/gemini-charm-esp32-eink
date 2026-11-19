#pragma once
// Target board selection.
// Define BOARD_M5STICKS3 for the M5Stack StickS3 (ESP32-S3, 1.14" 135x240 ST7789 color LCD,
// ES8311 half-duplex mic/speaker, M5PM1 PMIC, BtnA=GPIO11, BtnB=GPIO12).
// Comment out BOARD_M5STICKS3 to build for the original Waveshare ESP32-S3-ePaper-1.54 V2.
#define BOARD_M5STICKS3

#if defined(BOARD_M5STICKS3)
  constexpr int MAIN_BUTTON_PIN = 11; // Front M5 button (BtnA)
  constexpr int SIDE_BUTTON_PIN = 12; // Side button (BtnB)
  constexpr int SCREEN_W = 135;
  constexpr int SCREEN_H = 240;
  #define MAIN_BTN_NAME "A"
  #define SIDE_BTN_NAME "B"
#else
  constexpr int MAIN_BUTTON_PIN = 0;  // BOOT button
  constexpr int SIDE_BUTTON_PIN = 18; // Side / PWR button
  constexpr int SCREEN_W = 200;
  constexpr int SCREEN_H = 200;
  #define MAIN_BTN_NAME "BOOT"
  #define SIDE_BTN_NAME "Side"
#endif
