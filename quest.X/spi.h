#ifndef SPI_H
#define SPI_H

#include <stdint.h>

void SPI1_Init(void);

void SPI1_Write(uint8_t data);
void SPI1_WriteBuffer(const uint8_t *data, uint32_t len);

void SPI1_Write16(uint16_t data);
void SPI1_Write16Buffer(const uint16_t *data, uint32_t count);
void SPI1_WriteColor(uint16_t color, uint32_t count);

#endif