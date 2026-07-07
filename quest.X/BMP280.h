#ifndef BMP280_H
#define BMP280_H

#include <stdint.h>

#define BMP280_ADDR_76 0x76
#define BMP280_ADDR_77 0x77

typedef struct
{
    uint8_t address;
    uint8_t chip_id;

    float temperature_C;
    float pressure_Pa;
    float pressure_hPa;
    float altitude_m;

    float sea_level_hPa;
} BMP280_t;

uint8_t BMP280_Init(BMP280_t *bmp);
uint8_t BMP280_Update(BMP280_t *bmp);
uint8_t BMP280_WhoAmI(uint8_t address);

void BMP280_SetSeaLevelPressure(BMP280_t *bmp, float seaLevel_hPa);

#endif