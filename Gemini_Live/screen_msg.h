#pragma once
#include <stdint.h>
#include <stddef.h>
// Kept in a header so Arduino's auto-generated prototypes can reference these types.
constexpr size_t TRANSCRIPT_CAP=640;
enum TranscriptKind:uint8_t{TRANSCRIPT_NONE=0,TRANSCRIPT_INTERIM=1,TRANSCRIPT_SMART=2};
struct ScreenMsg { char status[96]; uint8_t transcriptKind; char transcript[TRANSCRIPT_CAP]; };
