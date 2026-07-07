#ifndef I2C_H
#define I2C_H

#include <stdint.h>

void I2C1_Init(uint32_t baudrate);
uint8_t I2C1_DeviceReady(uint8_t addr);

uint8_t I2C1_WriteRegister(uint8_t devAddr, uint8_t regAddr, uint8_t data);
uint8_t I2C1_ReadRegister(uint8_t devAddr, uint8_t regAddr, uint8_t *data);
uint8_t I2C1_ReadRegisters(uint8_t devAddr, uint8_t regAddr, uint8_t *buffer, uint8_t length);
uint8_t I2C1_WriteByteToDevice(uint8_t devAddr, uint8_t data);

#endif