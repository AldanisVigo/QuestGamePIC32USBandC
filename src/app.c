#include "app.h"
#include "definitions.h"
#include "../quest.X/lcd_i2c.h"
#include <string.h>

#define APP_READ_BUFFER_SIZE 64
#define APP_WRITE_BUFFER_SIZE 64
#define APP_LCD_COLUMNS 16U
#define APP_LCD_ROWS 2U
#define APP_TEXT_BUFFER_SIZE ((APP_LCD_COLUMNS * APP_LCD_ROWS) + 1U)
#define APP_LCD_MAX_FOUND 16U
#define APP_LCD_STATUS_MESSAGE_SIZE 128U

static USB_DEVICE_HANDLE usbDeviceHandle = USB_DEVICE_HANDLE_INVALID;
static USB_DEVICE_CDC_TRANSFER_HANDLE readTransferHandle;
static USB_DEVICE_CDC_TRANSFER_HANDLE writeTransferHandle;
static USB_DEVICE_CDC_TRANSFER_HANDLE serialStateTransferHandle;
static size_t readLength = 0;

static bool deviceConfigured = false;
static bool readComplete = false;
static bool writeComplete = true;
static bool readyMessageSent = false;
static bool lcdTestComplete = false;
static bool serialStateNotificationPending = false;
static bool serialStateNotificationBusy = false;

static uint8_t readBuffer[APP_READ_BUFFER_SIZE] __attribute__((coherent, aligned(16)));
static uint8_t writeBuffer[APP_WRITE_BUFFER_SIZE] __attribute__((coherent, aligned(16)));
static char lcdStatusMessage[APP_LCD_STATUS_MESSAGE_SIZE] __attribute__((coherent, aligned(16)));
static uint32_t lastHeartbeatCount = 0;
static uint8_t lcdFoundAddresses[APP_LCD_MAX_FOUND];
static uint8_t lcdFoundCount = 0;
static char pendingText[APP_TEXT_BUFFER_SIZE];
static size_t pendingTextLength = 0;
static bool pendingTextTruncated = false;

static USB_CDC_LINE_CODING lineCoding __attribute__((coherent, aligned(16))) =
{
    115200,
    0,
    0,
    8
};

static USB_CDC_SERIAL_STATE serialState __attribute__((coherent, aligned(16))) =
{
    .bRxCarrier = 1,
    .bTxCarrier = 1,
    .bBreak = 0,
    .bRingSignal = 0,
    .bFraming = 0,
    .bParity = 0,
    .bOverRun = 0
};

static void APP_USBDeviceEventHandler(USB_DEVICE_EVENT event, void * eventData, uintptr_t context);

static void APP_MessageAppend(char *message, size_t messageSize, size_t *length, const char *text)
{
    while((*text != '\0') && (*length < (messageSize - 1U)))
    {
        message[*length] = *text;
        *length += 1U;
        text++;
    }

    message[*length] = '\0';
}

static void APP_MessageAppendHexByte(char *message, size_t messageSize, size_t *length, uint8_t value)
{
    static const char hexDigits[] = "0123456789ABCDEF";
    char text[3];

    text[0] = hexDigits[(value >> 4) & 0x0F];
    text[1] = hexDigits[value & 0x0F];
    text[2] = '\0';

    APP_MessageAppend(message, messageSize, length, text);
}

static bool APP_LCDWriteRowText(const char *text, size_t *offset)
{
    for(uint8_t column = 0; column < APP_LCD_COLUMNS; column++)
    {
        char c = text[*offset];

        if(c == '\0')
        {
            break;
        }

        if(!LCD_I2C_WriteChar(c))
        {
            return false;
        }

        *offset += 1U;
    }

    return true;
}

static bool APP_LCDWriteTextToAddress(uint8_t address, const char *text)
{
    size_t offset = 0;

    if(!LCD_I2C_Init(address))
    {
        return false;
    }

    if(!LCD_I2C_Clear())
    {
        return false;
    }

    if(!LCD_I2C_SetCursor(0, 0))
    {
        return false;
    }

    if(!APP_LCDWriteRowText(text, &offset))
    {
        return false;
    }

    if(!LCD_I2C_SetCursor(1, 0))
    {
        return false;
    }

    return APP_LCDWriteRowText(text, &offset);
}

static uint8_t APP_LCDDisplayText(const char *text)
{
    uint8_t writtenCount = 0;

    for(uint8_t i = 0; i < lcdFoundCount; i++)
    {
        if(APP_LCDWriteTextToAddress(lcdFoundAddresses[i], text))
        {
            writtenCount++;
        }
    }

    return writtenCount;
}

static void APP_TextReset(void)
{
    pendingText[0] = '\0';
    pendingTextLength = 0;
    pendingTextTruncated = false;
}

static void APP_TextAppend(char c)
{
    if(pendingTextLength < (APP_TEXT_BUFFER_SIZE - 1U))
    {
        pendingText[pendingTextLength] = c;
        pendingTextLength++;
    }
    else
    {
        pendingTextTruncated = true;
    }
}

static const char *APP_TextCommit(void)
{
    uint8_t writtenCount;
    bool wasTruncated = pendingTextTruncated;

    pendingText[pendingTextLength] = '\0';

    if((pendingTextLength == 0U) && !wasTruncated)
    {
        APP_TextReset();
        return "Empty text ignored\r\n";
    }

    if(!lcdTestComplete)
    {
        APP_TextReset();
        return "LCD addresses not ready\r\n";
    }

    if(lcdFoundCount == 0U)
    {
        APP_TextReset();
        return "No LCDs found\r\n";
    }

    writtenCount = APP_LCDDisplayText(pendingText);
    APP_TextReset();

    if(writtenCount == 0U)
    {
        return "LCD write failed\r\n";
    }

    if(wasTruncated)
    {
        return "LCD updated (truncated)\r\n";
    }

    return "LCD updated\r\n";
}

static const char *APP_ProcessReceivedData(const uint8_t *data, size_t length)
{
    const char *status = NULL;

    for(size_t i = 0; i < length; i++)
    {
        uint8_t value = data[i];

        if(value == '\r')
        {
            continue;
        }

        if((value == '\n') || (value == '\0'))
        {
            status = APP_TextCommit();
            continue;
        }

        if(value == '\t')
        {
            APP_TextAppend(' ');
        }
        else if((value >= 32U) && (value <= 126U))
        {
            APP_TextAppend((char)value);
        }
        else
        {
            APP_TextAppend('?');
        }
    }

    return status;
}

static void APP_USBSendString(const char *message)
{
    size_t length = strlen(message);
    USB_DEVICE_CDC_RESULT writeResult;

    if(length > APP_WRITE_BUFFER_SIZE)
    {
        length = APP_WRITE_BUFFER_SIZE;
    }

    memcpy(writeBuffer, message, length);
    writeComplete = false;

    writeResult = USB_DEVICE_CDC_Write(
        USB_DEVICE_CDC_INDEX_0,
        &writeTransferHandle,
        writeBuffer,
        length,
        USB_DEVICE_CDC_TRANSFER_FLAGS_DATA_COMPLETE
    );

    if(writeResult != USB_DEVICE_CDC_RESULT_OK)
    {
        writeComplete = true;
        writeTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
    }
}

static void APP_USBRequestSerialStateNotification(void)
{
    serialStateNotificationPending = true;
}

static void APP_USBSendSerialStateNotification(void)
{
    USB_DEVICE_CDC_RESULT result;

    if(!deviceConfigured || !serialStateNotificationPending || serialStateNotificationBusy)
    {
        return;
    }

    result = USB_DEVICE_CDC_SerialStateNotificationSend(
        USB_DEVICE_CDC_INDEX_0,
        &serialStateTransferHandle,
        &serialState
    );

    if(result == USB_DEVICE_CDC_RESULT_OK)
    {
        serialStateNotificationPending = false;
        serialStateNotificationBusy = true;
    }
}

static void APP_LCDRecordFoundAddress(uint8_t address)
{
    if(lcdFoundCount < APP_LCD_MAX_FOUND)
    {
        lcdFoundAddresses[lcdFoundCount] = address;
        lcdFoundCount++;
    }
}

static void APP_LCDUseConfiguredAddresses(void)
{
    static const uint8_t configuredAddresses[] =
    {
        0x26, /* PCF8574 backpack with A0 bridged */
        0x23  /* PCF8574 backpack with A2 bridged */
    };

    for(uint8_t i = 0; i < (sizeof(configuredAddresses) / sizeof(configuredAddresses[0])); i++)
    {
        APP_LCDRecordFoundAddress(configuredAddresses[i]);
    }
}

static void APP_LCDBuildStatusMessage(void)
{
    size_t length = 0;

    lcdStatusMessage[0] = '\0';
    APP_MessageAppend(lcdStatusMessage, sizeof(lcdStatusMessage), &length, "PIC32 Harmony CDC READY\r\n");

    if(lcdFoundCount == 0U)
    {
        APP_MessageAppend(lcdStatusMessage, sizeof(lcdStatusMessage), &length, "I2C LCD addresses: none configured\r\n");
        return;
    }

    APP_MessageAppend(lcdStatusMessage, sizeof(lcdStatusMessage), &length, "I2C LCD addresses:");

    for(uint8_t i = 0; i < lcdFoundCount; i++)
    {
        APP_MessageAppend(lcdStatusMessage, sizeof(lcdStatusMessage), &length, " 0x");
        APP_MessageAppendHexByte(lcdStatusMessage, sizeof(lcdStatusMessage), &length, lcdFoundAddresses[i]);
    }

    APP_MessageAppend(lcdStatusMessage, sizeof(lcdStatusMessage), &length, "\r\nLCD writes deferred until text is sent\r\n");
}

static void APP_LCDPrepareConfiguredAddresses(void)
{
    if(lcdTestComplete)
    {
        return;
    }

    lcdTestComplete = true;
    lcdFoundCount = 0;

    APP_LCDUseConfiguredAddresses();
    APP_LCDBuildStatusMessage();
}

static void APP_USBDeviceOpenAndAttach(void)
{
    if (usbDeviceHandle != USB_DEVICE_HANDLE_INVALID)
    {
        return;
    }

    usbDeviceHandle = USB_DEVICE_Open(USB_DEVICE_INDEX_0, DRV_IO_INTENT_READWRITE);

    if (usbDeviceHandle != USB_DEVICE_HANDLE_INVALID)
    {
        USB_DEVICE_EventHandlerSet(
            usbDeviceHandle,
            APP_USBDeviceEventHandler,
            0
        );

        USB_DEVICE_PowerStateSet(usbDeviceHandle, USB_DEVICE_POWER_STATE_BUS_POWERED);
        USB_DEVICE_Attach(usbDeviceHandle);
    }
}

static USB_DEVICE_CDC_EVENT_RESPONSE APP_USBDeviceCDCEventHandler
(
    USB_DEVICE_CDC_INDEX index,
    USB_DEVICE_CDC_EVENT event,
    void * pData,
    uintptr_t userData
)
{
    switch (event)
    {
        case USB_DEVICE_CDC_EVENT_GET_LINE_CODING:
            USB_DEVICE_ControlSend(usbDeviceHandle, &lineCoding, sizeof(lineCoding));
            break;

        case USB_DEVICE_CDC_EVENT_SET_LINE_CODING:
            USB_DEVICE_ControlReceive(usbDeviceHandle, &lineCoding, sizeof(lineCoding));
            break;

        case USB_DEVICE_CDC_EVENT_CONTROL_TRANSFER_DATA_RECEIVED:
            USB_DEVICE_ControlStatus(usbDeviceHandle, USB_DEVICE_CONTROL_STATUS_OK);
            break;

        case USB_DEVICE_CDC_EVENT_CONTROL_TRANSFER_DATA_SENT:
            break;

        case USB_DEVICE_CDC_EVENT_SET_CONTROL_LINE_STATE:
            USB_DEVICE_ControlStatus(usbDeviceHandle, USB_DEVICE_CONTROL_STATUS_OK);
            APP_USBRequestSerialStateNotification();
            break;

        case USB_DEVICE_CDC_EVENT_SEND_BREAK:
            USB_DEVICE_ControlStatus(usbDeviceHandle, USB_DEVICE_CONTROL_STATUS_OK);
            break;

        case USB_DEVICE_CDC_EVENT_READ_COMPLETE:
        {
            USB_DEVICE_CDC_EVENT_DATA_READ_COMPLETE *readData =
                (USB_DEVICE_CDC_EVENT_DATA_READ_COMPLETE *)pData;

            if((readData != NULL) && (readData->status == USB_DEVICE_CDC_RESULT_OK))
            {
                readLength = readData->length;
                readComplete = true;
            }
            else
            {
                readTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
            }
            break;
        }

        case USB_DEVICE_CDC_EVENT_WRITE_COMPLETE:
            writeComplete = true;
            writeTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
            break;

        case USB_DEVICE_CDC_EVENT_SERIAL_STATE_NOTIFICATION_COMPLETE:
            serialStateNotificationBusy = false;
            serialStateTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
            break;

        default:
            break;
    }

    return USB_DEVICE_CDC_EVENT_RESPONSE_NONE;
}

static void APP_USBDeviceEventHandler
(
    USB_DEVICE_EVENT event,
    void * eventData,
    uintptr_t context
)
{
    switch (event)
    {
        case USB_DEVICE_EVENT_CONFIGURED:
            deviceConfigured = true;
            readyMessageSent = false;
            APP_USBRequestSerialStateNotification();
            USB_DEVICE_CDC_EventHandlerSet(
                USB_DEVICE_CDC_INDEX_0,
                APP_USBDeviceCDCEventHandler,
                0
            );
            break;

        case USB_DEVICE_EVENT_DECONFIGURED:
        case USB_DEVICE_EVENT_RESET:
        case USB_DEVICE_EVENT_SUSPENDED:
            deviceConfigured = false;
            readyMessageSent = false;
            readComplete = false;
            writeComplete = true;
            readLength = 0;
            readTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
            writeTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
            serialStateTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
            serialStateNotificationPending = false;
            serialStateNotificationBusy = false;
            APP_TextReset();
            break;

        default:
            break;
    }
}

void APP_Initialize(void)
{
    TRISBbits.TRISB7 = 0;
    LATBbits.LATB7 = 0;
    lastHeartbeatCount = _CP0_GET_COUNT();

    readTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
    writeTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
    serialStateTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
    APP_TextReset();
}

void APP_Tasks(void)
{
    uint32_t currentCount = _CP0_GET_COUNT();
    uint32_t heartbeatPeriod = (OSCCONbits.ULOCK != 0U) ?
        (CPU_CLOCK_FREQUENCY / 2U) :
        (CPU_CLOCK_FREQUENCY / 10U);

    if ((currentCount - lastHeartbeatCount) >= heartbeatPeriod)
    {
        LATBINV = (1UL << 7);
        lastHeartbeatCount = currentCount;
    }

    APP_USBDeviceOpenAndAttach();
    APP_LCDPrepareConfiguredAddresses();
    APP_USBSendSerialStateNotification();

    if (!deviceConfigured)
    {
        return;
    }

    if (!readyMessageSent && writeComplete && lcdTestComplete)
    {
        USB_DEVICE_CDC_RESULT writeResult;

        writeComplete = false;
        readyMessageSent = true;

        writeResult = USB_DEVICE_CDC_Write(
            USB_DEVICE_CDC_INDEX_0,
            &writeTransferHandle,
            (void *)lcdStatusMessage,
            strlen(lcdStatusMessage),
            USB_DEVICE_CDC_TRANSFER_FLAGS_DATA_COMPLETE
        );

        if(writeResult != USB_DEVICE_CDC_RESULT_OK)
        {
            writeComplete = true;
            readyMessageSent = false;
            writeTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
        }
    }

    if (readTransferHandle == USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID)
    {
        USB_DEVICE_CDC_RESULT readResult;

        readComplete = false;
        readLength = 0;

        readResult = USB_DEVICE_CDC_Read(
            USB_DEVICE_CDC_INDEX_0,
            &readTransferHandle,
            readBuffer,
            APP_READ_BUFFER_SIZE
        );

        if(readResult != USB_DEVICE_CDC_RESULT_OK)
        {
            readTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
        }
    }

    if (readComplete && writeComplete)
    {
        const char *response;

        readComplete = false;
        response = APP_ProcessReceivedData(readBuffer, readLength);
        readLength = 0;

        readTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;

        if(response != NULL)
        {
            APP_USBSendString(response);
        }
    }
}
