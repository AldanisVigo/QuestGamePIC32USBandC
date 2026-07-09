# PIC32MX USB CDC Library

This is a small bare-metal USB CDC ACM device stack for the PIC32MX USBFS peripheral, written as a reusable pair of files:

- `pic32_usb_cdc.h`
- `pic32_usb_cdc.c`

It is intended to be copied into plain XC32 projects without bringing Harmony along. The default descriptors match the CDC device we already proved works on the PIC32MX270F256B: Microchip VID `0x04D8`, PID `0x0001`, manufacturer `VigoBeatz`, product `VQuest CDC`.

## Status

This is the first standalone implementation. It contains the real pieces needed for CDC:

- USBFS register setup and Buffer Descriptor Table management
- EP0 enumeration and standard device requests
- CDC ACM requests for line coding, DTR/RTS, and break
- Bulk OUT receive on EP2 and bulk IN transmit on EP3
- Polling API, no Harmony runtime, no OS
- Small RX/TX ring buffers

It still needs board testing before we replace the current Harmony firmware with it. The current game firmware remains untouched.

## API Shape

```c
#include "pic32_usb_cdc.h"

int main(void)
{
    PIC32_USB_CDC_Config usb;
    PIC32_USB_CDC_ConfigDefault(&usb);
    usb.product = "My PIC32 CDC";

    /* Configure oscillator/config bits before this. USB needs a valid 48 MHz USB clock. */
    PIC32_USB_CDC_Init(&usb);

    for (;;)
    {
        uint8_t buffer[64];
        size_t n;

        PIC32_USB_CDC_Task();

        n = PIC32_USB_CDC_Read(buffer, sizeof(buffer));
        if (n > 0U)
        {
            PIC32_USB_CDC_Write(buffer, n);
        }
    }
}
```

## Integration Notes

- Compile with XC32 for a PIC32MX device that has `U1` USBFS registers.
- This library does not set configuration bits or oscillator registers. Your project must configure the USB clock correctly first.
- Call `PIC32_USB_CDC_Task()` very often from the main loop. This is a polling stack.
- `PIC32_USB_CDC_IsOpen()` becomes true after the host asserts CDC DTR.
- Defaults use an 8-byte EP0 packet size because that is what the working Harmony project currently uses.

## Extension Plan

The implementation keeps USB device setup, endpoint BDT handling, and CDC class handling separated internally. To add HID later, the next step is to replace the fixed CDC descriptor/request handling with a small class-driver table:

- descriptor provider
- class request handler
- endpoint open/reset hooks
- endpoint transfer callbacks

That would let CDC and HID share the same EP0/device core.
