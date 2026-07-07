#ifndef LCD_I2C_H
#define LCD_I2C_H

#include <stdint.h>
#include <stdbool.h>

#define LCD_I2C_ADDR_DEFAULT  0x27

typedef void (*LCD_I2C_ServiceCallback)(void);

void LCD_I2C_SetServiceCallback(LCD_I2C_ServiceCallback callback);
bool LCD_I2C_Probe(uint8_t addr);
void LCD_I2C_Select(uint8_t addr);
bool LCD_I2C_Init(uint8_t addr);
bool LCD_I2C_Clear(void);
bool LCD_I2C_Home(void);
bool LCD_I2C_SetCursor(uint8_t row, uint8_t col);
bool LCD_I2C_WriteChar(char c);
bool LCD_I2C_WriteString(const char *str);
bool LCD_I2C_Backlight(uint8_t on);
bool LCD_I2C_Command(uint8_t cmd);

#endif
