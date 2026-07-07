// delay.c
#include <xc.h>
#include <stdint.h>
#include "delay.h"

#define SYS_FREQ 40000000UL

void __delay_us(uint32_t us)
{
    uint32_t ticks = (SYS_FREQ / 2000000UL) * us;

    _CP0_SET_COUNT(0);
    while (_CP0_GET_COUNT() < ticks);
}

void __delay_ms(uint32_t ms)
{
    while (ms--)
    {
        __delay_us(1000);
    }
}