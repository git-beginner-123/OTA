#pragma once
#include <stdint.h>
#include <stdbool.h>

void St7789_Init(void);
void St7789_Fill(uint16_t color565);
void St7789_DrawPixel(int x, int y, uint16_t color565);

void St7789_BlitRect(int x, int y, int w, int h, const uint16_t* pixels565);
void St7789_FillRect(int x, int y, int w, int h, uint16_t color565);

int St7789_Width(void);
int St7789_Height(void);

void St7789_Flush(void);

// Panel control helpers
void St7789_SetInversion(bool on);
bool St7789_GetInversion(void);

// Software color correction (applied to all pixels before sending)
void St7789_SetSoftwareInvert(bool on);
void St7789_SetSoftwareRBSwap(bool on);
bool St7789_GetSoftwareInvert(void);
bool St7789_GetSoftwareRBSwap(void);

// Apply panel-level default color profile (for ILI9341 in this project)
void St7789_ApplyPanelDefaultProfile(void);
