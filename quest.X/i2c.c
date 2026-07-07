#include <xc.h>
#include <stdint.h>
#include "i2c.h"

#define PBCLK 40000000UL

static void I2C1_WaitIdle(void)
{
    while(I2C1CONbits.SEN  ||
          I2C1CONbits.RSEN ||
          I2C1CONbits.PEN  ||
          I2C1CONbits.RCEN ||
          I2C1CONbits.ACKEN ||
          I2C1STATbits.TRSTAT);
}

void I2C1_Init(uint32_t baudrate)
{
    I2C1CONbits.ON = 0;

    ANSELB = 0;

    TRISBbits.TRISB8 = 1; // SCL1 pin 17
    TRISBbits.TRISB9 = 1; // SDA1 pin 18

    I2C1CON = 0;
    I2C1STAT = 0;

    I2C1BRG = ((PBCLK / baudrate) - (PBCLK / 10000000UL)) / 2 - 2;

    I2C1CONbits.DISSLW = 1;
    I2C1CONbits.ON = 1;
}

static void I2C1_Start(void)
{
    I2C1_WaitIdle();
    I2C1CONbits.SEN = 1;
    while(I2C1CONbits.SEN);
}

static void I2C1_Restart(void)
{
    I2C1_WaitIdle();
    I2C1CONbits.RSEN = 1;
    while(I2C1CONbits.RSEN);
}

static void I2C1_Stop(void)
{
    I2C1_WaitIdle();
    I2C1CONbits.PEN = 1;
    while(I2C1CONbits.PEN);
}

static uint8_t I2C1_WriteByte(uint8_t data)
{
    I2C1_WaitIdle();

    I2C1STATbits.BCL = 0;
    I2C1STATbits.IWCOL = 0;

    I2C1TRN = data;

    while(I2C1STATbits.TBF);
    while(I2C1STATbits.TRSTAT);

    if(I2C1STATbits.BCL || I2C1STATbits.IWCOL)
        return 0;

    return (I2C1STATbits.ACKSTAT == 0);
}

static uint8_t I2C1_ReadByte(uint8_t ack)
{
    I2C1_WaitIdle();

    I2C1CONbits.RCEN = 1;
    while(!I2C1STATbits.RBF);

    uint8_t data = I2C1RCV;

    I2C1_WaitIdle();

    I2C1CONbits.ACKDT = ack ? 0 : 1; // ack=1 sends ACK, ack=0 sends NACK
    I2C1CONbits.ACKEN = 1;
    while(I2C1CONbits.ACKEN);

    return data;
}

uint8_t I2C1_DeviceReady(uint8_t addr)
{
    uint8_t result;

    I2C1_Start();
    result = I2C1_WriteByte((addr << 1) | 0);
    I2C1_Stop();

    return result;
}

uint8_t I2C1_WriteRegister(uint8_t devAddr, uint8_t regAddr, uint8_t data)
{
    I2C1_Start();

    if(!I2C1_WriteByte((devAddr << 1) | 0))
    {
        I2C1_Stop();
        return 0;
    }

    if(!I2C1_WriteByte(regAddr))
    {
        I2C1_Stop();
        return 0;
    }

    if(!I2C1_WriteByte(data))
    {
        I2C1_Stop();
        return 0;
    }

    I2C1_Stop();
    return 1;
}

uint8_t I2C1_ReadRegister(uint8_t devAddr, uint8_t regAddr, uint8_t *data)
{
    return I2C1_ReadRegisters(devAddr, regAddr, data, 1);
}

uint8_t I2C1_ReadRegisters(uint8_t devAddr, uint8_t regAddr, uint8_t *buffer, uint8_t length)
{
    I2C1_Start();

    if(!I2C1_WriteByte((devAddr << 1) | 0))
    {
        I2C1_Stop();
        return 0;
    }

    if(!I2C1_WriteByte(regAddr))
    {
        I2C1_Stop();
        return 0;
    }

    I2C1_Restart();

    if(!I2C1_WriteByte((devAddr << 1) | 1))
    {
        I2C1_Stop();
        return 0;
    }

    for(uint8_t i = 0; i < length; i++)
    {
        buffer[i] = I2C1_ReadByte(i < (length - 1));
    }

    I2C1_Stop();
    return 1;
}

uint8_t I2C1_WriteByteToDevice(uint8_t devAddr, uint8_t data)
{
    I2C1_Start();

    if(!I2C1_WriteByte((devAddr << 1) | 0))
    {
        I2C1_Stop();
        return 0;
    }

    if(!I2C1_WriteByte(data))
    {
        I2C1_Stop();
        return 0;
    }

    I2C1_Stop();
    return 1;
}