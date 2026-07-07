#ifndef V_MENU_SYS_H
#define V_MENU_SYS_H

#include <stdint.h>
#include <stdbool.h>
#include "ST7735.h"

typedef struct VWindow VWindow_t;
typedef struct VLabel VLabel_t;
typedef struct VButton VButton_t;
typedef struct VCheckBox VCheckBox_t;
typedef struct VSlider VSlider_t;
typedef struct VComboBox VComboBox_t;

typedef void (*ButtonCallback_t)(VButton_t *button);
typedef void (*CheckBoxCallback_t)(VCheckBox_t *checkbox, bool checked);
typedef void (*SliderCallback_t)(VSlider_t *slider, uint16_t value);
typedef void (*ComboBoxCallback_t)(VComboBox_t *combo, uint8_t selectedIndex);

struct VWindow {
    ST7735_t *tft;
    uint16_t posx;
    uint16_t posy;
    uint16_t width;
    uint16_t height;
    const char *title;
};

struct VLabel {
    VWindow_t *window;
    uint16_t posx;
    uint16_t posy;
    const char *text;
};

struct VButton {
    VWindow_t *window;
    uint16_t posx;
    uint16_t posy;
    uint16_t width;
    uint16_t height;
    const char *label;
    ButtonCallback_t onClick;
    bool visible;
    bool enabled;
    bool pressed;
};

struct VCheckBox {
    VWindow_t *window;
    uint16_t posx;
    uint16_t posy;
    const char *label;
    bool checked;
    bool enabled;
    CheckBoxCallback_t onChange;
};

struct VSlider {
    VWindow_t *window;
    uint16_t posx;
    uint16_t posy;
    uint16_t width;
    uint16_t min;
    uint16_t max;
    uint16_t value;
    SliderCallback_t onChange;
};

struct VComboBox {
    VWindow_t *window;
    uint16_t posx;
    uint16_t posy;
    uint16_t width;
    uint16_t height;
    const char **items;
    uint8_t itemCount;
    uint8_t selectedIndex;
    ComboBoxCallback_t onChange;
};

/* Window */
void VWindow_Init(VWindow_t *window, ST7735_t *tft,
                  uint16_t posx, uint16_t posy,
                  uint16_t width, uint16_t height,
                  const char *title);

void VWindow_Draw(VWindow_t *window);

/* Label */
void VLabel_Init(VLabel_t *label, VWindow_t *window,
                 uint16_t posx, uint16_t posy,
                 const char *text);

void VLabel_Draw(VLabel_t *label);

/* Button */
void VButton_Init(VButton_t *button, VWindow_t *window,
                  uint16_t posx, uint16_t posy,
                  uint16_t width, uint16_t height,
                  const char *label,
                  ButtonCallback_t callback);

void VButton_Draw(VButton_t *button);
bool VButton_ContainsPoint(VButton_t *button, uint16_t x, uint16_t y);
void VButton_Click(VButton_t *button);

/* CheckBox */
void VCheckBox_Init(VCheckBox_t *checkbox, VWindow_t *window,
                    uint16_t posx, uint16_t posy,
                    const char *label,
                    bool checked,
                    CheckBoxCallback_t callback);

void VCheckBox_Draw(VCheckBox_t *checkbox);
bool VCheckBox_ContainsPoint(VCheckBox_t *checkbox, uint16_t x, uint16_t y);
void VCheckBox_Click(VCheckBox_t *checkbox);

/* Slider */
void VSlider_Init(VSlider_t *slider, VWindow_t *window,
                  uint16_t posx, uint16_t posy,
                  uint16_t width,
                  uint16_t min,
                  uint16_t max,
                  uint16_t value,
                  SliderCallback_t callback);

void VSlider_Draw(VSlider_t *slider);
bool VSlider_ContainsPoint(VSlider_t *slider, uint16_t x, uint16_t y);
void VSlider_SetValue(VSlider_t *slider, uint16_t value);

/* ComboBox */
void VComboBox_Init(VComboBox_t *combo, VWindow_t *window,
                    uint16_t posx, uint16_t posy,
                    uint16_t width, uint16_t height,
                    const char **items,
                    uint8_t itemCount,
                    uint8_t selectedIndex,
                    ComboBoxCallback_t callback);

void VComboBox_Draw(VComboBox_t *combo);
bool VComboBox_ContainsPoint(VComboBox_t *combo, uint16_t x, uint16_t y);
void VComboBox_Next(VComboBox_t *combo);

#endif