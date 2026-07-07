#include <xc.h>
#include <stdint.h>
#include "spi.h"
#include "ST7735.h"
#include "Font5x7.h"

#define ST7735_SWRESET 0x01
#define ST7735_SLPOUT  0x11
#define ST7735_COLMOD  0x3A
#define ST7735_MADCTL  0x36
#define ST7735_CASET   0x2A
#define ST7735_RASET   0x2B
#define ST7735_RAMWR   0x2C
#define ST7735_DISPON  0x29

#define SYS_FREQ 40000000UL

/*
 * original MADCTL was 0xC8.
 * 180 degrees from that is 0x08.
 *
 * If colors look wrong, use 0x00.
*/
#define ST7735_MADCTL_180 0x08

/*
 * Offset fix for the 2 jumbled columns.
 * Start with XSTART = 2.
 * If the garbage gets worse, try XSTART = 0 or 1.
*/
#define ST7735_XSTART 2
#define ST7735_YSTART 0

static void pin_tris_output(ST7735_Pin pin)
{
    switch(pin)
    {
        case ST7735_PIN_RB2:  TRISBbits.TRISB2 = 0; break;
        case ST7735_PIN_RB3:  TRISBbits.TRISB3 = 0; break;
        case ST7735_PIN_RB4:  TRISBbits.TRISB4 = 0; break;
        case ST7735_PIN_RB7:  TRISBbits.TRISB7 = 0; break;
        case ST7735_PIN_RB10: TRISBbits.TRISB10 = 0; break;
        case ST7735_PIN_RB11: TRISBbits.TRISB11 = 0; break;
    }
}

static void pin_write(ST7735_Pin pin, uint8_t value)
{
    switch(pin)
    {
        case ST7735_PIN_RB2:  LATBbits.LATB2 = value; break;
        case ST7735_PIN_RB3:  LATBbits.LATB3 = value; break;
        case ST7735_PIN_RB4:  LATBbits.LATB4 = value; break;
        case ST7735_PIN_RB7:  LATBbits.LATB7 = value; break;
        case ST7735_PIN_RB10: LATBbits.LATB10 = value; break;
        case ST7735_PIN_RB11: LATBbits.LATB11 = value; break;
    }
}

static void delay_ms(uint32_t ms)
{
    while(ms--)
    {
        uint32_t start = _CP0_GET_COUNT();
        while((_CP0_GET_COUNT() - start) < (SYS_FREQ / 2000));
    }
}

void ST7735_Config(ST7735_t *tft, ST7735_Pin cs, ST7735_Pin dc, ST7735_Pin rst)
{
    tft->cs = cs;
    tft->dc = dc;
    tft->rst = rst;
}

static void cs_low(ST7735_t *tft)  { pin_write(tft->cs, 0); }
static void cs_high(ST7735_t *tft) { pin_write(tft->cs, 1); }
static void dc_low(ST7735_t *tft)  { pin_write(tft->dc, 0); }
static void dc_high(ST7735_t *tft) { pin_write(tft->dc, 1); }

static void write_cmd(ST7735_t *tft, uint8_t cmd)
{
    dc_low(tft);
    cs_low(tft);
    SPI1_Write(cmd);
    cs_high(tft);
}

static void write_data(ST7735_t *tft, uint8_t data)
{
    dc_high(tft);
    cs_low(tft);
    SPI1_Write(data);
    cs_high(tft);
}

static void write_data16(uint16_t data)
{
    SPI1_Write(data >> 8);
    SPI1_Write(data & 0xFF);
}

static void set_window(ST7735_t *tft, uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    x0 += ST7735_XSTART;
    x1 += ST7735_XSTART;
    y0 += ST7735_YSTART;
    y1 += ST7735_YSTART;

    write_cmd(tft, ST7735_CASET);
    write_data(tft, 0x00);
    write_data(tft, x0);
    write_data(tft, 0x00);
    write_data(tft, x1);

    write_cmd(tft, ST7735_RASET);
    write_data(tft, 0x00);
    write_data(tft, y0);
    write_data(tft, 0x00);
    write_data(tft, y1);

    write_cmd(tft, ST7735_RAMWR);
}

void ST7735_Init(ST7735_t *tft)
{
    pin_tris_output(tft->cs);
    pin_tris_output(tft->dc);
    pin_tris_output(tft->rst);

    pin_write(tft->cs, 1);
    pin_write(tft->dc, 1);
    pin_write(tft->rst, 1);

    delay_ms(100);
    pin_write(tft->rst, 0);
    delay_ms(100);
    pin_write(tft->rst, 1);
    delay_ms(200);

    write_cmd(tft, ST7735_SWRESET);
    delay_ms(150);

    write_cmd(tft, ST7735_SLPOUT);
    delay_ms(150);

    write_cmd(tft, ST7735_COLMOD);
    write_data(tft, 0x05);

    write_cmd(tft, ST7735_MADCTL);
    write_data(tft, ST7735_MADCTL_180);

    write_cmd(tft, ST7735_DISPON);
    delay_ms(100);
}

void ST7735_FillScreen(ST7735_t *tft, uint16_t color)
{
    ST7735_FillRect(tft, 0, 0, ST7735_WIDTH, ST7735_HEIGHT, color);
}

void ST7735_DrawPixel(ST7735_t *tft, uint8_t x, uint8_t y, uint16_t color)
{
    if(x >= ST7735_WIDTH || y >= ST7735_HEIGHT) return;

    set_window(tft, x, y, x, y);

    dc_high(tft);
    cs_low(tft);
    write_data16(color);
    cs_high(tft);
}

// void ST7735_FillRect(ST7735_t *tft, uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t color)
// {
//     if(x >= ST7735_WIDTH || y >= ST7735_HEIGHT) return;

//     if((x + w - 1) >= ST7735_WIDTH)  w = ST7735_WIDTH - x;
//     if((y + h - 1) >= ST7735_HEIGHT) h = ST7735_HEIGHT - y;

//     set_window(tft, x, y, x + w - 1, y + h - 1);

//     dc_high(tft);
//     cs_low(tft);

//     for(uint32_t i = 0; i < (uint32_t)w * h; i++)
//     {
//         write_data16(color);
//     }

//     cs_high(tft);
// }
void ST7735_FillRect(ST7735_t *tft, uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t color)
{
    if(x >= ST7735_WIDTH || y >= ST7735_HEIGHT) return;

    if((x + w - 1) >= ST7735_WIDTH)  w = ST7735_WIDTH - x;
    if((y + h - 1) >= ST7735_HEIGHT) h = ST7735_HEIGHT - y;

    set_window(tft, x, y, x + w - 1, y + h - 1);

    dc_high(tft);
    cs_low(tft);

    SPI1_WriteColor(color, (uint32_t)w * h);

    cs_high(tft);
}

void ST7735_DrawFastHLine(ST7735_t *tft, uint8_t x, uint8_t y, uint8_t w, uint16_t color)
{
    ST7735_FillRect(tft, x, y, w, 1, color);
}

void ST7735_DrawFastVLine(ST7735_t *tft, uint8_t x, uint8_t y, uint8_t h, uint16_t color)
{
    ST7735_FillRect(tft, x, y, 1, h, color);
}

void ST7735_DrawChar(ST7735_t *tft, uint8_t x, uint8_t y, char c, uint16_t color, uint16_t bg, uint8_t size)
{
    if(c < 32 || c > 127) c = '?';

    for(uint8_t i = 0; i < 5; i++)
    {
        uint8_t line = font5x7[c - 32][i];

        for(uint8_t j = 0; j < 8; j++)
        {
            if(line & 0x01)
            {
                if(size == 1)
                    ST7735_DrawPixel(tft, x + i, y + j, color);
                else
                    ST7735_FillRect(tft, x + i * size, y + j * size, size, size, color);
            }
            else
            {
                if(bg != color)
                {
                    if(size == 1)
                        ST7735_DrawPixel(tft, x + i, y + j, bg);
                    else
                        ST7735_FillRect(tft, x + i * size, y + j * size, size, size, bg);
                }
            }

            line >>= 1;
        }
    }

    if(bg != color)
    {
        if(size == 1)
            ST7735_FillRect(tft, x + 5, y, 1, 8, bg);
        else
            ST7735_FillRect(tft, x + 5 * size, y, size, 8 * size, bg);
    }
}

void ST7735_Print(ST7735_t *tft, uint8_t x, uint8_t y, const char *str, uint16_t color, uint16_t bg, uint8_t size)
{
    while(*str)
    {
        ST7735_DrawChar(tft, x, y, *str, color, bg, size);
        x += 6 * size;

        if(x > ST7735_WIDTH - (6 * size))
        {
            x = 0;
            y += 8 * size;
        }

        str++;
    }
}

static int iabs_int(int v)
{
    return (v < 0) ? -v : v;
}

void ST7735_DrawLine(ST7735_t *tft, int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = iabs_int(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -iabs_int(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while(1)
    {
        ST7735_DrawPixel(tft, x0, y0, color);

        if(x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;

        if(e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }

        if(e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

void ST7735_DrawCircle(ST7735_t *tft, int xc, int yc, int r, uint16_t color)
{
    int x = 0;
    int y = r;
    int d = 3 - 2 * r;

    while(y >= x)
    {
        ST7735_DrawPixel(tft, xc + x, yc + y, color);
        ST7735_DrawPixel(tft, xc - x, yc + y, color);
        ST7735_DrawPixel(tft, xc + x, yc - y, color);
        ST7735_DrawPixel(tft, xc - x, yc - y, color);
        ST7735_DrawPixel(tft, xc + y, yc + x, color);
        ST7735_DrawPixel(tft, xc - y, yc + x, color);
        ST7735_DrawPixel(tft, xc + y, yc - x, color);
        ST7735_DrawPixel(tft, xc - y, yc - x, color);

        x++;

        if(d > 0)
        {
            y--;
            d += 4 * (x - y) + 10;
        }
        else
        {
            d += 4 * x + 6;
        }
    }
}

void ST7735_DrawTriangle(ST7735_t *tft, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    ST7735_DrawLine(tft, x0, y0, x1, y1, color);
    ST7735_DrawLine(tft, x1, y1, x2, y2, color);
    ST7735_DrawLine(tft, x2, y2, x0, y0, color);
}

static void swap_int(int *a, int *b)
{
    int t = *a;
    *a = *b;
    *b = t;
}

void ST7735_FillTriangle(ST7735_t *tft, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    if(y0 > y1) { swap_int(&y0, &y1); swap_int(&x0, &x1); }
    if(y1 > y2) { swap_int(&y1, &y2); swap_int(&x1, &x2); }
    if(y0 > y1) { swap_int(&y0, &y1); swap_int(&x0, &x1); }

    if(y0 == y2)
    {
        int a = x0;
        int b = x0;

        if(x1 < a) a = x1;
        else if(x1 > b) b = x1;

        if(x2 < a) a = x2;
        else if(x2 > b) b = x2;

        ST7735_DrawFastHLine(tft, a, y0, b - a + 1, color);
        return;
    }

    int dx01 = x1 - x0;
    int dy01 = y1 - y0;
    int dx02 = x2 - x0;
    int dy02 = y2 - y0;
    int dx12 = x2 - x1;
    int dy12 = y2 - y1;

    int sa = 0;
    int sb = 0;

    int last = (y1 == y2) ? y1 : y1 - 1;

    for(int y = y0; y <= last; y++)
    {
        int a = x0 + sa / dy01;
        int b = x0 + sb / dy02;

        sa += dx01;
        sb += dx02;

        if(a > b) swap_int(&a, &b);

        ST7735_DrawFastHLine(tft, a, y, b - a + 1, color);
    }

    sa = dx12 * (last + 1 - y1);
    sb = dx02 * (last + 1 - y0);

    for(int y = last + 1; y <= y2; y++)
    {
        int a = x1 + sa / dy12;
        int b = x0 + sb / dy02;

        sa += dx12;
        sb += dx02;

        if(a > b) swap_int(&a, &b);

        ST7735_DrawFastHLine(tft, a, y, b - a + 1, color);
    }
}

// void ST7735_DrawRGBBitmap(ST7735_t *tft, uint8_t x, uint8_t y, const uint16_t *bitmap, uint8_t w, uint8_t h)
// {
//     if(x >= ST7735_WIDTH || y >= ST7735_HEIGHT) return;

//     if((x + w - 1) >= ST7735_WIDTH)  w = ST7735_WIDTH - x;
//     if((y + h - 1) >= ST7735_HEIGHT) h = ST7735_HEIGHT - y;

//     set_window(tft, x, y, x + w - 1, y + h - 1);

//     dc_high(tft);
//     cs_low(tft);

//     for(uint32_t i = 0; i < (uint32_t)w * h; i++)
//     {
//         SPI1_Write(bitmap[i] >> 8);
//         SPI1_Write(bitmap[i] & 0xFF);
//     }

//     cs_high(tft);
// }
void ST7735_DrawRGBBitmap(ST7735_t *tft, uint8_t x, uint8_t y, const uint16_t *bitmap, uint8_t w, uint8_t h)
{
    if(x >= ST7735_WIDTH || y >= ST7735_HEIGHT) return;

    if((x + w - 1) >= ST7735_WIDTH)  w = ST7735_WIDTH - x;
    if((y + h - 1) >= ST7735_HEIGHT) h = ST7735_HEIGHT - y;

    set_window(tft, x, y, x + w - 1, y + h - 1);

    dc_high(tft);
    cs_low(tft);

    SPI1_Write16Buffer(bitmap, (uint32_t)w * h);

    cs_high(tft);
}