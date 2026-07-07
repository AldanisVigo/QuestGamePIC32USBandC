#ifndef MPU6050_H
#define MPU6050_H

#include <stdint.h>

#define MPU6050_ADDR 0x68

#define MPU_AXIS_ROLL   0
#define MPU_AXIS_PITCH  1

/*
   Mounting correction
*/
#define MPU_ROLL_SOURCE     MPU_AXIS_PITCH
#define MPU_ROLL_SIGN       1.0f

#define MPU_PITCH_SOURCE    MPU_AXIS_ROLL
#define MPU_PITCH_SIGN      -1.0f

#define MPU_YAW_SIGN        -1.0f

typedef struct
{
    int16_t ax_raw;
    int16_t ay_raw;
    int16_t az_raw;

    int16_t gx_raw;
    int16_t gy_raw;
    int16_t gz_raw;

    float ax_g;
    float ay_g;
    float az_g;

    float gx_dps;
    float gy_dps;
    float gz_dps;

    float raw_roll;
    float raw_pitch;
    float raw_yaw;

    float roll;
    float pitch;
    float yaw;
} MPU6050_t;

uint8_t MPU6050_Init(void);
uint8_t MPU6050_WhoAmI(void);
uint8_t MPU6050_ReadRaw(MPU6050_t *imu);
uint8_t MPU6050_Update(MPU6050_t *imu, float dt_seconds);

#endif