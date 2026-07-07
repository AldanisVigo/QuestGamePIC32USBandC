#include "VMenuSys.h"

#define VMENU_BLACK   ST7735_BLACK
#define VMENU_WHITE   ST7735_WHITE
#define VMENU_BLUE    ST7735_BLUE
#define VMENU_GREEN   ST7735_GREEN
#define VMENU_RED     ST7735_RED
#define VMENU_YELLOW  ST7735_YELLOW
#define VMENU_GRAY    0x8410

static uint8_t VAbsX(VWindow_t *window, uint16_t x)
{
    return (uint8_t)(window->posx + x);
}

static uint8_t VAbsY(VWindow_t *window, uint16_t y)
{
    return (uint8_t)(window->posy + y);
}

static void VDrawRect(ST7735_t *tft,
                      uint8_t x,
                      uint8_t y,
                      uint8_t w,
                      uint8_t h,
                      uint16_t color)
{
    ST7735_DrawFastHLine(tft, x, y, w, color);
    ST7735_DrawFastHLine(tft, x, y + h - 1, w, color);
    ST7735_DrawFastVLine(tft, x, y, h, color);
    ST7735_DrawFastVLine(tft, x + w - 1, y, h, color);
}

/* ---------------- Window ---------------- */

void VWindow_Init(VWindow_t *window, ST7735_t *tft,
                  uint16_t posx, uint16_t posy,
                  uint16_t width, uint16_t height,
                  const char *title)
{
    window->tft = tft;
    window->posx = posx;
    window->posy = posy;
    window->width = width;
    window->height = height;
    window->title = title;
}

void VWindow_Draw(VWindow_t *window)
{
    ST7735_FillRect(window->tft,
                    window->posx,
                    window->posy,
                    window->width,
                    window->height,
                    VMENU_WHITE);

    VDrawRect(window->tft,
              window->posx,
              window->posy,
              window->width,
              window->height,
              VMENU_BLUE);

    ST7735_FillRect(window->tft,
                    window->posx,
                    window->posy,
                    window->width,
                    14,
                    VMENU_BLUE);

    ST7735_Print(window->tft,
                 window->posx + 3,
                 window->posy + 3,
                 window->title,
                 VMENU_WHITE,
                 VMENU_BLUE,
                 1);
}

/* ---------------- Label ---------------- */

void VLabel_Init(VLabel_t *label, VWindow_t *window,
                 uint16_t posx, uint16_t posy,
                 const char *text)
{
    label->window = window;
    label->posx = posx;
    label->posy = posy;
    label->text = text;
}

void VLabel_Draw(VLabel_t *label)
{
    ST7735_Print(label->window->tft,
                 VAbsX(label->window, label->posx),
                 VAbsY(label->window, label->posy),
                 label->text,
                 VMENU_BLACK,
                 VMENU_WHITE,
                 1);
}

/* ---------------- Button ---------------- */

void VButton_Init(VButton_t *button, VWindow_t *window,
                  uint16_t posx, uint16_t posy,
                  uint16_t width, uint16_t height,
                  const char *label,
                  ButtonCallback_t callback)
{
    button->window = window;
    button->posx = posx;
    button->posy = posy;
    button->width = width;
    button->height = height;
    button->label = label;
    button->onClick = callback;
    button->visible = true;
    button->enabled = true;
    button->pressed = false;
}

void VButton_Draw(VButton_t *button)
{
    if (!button->visible) return;

    uint8_t x = VAbsX(button->window, button->posx);
    uint8_t y = VAbsY(button->window, button->posy);

    uint16_t fill = button->pressed ? VMENU_GRAY : VMENU_WHITE;
    uint16_t border = button->enabled ? VMENU_BLUE : VMENU_GRAY;

    ST7735_FillRect(button->window->tft,
                    x, y,
                    button->width,
                    button->height,
                    fill);

    VDrawRect(button->window->tft,
              x, y,
              button->width,
              button->height,
              border);

    ST7735_Print(button->window->tft,
                 x + 4,
                 y + 6,
                 button->label,
                 VMENU_BLACK,
                 fill,
                 1);
}

bool VButton_ContainsPoint(VButton_t *button, uint16_t x, uint16_t y)
{
    uint16_t bx = button->window->posx + button->posx;
    uint16_t by = button->window->posy + button->posy;

    return x >= bx &&
           x <  bx + button->width &&
           y >= by &&
           y <  by + button->height;
}

void VButton_Click(VButton_t *button)
{
    if (!button->enabled) return;
    if (!button->visible) return;

    button->pressed = true;
    VButton_Draw(button);

    button->pressed = false;
    VButton_Draw(button);

    if (button->onClick != 0)
        button->onClick(button);
}

/* ---------------- CheckBox ---------------- */

void VCheckBox_Init(VCheckBox_t *checkbox, VWindow_t *window,
                    uint16_t posx, uint16_t posy,
                    const char *label,
                    bool checked,
                    CheckBoxCallback_t callback)
{
    checkbox->window = window;
    checkbox->posx = posx;
    checkbox->posy = posy;
    checkbox->label = label;
    checkbox->checked = checked;
    checkbox->enabled = true;
    checkbox->onChange = callback;
}

void VCheckBox_Draw(VCheckBox_t *checkbox)
{
    uint8_t x = VAbsX(checkbox->window, checkbox->posx);
    uint8_t y = VAbsY(checkbox->window, checkbox->posy);

    VDrawRect(checkbox->window->tft,
              x, y,
              10, 10,
              checkbox->enabled ? VMENU_BLACK : VMENU_GRAY);

    ST7735_FillRect(checkbox->window->tft,
                    x + 2,
                    y + 2,
                    6,
                    6,
                    checkbox->checked ? VMENU_GREEN : VMENU_WHITE);

    ST7735_Print(checkbox->window->tft,
                 x + 14,
                 y,
                 checkbox->label,
                 checkbox->enabled ? VMENU_BLACK : VMENU_GRAY,
                 VMENU_WHITE,
                 1);
}

bool VCheckBox_ContainsPoint(VCheckBox_t *checkbox, uint16_t x, uint16_t y)
{
    uint16_t cx = checkbox->window->posx + checkbox->posx;
    uint16_t cy = checkbox->window->posy + checkbox->posy;

    return x >= cx &&
           x <  cx + 100 &&
           y >= cy &&
           y <  cy + 12;
}

void VCheckBox_Click(VCheckBox_t *checkbox)
{
    if (!checkbox->enabled) return;

    checkbox->checked = !checkbox->checked;
    VCheckBox_Draw(checkbox);

    if (checkbox->onChange != 0)
        checkbox->onChange(checkbox, checkbox->checked);
}

/* ---------------- Slider ---------------- */

void VSlider_Init(VSlider_t *slider, VWindow_t *window,
                  uint16_t posx, uint16_t posy,
                  uint16_t width,
                  uint16_t min,
                  uint16_t max,
                  uint16_t value,
                  SliderCallback_t callback)
{
    slider->window = window;
    slider->posx = posx;
    slider->posy = posy;
    slider->width = width;
    slider->min = min;
    slider->max = max;
    slider->value = value;
    slider->onChange = callback;

    if (slider->value < slider->min)
        slider->value = slider->min;

    if (slider->value > slider->max)
        slider->value = slider->max;
}

void VSlider_Draw(VSlider_t *slider)
{
    uint8_t x = VAbsX(slider->window, slider->posx);
    uint8_t y = VAbsY(slider->window, slider->posy);

    uint8_t knobX;

    ST7735_FillRect(slider->window->tft,
                    x,
                    y,
                    slider->width + 4,
                    14,
                    VMENU_WHITE);

    if (slider->max == slider->min) {
        knobX = x;
    } else {
        knobX = x + (uint8_t)(((uint32_t)(slider->value - slider->min) * slider->width) /
                              (slider->max - slider->min));
    }

    ST7735_DrawFastHLine(slider->window->tft,
                         x,
                         y + 6,
                         slider->width,
                         VMENU_BLACK);

    ST7735_FillRect(slider->window->tft,
                    knobX - 2,
                    y,
                    5,
                    12,
                    VMENU_BLUE);
}

bool VSlider_ContainsPoint(VSlider_t *slider, uint16_t x, uint16_t y)
{
    uint16_t sx = slider->window->posx + slider->posx;
    uint16_t sy = slider->window->posy + slider->posy;

    return x >= sx &&
           x <  sx + slider->width &&
           y >= sy &&
           y <  sy + 14;
}

void VSlider_SetValue(VSlider_t *slider, uint16_t value)
{
    if (value < slider->min)
        value = slider->min;

    if (value > slider->max)
        value = slider->max;

    slider->value = value;
    VSlider_Draw(slider);

    if (slider->onChange != 0)
        slider->onChange(slider, slider->value);
}

/* ---------------- ComboBox ---------------- */

void VComboBox_Init(VComboBox_t *combo, VWindow_t *window,
                    uint16_t posx, uint16_t posy,
                    uint16_t width, uint16_t height,
                    const char **items,
                    uint8_t itemCount,
                    uint8_t selectedIndex,
                    ComboBoxCallback_t callback)
{
    combo->window = window;
    combo->posx = posx;
    combo->posy = posy;
    combo->width = width;
    combo->height = height;
    combo->items = items;
    combo->itemCount = itemCount;
    combo->selectedIndex = selectedIndex;
    combo->onChange = callback;

    if (combo->itemCount == 0) {
        combo->selectedIndex = 0;
    } else if (combo->selectedIndex >= combo->itemCount) {
        combo->selectedIndex = 0;
    }
}

void VComboBox_Draw(VComboBox_t *combo)
{
    uint8_t x = VAbsX(combo->window, combo->posx);
    uint8_t y = VAbsY(combo->window, combo->posy);

    ST7735_FillRect(combo->window->tft,
                    x,
                    y,
                    combo->width,
                    combo->height,
                    VMENU_WHITE);

    VDrawRect(combo->window->tft,
              x,
              y,
              combo->width,
              combo->height,
              VMENU_BLUE);

    if (combo->itemCount > 0 && combo->items != 0) {
        ST7735_Print(combo->window->tft,
                     x + 4,
                     y + 5,
                     combo->items[combo->selectedIndex],
                     VMENU_BLACK,
                     VMENU_WHITE,
                     1);
    }

    ST7735_Print(combo->window->tft,
                 x + combo->width - 10,
                 y + 5,
                 "v",
                 VMENU_BLACK,
                 VMENU_WHITE,
                 1);
}

bool VComboBox_ContainsPoint(VComboBox_t *combo, uint16_t x, uint16_t y)
{
    uint16_t cx = combo->window->posx + combo->posx;
    uint16_t cy = combo->window->posy + combo->posy;

    return x >= cx &&
           x <  cx + combo->width &&
           y >= cy &&
           y <  cy + combo->height;
}

void VComboBox_Next(VComboBox_t *combo)
{
    if (combo->itemCount == 0) return;
    if (combo->items == 0) return;

    combo->selectedIndex++;

    if (combo->selectedIndex >= combo->itemCount)
        combo->selectedIndex = 0;

    VComboBox_Draw(combo);

    if (combo->onChange != 0)
        combo->onChange(combo, combo->selectedIndex);
}