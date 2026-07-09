#include "pic32_usb_cdc.h"

#include <string.h>
#include <sys/kmem.h>
#include <xc.h>

#ifndef U1CON
#error "pic32_usb_cdc.c requires a PIC32MX/PIC32MZ device with USBFS U1 registers."
#endif

#ifndef PIC32_USB_CDC_EP0_SIZE
#define PIC32_USB_CDC_EP0_SIZE 8U
#endif

#ifndef PIC32_USB_CDC_BULK_SIZE
#define PIC32_USB_CDC_BULK_SIZE 64U
#endif

#ifndef PIC32_USB_CDC_INT_SIZE
#define PIC32_USB_CDC_INT_SIZE 16U
#endif

#ifndef PIC32_USB_CDC_RX_BUFFER_SIZE
#define PIC32_USB_CDC_RX_BUFFER_SIZE 256U
#endif

#ifndef PIC32_USB_CDC_TX_BUFFER_SIZE
#define PIC32_USB_CDC_TX_BUFFER_SIZE 256U
#endif

#ifndef PIC32_USB_CDC_DEFAULT_VID
#define PIC32_USB_CDC_DEFAULT_VID 0x04D8U
#endif

#ifndef PIC32_USB_CDC_DEFAULT_PID
#define PIC32_USB_CDC_DEFAULT_PID 0x0001U
#endif

#ifndef PIC32_USB_CDC_DEFAULT_BCD_DEVICE
#define PIC32_USB_CDC_DEFAULT_BCD_DEVICE 0x0100U
#endif

#ifndef PIC32_USB_CDC_MAX_STRING_DESCRIPTOR
#define PIC32_USB_CDC_MAX_STRING_DESCRIPTOR 64U
#endif

#define USB_EP0 0U
#define USB_EP_CDC_NOTIFICATION 1U
#define USB_EP_CDC_OUT 2U
#define USB_EP_CDC_IN 3U
#define USB_ENDPOINTS 4U

#define USB_DIR_RX 0U
#define USB_DIR_TX 1U
#define USB_PP_EVEN 0U
#define USB_PP_ODD 1U

#define USB_BD_STALL 0x04U
#define USB_BD_DTS 0x08U
#define USB_BD_DATA1 0x40U
#define USB_BD_UOWN 0x80U
#define USB_BD_PID_MASK 0x3CU
#define USB_PID_OUT 0x04U
#define USB_PID_IN 0x24U
#define USB_PID_SETUP 0x34U

#define USB_REQ_GET_STATUS 0x00U
#define USB_REQ_CLEAR_FEATURE 0x01U
#define USB_REQ_SET_FEATURE 0x03U
#define USB_REQ_SET_ADDRESS 0x05U
#define USB_REQ_GET_DESCRIPTOR 0x06U
#define USB_REQ_GET_CONFIGURATION 0x08U
#define USB_REQ_SET_CONFIGURATION 0x09U
#define USB_REQ_GET_INTERFACE 0x0AU
#define USB_REQ_SET_INTERFACE 0x0BU

#define USB_DESC_DEVICE 0x01U
#define USB_DESC_CONFIGURATION 0x02U
#define USB_DESC_STRING 0x03U
#define USB_DESC_INTERFACE 0x04U
#define USB_DESC_ENDPOINT 0x05U
#define USB_DESC_CS_INTERFACE 0x24U

#define USB_CDC_SET_LINE_CODING 0x20U
#define USB_CDC_GET_LINE_CODING 0x21U
#define USB_CDC_SET_CONTROL_LINE_STATE 0x22U
#define USB_CDC_SEND_BREAK 0x23U

#define USB_RECIP_DEVICE 0x00U
#define USB_RECIP_INTERFACE 0x01U
#define USB_RECIP_ENDPOINT 0x02U
#define USB_RECIP_MASK 0x1FU

#define USB_FEATURE_ENDPOINT_HALT 0x00U

#define USB_EP_ATTR_CONTROL 0x0DU
#define USB_EP_ATTR_TX_HANDSHAKE 0x05U
#define USB_EP_ATTR_RX_HANDSHAKE 0x09U

#define U16_LO(v) ((uint8_t)((uint16_t)(v) & 0xFFU))
#define U16_HI(v) ((uint8_t)(((uint16_t)(v) >> 8) & 0xFFU))

typedef union __attribute__((packed, aligned(4)))
{
    uint8_t byte[8];
    uint16_t half[4];
    uint32_t word[2];
} Pic32UsbBdtEntry;

typedef struct
{
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} UsbSetupPacket;

typedef enum
{
    CTRL_IDLE = 0,
    CTRL_IN_DATA,
    CTRL_OUT_DATA,
    CTRL_STATUS_IN,
    CTRL_STATUS_OUT
} ControlState;

typedef struct
{
    ControlState state;
    UsbSetupPacket setup;
    const uint8_t *source;
    uint16_t total;
    uint16_t sent;
    uint16_t received;
    bool need_zlp;
    uint8_t pending_request;
} ControlTransfer;

static volatile Pic32UsbBdtEntry usb_bdt[USB_ENDPOINTS * 4U] __attribute__((coherent, aligned(512)));
static uint8_t ep0_rx_buffer[2][PIC32_USB_CDC_EP0_SIZE] __attribute__((coherent, aligned(16)));
static uint8_t ep0_tx_buffer[PIC32_USB_CDC_EP0_SIZE] __attribute__((coherent, aligned(16)));
static uint8_t ep0_out_buffer[16] __attribute__((coherent, aligned(16)));
static uint8_t bulk_rx_buffer[2][PIC32_USB_CDC_BULK_SIZE] __attribute__((coherent, aligned(16)));
static uint8_t bulk_tx_buffer[PIC32_USB_CDC_BULK_SIZE] __attribute__((coherent, aligned(16)));
static uint8_t notify_buffer[PIC32_USB_CDC_INT_SIZE] __attribute__((coherent, aligned(16)));

static uint8_t rx_ring[PIC32_USB_CDC_RX_BUFFER_SIZE];
static uint8_t tx_ring[PIC32_USB_CDC_TX_BUFFER_SIZE];
static uint8_t device_descriptor[18];
static uint8_t string_descriptor[PIC32_USB_CDC_MAX_STRING_DESCRIPTOR];

static const uint8_t language_string_descriptor[] = {4U, USB_DESC_STRING, 0x09U, 0x04U};

static const uint8_t configuration_descriptor[] =
{
    0x09U, USB_DESC_CONFIGURATION, 0x43U, 0x00U, 0x02U, 0x01U, 0x00U, 0x80U, 50U,
    0x09U, USB_DESC_INTERFACE, 0x00U, 0x00U, 0x01U, 0x02U, 0x02U, 0x01U, 0x00U,
    0x05U, USB_DESC_CS_INTERFACE, 0x00U, 0x20U, 0x01U,
    0x04U, USB_DESC_CS_INTERFACE, 0x02U, 0x02U,
    0x05U, USB_DESC_CS_INTERFACE, 0x06U, 0x00U, 0x01U,
    0x05U, USB_DESC_CS_INTERFACE, 0x01U, 0x00U, 0x01U,
    0x07U, USB_DESC_ENDPOINT, (uint8_t)(0x80U | USB_EP_CDC_NOTIFICATION), 0x03U,
    U16_LO(PIC32_USB_CDC_INT_SIZE), U16_HI(PIC32_USB_CDC_INT_SIZE), 0x02U,
    0x09U, USB_DESC_INTERFACE, 0x01U, 0x00U, 0x02U, 0x0AU, 0x00U, 0x00U, 0x00U,
    0x07U, USB_DESC_ENDPOINT, USB_EP_CDC_OUT, 0x02U,
    U16_LO(PIC32_USB_CDC_BULK_SIZE), U16_HI(PIC32_USB_CDC_BULK_SIZE), 0x00U,
    0x07U, USB_DESC_ENDPOINT, (uint8_t)(0x80U | USB_EP_CDC_IN), 0x02U,
    U16_LO(PIC32_USB_CDC_BULK_SIZE), U16_HI(PIC32_USB_CDC_BULK_SIZE), 0x00U
};

static PIC32_USB_CDC_Config usb_config;
static PIC32_USB_CDC_State usb_state = PIC32_USB_CDC_STATE_DETACHED;
static PIC32_USB_CDC_State state_before_suspend = PIC32_USB_CDC_STATE_DETACHED;
static PIC32_USB_CDC_LineCoding line_coding = {115200UL, 0U, 0U, 8U};
static ControlTransfer ctrl;

static uint16_t rx_head;
static uint16_t rx_tail;
static uint16_t tx_head;
static uint16_t tx_tail;
static uint32_t rx_dropped;

static uint8_t next_pp[USB_ENDPOINTS][2];
static uint8_t next_tx_data[USB_ENDPOINTS];
static uint8_t configuration_value;
static uint8_t pending_address = 0xFFU;
static bool usb_suspended;
static bool cdc_dtr;
static bool cdc_rts;
static bool bulk_tx_busy;
static bool notify_tx_busy;

static void usb_log(const char *message)
{
    if (usb_config.debug_log != NULL)
    {
        usb_config.debug_log(message);
    }
}

static void set_state(PIC32_USB_CDC_State state)
{
    if (usb_state != state)
    {
        usb_state = state;
        if (usb_config.on_state_changed != NULL)
        {
            usb_config.on_state_changed(state);
        }
    }
}

static uint16_t min_u16(uint16_t a, uint16_t b)
{
    return (a < b) ? a : b;
}

static uint16_t ring_next(uint16_t value, uint16_t size)
{
    value++;
    return (value >= size) ? 0U : value;
}

static size_t rx_count(void)
{
    if (rx_head >= rx_tail)
    {
        return (size_t)(rx_head - rx_tail);
    }
    return (size_t)(PIC32_USB_CDC_RX_BUFFER_SIZE - rx_tail + rx_head);
}

static size_t tx_count(void)
{
    if (tx_head >= tx_tail)
    {
        return (size_t)(tx_head - tx_tail);
    }
    return (size_t)(PIC32_USB_CDC_TX_BUFFER_SIZE - tx_tail + tx_head);
}

static void rx_push(uint8_t value)
{
    uint16_t next = ring_next(rx_head, PIC32_USB_CDC_RX_BUFFER_SIZE);

    if (next == rx_tail)
    {
        rx_dropped++;
        return;
    }

    rx_ring[rx_head] = value;
    rx_head = next;
}

static bool tx_pop(uint8_t *value)
{
    if (tx_tail == tx_head)
    {
        return false;
    }

    *value = tx_ring[tx_tail];
    tx_tail = ring_next(tx_tail, PIC32_USB_CDC_TX_BUFFER_SIZE);
    return true;
}

static uint8_t bdt_index(uint8_t endpoint, uint8_t direction, uint8_t ping_pong)
{
    return (uint8_t)((endpoint * 4U) + (direction * 2U) + ping_pong);
}

static void bdt_clear_all(void)
{
    uint8_t i;

    for (i = 0U; i < (USB_ENDPOINTS * 4U); i++)
    {
        usb_bdt[i].word[0] = 0U;
        usb_bdt[i].word[1] = 0U;
    }
}

static void bdt_arm_rx(uint8_t endpoint, uint8_t ping_pong, void *buffer, uint16_t size)
{
    volatile Pic32UsbBdtEntry *bd = &usb_bdt[bdt_index(endpoint, USB_DIR_RX, ping_pong)];

    bd->byte[0] = 0U;
    bd->word[1] = KVA_TO_PA(buffer);
    bd->half[1] = size;
    bd->byte[0] = USB_BD_UOWN;
}

static void bdt_arm_rx_next(uint8_t endpoint, void *buffer, uint16_t size)
{
    uint8_t ping_pong = next_pp[endpoint][USB_DIR_RX];

    bdt_arm_rx(endpoint, ping_pong, buffer, size);
    next_pp[endpoint][USB_DIR_RX] ^= 1U;
}

static void bdt_arm_tx_next(uint8_t endpoint, const void *buffer, uint16_t size)
{
    uint8_t ping_pong = next_pp[endpoint][USB_DIR_TX];
    volatile Pic32UsbBdtEntry *bd = &usb_bdt[bdt_index(endpoint, USB_DIR_TX, ping_pong)];
    uint8_t status = USB_BD_DTS;

    if (next_tx_data[endpoint] != 0U)
    {
        status |= USB_BD_DATA1;
    }

    bd->byte[0] = 0U;
    bd->word[1] = KVA_TO_PA(buffer);
    bd->half[1] = size;
    bd->byte[0] = (uint8_t)(status | USB_BD_UOWN);

    next_pp[endpoint][USB_DIR_TX] ^= 1U;
    next_tx_data[endpoint] ^= 1U;
}

static void bdt_stall_ep0(void)
{
    volatile Pic32UsbBdtEntry *tx_even = &usb_bdt[bdt_index(USB_EP0, USB_DIR_TX, USB_PP_EVEN)];
    volatile Pic32UsbBdtEntry *tx_odd = &usb_bdt[bdt_index(USB_EP0, USB_DIR_TX, USB_PP_ODD)];

    tx_even->byte[0] = (uint8_t)(USB_BD_STALL | USB_BD_UOWN);
    tx_odd->byte[0] = (uint8_t)(USB_BD_STALL | USB_BD_UOWN);
    U1EP0SET = _U1EP0_EPSTALL_MASK;
    ctrl.state = CTRL_IDLE;
}

static void endpoint_clear_stall(uint8_t endpoint)
{
    switch (endpoint)
    {
        case USB_EP0:
            U1EP0CLR = _U1EP0_EPSTALL_MASK;
            break;
        case USB_EP_CDC_NOTIFICATION:
            U1EP1CLR = _U1EP1_EPSTALL_MASK;
            break;
        case USB_EP_CDC_OUT:
            U1EP2CLR = _U1EP2_EPSTALL_MASK;
            break;
        case USB_EP_CDC_IN:
            U1EP3CLR = _U1EP3_EPSTALL_MASK;
            break;
        default:
            break;
    }
}

static void endpoint_set_stall(uint8_t endpoint)
{
    switch (endpoint)
    {
        case USB_EP0:
            U1EP0SET = _U1EP0_EPSTALL_MASK;
            break;
        case USB_EP_CDC_NOTIFICATION:
            U1EP1SET = _U1EP1_EPSTALL_MASK;
            break;
        case USB_EP_CDC_OUT:
            U1EP2SET = _U1EP2_EPSTALL_MASK;
            break;
        case USB_EP_CDC_IN:
            U1EP3SET = _U1EP3_EPSTALL_MASK;
            break;
        default:
            break;
    }
}

static bool endpoint_is_stalled(uint8_t endpoint)
{
    switch (endpoint)
    {
        case USB_EP0:
            return (U1EP0bits.EPSTALL != 0U);
        case USB_EP_CDC_NOTIFICATION:
            return (U1EP1bits.EPSTALL != 0U);
        case USB_EP_CDC_OUT:
            return (U1EP2bits.EPSTALL != 0U);
        case USB_EP_CDC_IN:
            return (U1EP3bits.EPSTALL != 0U);
        default:
            return false;
    }
}

static void build_device_descriptor(void)
{
    uint16_t vid = (usb_config.vid == 0U) ? PIC32_USB_CDC_DEFAULT_VID : usb_config.vid;
    uint16_t pid = (usb_config.pid == 0U) ? PIC32_USB_CDC_DEFAULT_PID : usb_config.pid;
    uint16_t bcd = (usb_config.bcd_device == 0U) ? PIC32_USB_CDC_DEFAULT_BCD_DEVICE : usb_config.bcd_device;
    uint8_t serial_index = ((usb_config.serial_number != NULL) && (usb_config.serial_number[0] != '\0')) ? 3U : 0U;

    device_descriptor[0] = 18U;
    device_descriptor[1] = USB_DESC_DEVICE;
    device_descriptor[2] = 0x00U;
    device_descriptor[3] = 0x02U;
    device_descriptor[4] = 0x02U;
    device_descriptor[5] = 0x02U;
    device_descriptor[6] = 0x00U;
    device_descriptor[7] = PIC32_USB_CDC_EP0_SIZE;
    device_descriptor[8] = U16_LO(vid);
    device_descriptor[9] = U16_HI(vid);
    device_descriptor[10] = U16_LO(pid);
    device_descriptor[11] = U16_HI(pid);
    device_descriptor[12] = U16_LO(bcd);
    device_descriptor[13] = U16_HI(bcd);
    device_descriptor[14] = 1U;
    device_descriptor[15] = 2U;
    device_descriptor[16] = serial_index;
    device_descriptor[17] = 1U;
}

static const uint8_t *make_string_descriptor(uint8_t index, uint16_t *length)
{
    const char *text = NULL;
    uint8_t pos = 2U;

    if (index == 0U)
    {
        *length = sizeof(language_string_descriptor);
        return language_string_descriptor;
    }

    if (index == 1U)
    {
        text = usb_config.manufacturer;
    }
    else if (index == 2U)
    {
        text = usb_config.product;
    }
    else if (index == 3U)
    {
        text = usb_config.serial_number;
    }

    if ((text == NULL) || (text[0] == '\0'))
    {
        return NULL;
    }

    while ((text[0] != '\0') && ((uint16_t)(pos + 1U) < PIC32_USB_CDC_MAX_STRING_DESCRIPTOR))
    {
        string_descriptor[pos++] = (uint8_t)*text++;
        string_descriptor[pos++] = 0U;
    }

    string_descriptor[0] = pos;
    string_descriptor[1] = USB_DESC_STRING;
    *length = pos;
    return string_descriptor;
}

static void ep0_send_next(void)
{
    uint16_t remaining;
    uint16_t chunk;

    if (ctrl.sent < ctrl.total)
    {
        remaining = (uint16_t)(ctrl.total - ctrl.sent);
        chunk = min_u16(remaining, PIC32_USB_CDC_EP0_SIZE);
        (void)memcpy(ep0_tx_buffer, &ctrl.source[ctrl.sent], chunk);
        ctrl.sent = (uint16_t)(ctrl.sent + chunk);
        bdt_arm_tx_next(USB_EP0, ep0_tx_buffer, chunk);
        return;
    }

    if (ctrl.need_zlp)
    {
        ctrl.need_zlp = false;
        bdt_arm_tx_next(USB_EP0, ep0_tx_buffer, 0U);
        return;
    }

    ctrl.state = CTRL_STATUS_OUT;
}

static void control_send(const uint8_t *data, uint16_t actual_length, uint16_t requested_length)
{
    ctrl.source = data;
    ctrl.total = min_u16(actual_length, requested_length);
    ctrl.sent = 0U;
    ctrl.received = 0U;
    ctrl.need_zlp = ((actual_length < requested_length) && ((actual_length % PIC32_USB_CDC_EP0_SIZE) == 0U));
    ctrl.state = CTRL_IN_DATA;
    ep0_send_next();
}

static void control_send_status_in(void)
{
    ctrl.state = CTRL_STATUS_IN;
    ctrl.sent = 0U;
    ctrl.total = 0U;
    ctrl.need_zlp = false;
    bdt_arm_tx_next(USB_EP0, ep0_tx_buffer, 0U);
}

static void control_expect_out(uint8_t request, uint16_t expected_length)
{
    ctrl.state = CTRL_OUT_DATA;
    ctrl.pending_request = request;
    ctrl.total = min_u16(expected_length, sizeof(ep0_out_buffer));
    ctrl.received = 0U;
}

static void apply_out_control_data(void)
{
    if ((ctrl.pending_request == USB_CDC_SET_LINE_CODING) && (ctrl.received >= 7U))
    {
        line_coding.baud_rate = (uint32_t)ep0_out_buffer[0]
            | ((uint32_t)ep0_out_buffer[1] << 8)
            | ((uint32_t)ep0_out_buffer[2] << 16)
            | ((uint32_t)ep0_out_buffer[3] << 24);
        line_coding.stop_bits = ep0_out_buffer[4];
        line_coding.parity = ep0_out_buffer[5];
        line_coding.data_bits = ep0_out_buffer[6];

        if (usb_config.on_line_coding_changed != NULL)
        {
            usb_config.on_line_coding_changed(&line_coding);
        }
    }
}

static void configure_data_endpoints(uint8_t config_value)
{
    configuration_value = config_value;
    bulk_tx_busy = false;
    notify_tx_busy = false;
    next_tx_data[USB_EP_CDC_NOTIFICATION] = 0U;
    next_tx_data[USB_EP_CDC_IN] = 0U;
    next_pp[USB_EP_CDC_NOTIFICATION][USB_DIR_TX] = 0U;
    next_pp[USB_EP_CDC_OUT][USB_DIR_RX] = 0U;
    next_pp[USB_EP_CDC_IN][USB_DIR_TX] = 0U;

    if (config_value == 1U)
    {
        U1EP1 = USB_EP_ATTR_TX_HANDSHAKE;
        U1EP2 = USB_EP_ATTR_RX_HANDSHAKE;
        U1EP3 = USB_EP_ATTR_TX_HANDSHAKE;
        bdt_arm_rx_next(USB_EP_CDC_OUT, bulk_rx_buffer[USB_PP_EVEN], PIC32_USB_CDC_BULK_SIZE);
        bdt_arm_rx_next(USB_EP_CDC_OUT, bulk_rx_buffer[USB_PP_ODD], PIC32_USB_CDC_BULK_SIZE);
        (void)memset(notify_buffer, 0, sizeof(notify_buffer));
        set_state(PIC32_USB_CDC_STATE_CONFIGURED);
        usb_log("USB configured");
    }
    else
    {
        U1EP1 = 0U;
        U1EP2 = 0U;
        U1EP3 = 0U;
        set_state((U1ADDRbits.DEVADDR == 0U) ? PIC32_USB_CDC_STATE_DEFAULT : PIC32_USB_CDC_STATE_ADDRESSED);
        usb_log("USB unconfigured");
    }
}

static void handle_get_status(void)
{
    uint8_t recipient = (uint8_t)(ctrl.setup.bmRequestType & USB_RECIP_MASK);

    ep0_tx_buffer[0] = 0U;
    ep0_tx_buffer[1] = 0U;

    if (recipient == USB_RECIP_ENDPOINT)
    {
        uint8_t endpoint = (uint8_t)(ctrl.setup.wIndex & 0x0FU);
        if (endpoint_is_stalled(endpoint))
        {
            ep0_tx_buffer[0] = 1U;
        }
    }

    control_send(ep0_tx_buffer, 2U, ctrl.setup.wLength);
}

static void handle_get_descriptor(void)
{
    uint8_t descriptor_type = (uint8_t)(ctrl.setup.wValue >> 8);
    uint8_t descriptor_index = (uint8_t)(ctrl.setup.wValue & 0xFFU);
    const uint8_t *descriptor = NULL;
    uint16_t length = 0U;

    switch (descriptor_type)
    {
        case USB_DESC_DEVICE:
            descriptor = device_descriptor;
            length = sizeof(device_descriptor);
            break;

        case USB_DESC_CONFIGURATION:
            descriptor = configuration_descriptor;
            length = sizeof(configuration_descriptor);
            break;

        case USB_DESC_STRING:
            descriptor = make_string_descriptor(descriptor_index, &length);
            break;

        default:
            break;
    }

    if (descriptor == NULL)
    {
        bdt_stall_ep0();
        return;
    }

    control_send(descriptor, length, ctrl.setup.wLength);
}

static void handle_standard_request(void)
{
    static uint8_t one_byte_response;
    uint8_t recipient;
    uint8_t endpoint;

    switch (ctrl.setup.bRequest)
    {
        case USB_REQ_GET_STATUS:
            handle_get_status();
            break;

        case USB_REQ_CLEAR_FEATURE:
            recipient = (uint8_t)(ctrl.setup.bmRequestType & USB_RECIP_MASK);
            if ((recipient == USB_RECIP_ENDPOINT) && (ctrl.setup.wValue == USB_FEATURE_ENDPOINT_HALT))
            {
                endpoint = (uint8_t)(ctrl.setup.wIndex & 0x0FU);
                endpoint_clear_stall(endpoint);
                control_send_status_in();
            }
            else
            {
                bdt_stall_ep0();
            }
            break;

        case USB_REQ_SET_FEATURE:
            recipient = (uint8_t)(ctrl.setup.bmRequestType & USB_RECIP_MASK);
            if ((recipient == USB_RECIP_ENDPOINT) && (ctrl.setup.wValue == USB_FEATURE_ENDPOINT_HALT))
            {
                endpoint = (uint8_t)(ctrl.setup.wIndex & 0x0FU);
                endpoint_set_stall(endpoint);
                control_send_status_in();
            }
            else
            {
                bdt_stall_ep0();
            }
            break;

        case USB_REQ_SET_ADDRESS:
            pending_address = (uint8_t)(ctrl.setup.wValue & 0x7FU);
            control_send_status_in();
            break;

        case USB_REQ_GET_DESCRIPTOR:
            handle_get_descriptor();
            break;

        case USB_REQ_GET_CONFIGURATION:
            one_byte_response = configuration_value;
            control_send(&one_byte_response, 1U, ctrl.setup.wLength);
            break;

        case USB_REQ_SET_CONFIGURATION:
            configure_data_endpoints((uint8_t)(ctrl.setup.wValue & 0xFFU));
            control_send_status_in();
            break;

        case USB_REQ_GET_INTERFACE:
            one_byte_response = 0U;
            control_send(&one_byte_response, 1U, ctrl.setup.wLength);
            break;

        case USB_REQ_SET_INTERFACE:
            if (ctrl.setup.wValue == 0U)
            {
                control_send_status_in();
            }
            else
            {
                bdt_stall_ep0();
            }
            break;

        default:
            bdt_stall_ep0();
            break;
    }
}

static void handle_cdc_request(void)
{
    uint8_t data[7];
    bool new_dtr;
    bool new_rts;

    switch (ctrl.setup.bRequest)
    {
        case USB_CDC_SET_LINE_CODING:
            if (ctrl.setup.wLength == 7U)
            {
                control_expect_out(USB_CDC_SET_LINE_CODING, 7U);
            }
            else
            {
                bdt_stall_ep0();
            }
            break;

        case USB_CDC_GET_LINE_CODING:
            data[0] = (uint8_t)(line_coding.baud_rate & 0xFFU);
            data[1] = (uint8_t)((line_coding.baud_rate >> 8) & 0xFFU);
            data[2] = (uint8_t)((line_coding.baud_rate >> 16) & 0xFFU);
            data[3] = (uint8_t)((line_coding.baud_rate >> 24) & 0xFFU);
            data[4] = line_coding.stop_bits;
            data[5] = line_coding.parity;
            data[6] = line_coding.data_bits;
            (void)memcpy(ep0_tx_buffer, data, sizeof(data));
            control_send(ep0_tx_buffer, sizeof(data), ctrl.setup.wLength);
            break;

        case USB_CDC_SET_CONTROL_LINE_STATE:
            new_dtr = ((ctrl.setup.wValue & 0x0001U) != 0U);
            new_rts = ((ctrl.setup.wValue & 0x0002U) != 0U);
            if ((new_dtr != cdc_dtr) || (new_rts != cdc_rts))
            {
                cdc_dtr = new_dtr;
                cdc_rts = new_rts;
                if (usb_config.on_control_line_changed != NULL)
                {
                    usb_config.on_control_line_changed(cdc_dtr, cdc_rts);
                }
            }
            control_send_status_in();
            break;

        case USB_CDC_SEND_BREAK:
            control_send_status_in();
            break;

        default:
            bdt_stall_ep0();
            break;
    }
}

static void handle_setup_packet(const uint8_t *setup_bytes)
{
    ctrl.setup.bmRequestType = setup_bytes[0];
    ctrl.setup.bRequest = setup_bytes[1];
    ctrl.setup.wValue = (uint16_t)setup_bytes[2] | ((uint16_t)setup_bytes[3] << 8);
    ctrl.setup.wIndex = (uint16_t)setup_bytes[4] | ((uint16_t)setup_bytes[5] << 8);
    ctrl.setup.wLength = (uint16_t)setup_bytes[6] | ((uint16_t)setup_bytes[7] << 8);
    ctrl.state = CTRL_IDLE;
    ctrl.source = NULL;
    ctrl.pending_request = 0U;
    next_tx_data[USB_EP0] = 1U;
    U1EP0CLR = _U1EP0_EPSTALL_MASK;
    usb_bdt[bdt_index(USB_EP0, USB_DIR_TX, USB_PP_EVEN)].byte[0] = 0U;
    usb_bdt[bdt_index(USB_EP0, USB_DIR_TX, USB_PP_ODD)].byte[0] = 0U;
    U1CONCLR = _U1CON_PKTDIS_MASK;

    if ((ctrl.setup.bmRequestType & 0x60U) == 0x00U)
    {
        handle_standard_request();
    }
    else if ((ctrl.setup.bmRequestType & 0x60U) == 0x20U)
    {
        handle_cdc_request();
    }
    else
    {
        bdt_stall_ep0();
    }
}

static void service_tx(void)
{
    uint16_t count = 0U;

    if ((usb_state != PIC32_USB_CDC_STATE_CONFIGURED) || bulk_tx_busy)
    {
        return;
    }

    while ((count < PIC32_USB_CDC_BULK_SIZE) && tx_pop(&bulk_tx_buffer[count]))
    {
        count++;
    }

    if (count > 0U)
    {
        bulk_tx_busy = true;
        bdt_arm_tx_next(USB_EP_CDC_IN, bulk_tx_buffer, count);
    }
}

static void handle_ep0_rx(uint8_t ping_pong, uint8_t pid, uint16_t count)
{
    uint8_t *buffer = ep0_rx_buffer[ping_pong];
    uint16_t copy_count;

    if (pid == USB_PID_SETUP)
    {
        handle_setup_packet(buffer);
    }
    else if (pid == USB_PID_OUT)
    {
        if (ctrl.state == CTRL_OUT_DATA)
        {
            copy_count = count;
            if ((uint16_t)(copy_count + ctrl.received) > ctrl.total)
            {
                copy_count = (uint16_t)(ctrl.total - ctrl.received);
            }

            if (copy_count > 0U)
            {
                (void)memcpy(&ep0_out_buffer[ctrl.received], buffer, copy_count);
                ctrl.received = (uint16_t)(ctrl.received + copy_count);
            }

            if ((ctrl.received >= ctrl.total) || (count < PIC32_USB_CDC_EP0_SIZE))
            {
                apply_out_control_data();
                control_send_status_in();
            }
        }
        else if (ctrl.state == CTRL_STATUS_OUT)
        {
            ctrl.state = CTRL_IDLE;
        }
    }

    bdt_arm_rx(USB_EP0, ping_pong, ep0_rx_buffer[ping_pong], PIC32_USB_CDC_EP0_SIZE);
}

static void handle_ep0_tx_complete(void)
{
    if (ctrl.state == CTRL_IN_DATA)
    {
        ep0_send_next();
    }
    else if (ctrl.state == CTRL_STATUS_IN)
    {
        if (pending_address != 0xFFU)
        {
            U1ADDR = pending_address;
            set_state((pending_address == 0U) ? PIC32_USB_CDC_STATE_DEFAULT : PIC32_USB_CDC_STATE_ADDRESSED);
            pending_address = 0xFFU;
        }
        ctrl.state = CTRL_IDLE;
    }
}

static void handle_bulk_rx(uint8_t ping_pong, uint16_t count)
{
    uint16_t i;

    for (i = 0U; i < count; i++)
    {
        rx_push(bulk_rx_buffer[ping_pong][i]);
    }

    bdt_arm_rx(USB_EP_CDC_OUT, ping_pong, bulk_rx_buffer[ping_pong], PIC32_USB_CDC_BULK_SIZE);
}

static void handle_token_done(void)
{
    uint32_t stat;
    uint8_t endpoint;
    uint8_t direction;
    uint8_t ping_pong;
    volatile Pic32UsbBdtEntry *bd;
    uint8_t pid;
    uint16_t count;

    while ((U1IR & _U1IR_TRNIF_MASK) != 0U)
    {
        stat = U1STAT;
        direction = ((stat & _U1STAT_DIR_MASK) != 0U) ? USB_DIR_TX : USB_DIR_RX;
        ping_pong = ((stat & _U1STAT_PPBI_MASK) != 0U) ? USB_PP_ODD : USB_PP_EVEN;
        endpoint = (uint8_t)((stat & _U1STAT_ENDPT_MASK) >> _U1STAT_ENDPT_POSITION);
        bd = &usb_bdt[bdt_index(endpoint, direction, ping_pong)];
        pid = (uint8_t)(bd->byte[0] & USB_BD_PID_MASK);
        count = bd->half[1];
        U1IRCLR = _U1IR_TRNIF_MASK;

        if (endpoint == USB_EP0)
        {
            if (direction == USB_DIR_RX)
            {
                handle_ep0_rx(ping_pong, pid, count);
            }
            else
            {
                bd->byte[0] = 0U;
                handle_ep0_tx_complete();
            }
        }
        else if ((endpoint == USB_EP_CDC_OUT) && (direction == USB_DIR_RX))
        {
            handle_bulk_rx(ping_pong, count);
        }
        else if ((endpoint == USB_EP_CDC_IN) && (direction == USB_DIR_TX))
        {
            bd->byte[0] = 0U;
            bulk_tx_busy = false;
            service_tx();
        }
        else if ((endpoint == USB_EP_CDC_NOTIFICATION) && (direction == USB_DIR_TX))
        {
            bd->byte[0] = 0U;
            notify_tx_busy = false;
        }
        else
        {
            bd->byte[0] = 0U;
        }
    }
}

static void bus_reset(void)
{
    uint8_t ep;

    U1CONSET = _U1CON_PPBRST_MASK;
    U1CONCLR = _U1CON_PPBRST_MASK;
    U1ADDR = 0U;
    U1EP0 = USB_EP_ATTR_CONTROL;
    U1EP1 = 0U;
    U1EP2 = 0U;
    U1EP3 = 0U;
    bdt_clear_all();

    for (ep = 0U; ep < USB_ENDPOINTS; ep++)
    {
        next_pp[ep][USB_DIR_RX] = 0U;
        next_pp[ep][USB_DIR_TX] = 0U;
        next_tx_data[ep] = 0U;
    }

    configuration_value = 0U;
    pending_address = 0xFFU;
    bulk_tx_busy = false;
    notify_tx_busy = false;
    ctrl.state = CTRL_IDLE;
    cdc_dtr = false;
    cdc_rts = false;
    next_tx_data[USB_EP0] = 1U;
    bdt_arm_rx_next(USB_EP0, ep0_rx_buffer[USB_PP_EVEN], PIC32_USB_CDC_EP0_SIZE);
    bdt_arm_rx_next(USB_EP0, ep0_rx_buffer[USB_PP_ODD], PIC32_USB_CDC_EP0_SIZE);
    set_state(PIC32_USB_CDC_STATE_DEFAULT);
    usb_log("USB reset");
}

static void set_bdt_base_address(void)
{
    uint32_t address = KVA_TO_PA((void *)usb_bdt);

    U1BDTP3bits.BDTPTRU = (uint8_t)((address >> 24) & 0xFFU);
    U1BDTP2bits.BDTPTRH = (uint8_t)((address >> 16) & 0xFFU);
    U1BDTP1bits.BDTPTRL = (uint8_t)((address >> 9) & 0x7FU);
}

void PIC32_USB_CDC_ConfigDefault(PIC32_USB_CDC_Config *config)
{
    if (config == NULL)
    {
        return;
    }

    config->vid = PIC32_USB_CDC_DEFAULT_VID;
    config->pid = PIC32_USB_CDC_DEFAULT_PID;
    config->bcd_device = PIC32_USB_CDC_DEFAULT_BCD_DEVICE;
    config->manufacturer = "VigoBeatz";
    config->product = "VQuest CDC";
    config->serial_number = "";
    config->on_line_coding_changed = NULL;
    config->on_control_line_changed = NULL;
    config->on_state_changed = NULL;
    config->debug_log = NULL;
}

void PIC32_USB_CDC_Init(const PIC32_USB_CDC_Config *config)
{
    PIC32_USB_CDC_ConfigDefault(&usb_config);
    if (config != NULL)
    {
        usb_config = *config;
        if (usb_config.manufacturer == NULL)
        {
            usb_config.manufacturer = "VigoBeatz";
        }
        if (usb_config.product == NULL)
        {
            usb_config.product = "VQuest CDC";
        }
    }

    build_device_descriptor();
    rx_head = 0U;
    rx_tail = 0U;
    tx_head = 0U;
    tx_tail = 0U;
    rx_dropped = 0U;
    line_coding.baud_rate = 115200UL;
    line_coding.stop_bits = 0U;
    line_coding.parity = 0U;
    line_coding.data_bits = 8U;
    usb_suspended = false;
    state_before_suspend = PIC32_USB_CDC_STATE_DETACHED;

    U1PWRCSET = _U1PWRC_USBPWR_MASK;
    U1CON = 0U;
    U1IE = 0U;
    U1EIE = 0U;
    U1IRCLR = 0xFFU;
    U1EIRCLR = 0xFFU;
    set_bdt_base_address();
    bus_reset();
    U1IE = (uint32_t)(_U1IE_URSTIE_MASK | _U1IE_TRNIE_MASK | _U1IE_IDLEIE_MASK
        | _U1IE_RESUMEIE_MASK | _U1IE_STALLIE_MASK | _U1IE_UERRIE_MASK);
    U1EIE = 0xFFU;
    U1CONSET = _U1CON_USBEN_MASK;
    usb_log("USB CDC initialized");
}

void PIC32_USB_CDC_Detach(void)
{
    U1CONCLR = _U1CON_USBEN_MASK;
    U1PWRCCLR = _U1PWRC_USBPWR_MASK;
    set_state(PIC32_USB_CDC_STATE_DETACHED);
}

void PIC32_USB_CDC_Task(void)
{
    if ((U1IR & _U1IR_URSTIF_MASK) != 0U)
    {
        U1IRCLR = _U1IR_URSTIF_MASK;
        usb_suspended = false;
        bus_reset();
    }

    if ((U1IR & _U1IR_UERRIF_MASK) != 0U)
    {
        U1EIRCLR = 0xFFU;
        U1IRCLR = _U1IR_UERRIF_MASK;
        usb_log("USB error flags cleared");
    }

    handle_token_done();

    if ((U1IR & _U1IR_STALLIF_MASK) != 0U)
    {
        endpoint_clear_stall(USB_EP0);
        U1IRCLR = _U1IR_STALLIF_MASK;
    }

    if ((U1IR & _U1IR_IDLEIF_MASK) != 0U)
    {
        U1IRCLR = _U1IR_IDLEIF_MASK;
        if (!usb_suspended)
        {
            usb_suspended = true;
            state_before_suspend = usb_state;
            set_state(PIC32_USB_CDC_STATE_SUSPENDED);
            usb_log("USB suspended");
        }
    }

    if ((U1IR & _U1IR_RESUMEIF_MASK) != 0U)
    {
        U1IRCLR = _U1IR_RESUMEIF_MASK;
        if (usb_suspended)
        {
            usb_suspended = false;
            set_state(state_before_suspend);
            usb_log("USB resumed");
        }
    }

    service_tx();
}

PIC32_USB_CDC_State PIC32_USB_CDC_StateGet(void)
{
    return usb_state;
}

bool PIC32_USB_CDC_Configured(void)
{
    return (usb_state == PIC32_USB_CDC_STATE_CONFIGURED);
}

bool PIC32_USB_CDC_IsOpen(void)
{
    return PIC32_USB_CDC_Configured() && cdc_dtr;
}

bool PIC32_USB_CDC_Suspended(void)
{
    return usb_suspended;
}

size_t PIC32_USB_CDC_Read(void *buffer, size_t length)
{
    uint8_t *out = (uint8_t *)buffer;
    size_t count = 0U;

    if ((buffer == NULL) || (length == 0U))
    {
        return 0U;
    }

    while ((count < length) && (rx_tail != rx_head))
    {
        out[count++] = rx_ring[rx_tail];
        rx_tail = ring_next(rx_tail, PIC32_USB_CDC_RX_BUFFER_SIZE);
    }

    return count;
}

size_t PIC32_USB_CDC_Write(const void *buffer, size_t length)
{
    const uint8_t *in = (const uint8_t *)buffer;
    size_t count = 0U;

    if ((buffer == NULL) || (length == 0U) || !PIC32_USB_CDC_Configured())
    {
        return 0U;
    }

    while (count < length)
    {
        uint16_t next = ring_next(tx_head, PIC32_USB_CDC_TX_BUFFER_SIZE);
        if (next == tx_tail)
        {
            break;
        }
        tx_ring[tx_head] = in[count++];
        tx_head = next;
    }

    service_tx();
    return count;
}

size_t PIC32_USB_CDC_WriteString(const char *text)
{
    if (text == NULL)
    {
        return 0U;
    }

    return PIC32_USB_CDC_Write(text, strlen(text));
}

size_t PIC32_USB_CDC_RxAvailable(void)
{
    return rx_count();
}

size_t PIC32_USB_CDC_TxAvailable(void)
{
    return (PIC32_USB_CDC_TX_BUFFER_SIZE - 1U) - tx_count();
}

uint32_t PIC32_USB_CDC_RxDropped(void)
{
    return rx_dropped;
}

const PIC32_USB_CDC_LineCoding *PIC32_USB_CDC_LineCodingGet(void)
{
    return &line_coding;
}

bool PIC32_USB_CDC_DTR(void)
{
    return cdc_dtr;
}

bool PIC32_USB_CDC_RTS(void)
{
    return cdc_rts;
}
