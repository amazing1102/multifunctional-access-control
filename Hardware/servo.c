#include "servo.h"

/*
 * Servo output:
 *   TIM2_CH1 -> PA0
 *
 * The timer is configured for a 50 Hz PWM frame with 1 us resolution,
 * so pulse widths can be set precisely for lock/unlock positioning.
 */

#define SERVO_GPIO_RCC             RCC_APB2Periph_GPIOA
#define SERVO_TIM_RCC              RCC_APB1Periph_TIM2
#define SERVO_GPIO_PORT            GPIOA
#define SERVO_PWM_PIN              GPIO_Pin_0
#define SERVO_TIM                  TIM2

#define SERVO_MIN_PULSE_US         500U
#define SERVO_MAX_PULSE_US         2500U
#define SERVO_FRAME_US             20000U

#define SERVO_LOCK_ANGLE           0U
#define SERVO_UNLOCK_ANGLE         90U

static uint32_t Servo_GetTimerClock(void)
{
    RCC_ClocksTypeDef RCC_Clock;

    RCC_GetClocksFreq(&RCC_Clock);

    if ((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_CFGR_PPRE1_DIV1)
    {
        return RCC_Clock.PCLK1_Frequency;
    }

    return RCC_Clock.PCLK1_Frequency * 2U;
}

static uint16_t Servo_ClampPulse(uint32_t PulseUs)
{
    if (PulseUs < SERVO_MIN_PULSE_US)
    {
        return (uint16_t)SERVO_MIN_PULSE_US;
    }

    if (PulseUs > SERVO_MAX_PULSE_US)
    {
        return (uint16_t)SERVO_MAX_PULSE_US;
    }

    return (uint16_t)PulseUs;
}

void Servo_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStructure;
    TIM_OCInitTypeDef TIM_OCInitStructure;
    uint32_t TimerClock;
    uint16_t Prescaler;

    RCC_APB2PeriphClockCmd(SERVO_GPIO_RCC, ENABLE);
    RCC_APB1PeriphClockCmd(SERVO_TIM_RCC, ENABLE);

    GPIO_InitStructure.GPIO_Pin = SERVO_PWM_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(SERVO_GPIO_PORT, &GPIO_InitStructure);

    TIM_DeInit(SERVO_TIM);

    TimerClock = Servo_GetTimerClock();
    Prescaler = (uint16_t)(TimerClock / 1000000U);
    if (Prescaler == 0U)
    {
        Prescaler = 1U;
    }
    else
    {
        Prescaler = (uint16_t)(Prescaler - 1U);
    }

    TIM_TimeBaseInitStructure.TIM_Prescaler = Prescaler;
    TIM_TimeBaseInitStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInitStructure.TIM_Period = SERVO_FRAME_US - 1U;
    TIM_TimeBaseInitStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInit(SERVO_TIM, &TIM_TimeBaseInitStructure);

    TIM_OCStructInit(&TIM_OCInitStructure);
    TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStructure.TIM_Pulse = SERVO_MIN_PULSE_US;
    TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OC1Init(SERVO_TIM, &TIM_OCInitStructure);
    TIM_OC1PreloadConfig(SERVO_TIM, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(SERVO_TIM, ENABLE);

    TIM_Cmd(SERVO_TIM, ENABLE);
    Servo_Lock();
}

void Servo_SetPulseUs(uint16_t PulseUs)
{
    uint16_t ClampedPulse = Servo_ClampPulse(PulseUs);
    TIM_SetCompare1(SERVO_TIM, ClampedPulse);
}

void Servo_SetAngle(uint8_t Angle)
{
    uint32_t PulseUs;

    if (Angle > 180U)
    {
        Angle = 180U;
    }

    PulseUs = SERVO_MIN_PULSE_US +
              ((uint32_t)Angle * (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US)) / 180U;
    Servo_SetPulseUs((uint16_t)PulseUs);
}

void Servo_Lock(void)
{
    Servo_SetAngle(SERVO_LOCK_ANGLE);
}

void Servo_Unlock(void)
{
    Servo_SetAngle(SERVO_UNLOCK_ANGLE);
}
