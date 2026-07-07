#include <xc.h>
#include <stdint.h>
#include <math.h>
#include "definitions.h"
#include "MPU6050.h"

#define MPU6050_REG_SMPLRT_DIV    0x19
#define MPU6050_REG_CONFIG        0x1A
#define MPU6050_REG_GYRO_CONFIG   0x1B
#define MPU6050_REG_ACCEL_CONFIG  0x1C
#define MPU6050_REG_ACCEL_XOUT_H  0x3B
#define MPU6050_REG_PWR_MGMT_1    0x6B
#define MPU6050_REG_WHO_AM_I      0x75

#define RAD_TO_DEG 57.2957795f

static uint8_t MPU6050_I2CWriteRegister(uint8_t devAddr, uint8_t regAddr, uint8_t data)
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

static uint8_t MPU6050_I2CReadRegisters(uint8_t devAddr, uint8_t regAddr, uint8_t *buffer, uint8_t length)
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

static uint8_t MPU6050_I2CReadRegister(uint8_t devAddr, uint8_t regAddr, uint8_t *data)
{
    return MPU6050_I2CReadRegisters(devAddr, regAddr, data, 1);
}

static int16_t make_int16(uint8_t high, uint8_t low)
{
    return (int16_t)((high << 8) | low);
}

static float select_axis(float roll, float pitch, uint8_t source)
{
    if(source == MPU_AXIS_PITCH)
        return pitch;

    return roll;
}

static float wrap_angle(float angle)
{
    while(angle > 180.0f) angle -= 360.0f;
    while(angle < -180.0f) angle += 360.0f;
    return angle;
}

uint8_t MPU6050_WhoAmI(void)
{
    uint8_t id = 0;

    if(!MPU6050_I2CReadRegister(MPU6050_ADDR, MPU6050_REG_WHO_AM_I, &id))
        return 0;

    return id;
}

uint8_t MPU6050_Init(void)
{
    uint8_t id;

    id = MPU6050_WhoAmI();

    if(!(id == 0x68 || id == 0x70))
        return 0;

    // Wake device
    if(!MPU6050_I2CWriteRegister(MPU6050_ADDR, MPU6050_REG_PWR_MGMT_1, 0x00))
        return 0;

    // Sample rate divider
    MPU6050_I2CWriteRegister(MPU6050_ADDR, MPU6050_REG_SMPLRT_DIV, 0x07);

    // Digital low-pass filter
    MPU6050_I2CWriteRegister(MPU6050_ADDR, MPU6050_REG_CONFIG, 0x03);

    // Gyro full scale: +/-250 degrees/sec
    MPU6050_I2CWriteRegister(MPU6050_ADDR, MPU6050_REG_GYRO_CONFIG, 0x00);

    // Accel full scale: +/-2g
    MPU6050_I2CWriteRegister(MPU6050_ADDR, MPU6050_REG_ACCEL_CONFIG, 0x00);

    return 1;
}

uint8_t MPU6050_ReadRaw(MPU6050_t *imu)
{
    uint8_t data[14];

    if(!MPU6050_I2CReadRegisters(MPU6050_ADDR, MPU6050_REG_ACCEL_XOUT_H, data, 14))
        return 0;

    imu->ax_raw = make_int16(data[0], data[1]);
    imu->ay_raw = make_int16(data[2], data[3]);
    imu->az_raw = make_int16(data[4], data[5]);

    imu->gx_raw = make_int16(data[8], data[9]);
    imu->gy_raw = make_int16(data[10], data[11]);
    imu->gz_raw = make_int16(data[12], data[13]);

    return 1;
}

uint8_t MPU6050_Update(MPU6050_t *imu, float dt_seconds)
{
    float mapped_roll;
    float mapped_pitch;

    if(!MPU6050_ReadRaw(imu))
        return 0;

    // +/-2g accel scale
    imu->ax_g = imu->ax_raw / 16384.0f;
    imu->ay_g = imu->ay_raw / 16384.0f;
    imu->az_g = imu->az_raw / 16384.0f;

    // +/-250 dps gyro scale
    imu->gx_dps = imu->gx_raw / 131.0f;
    imu->gy_dps = imu->gy_raw / 131.0f;
    imu->gz_dps = imu->gz_raw / 131.0f;

    /*
       Raw tilt angles for your board orientation.
       These are then remapped using the mounting defines in MPU6050.h.
    */
    imu->raw_roll = atan2f(imu->ax_g, imu->az_g) * RAD_TO_DEG;

    imu->raw_pitch = atan2f(imu->ay_g,
                            sqrtf((imu->ax_g * imu->ax_g) +
                                  (imu->az_g * imu->az_g))) * RAD_TO_DEG;

    // Yaw is gyro integration only. It will drift because MPU6050 has no magnetometer.
    imu->raw_yaw += imu->gz_dps * dt_seconds;
    imu->raw_yaw = wrap_angle(imu->raw_yaw);

    mapped_roll = select_axis(imu->raw_roll, imu->raw_pitch, MPU_ROLL_SOURCE);
    mapped_pitch = select_axis(imu->raw_roll, imu->raw_pitch, MPU_PITCH_SOURCE);

    imu->roll = mapped_roll * MPU_ROLL_SIGN;
    imu->pitch = mapped_pitch * MPU_PITCH_SIGN;
    imu->yaw = wrap_angle(imu->raw_yaw * MPU_YAW_SIGN);

    return 1;
}
