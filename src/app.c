#include "app.h"
#include "definitions.h"
#include "../quest.X/lcd_i2c.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define APP_READ_BUFFER_SIZE 64U
#define APP_WRITE_BUFFER_SIZE 64U
#define APP_LCD_COLUMNS 16U
#define APP_COMMAND_BUFFER_SIZE 768U
#define APP_TX_QUEUE_COUNT 10U
#define APP_TX_QUEUE_MESSAGE_SIZE 64U
#define APP_PLAYER_COUNT 2U
#define APP_OPTION_COUNT 4U
#define APP_QUESTION_TEXT_SIZE 241U
#define APP_OPTION_TEXT_SIZE 81U
#define APP_SCROLL_WINDOW_CHARS (APP_LCD_COLUMNS - 2U)
#define APP_SCROLL_GAP 4U
#define APP_DISPLAY_SCROLL_COUNTS (CPU_CLOCK_FREQUENCY / 2U)
#define APP_LED_TASK_COUNTS (CPU_CLOCK_FREQUENCY / 8U)
#define APP_LED_SLOW_DIVIDER 4U

#define APP_PLAYER1_LCD_ADDRESS 0x26U
#define APP_PLAYER2_LCD_ADDRESS 0x23U

/*
 * Default button wiring:
 *   P1 next/select: chip pins 6/7  (RB2/RB3)
 *   P2 next/select: chip pins 11/14 (RB4/RB5)
 *
 * Default LED wiring:
 *   P1 next/select LED cathodes: chip pins 16/24 (RB7/RB13)
 *   P2 next/select LED cathodes: chip pins 25/26 (RB14/RB15)
 *
 * Wire each button between the pin and GND. The firmware enables weak pull-ups,
 * so a pressed button reads low. Wire each LED anode to 3.3V through a
 * current-limiting resistor; the firmware sinks current to turn LEDs on.
 */
#define APP_BUTTON_P1_NEXT_MASK   (1UL << 2)
#define APP_BUTTON_P1_SELECT_MASK (1UL << 3)
#define APP_BUTTON_P2_NEXT_MASK   (1UL << 4)
#define APP_BUTTON_P2_SELECT_MASK (1UL << 5)
#define APP_BUTTON_MASK (APP_BUTTON_P1_NEXT_MASK | APP_BUTTON_P1_SELECT_MASK | \
                         APP_BUTTON_P2_NEXT_MASK | APP_BUTTON_P2_SELECT_MASK)
#define APP_LED_P1_NEXT_MASK      (1UL << 7)
#define APP_LED_P1_SELECT_MASK    (1UL << 13)
#define APP_LED_P2_NEXT_MASK      (1UL << 14)
#define APP_LED_P2_SELECT_MASK    (1UL << 15)
#define APP_LED_MASK (APP_LED_P1_NEXT_MASK | APP_LED_P1_SELECT_MASK | \
                      APP_LED_P2_NEXT_MASK | APP_LED_P2_SELECT_MASK)
#define APP_BUTTON_POLL_COUNTS (CPU_CLOCK_FREQUENCY / 500U)
#define APP_BUTTON_DEBOUNCE_COUNTS (CPU_CLOCK_FREQUENCY / 50U)

typedef struct
{
    uint32_t mask;
    const char *eventText;
} APP_BUTTON_EVENT;

typedef enum
{
    APP_LED_MODE_OFF,
    APP_LED_MODE_ON,
    APP_LED_MODE_SLOW,
    APP_LED_MODE_FAST
} APP_LED_MODE;

typedef struct
{
    uint32_t mask;
    APP_LED_MODE mode;
} APP_LED_CONTROL;

static USB_DEVICE_HANDLE usbDeviceHandle = USB_DEVICE_HANDLE_INVALID;
static USB_DEVICE_CDC_TRANSFER_HANDLE readTransferHandle;
static USB_DEVICE_CDC_TRANSFER_HANDLE writeTransferHandle;
static USB_DEVICE_CDC_TRANSFER_HANDLE serialStateTransferHandle;
static size_t readLength = 0;

static bool deviceConfigured = false;
static bool readComplete = false;
static bool writeComplete = true;
static bool readyMessagesQueued = false;
static bool lcdAddressesPrepared = false;
static bool serialStateNotificationPending = false;
static bool serialStateNotificationBusy = false;

static uint8_t readBuffer[APP_READ_BUFFER_SIZE] __attribute__((coherent, aligned(16)));
static uint8_t writeBuffer[APP_WRITE_BUFFER_SIZE] __attribute__((coherent, aligned(16)));

static char txQueue[APP_TX_QUEUE_COUNT][APP_TX_QUEUE_MESSAGE_SIZE];
static uint8_t txQueueHead = 0;
static uint8_t txQueueTail = 0;
static uint8_t txQueueCount = 0;

static char pendingCommand[APP_COMMAND_BUFFER_SIZE];
static size_t pendingCommandLength = 0;
static bool pendingCommandTruncated = false;

static const uint8_t playerLcdAddresses[APP_PLAYER_COUNT] =
{
    APP_PLAYER1_LCD_ADDRESS,
    APP_PLAYER2_LCD_ADDRESS
};

static bool playerLcdInitialized[APP_PLAYER_COUNT] =
{
    false,
    false
};

static const APP_BUTTON_EVENT buttonEvents[] =
{
    { APP_BUTTON_P1_NEXT_MASK,   "BTN:P1:NEXT\r\n" },
    { APP_BUTTON_P1_SELECT_MASK, "BTN:P1:SELECT\r\n" },
    { APP_BUTTON_P2_NEXT_MASK,   "BTN:P2:NEXT\r\n" },
    { APP_BUTTON_P2_SELECT_MASK, "BTN:P2:SELECT\r\n" }
};

static APP_LED_CONTROL ledControls[] =
{
    { APP_LED_P1_NEXT_MASK,   APP_LED_MODE_OFF },
    { APP_LED_P1_SELECT_MASK, APP_LED_MODE_OFF },
    { APP_LED_P2_NEXT_MASK,   APP_LED_MODE_OFF },
    { APP_LED_P2_SELECT_MASK, APP_LED_MODE_OFF }
};

static uint32_t buttonLastPollCount = 0;
static uint32_t buttonLastChangeCount = 0;
static uint32_t buttonLastRawPressedMask = 0;
static uint32_t buttonStablePressedMask = 0;
static uint32_t ledLastTaskCount = 0;
static uint8_t ledSlowDivider = 0;
static bool ledFastPhase = false;
static bool ledSlowPhase = false;

static char displayQuestion[APP_QUESTION_TEXT_SIZE];
static char displayOptions[APP_OPTION_COUNT][APP_OPTION_TEXT_SIZE];
static bool playerQuestionDisplayActive[APP_PLAYER_COUNT] =
{
    false,
    false
};
static uint8_t playerQuestionAnswerIndex[APP_PLAYER_COUNT] =
{
    0,
    0
};
static uint16_t displayQuestionScrollTick = 0;
static uint16_t playerOptionScrollTick[APP_PLAYER_COUNT] =
{
    0,
    0
};
static uint8_t displayNextScrollPlayer = 0;
static uint32_t displayLastScrollCount = 0;

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

static void APP_USBDeviceEventHandler(USB_DEVICE_EVENT event, void *eventData, uintptr_t context);

static void APP_USBResetTxQueue(void)
{
    txQueueHead = 0;
    txQueueTail = 0;
    txQueueCount = 0;
}

static bool APP_USBQueueString(const char *message)
{
    size_t length;

    if(txQueueCount >= APP_TX_QUEUE_COUNT)
    {
        return false;
    }

    length = strlen(message);
    if(length >= APP_TX_QUEUE_MESSAGE_SIZE)
    {
        length = APP_TX_QUEUE_MESSAGE_SIZE - 1U;
    }

    memcpy(txQueue[txQueueTail], message, length);
    txQueue[txQueueTail][length] = '\0';

    txQueueTail = (uint8_t)((txQueueTail + 1U) % APP_TX_QUEUE_COUNT);
    txQueueCount++;

    return true;
}

static void APP_USBServiceWrites(void)
{
    USB_DEVICE_CDC_RESULT writeResult;
    size_t length;

    if(!deviceConfigured || !writeComplete || (txQueueCount == 0U))
    {
        return;
    }

    length = strlen(txQueue[txQueueHead]);
    if(length == 0U)
    {
        txQueueHead = (uint8_t)((txQueueHead + 1U) % APP_TX_QUEUE_COUNT);
        txQueueCount--;
        return;
    }

    if(length > APP_WRITE_BUFFER_SIZE)
    {
        length = APP_WRITE_BUFFER_SIZE;
    }

    memcpy(writeBuffer, txQueue[txQueueHead], length);
    writeComplete = false;

    writeResult = USB_DEVICE_CDC_Write(
        USB_DEVICE_CDC_INDEX_0,
        &writeTransferHandle,
        writeBuffer,
        length,
        USB_DEVICE_CDC_TRANSFER_FLAGS_DATA_COMPLETE
    );

    if(writeResult == USB_DEVICE_CDC_RESULT_OK)
    {
        txQueueHead = (uint8_t)((txQueueHead + 1U) % APP_TX_QUEUE_COUNT);
        txQueueCount--;
    }
    else
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

static bool APP_StringStartsWith(const char *text, const char *prefix)
{
    while(*prefix != '\0')
    {
        if(*text != *prefix)
        {
            return false;
        }

        text++;
        prefix++;
    }

    return true;
}

static void APP_CopyDelimitedDisplayField(const char **cursor, char *dest, size_t destSize)
{
    size_t length = 0U;

    if(destSize == 0U)
    {
        return;
    }

    while((**cursor != '\0') && (**cursor != '|'))
    {
        char c = **cursor;

        if((c < 32) || (c > 126))
        {
            c = '?';
        }

        if((length + 1U) < destSize)
        {
            dest[length] = c;
            length++;
        }

        *cursor += 1U;
    }

    while((length > 0U) && (dest[length - 1U] == ' '))
    {
        length--;
    }

    dest[length] = '\0';

    if(**cursor == '|')
    {
        *cursor += 1U;
    }
}

static void APP_DisplayScrollWindow(const char *source,
                                    const char *fallback,
                                    uint16_t tick,
                                    char *dest,
                                    size_t destSize)
{
    const char *text = source;
    size_t visible;
    size_t length;

    if(destSize == 0U)
    {
        return;
    }

    if((text == NULL) || (text[0] == '\0'))
    {
        text = fallback;
    }

    if(text == NULL)
    {
        text = "";
    }

    visible = destSize - 1U;
    length = strlen(text);

    if(length == 0U)
    {
        memset(dest, ' ', visible);
    }
    else if(length <= visible)
    {
        memcpy(dest, text, length);
        memset(dest + length, ' ', visible - length);
    }
    else
    {
        size_t cycleLength = length + APP_SCROLL_GAP;
        size_t start = ((size_t)tick) % cycleLength;

        for(size_t i = 0U; i < visible; i++)
        {
            size_t index = (start + i) % cycleLength;
            dest[i] = index < length ? text[index] : ' ';
        }
    }

    dest[visible] = '\0';
}

static bool APP_LCDWriteFixedLine(const char *text)
{
    bool padding = false;

    for(uint8_t column = 0; column < APP_LCD_COLUMNS; column++)
    {
        char c = ' ';

        if(!padding)
        {
            c = *text;

            if(c == '\0')
            {
                padding = true;
                c = ' ';
            }
            else
            {
                text++;
            }
        }

        if(!LCD_I2C_WriteChar(c))
        {
            return false;
        }
    }

    return true;
}

static bool APP_LCDWriteLinesToPlayer(uint8_t playerIndex, const char *line1, const char *line2)
{
    uint8_t address;

    if(playerIndex >= APP_PLAYER_COUNT)
    {
        return false;
    }

    address = playerLcdAddresses[playerIndex];

    if(!playerLcdInitialized[playerIndex])
    {
        if(!LCD_I2C_Init(address))
        {
            return false;
        }

        playerLcdInitialized[playerIndex] = true;
    }
    else
    {
        LCD_I2C_Select(address);
    }

    if(!LCD_I2C_SetCursor(0, 0))
    {
        playerLcdInitialized[playerIndex] = false;
        return false;
    }

    if(!APP_LCDWriteFixedLine(line1))
    {
        playerLcdInitialized[playerIndex] = false;
        return false;
    }

    if(!LCD_I2C_SetCursor(1, 0))
    {
        playerLcdInitialized[playerIndex] = false;
        return false;
    }

    if(!APP_LCDWriteFixedLine(line2))
    {
        playerLcdInitialized[playerIndex] = false;
        return false;
    }

    return true;
}

static void APP_CopyLCDLine(const char **cursor, char *line)
{
    size_t length = 0;

    while((**cursor != '\0') && (**cursor != '|'))
    {
        if(length < APP_LCD_COLUMNS)
        {
            line[length] = **cursor;
            length++;
        }

        *cursor += 1U;
    }

    line[length] = '\0';

    if(**cursor == '|')
    {
        *cursor += 1U;
    }
}

static void APP_ParseLCDPayload(const char *payload, char *line1, char *line2)
{
    APP_CopyLCDLine(&payload, line1);
    APP_CopyLCDLine(&payload, line2);
}

static void APP_ParseLegacyPayload(const char *payload, char *line1, char *line2)
{
    size_t i = 0;

    while((payload[i] != '\0') && (i < APP_LCD_COLUMNS))
    {
        line1[i] = payload[i];
        i++;
    }
    line1[i] = '\0';

    payload += i;
    i = 0;

    while((payload[i] != '\0') && (i < APP_LCD_COLUMNS))
    {
        line2[i] = payload[i];
        i++;
    }
    line2[i] = '\0';
}

static void APP_QueuePlayerResult(uint8_t playerIndex, bool success)
{
    if(playerIndex == 0U)
    {
        APP_USBQueueString(success ? "OK:P1\r\n" : "ERR:P1 LCD write failed\r\n");
    }
    else
    {
        APP_USBQueueString(success ? "OK:P2\r\n" : "ERR:P2 LCD write failed\r\n");
    }
}

static void APP_PlayerDisplaySetStatic(uint8_t playerIndex)
{
    if(playerIndex < APP_PLAYER_COUNT)
    {
        playerQuestionDisplayActive[playerIndex] = false;
    }
}

static void APP_AllDisplaysSetStatic(void)
{
    for(uint8_t playerIndex = 0U; playerIndex < APP_PLAYER_COUNT; playerIndex++)
    {
        APP_PlayerDisplaySetStatic(playerIndex);
    }
}

static void APP_WritePlayerCommand(uint8_t playerIndex, const char *payload)
{
    char line1[APP_LCD_COLUMNS + 1U];
    char line2[APP_LCD_COLUMNS + 1U];
    bool success;

    APP_PlayerDisplaySetStatic(playerIndex);
    APP_ParseLCDPayload(payload, line1, line2);
    success = APP_LCDWriteLinesToPlayer(playerIndex, line1, line2);
    APP_QueuePlayerResult(playerIndex, success);
}

static void APP_WriteBothCommand(const char *payload)
{
    char line1[APP_LCD_COLUMNS + 1U];
    char line2[APP_LCD_COLUMNS + 1U];
    bool p1Success;
    bool p2Success;

    APP_AllDisplaysSetStatic();
    APP_ParseLCDPayload(payload, line1, line2);
    p1Success = APP_LCDWriteLinesToPlayer(0U, line1, line2);
    p2Success = APP_LCDWriteLinesToPlayer(1U, line1, line2);

    APP_USBQueueString((p1Success && p2Success) ? "OK:BOTH\r\n" : "ERR:BOTH LCD write failed\r\n");
}

static void APP_WriteLegacyText(const char *payload)
{
    char line1[APP_LCD_COLUMNS + 1U];
    char line2[APP_LCD_COLUMNS + 1U];
    bool p1Success;
    bool p2Success;

    APP_AllDisplaysSetStatic();
    APP_ParseLegacyPayload(payload, line1, line2);
    p1Success = APP_LCDWriteLinesToPlayer(0U, line1, line2);
    p2Success = APP_LCDWriteLinesToPlayer(1U, line1, line2);

    APP_USBQueueString((p1Success && p2Success) ? "OK:BOTH legacy text\r\n" : "ERR:BOTH LCD write failed\r\n");
}

static bool APP_RenderQuestionPlayer(uint8_t playerIndex)
{
    static const char answerLetters[APP_OPTION_COUNT] =
    {
        'A',
        'B',
        'C',
        'D'
    };
    char questionWindow[APP_SCROLL_WINDOW_CHARS + 1U];
    char optionWindow[APP_SCROLL_WINDOW_CHARS + 1U];
    char line1[APP_LCD_COLUMNS + 1U];
    char line2[APP_LCD_COLUMNS + 1U];
    uint8_t answerIndex;

    if(playerIndex >= APP_PLAYER_COUNT)
    {
        return false;
    }

    answerIndex = playerQuestionAnswerIndex[playerIndex];
    if(answerIndex >= APP_OPTION_COUNT)
    {
        answerIndex = 0U;
    }

    APP_DisplayScrollWindow(displayQuestion,
                            "Question",
                            displayQuestionScrollTick,
                            questionWindow,
                            sizeof(questionWindow));
    APP_DisplayScrollWindow(displayOptions[answerIndex],
                            "No answer text",
                            playerOptionScrollTick[playerIndex],
                            optionWindow,
                            sizeof(optionWindow));

    line1[0] = 'Q';
    line1[1] = ':';
    memcpy(&line1[2], questionWindow, APP_SCROLL_WINDOW_CHARS);
    line1[APP_LCD_COLUMNS] = '\0';

    line2[0] = answerLetters[answerIndex];
    line2[1] = '>';
    memcpy(&line2[2], optionWindow, APP_SCROLL_WINDOW_CHARS);
    line2[APP_LCD_COLUMNS] = '\0';

    return APP_LCDWriteLinesToPlayer(playerIndex, line1, line2);
}

static void APP_ProcessQuestionSetCommand(const char *payload)
{
    const char *cursor = payload;
    bool p1Success;
    bool p2Success;

    APP_CopyDelimitedDisplayField(&cursor, displayQuestion, sizeof(displayQuestion));
    for(uint8_t option = 0U; option < APP_OPTION_COUNT; option++)
    {
        APP_CopyDelimitedDisplayField(&cursor, displayOptions[option], sizeof(displayOptions[option]));
    }

    displayQuestionScrollTick = 0U;
    displayNextScrollPlayer = 0U;
    displayLastScrollCount = _CP0_GET_COUNT();

    for(uint8_t playerIndex = 0U; playerIndex < APP_PLAYER_COUNT; playerIndex++)
    {
        playerQuestionDisplayActive[playerIndex] = true;
        playerQuestionAnswerIndex[playerIndex] = 0U;
        playerOptionScrollTick[playerIndex] = 0U;
    }

    p1Success = APP_RenderQuestionPlayer(0U);
    p2Success = APP_RenderQuestionPlayer(1U);
    APP_USBQueueString((p1Success && p2Success) ? "OK:QSET\r\n" : "ERR:QSET LCD write failed\r\n");
}

static bool APP_ParseQuestionSelection(const char *command, uint8_t *playerIndex, uint8_t *answerIndex)
{
    const char *value;
    char answer;

    if(APP_StringStartsWith(command, "QSEL:P1:"))
    {
        *playerIndex = 0U;
        value = command + 8U;
    }
    else if(APP_StringStartsWith(command, "QSEL:P2:"))
    {
        *playerIndex = 1U;
        value = command + 8U;
    }
    else
    {
        return false;
    }

    answer = *value;
    if((answer >= 'A') && (answer <= 'D'))
    {
        *answerIndex = (uint8_t)(answer - 'A');
        return true;
    }

    if((answer >= 'a') && (answer <= 'd'))
    {
        *answerIndex = (uint8_t)(answer - 'a');
        return true;
    }

    if((answer >= '0') && (answer <= '3'))
    {
        *answerIndex = (uint8_t)(answer - '0');
        return true;
    }

    return false;
}

static void APP_ProcessQuestionSelectCommand(const char *command)
{
    uint8_t playerIndex;
    uint8_t answerIndex;
    bool success = true;

    if(!APP_ParseQuestionSelection(command, &playerIndex, &answerIndex))
    {
        APP_USBQueueString("ERR QSEL command\r\n");
        return;
    }

    playerQuestionAnswerIndex[playerIndex] = answerIndex;
    playerOptionScrollTick[playerIndex] = 0U;

    if(playerQuestionDisplayActive[playerIndex])
    {
        success = APP_RenderQuestionPlayer(playerIndex);
    }

    APP_USBQueueString(success ? "OK:QSEL\r\n" : "ERR:QSEL LCD write failed\r\n");
}

static bool APP_AnyQuestionDisplayActive(void)
{
    for(uint8_t playerIndex = 0U; playerIndex < APP_PLAYER_COUNT; playerIndex++)
    {
        if(playerQuestionDisplayActive[playerIndex])
        {
            return true;
        }
    }

    return false;
}

static void APP_QuestionDisplayTask(void)
{
    uint32_t currentCount;

    if(!APP_AnyQuestionDisplayActive())
    {
        return;
    }

    currentCount = _CP0_GET_COUNT();
    if((currentCount - displayLastScrollCount) < APP_DISPLAY_SCROLL_COUNTS)
    {
        return;
    }
    displayLastScrollCount = currentCount;

    for(uint8_t attempt = 0U; attempt < APP_PLAYER_COUNT; attempt++)
    {
        uint8_t playerIndex = displayNextScrollPlayer;
        displayNextScrollPlayer = (uint8_t)((displayNextScrollPlayer + 1U) % APP_PLAYER_COUNT);

        if(playerQuestionDisplayActive[playerIndex])
        {
            displayQuestionScrollTick++;
            playerOptionScrollTick[playerIndex]++;
            (void)APP_RenderQuestionPlayer(playerIndex);
            return;
        }
    }
}

static void APP_LEDSet(uint32_t mask, bool on)
{
    if(on)
    {
        LATBCLR = mask;
    }
    else
    {
        LATBSET = mask;
    }
}

static void APP_LEDApplyModes(void)
{
    uint32_t onMask = 0U;

    for(size_t i = 0U; i < (sizeof(ledControls) / sizeof(ledControls[0])); i++)
    {
        bool on = false;

        switch(ledControls[i].mode)
        {
            case APP_LED_MODE_ON:
                on = true;
                break;

            case APP_LED_MODE_SLOW:
                on = ledSlowPhase;
                break;

            case APP_LED_MODE_FAST:
                on = ledFastPhase;
                break;

            case APP_LED_MODE_OFF:
            default:
                on = false;
                break;
        }

        if(on)
        {
            onMask |= ledControls[i].mask;
        }
    }

    APP_LEDSet(APP_LED_MASK & ~onMask, false);
    APP_LEDSet(onMask, true);
}

static void APP_LEDSetMode(uint32_t mask, APP_LED_MODE mode)
{
    for(size_t i = 0U; i < (sizeof(ledControls) / sizeof(ledControls[0])); i++)
    {
        if((ledControls[i].mask & mask) != 0U)
        {
            ledControls[i].mode = mode;
        }
    }

    APP_LEDApplyModes();
}

static void APP_LEDsInitialize(void)
{
    ANSELBCLR = APP_LED_MASK;
    CNPUBCLR = APP_LED_MASK;
    CNPDBCLR = APP_LED_MASK;
    LATBSET = APP_LED_MASK;
    TRISBCLR = APP_LED_MASK;

    for(size_t i = 0U; i < (sizeof(ledControls) / sizeof(ledControls[0])); i++)
    {
        ledControls[i].mode = APP_LED_MODE_OFF;
    }

    ledLastTaskCount = _CP0_GET_COUNT();
    ledSlowDivider = 0U;
    ledFastPhase = false;
    ledSlowPhase = false;
    APP_LEDApplyModes();
}

static bool APP_ParseLEDMode(const char *text, APP_LED_MODE *mode)
{
    if(strcmp(text, "ON") == 0)
    {
        *mode = APP_LED_MODE_ON;
        return true;
    }

    if(strcmp(text, "OFF") == 0)
    {
        *mode = APP_LED_MODE_OFF;
        return true;
    }

    if(strcmp(text, "SLOW") == 0)
    {
        *mode = APP_LED_MODE_SLOW;
        return true;
    }

    if((strcmp(text, "FAST") == 0) || (strcmp(text, "BLINK") == 0))
    {
        *mode = APP_LED_MODE_FAST;
        return true;
    }

    return false;
}

static bool APP_ParseLEDTarget(const char *command, uint32_t *mask, const char **modeText)
{
    static const char ledAllPrefix[] = "LED:ALL:";
    static const char p1NextPrefix[] = "LED:P1:NEXT:";
    static const char p1SelectPrefix[] = "LED:P1:SELECT:";
    static const char p2NextPrefix[] = "LED:P2:NEXT:";
    static const char p2SelectPrefix[] = "LED:P2:SELECT:";

    if(APP_StringStartsWith(command, ledAllPrefix))
    {
        *mask = APP_LED_MASK;
        *modeText = command + (sizeof(ledAllPrefix) - 1U);
        return true;
    }

    if(APP_StringStartsWith(command, p1NextPrefix))
    {
        *mask = APP_LED_P1_NEXT_MASK;
        *modeText = command + (sizeof(p1NextPrefix) - 1U);
        return true;
    }

    if(APP_StringStartsWith(command, p1SelectPrefix))
    {
        *mask = APP_LED_P1_SELECT_MASK;
        *modeText = command + (sizeof(p1SelectPrefix) - 1U);
        return true;
    }

    if(APP_StringStartsWith(command, p2NextPrefix))
    {
        *mask = APP_LED_P2_NEXT_MASK;
        *modeText = command + (sizeof(p2NextPrefix) - 1U);
        return true;
    }

    if(APP_StringStartsWith(command, p2SelectPrefix))
    {
        *mask = APP_LED_P2_SELECT_MASK;
        *modeText = command + (sizeof(p2SelectPrefix) - 1U);
        return true;
    }

    return false;
}

static bool APP_ProcessLEDCommand(const char *command)
{
    uint32_t mask = 0;
    const char *modeText;
    APP_LED_MODE mode;

    if(!APP_ParseLEDTarget(command, &mask, &modeText))
    {
        return false;
    }

    if(!APP_ParseLEDMode(modeText, &mode))
    {
        return false;
    }

    APP_LEDSetMode(mask, mode);
    return true;
}

static void APP_LEDTask(void)
{
    uint32_t currentCount = _CP0_GET_COUNT();

    if((currentCount - ledLastTaskCount) < APP_LED_TASK_COUNTS)
    {
        return;
    }

    ledLastTaskCount = currentCount;
    ledFastPhase = !ledFastPhase;
    ledSlowDivider++;

    if(ledSlowDivider >= APP_LED_SLOW_DIVIDER)
    {
        ledSlowDivider = 0U;
        ledSlowPhase = !ledSlowPhase;
    }

    APP_LEDApplyModes();
}

static void APP_CommandReset(void)
{
    pendingCommand[0] = '\0';
    pendingCommandLength = 0;
    pendingCommandTruncated = false;
}

static void APP_CommandAppend(char c)
{
    if(pendingCommandLength < (APP_COMMAND_BUFFER_SIZE - 1U))
    {
        pendingCommand[pendingCommandLength] = c;
        pendingCommandLength++;
    }
    else
    {
        pendingCommandTruncated = true;
    }
}

static void APP_CommandCommit(void)
{
    bool wasTruncated = pendingCommandTruncated;

    pendingCommand[pendingCommandLength] = '\0';

    if((pendingCommandLength == 0U) && !wasTruncated)
    {
        APP_CommandReset();
        return;
    }

    if(wasTruncated)
    {
        APP_USBQueueString("ERR command too long\r\n");
        APP_CommandReset();
        return;
    }

    if(!lcdAddressesPrepared)
    {
        APP_USBQueueString("ERR LCD addresses not ready\r\n");
        APP_CommandReset();
        return;
    }

    if(strcmp(pendingCommand, "PING") == 0)
    {
        APP_USBQueueString("PONG\r\n");
    }
    else if(APP_StringStartsWith(pendingCommand, "LED:"))
    {
        if(!APP_ProcessLEDCommand(pendingCommand))
        {
            APP_USBQueueString("ERR LED command\r\n");
        }
    }
    else if(APP_StringStartsWith(pendingCommand, "QSET:"))
    {
        APP_ProcessQuestionSetCommand(pendingCommand + 5U);
    }
    else if(APP_StringStartsWith(pendingCommand, "QSEL:"))
    {
        APP_ProcessQuestionSelectCommand(pendingCommand);
    }
    else if(strcmp(pendingCommand, "CLR") == 0)
    {
        APP_WriteBothCommand("|");
    }
    else if(APP_StringStartsWith(pendingCommand, "P1:"))
    {
        APP_WritePlayerCommand(0U, pendingCommand + 3U);
    }
    else if(APP_StringStartsWith(pendingCommand, "P2:"))
    {
        APP_WritePlayerCommand(1U, pendingCommand + 3U);
    }
    else if(APP_StringStartsWith(pendingCommand, "BOTH:"))
    {
        APP_WriteBothCommand(pendingCommand + 5U);
    }
    else
    {
        APP_WriteLegacyText(pendingCommand);
    }

    APP_CommandReset();
}

static void APP_ProcessReceivedData(const uint8_t *data, size_t length)
{
    for(size_t i = 0; i < length; i++)
    {
        uint8_t value = data[i];

        if(value == '\r')
        {
            continue;
        }

        if((value == '\n') || (value == '\0'))
        {
            APP_CommandCommit();
            continue;
        }

        if(value == '\t')
        {
            APP_CommandAppend(' ');
        }
        else if((value >= 32U) && (value <= 126U))
        {
            APP_CommandAppend((char)value);
        }
        else
        {
            APP_CommandAppend('?');
        }
    }
}

static uint32_t APP_ButtonReadPressedMask(void)
{
    return (~PORTB) & APP_BUTTON_MASK;
}

static void APP_ButtonsInitialize(void)
{
    ANSELBCLR = APP_BUTTON_MASK;
    CNPDBCLR = APP_BUTTON_MASK;
    TRISBSET = APP_BUTTON_MASK;
    CNPUBSET = APP_BUTTON_MASK;

    buttonLastRawPressedMask = APP_ButtonReadPressedMask();
    buttonStablePressedMask = buttonLastRawPressedMask;
    buttonLastChangeCount = _CP0_GET_COUNT();
    buttonLastPollCount = buttonLastChangeCount;
}

static void APP_ButtonsTask(void)
{
    uint32_t currentCount = _CP0_GET_COUNT();
    uint32_t rawPressedMask;
    uint32_t newPresses;

    if((currentCount - buttonLastPollCount) < APP_BUTTON_POLL_COUNTS)
    {
        return;
    }
    buttonLastPollCount = currentCount;

    rawPressedMask = APP_ButtonReadPressedMask();

    if(rawPressedMask != buttonLastRawPressedMask)
    {
        buttonLastRawPressedMask = rawPressedMask;
        buttonLastChangeCount = currentCount;
        return;
    }

    if((currentCount - buttonLastChangeCount) < APP_BUTTON_DEBOUNCE_COUNTS)
    {
        return;
    }

    if(rawPressedMask == buttonStablePressedMask)
    {
        return;
    }

    newPresses = rawPressedMask & ~buttonStablePressedMask;
    buttonStablePressedMask = rawPressedMask;

    for(size_t i = 0; i < (sizeof(buttonEvents) / sizeof(buttonEvents[0])); i++)
    {
        if((newPresses & buttonEvents[i].mask) != 0U)
        {
            APP_USBQueueString(buttonEvents[i].eventText);
        }
    }
}

static void APP_LCDServiceCallback(void)
{
    APP_ButtonsTask();
    APP_LEDTask();
}

static void APP_LCDPrepareConfiguredAddresses(void)
{
    if(lcdAddressesPrepared)
    {
        return;
    }

    lcdAddressesPrepared = true;
    playerLcdInitialized[0] = false;
    playerLcdInitialized[1] = false;
}

static void APP_QueueReadyMessages(void)
{
    APP_USBQueueString("PIC32 Quest CDC READY\r\n");
    APP_USBQueueString("LCD P1=0x26 P2=0x23\r\n");
    APP_USBQueueString("Buttons pins 6/7/11/14 active-low\r\n");
    APP_USBQueueString("LEDs pins 16/24/25/26 active-low\r\n");
    APP_USBQueueString("Protocol P1/P2/BOTH/LED/QSET/QSEL + BTN events\r\n");
}

static void APP_USBDeviceOpenAndAttach(void)
{
    if(usbDeviceHandle != USB_DEVICE_HANDLE_INVALID)
    {
        return;
    }

    usbDeviceHandle = USB_DEVICE_Open(USB_DEVICE_INDEX_0, DRV_IO_INTENT_READWRITE);

    if(usbDeviceHandle != USB_DEVICE_HANDLE_INVALID)
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
    void *pData,
    uintptr_t userData
)
{
    (void)index;
    (void)userData;

    switch(event)
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
    void *eventData,
    uintptr_t context
)
{
    (void)eventData;
    (void)context;

    switch(event)
    {
        case USB_DEVICE_EVENT_CONFIGURED:
            deviceConfigured = true;
            readyMessagesQueued = false;
            APP_USBResetTxQueue();
            APP_CommandReset();
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
            readyMessagesQueued = false;
            readComplete = false;
            writeComplete = true;
            readLength = 0;
            readTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
            writeTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
            serialStateTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
            serialStateNotificationPending = false;
            serialStateNotificationBusy = false;
            APP_USBResetTxQueue();
            APP_CommandReset();
            break;

        default:
            break;
    }
}

void APP_Initialize(void)
{
    APP_LEDsInitialize();
    APP_ButtonsInitialize();
    LCD_I2C_SetServiceCallback(APP_LCDServiceCallback);

    readTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
    writeTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
    serialStateTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
    APP_CommandReset();
    APP_USBResetTxQueue();
}

void APP_Tasks(void)
{
    APP_USBDeviceOpenAndAttach();
    APP_LCDPrepareConfiguredAddresses();
    APP_USBSendSerialStateNotification();

    if(!deviceConfigured)
    {
        return;
    }

    if(!readyMessagesQueued && lcdAddressesPrepared)
    {
        readyMessagesQueued = true;
        APP_QueueReadyMessages();
        (void)APP_LCDWriteLinesToPlayer(0U, "P1 READY", "Connect app");
        (void)APP_LCDWriteLinesToPlayer(1U, "P2 READY", "Connect app");
    }

    APP_ButtonsTask();
    APP_LEDTask();
    APP_QuestionDisplayTask();
    APP_USBServiceWrites();

    if(readTransferHandle == USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID)
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

    if(readComplete)
    {
        readComplete = false;
        APP_ProcessReceivedData(readBuffer, readLength);
        readLength = 0;
        readTransferHandle = USB_DEVICE_CDC_TRANSFER_HANDLE_INVALID;
    }

    APP_USBServiceWrites();
}
