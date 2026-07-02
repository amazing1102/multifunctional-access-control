#ifndef __BUZZER_H
#define __BUZZER_H

#include "stm32f10x.h"

void Buzzer_Init(void);
void Buzzer_Off(void);
void Buzzer_SetFrequency(uint16_t FrequencyHz);
void Buzzer_Beep(uint16_t FrequencyHz, uint16_t DurationMs);

#endif
