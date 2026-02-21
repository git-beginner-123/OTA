#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns pointer to 16 bytes bitmap for ASCII char.
// Each byte is one row, MSB is leftmost pixel.
// Unsupported chars return '?'.
const uint8_t* Font8x16_Get(char c);

// Returns 8x16 glyph for numbered notation note.
// degree: 0..7 (0 is rest), octave: -1 low, 0 middle, +1 high.
const uint8_t* Font8x16_GetNumberedNoteGlyph(uint8_t degree, int8_t octave);

#ifdef __cplusplus
}
#endif
