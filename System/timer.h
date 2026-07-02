#ifndef __TIMER_H
#define __TIMER_H

#include "stm32f10x.h"

extern volatile uint32_t SystemTicks;

void Timer_Init(void);
void Timer_Stop(void);
uint32_t GetSystemTick(void);

#endif
