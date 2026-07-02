#ifndef __SERVO_H
#define __SERVO_H

#include "stm32f10x.h"

void Servo_Init(void);
void Servo_SetPulseUs(uint16_t PulseUs);
void Servo_SetAngle(uint8_t Angle);
void Servo_Lock(void);
void Servo_Unlock(void);

#endif
