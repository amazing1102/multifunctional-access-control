#include "delay.h"
#include "iwdg.h"

static u8 fac_us = 0;

void delay_init(void)
{
    SysTick_CLKSourceConfig(SysTick_CLKSource_HCLK_Div8);
    fac_us = (u8)(SystemCoreClock / 8000000U);
    if (fac_us == 0U)
    {
        fac_us = 1U;
    }
}

void delay_us(u32 nus)
{
    u32 temp;
    u32 load = nus * fac_us;

    if (load == 0U)
    {
        return;
    }

    if (load > 0x00FFFFFFU)
    {
        load = 0x00FFFFFFU;
    }

    SysTick->LOAD = load;
    SysTick->VAL = 0x00;
    SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;

    do
    {
        temp = SysTick->CTRL;
    } while ((temp & SysTick_CTRL_ENABLE_Msk) != 0U && (temp & SysTick_CTRL_COUNTFLAG_Msk) == 0U);

    SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;
    SysTick->VAL = 0x00;
}

void delay_ms(u32 nms)
{
    while (nms-- != 0U)
    {
        delay_us(1000U);
        IWDG_Feed();
    }
}
