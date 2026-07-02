#ifndef __LED_H
#define __LED_H

#include "stm32f10x.h"

void LED_Init(void);
void LED_RedOn(void);
void LED_RedOff(void);
void LED_GreenOn(void);
void LED_GreenOff(void);
void LED_AllOff(void);
void LED_ShowFail(void);
void LED_RedBlink(uint8_t Times, uint32_t OnMs, uint32_t OffMs);
void LED_ShowSuccess(void);
void LED_ShowLocked(void);
void LED_ShowUnlocked(void);

#endif
