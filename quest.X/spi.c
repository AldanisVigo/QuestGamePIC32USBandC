#include <xc.h>
#include <stdint.h>
#include "spi.h"

// PIC32MX270F256B 28-pin DIP
// ST7735 SDA/MOSI -> pin 24 / RPB13 / SDO1
// ST7735 SCK      -> pin 25 / RB14  / SCK1

#define DMA_CH_TX   0
#define DMA_CH_RX   1

#define DMA_BYTE_CHUNK   256
#define DMA_WORD_CHUNK   256

#ifndef KVA_TO_PA
#define KVA_TO_PA(v)  ((uint32_t)(v) & 0x1FFFFFFFUL)
#endif

static volatile uint8_t  rxTrash8[DMA_BYTE_CHUNK];
static volatile uint16_t rxTrash16[DMA_WORD_CHUNK];
static uint16_t colorBuf[DMA_WORD_CHUNK];

static void DMA_Init(void)
{
    DMACONbits.ON = 1;

    DCH0CON = 0;
    DCH1CON = 0;

    DCH0INT = 0;
    DCH1INT = 0;

    DCH0ECON = 0;
    DCH1ECON = 0;
}

void SPI1_Init(void)
{
    // Unlock PPS
    SYSKEY = 0x00000000;
    SYSKEY = 0xAA996655;
    SYSKEY = 0x556699AA;
    CFGCONbits.IOLOCK = 0;

    // Map SDO1 to RPB13
    RPB13Rbits.RPB13R = 0b0011;

    // Lock PPS
    CFGCONbits.IOLOCK = 1;
    SYSKEY = 0x00000000;

    TRISBbits.TRISB13 = 0; // SDO1 / MOSI / SDA
    TRISBbits.TRISB14 = 0; // SCK1

    SPI1CON = 0;
    SPI1STATbits.SPIROV = 0;

    // PBCLK = 40 MHz
    // SPI clock = PBCLK / (2 * (SPI1BRG + 1))
    // SPI1BRG = 0 gives 20 MHz
    SPI1BRG = 0;

    SPI1CONbits.MSTEN = 1;
    SPI1CONbits.CKP = 0;
    SPI1CONbits.CKE = 1;
    SPI1CONbits.SMP = 0;
    SPI1CONbits.MODE16 = 0;
    SPI1CONbits.MODE32 = 0;

    SPI1CONbits.ON = 1;

    DMA_Init();
}

static void SPI1_Set8Bit(void)
{
    while(SPI1STATbits.SPIBUSY);

    SPI1CONbits.ON = 0;
    SPI1CONbits.MODE16 = 0;
    SPI1CONbits.MODE32 = 0;
    SPI1STATbits.SPIROV = 0;
    SPI1CONbits.ON = 1;
}

static void SPI1_Set16Bit(void)
{
    while(SPI1STATbits.SPIBUSY);

    SPI1CONbits.ON = 0;
    SPI1CONbits.MODE16 = 1;
    SPI1CONbits.MODE32 = 0;
    SPI1STATbits.SPIROV = 0;
    SPI1CONbits.ON = 1;
}

void SPI1_Write(uint8_t data)
{
    SPI1_Set8Bit();

    while(SPI1STATbits.SPITBF);
    SPI1BUF = data;

    while(!SPI1STATbits.SPIRBF);
    volatile uint8_t dummy = SPI1BUF;
    (void)dummy;
}

void SPI1_Write16(uint16_t data)
{
    SPI1_Set16Bit();

    while(SPI1STATbits.SPITBF);
    SPI1BUF = data;

    while(!SPI1STATbits.SPIRBF);
    volatile uint16_t dummy = SPI1BUF;
    (void)dummy;

    SPI1_Set8Bit();
}

static void SPI1_DMA_Write8_Chunk(const uint8_t *data, uint32_t len)
{
    if(len == 0) return;

    SPI1_Set8Bit();

    DCH0CON = 0;
    DCH1CON = 0;

    DCH0INT = 0;
    DCH1INT = 0;

    // RX DMA: SPI1BUF -> rxTrash8[]
    DCH1SSA = KVA_TO_PA(&SPI1BUF);
    DCH1DSA = KVA_TO_PA(rxTrash8);
    DCH1SSIZ = 1;
    DCH1DSIZ = len;
    DCH1CSIZ = 1;

    DCH1ECON = 0;
    DCH1ECONbits.CHSIRQ = _SPI1_RX_IRQ;
    DCH1ECONbits.SIRQEN = 1;

    // TX DMA: data[] -> SPI1BUF
    DCH0SSA = KVA_TO_PA(data);
    DCH0DSA = KVA_TO_PA(&SPI1BUF);
    DCH0SSIZ = len;
    DCH0DSIZ = 1;
    DCH0CSIZ = 1;

    DCH0ECON = 0;
    DCH0ECONbits.CHSIRQ = _SPI1_TX_IRQ;
    DCH0ECONbits.SIRQEN = 1;

    IFS1CLR = _IFS1_SPI1RXIF_MASK | _IFS1_SPI1TXIF_MASK;

    DCH1CONbits.CHEN = 1;
    DCH0CONbits.CHEN = 1;

    // Kick first TX cell
    DCH0ECONbits.CFORCE = 1;

    while(!DCH0INTbits.CHBCIF);
    while(!DCH1INTbits.CHBCIF);

    while(SPI1STATbits.SPIBUSY);

    DCH0CONbits.CHEN = 0;
    DCH1CONbits.CHEN = 0;

    DCH0INT = 0;
    DCH1INT = 0;

    SPI1STATbits.SPIROV = 0;
}

void SPI1_WriteBuffer(const uint8_t *data, uint32_t len)
{
    while(len)
    {
        uint32_t chunk = len;

        if(chunk > DMA_BYTE_CHUNK)
            chunk = DMA_BYTE_CHUNK;

        SPI1_DMA_Write8_Chunk(data, chunk);

        data += chunk;
        len -= chunk;
    }
}

static void SPI1_DMA_Write16_Chunk(const uint16_t *data, uint32_t count)
{
    if(count == 0) return;

    SPI1_Set16Bit();

    DCH0CON = 0;
    DCH1CON = 0;

    DCH0INT = 0;
    DCH1INT = 0;

    // RX DMA: SPI1BUF -> rxTrash16[]
    DCH1SSA = KVA_TO_PA(&SPI1BUF);
    DCH1DSA = KVA_TO_PA(rxTrash16);
    DCH1SSIZ = 2;
    DCH1DSIZ = count * 2;
    DCH1CSIZ = 2;

    DCH1ECON = 0;
    DCH1ECONbits.CHSIRQ = _SPI1_RX_IRQ;
    DCH1ECONbits.SIRQEN = 1;

    // TX DMA: uint16_t pixels -> SPI1BUF
    DCH0SSA = KVA_TO_PA(data);
    DCH0DSA = KVA_TO_PA(&SPI1BUF);
    DCH0SSIZ = count * 2;
    DCH0DSIZ = 2;
    DCH0CSIZ = 2;

    DCH0ECON = 0;
    DCH0ECONbits.CHSIRQ = _SPI1_TX_IRQ;
    DCH0ECONbits.SIRQEN = 1;

    IFS1CLR = _IFS1_SPI1RXIF_MASK | _IFS1_SPI1TXIF_MASK;

    DCH1CONbits.CHEN = 1;
    DCH0CONbits.CHEN = 1;

    // Kick first TX cell
    DCH0ECONbits.CFORCE = 1;

    while(!DCH0INTbits.CHBCIF);
    while(!DCH1INTbits.CHBCIF);

    while(SPI1STATbits.SPIBUSY);

    DCH0CONbits.CHEN = 0;
    DCH1CONbits.CHEN = 0;

    DCH0INT = 0;
    DCH1INT = 0;

    SPI1STATbits.SPIROV = 0;
}

void SPI1_Write16Buffer(const uint16_t *data, uint32_t count)
{
    while(count)
    {
        uint32_t chunk = count;

        if(chunk > DMA_WORD_CHUNK)
            chunk = DMA_WORD_CHUNK;

        SPI1_DMA_Write16_Chunk(data, chunk);

        data += chunk;
        count -= chunk;
    }

    SPI1_Set8Bit();
}

void SPI1_WriteColor(uint16_t color, uint32_t count)
{
    for(uint32_t i = 0; i < DMA_WORD_CHUNK; i++)
        colorBuf[i] = color;

    while(count)
    {
        uint32_t chunk = count;

        if(chunk > DMA_WORD_CHUNK)
            chunk = DMA_WORD_CHUNK;

        SPI1_DMA_Write16_Chunk(colorBuf, chunk);

        count -= chunk;
    }

    SPI1_Set8Bit();
}