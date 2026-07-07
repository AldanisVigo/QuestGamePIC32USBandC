#include <xc.h>
#include <stdint.h>
#include <math.h>
#include "definitions.h"
#include "BMP280.h"

#define BMP280_REG_ID          0xD0
#define BMP280_REG_RESET       0xE0
#define BMP280_REG_STATUS      0xF3
#define BMP280_REG_CTRL_MEAS   0xF4
#define BMP280_REG_CONFIG      0xF5
#define BMP280_REG_PRESS_MSB   0xF7
#define BMP280_REG_CALIB       0x88

#define BMP280_CHIP_ID         0x58
#define BME280_CHIP_ID         0x60

static uint8_t BMP280_I2CWriteRegister(uint8_t devAddr, uint8_t regAddr, uint8_t data)
{
    uint8_t txBuffer[2] = {regAddr, data};

    while(I2C1_IsBusy())
    {
        ;
    }

    if(!I2C1_Write(devAddr, txBuffer, sizeof(txBuffer)))
    {
        return 0;
    }

    while(I2C1_IsBusy())
    {
        ;
    }

    return (I2C1_ErrorGet() == I2C_ERROR_NONE);
}

static uint8_t BMP280_I2CReadRegisters(uint8_t devAddr, uint8_t regAddr, uint8_t *buffer, uint8_t length)
{
    while(I2C1_IsBusy())
    {
        ;
    }

    if(!I2C1_WriteRead(devAddr, &regAddr, 1, buffer, length))
    {
        return 0;
    }

    while(I2C1_IsBusy())
    {
        ;
    }

    return (I2C1_ErrorGet() == I2C_ERROR_NONE);
}

static uint8_t BMP280_I2CReadRegister(uint8_t devAddr, uint8_t regAddr, uint8_t *data)
{
    return BMP280_I2CReadRegisters(devAddr, regAddr, data, 1);
}

typedef struct
{
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;

    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;

    int32_t t_fine;
} BMP280_Calib_t;

static BMP280_Calib_t cal;

static uint16_t u16_le(uint8_t lsb, uint8_t msb)
{
    return (uint16_t)((msb << 8) | lsb);
}

static int16_t s16_le(uint8_t lsb, uint8_t msb)
{
    return (int16_t)((msb << 8) | lsb);
}

uint8_t BMP280_WhoAmI(uint8_t address)
{
    uint8_t id = 0;

    if(!BMP280_I2CReadRegister(address, BMP280_REG_ID, &id))
        return 0;

    return id;
}

static uint8_t BMP280_ReadCalibration(BMP280_t *bmp)
{
    uint8_t data[24];

    if(!BMP280_I2CReadRegisters(bmp->address, BMP280_REG_CALIB, data, 24))
        return 0;

    cal.dig_T1 = u16_le(data[0],  data[1]);
    cal.dig_T2 = s16_le(data[2],  data[3]);
    cal.dig_T3 = s16_le(data[4],  data[5]);

    cal.dig_P1 = u16_le(data[6],  data[7]);
    cal.dig_P2 = s16_le(data[8],  data[9]);
    cal.dig_P3 = s16_le(data[10], data[11]);
    cal.dig_P4 = s16_le(data[12], data[13]);
    cal.dig_P5 = s16_le(data[14], data[15]);
    cal.dig_P6 = s16_le(data[16], data[17]);
    cal.dig_P7 = s16_le(data[18], data[19]);
    cal.dig_P8 = s16_le(data[20], data[21]);
    cal.dig_P9 = s16_le(data[22], data[23]);

    return 1;
}

uint8_t BMP280_Init(BMP280_t *bmp)
{
    uint8_t id76 = BMP280_WhoAmI(BMP280_ADDR_76);
    uint8_t id77 = BMP280_WhoAmI(BMP280_ADDR_77);

    if(id76 == BMP280_CHIP_ID || id76 == BME280_CHIP_ID)
    {
        bmp->address = BMP280_ADDR_76;
        bmp->chip_id = id76;
    }
    else if(id77 == BMP280_CHIP_ID || id77 == BME280_CHIP_ID)
    {
        bmp->address = BMP280_ADDR_77;
        bmp->chip_id = id77;
    }
    else
    {
        return 0;
    }

    bmp->sea_level_hPa = 1013.25f;

    if(!BMP280_ReadCalibration(bmp))
        return 0;

    // standby 250ms, filter x4
    BMP280_I2CWriteRegister(bmp->address, BMP280_REG_CONFIG, 0x80);

    // temp oversampling x1, pressure oversampling x4, normal mode
    BMP280_I2CWriteRegister(bmp->address, BMP280_REG_CTRL_MEAS, 0x2F);

    return 1;
}

void BMP280_SetSeaLevelPressure(BMP280_t *bmp, float seaLevel_hPa)
{
    bmp->sea_level_hPa = seaLevel_hPa;
}

static int32_t compensate_temperature(int32_t adc_T)
{
    int32_t var1, var2, T;

    var1 = ((((adc_T >> 3) - ((int32_t)cal.dig_T1 << 1))) *
            ((int32_t)cal.dig_T2)) >> 11;

    var2 = (((((adc_T >> 4) - ((int32_t)cal.dig_T1)) *
              ((adc_T >> 4) - ((int32_t)cal.dig_T1))) >> 12) *
              ((int32_t)cal.dig_T3)) >> 14;

    cal.t_fine = var1 + var2;

    T = (cal.t_fine * 5 + 128) >> 8;

    return T; // Celsius * 100
}

static uint32_t compensate_pressure(int32_t adc_P)
{
    int64_t var1, var2, p;

    var1 = ((int64_t)cal.t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)cal.dig_P6;
    var2 = var2 + ((var1 * (int64_t)cal.dig_P5) << 17);
    var2 = var2 + (((int64_t)cal.dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)cal.dig_P3) >> 8) +
           ((var1 * (int64_t)cal.dig_P2) << 12);

    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)cal.dig_P1) >> 33;

    if(var1 == 0)
        return 0;

    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;

    var1 = (((int64_t)cal.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)cal.dig_P8) * p) >> 19;

    p = ((p + var1 + var2) >> 8) + (((int64_t)cal.dig_P7) << 4);

    return (uint32_t)p; // Pa * 256
}

uint8_t BMP280_Update(BMP280_t *bmp)
{
    uint8_t data[6];

    int32_t adc_P;
    int32_t adc_T;

    int32_t temp_x100;
    uint32_t pressure_x256;

    if(!BMP280_I2CReadRegisters(bmp->address, BMP280_REG_PRESS_MSB, data, 6))
        return 0;

    adc_P = ((int32_t)data[0] << 12) |
            ((int32_t)data[1] << 4)  |
            ((int32_t)data[2] >> 4);

    adc_T = ((int32_t)data[3] << 12) |
            ((int32_t)data[4] << 4)  |
            ((int32_t)data[5] >> 4);

    temp_x100 = compensate_temperature(adc_T);
    pressure_x256 = compensate_pressure(adc_P);

    bmp->temperature_C = temp_x100 / 100.0f;
    bmp->pressure_Pa = pressure_x256 / 256.0f;
    bmp->pressure_hPa = bmp->pressure_Pa / 100.0f;

    bmp->altitude_m = 44330.0f *
        (1.0f - powf(bmp->pressure_hPa / bmp->sea_level_hPa, 0.19029495f));

    return 1;
}
