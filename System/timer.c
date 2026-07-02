#include "timer.h"
#include "keypad.h"
#include "misc.h"

/*
 * TIM3 is used as a 10 ms system tick for keypad scanning.
 * The interrupt stays lightweight: it only clears the update flag
 * and calls the keypad debounce state machine.
 */

volatile uint32_t SystemTicks = 0U;

static uint32_t Timer_GetTIM3Clock(void)
{
    RCC_ClocksTypeDef RCC_ClocksStatus;

    RCC_GetClocksFreq(&RCC_ClocksStatus);

    if ((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_CFGR_PPRE1_DIV1)
    {
        return RCC_ClocksStatus.PCLK1_Frequency;
    }

    return RCC_ClocksStatus.PCLK1_Frequency * 2U;
}

void Timer_Init(void)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;
    uint32_t TimerClock;
    uint16_t Prescaler;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);

    TimerClock = Timer_GetTIM3Clock();
    Prescaler = (uint16_t)(TimerClock / 10000U);
    if (Prescaler == 0U)
    {
        Prescaler = 1U;
    }
    else
    {
        Prescaler = (uint16_t)(Prescaler - 1U);
    }

    TIM_TimeBaseInitStructure.TIM_Period = 100U - 1U;
    TIM_TimeBaseInitStructure.TIM_Prescaler = Prescaler;
    TIM_TimeBaseInitStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInitStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &TIM_TimeBaseInitStructure);

    TIM_ClearFlag(TIM3, TIM_FLAG_Update);
    TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel = TIM3_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2U;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2U;
    NVIC_Init(&NVIC_InitStructure);

    TIM_Cmd(TIM3, ENABLE);
}

void Timer_Stop(void)
{
    TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);
    TIM_Cmd(TIM3, DISABLE);
    TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
}

void TIM3_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM3, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
        SystemTicks += 10U;
        Keypad_Scan10ms();
    }
}

uint32_t GetSystemTick(void)
{
    return SystemTicks;
}
