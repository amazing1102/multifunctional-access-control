#include "buzzer.h"
#include "delay.h"

/*
 * Buzzer output:
 *   PB8 -> TIM4_CH3
 *
 * The channel is driven with PWM so it can work with either an active buzzer
 * (simple tone on/off) or a passive buzzer (actual pitch generation).
 */

#define BUZZER_GPIO_RCC            RCC_APB2Periph_GPIOB
#define BUZZER_TIM_RCC             RCC_APB1Periph_TIM4
#define BUZZER_GPIO_PORT           GPIOB
#define BUZZER_PWM_PIN             GPIO_Pin_8
#define BUZZER_TIM                 TIM4
#define BUZZER_TIMER_TICK_HZ       1000000U

/*
 * The buzzer module on this board is treated as active-low when idle:
 * keeping PB8 high silences it, while TIM4 takes control only during beeps.
 */
#define BUZZER_IDLE_LEVEL_HIGH      1U

static void Buzzer_SetPinMode(uint8_t AlternateFunction)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    GPIO_InitStructure.GPIO_Pin = BUZZER_PWM_PIN;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = (AlternateFunction != 0U) ? GPIO_Mode_AF_PP : GPIO_Mode_Out_PP;
    GPIO_Init(BUZZER_GPIO_PORT, &GPIO_InitStructure);

    if (AlternateFunction == 0U)
    {
#if BUZZER_IDLE_LEVEL_HIGH
        GPIO_SetBits(BUZZER_GPIO_PORT, BUZZER_PWM_PIN);
#else
        GPIO_ResetBits(BUZZER_GPIO_PORT, BUZZER_PWM_PIN);
#endif
    }
}

static uint32_t Buzzer_GetTimerClock(void)
{
    RCC_ClocksTypeDef RCC_Clock;

    RCC_GetClocksFreq(&RCC_Clock);

    if ((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_CFGR_PPRE1_DIV1)
    {
        return RCC_Clock.PCLK1_Frequency;
    }

    return RCC_Clock.PCLK1_Frequency * 2U;
}

void Buzzer_Init(void)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStructure;
    TIM_OCInitTypeDef TIM_OCInitStructure;
    uint32_t TimerClock;
    uint16_t Prescaler;

    RCC_APB2PeriphClockCmd(BUZZER_GPIO_RCC, ENABLE);
    RCC_APB1PeriphClockCmd(BUZZER_TIM_RCC, ENABLE);

    Buzzer_SetPinMode(1U);

    TIM_DeInit(BUZZER_TIM);

    TimerClock = Buzzer_GetTimerClock();
    Prescaler = (uint16_t)(TimerClock / BUZZER_TIMER_TICK_HZ);
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
    TIM_TimeBaseInitStructure.TIM_Period = 1000U - 1U;
    TIM_TimeBaseInitStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInit(BUZZER_TIM, &TIM_TimeBaseInitStructure);

    TIM_OCStructInit(&TIM_OCInitStructure);
    TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStructure.TIM_Pulse = 0U;
    TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OC3Init(BUZZER_TIM, &TIM_OCInitStructure);
    TIM_OC3PreloadConfig(BUZZER_TIM, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(BUZZER_TIM, ENABLE);

    TIM_Cmd(BUZZER_TIM, ENABLE);
    Buzzer_Off();
}

void Buzzer_Off(void)
{
    TIM_CCxCmd(BUZZER_TIM, TIM_Channel_3, DISABLE);
    TIM_SetCompare3(BUZZER_TIM, 0U);
    Buzzer_SetPinMode(0U);
}

void Buzzer_SetFrequency(uint16_t FrequencyHz)
{
    uint32_t Period;
    uint32_t Pulse;

    if (FrequencyHz == 0U)
    {
        Buzzer_Off();
        return;
    }

    Buzzer_SetPinMode(1U);
    Period = BUZZER_TIMER_TICK_HZ / FrequencyHz;
    if (Period == 0U)
    {
        Buzzer_Off();
        return;
    }

    if (Period > 0x10000U)
    {
        Period = 0x10000U;
    }

    TIM_SetAutoreload(BUZZER_TIM, (uint16_t)(Period - 1U));
    TIM_SetCounter(BUZZER_TIM, 0U);
    TIM_GenerateEvent(BUZZER_TIM, TIM_EventSource_Update);
    Pulse = Period / 2U;
    if (Pulse == 0U)
    {
        Pulse = 1U;
    }
    TIM_SetCompare3(BUZZER_TIM, (uint16_t)Pulse);
    TIM_CCxCmd(BUZZER_TIM, TIM_Channel_3, ENABLE);
}

void Buzzer_Beep(uint16_t FrequencyHz, uint16_t DurationMs)
{
    Buzzer_SetFrequency(FrequencyHz);
    delay_ms(DurationMs);
    Buzzer_Off();
}
