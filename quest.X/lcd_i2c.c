#include <xc.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "definitions.h"
#include "lcd_i2c.h"

#define LCD_RS  0x01
#define LCD_RW  0x02
#define LCD_EN  0x04
#define LCD_BL  0x08
#define LCD_I2C_TIMEOUT_COUNTS (CPU_CLOCK_FREQUENCY / 20U)

static uint8_t lcd_addr = LCD_I2C_ADDR_DEFAULT;
static uint8_t lcd_backlight = LCD_BL;
static LCD_I2C_ServiceCallback lcd_service_callback = NULL;

void LCD_I2C_SetServiceCallback(LCD_I2C_ServiceCallback callback)
{
    lcd_service_callback = callback;
}

static void LCD_ServiceCallback(void)
{
    if(lcd_service_callback != NULL)
    {
        lcd_service_callback();
    }
}

static void LCD_DelayMs(uint32_t ms)
{
    for(uint32_t i = 0; i < ms; i++)
    {
        for(volatile uint32_t j = 0; j < 6000; j++)
        {
            if((j & 0x03FFU) == 0U)
            {
                LCD_ServiceCallback();
            }
        }
    }
}

static bool LCD_WaitForI2CIdle(void)
{
    uint32_t startCount = _CP0_GET_COUNT();

    while(I2C1_IsBusy())
    {
        LCD_ServiceCallback();

        if((_CP0_GET_COUNT() - startCount) > LCD_I2C_TIMEOUT_COUNTS)
        {
            I2C1_TransferAbort();
            return false;
        }
    }

    return true;
}

static bool LCD_WriteRaw(uint8_t data)
{
    uint8_t value = data | lcd_backlight;

    if(!LCD_WaitForI2CIdle())
    {
        return false;
    }

    if(!I2C1_Write(lcd_addr, &value, 1))
    {
        return false;
    }

    if(!LCD_WaitForI2CIdle())
    {
        return false;
    }

    return (I2C1_ErrorGet() == I2C_ERROR_NONE);
}

bool LCD_I2C_Probe(uint8_t addr)
{
    uint8_t previousAddress = lcd_addr;
    bool found = false;

    lcd_addr = addr;
    found = LCD_WriteRaw(0x00);
    lcd_addr = previousAddress;

    return found;
}

void LCD_I2C_Select(uint8_t addr)
{
    lcd_addr = addr;
}

static bool LCD_PulseEnable(uint8_t data)
{
    if(!LCD_WriteRaw(data | LCD_EN))
    {
        return false;
    }

    LCD_DelayMs(1);

    if(!LCD_WriteRaw(data & ~LCD_EN))
    {
        return false;
    }

    LCD_DelayMs(1);

    return true;
}

static bool LCD_Write4Bits(uint8_t nibble, uint8_t mode)
{
    uint8_t data = 0;

    data |= (nibble & 0xF0);
    data |= mode;

    return LCD_PulseEnable(data);
}

static bool LCD_Send(uint8_t value, uint8_t mode)
{
    if(!LCD_Write4Bits(value & 0xF0, mode))
    {
        return false;
    }

    return LCD_Write4Bits((value << 4) & 0xF0, mode);
}

bool LCD_I2C_Command(uint8_t cmd)
{
    return LCD_Send(cmd, 0);
}

bool LCD_I2C_WriteChar(char c)
{
    return LCD_Send((uint8_t)c, LCD_RS);
}

bool LCD_I2C_WriteString(const char *str)
{
    while(*str)
    {
        if(!LCD_I2C_WriteChar(*str++))
        {
            return false;
        }
    }

    return true;
}

bool LCD_I2C_Clear(void)
{
    if(!LCD_I2C_Command(0x01))
    {
        return false;
    }

    LCD_DelayMs(2);

    return true;
}

bool LCD_I2C_Home(void)
{
    if(!LCD_I2C_Command(0x02))
    {
        return false;
    }

    LCD_DelayMs(2);

    return true;
}

bool LCD_I2C_SetCursor(uint8_t row, uint8_t col)
{
    uint8_t row_offsets[] = {0x00, 0x40, 0x14, 0x54};

    if(row > 3)
        row = 3;

    return LCD_I2C_Command(0x80 | (col + row_offsets[row]));
}

bool LCD_I2C_Backlight(uint8_t on)
{
    if(on)
        lcd_backlight = LCD_BL;
    else
        lcd_backlight = 0x00;

    return LCD_WriteRaw(0x00);
}

bool LCD_I2C_Init(uint8_t addr)
{
    lcd_addr = addr;
    lcd_backlight = LCD_BL;

    LCD_DelayMs(50);

    if(!LCD_Write4Bits(0x30, 0))
    {
        return false;
    }

    LCD_DelayMs(5);

    if(!LCD_Write4Bits(0x30, 0))
    {
        return false;
    }

    LCD_DelayMs(5);

    if(!LCD_Write4Bits(0x30, 0))
    {
        return false;
    }

    LCD_DelayMs(5);

    if(!LCD_Write4Bits(0x20, 0))
    {
        return false;
    }

    LCD_DelayMs(5);

    if(!LCD_I2C_Command(0x28)) // 4-bit, 2-line, 5x8 font
    {
        return false;
    }

    if(!LCD_I2C_Command(0x08)) // display off
    {
        return false;
    }

    if(!LCD_I2C_Clear())
    {
        return false;
    }

    if(!LCD_I2C_Command(0x06)) // entry mode
    {
        return false;
    }

    return LCD_I2C_Command(0x0C); // display on, cursor off
}
