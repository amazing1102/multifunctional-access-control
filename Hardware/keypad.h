#ifndef __KEYPAD_H
#define __KEYPAD_H

#include "stm32f10x.h"

extern volatile uint8_t Keypad_KeyValue;
extern volatile uint8_t Keypad_KeyFlag;

void Keypad_Init(void);
void Keypad_Scan10ms(void);

#endif
