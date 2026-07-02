#include "led.h"
#include "delay.h"

/*
 * Dual-color status LED:
 *   Red   -> PA11
 *   Green -> PA12
 *
 * The outputs are driven as open-drain GPIOs with active-low lighting.
 * That matches a common-anode / pulled-up external LED module:
 *   GPIO low  -> LED on
 *   GPIO high -> LED off
 */

#define LED_GPIO_RCC              RCC_APB2Periph_GPIOA
#define LED_GPIO_PORT             GPIOA
#define LED_RED_PIN               GPIO_Pin_11
#define LED_GREEN_PIN             GPIO_Pin_12

static void LED_WriteRed(FunctionalState State)
{
    if (State != DISABLE)
    {
        GPIO_ResetBits(LED_GPIO_PORT, LED_RED_PIN);
    }
    else
    {
        GPIO_SetBits(LED_GPIO_PORT, LED_RED_PIN);
    }
}

static void LED_WriteGreen(FunctionalState State)
{
    if (State != DISABLE)
    {
        GPIO_ResetBits(LED_GPIO_PORT, LED_GREEN_PIN);
    }
    else
    {
        GPIO_SetBits(LED_GPIO_PORT, LED_GREEN_PIN);
    }
}

void LED_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(LED_GPIO_RCC, ENABLE);

    GPIO_InitStructure.GPIO_Pin = LED_RED_PIN | LED_GREEN_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LED_GPIO_PORT, &GPIO_InitStructure);

    LED_AllOff();
}

void LED_RedOn(void)
{
    LED_WriteRed(ENABLE);
}

void LED_RedOff(void)
{
    LED_WriteRed(DISABLE);
}

void LED_GreenOn(void)
{
    LED_WriteGreen(ENABLE);
}

void LED_GreenOff(void)
{
    LED_WriteGreen(DISABLE);
}

void LED_AllOff(void)
{
    LED_RedOff();
    LED_GreenOff();
}

void LED_ShowFail(void)
{
    LED_RedOn();
    LED_GreenOff();
}

void LED_RedBlink(uint8_t Times, uint32_t OnMs, uint32_t OffMs)
{
    uint8_t i;

    LED_GreenOff();
    for (i = 0U; i < Times; i++)
    {
        LED_RedOn();
        delay_ms(OnMs);
        LED_RedOff();
        if ((i + 1U) < Times)
        {
            delay_ms(OffMs);
        }
    }
}

void LED_ShowSuccess(void)
{
    LED_RedOff();
    LED_GreenOn();
}

void LED_ShowLocked(void)
{
    LED_ShowFail();
}

void LED_ShowUnlocked(void)
{
    LED_ShowSuccess();
}
