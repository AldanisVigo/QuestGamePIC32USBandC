#ifndef ST7735_H
#define ST7735_H

#include <stdint.h>

#define ST7735_WIDTH   128
#define ST7735_HEIGHT  160

#define ST7735_BLACK   0x0000
#define ST7735_BLUE    0x001F
#define ST7735_RED     0xF800
#define ST7735_GREEN   0x07E0
#define ST7735_CYAN    0x07FF
#define ST7735_MAGENTA 0xF81F
#define ST7735_YELLOW  0xFFE0
#define ST7735_WHITE   0xFFFF

typedef enum
{
    ST7735_PIN_RB2,
    ST7735_PIN_RB3,
    ST7735_PIN_RB4,
    ST7735_PIN_RB7,
    ST7735_PIN_RB10,
    ST7735_PIN_RB11
} ST7735_Pin;

typedef struct
{
    ST7735_Pin cs;
    ST7735_Pin dc;
    ST7735_Pin rst;
} ST7735_t;

void ST7735_Config(ST7735_t *tft, ST7735_Pin cs, ST7735_Pin dc, ST7735_Pin rst);
void ST7735_Init(ST7735_t *tft);

void ST7735_FillScreen(ST7735_t *tft, uint16_t color);
void ST7735_DrawPixel(ST7735_t *tft, uint8_t x, uint8_t y, uint16_t color);
void ST7735_FillRect(ST7735_t *tft, uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t color);
void ST7735_DrawFastHLine(ST7735_t *tft, uint8_t x, uint8_t y, uint8_t w, uint16_t color);
void ST7735_DrawFastVLine(ST7735_t *tft, uint8_t x, uint8_t y, uint8_t h, uint16_t color);

void ST7735_DrawChar(ST7735_t *tft, uint8_t x, uint8_t y, char c, uint16_t color, uint16_t bg, uint8_t size);
void ST7735_Print(ST7735_t *tft, uint8_t x, uint8_t y, const char *str, uint16_t color, uint16_t bg, uint8_t size);

void ST7735_DrawLine(ST7735_t *tft, int x0, int y0, int x1, int y1, uint16_t color);
void ST7735_DrawCircle(ST7735_t *tft, int xc, int yc, int r, uint16_t color);
void ST7735_DrawTriangle(ST7735_t *tft, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);
void ST7735_FillTriangle(ST7735_t *tft, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);

void ST7735_DrawRGBBitmap(ST7735_t *tft, uint8_t x, uint8_t y, const uint16_t *bitmap, uint8_t w, uint8_t h);

#endif