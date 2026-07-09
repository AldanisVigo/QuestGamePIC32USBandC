#ifndef PIC32_USB_CDC_H
#define PIC32_USB_CDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    PIC32_USB_CDC_STATE_DETACHED = 0,
    PIC32_USB_CDC_STATE_DEFAULT,
    PIC32_USB_CDC_STATE_ADDRESSED,
    PIC32_USB_CDC_STATE_CONFIGURED,
    PIC32_USB_CDC_STATE_SUSPENDED
} PIC32_USB_CDC_State;

typedef struct
{
    uint32_t baud_rate;
    uint8_t stop_bits;
    uint8_t parity;
    uint8_t data_bits;
} PIC32_USB_CDC_LineCoding;

typedef void (*PIC32_USB_CDC_LineCodingChanged)(const PIC32_USB_CDC_LineCoding *line_coding);
typedef void (*PIC32_USB_CDC_ControlLineChanged)(bool dtr, bool rts);
typedef void (*PIC32_USB_CDC_StateChanged)(PIC32_USB_CDC_State state);
typedef void (*PIC32_USB_CDC_DebugLog)(const char *message);

typedef struct
{
    uint16_t vid;
    uint16_t pid;
    uint16_t bcd_device;
    const char *manufacturer;
    const char *product;
    const char *serial_number;
    PIC32_USB_CDC_LineCodingChanged on_line_coding_changed;
    PIC32_USB_CDC_ControlLineChanged on_control_line_changed;
    PIC32_USB_CDC_StateChanged on_state_changed;
    PIC32_USB_CDC_DebugLog debug_log;
} PIC32_USB_CDC_Config;

void PIC32_USB_CDC_ConfigDefault(PIC32_USB_CDC_Config *config);

void PIC32_USB_CDC_Init(const PIC32_USB_CDC_Config *config);
void PIC32_USB_CDC_Detach(void);
void PIC32_USB_CDC_Task(void);

PIC32_USB_CDC_State PIC32_USB_CDC_StateGet(void);
bool PIC32_USB_CDC_Configured(void);
bool PIC32_USB_CDC_IsOpen(void);
bool PIC32_USB_CDC_Suspended(void);

size_t PIC32_USB_CDC_Read(void *buffer, size_t length);
size_t PIC32_USB_CDC_Write(const void *buffer, size_t length);
size_t PIC32_USB_CDC_WriteString(const char *text);
size_t PIC32_USB_CDC_RxAvailable(void);
size_t PIC32_USB_CDC_TxAvailable(void);
uint32_t PIC32_USB_CDC_RxDropped(void);

const PIC32_USB_CDC_LineCoding *PIC32_USB_CDC_LineCodingGet(void);
bool PIC32_USB_CDC_DTR(void);
bool PIC32_USB_CDC_RTS(void);

#ifdef __cplusplus
}
#endif

#endif
