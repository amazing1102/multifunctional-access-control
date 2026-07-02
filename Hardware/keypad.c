#include "keypad.h"

/*
 * Keypad wiring from the project design:
 *   Rows:    PB0, PB1, PB12, PB13
 *   Columns: PB14, PB15, PA8, PB3
 *
 * PB3 is a default JTAG pin, so JTAG is disabled while keeping SWD enabled.
 * The keypad is scanned by Timer3 every 10 ms and uses software debounce.
 */

static const uint16_t Keypad_RowPins[4] = {
    GPIO_Pin_0,
    GPIO_Pin_1,
    GPIO_Pin_12,
    GPIO_Pin_13
};

static GPIO_TypeDef *const Keypad_RowPorts[4] = {
    GPIOB,
    GPIOB,
    GPIOB,
    GPIOB
};

static const uint16_t Keypad_ColPins[4] = {
    GPIO_Pin_14,
    GPIO_Pin_15,
    GPIO_Pin_8,
    GPIO_Pin_3
};

static GPIO_TypeDef *const Keypad_ColPorts[4] = {
    GPIOB,
    GPIOB,
    GPIOA,
    GPIOB
};

static const uint8_t Keypad_KeyMap[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

volatile uint8_t Keypad_KeyValue = 0U;
volatile uint8_t Keypad_KeyFlag = 0U;

static void Keypad_SetAllColumns(uint8_t Level)
{
    uint8_t i;

    for (i = 0U; i < 4U; i++)
    {
        if (Level != 0U)
        {
            GPIO_SetBits(Keypad_ColPorts[i], Keypad_ColPins[i]);
        }
        else
        {
            GPIO_ResetBits(Keypad_ColPorts[i], Keypad_ColPins[i]);
        }
    }
}

static uint8_t Keypad_ScanRaw(void)
{
    uint8_t col;
    uint8_t row;

    Keypad_SetAllColumns(1U);

    for (col = 0U; col < 4U; col++)
    {
        Keypad_SetAllColumns(1U);
        GPIO_ResetBits(Keypad_ColPorts[col], Keypad_ColPins[col]);

        for (row = 0U; row < 4U; row++)
        {
            if (GPIO_ReadInputDataBit(Keypad_RowPorts[row], Keypad_RowPins[row]) == Bit_RESET)
            {
                Keypad_SetAllColumns(1U);
                return Keypad_KeyMap[row][col];
            }
        }
    }

    Keypad_SetAllColumns(1U);
    return 0U;
}

void Keypad_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO, ENABLE);
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_12 | GPIO_Pin_13;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_14 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    Keypad_SetAllColumns(1U);

    Keypad_KeyValue = 0U;
    Keypad_KeyFlag = 0U;
}

void Keypad_Scan10ms(void)
{
    static uint8_t LastRawKey = 0U;
    static uint8_t StableCount = 0U;
    static uint8_t KeyLatched = 0U;
    uint8_t RawKey;

    RawKey = Keypad_ScanRaw();

    if (KeyLatched != 0U)
    {
        if (RawKey == 0U)
        {
            KeyLatched = 0U;
        }
        return;
    }

    if (RawKey != 0U)
    {
        if (RawKey == LastRawKey)
        {
            if (StableCount < 2U)
            {
                StableCount++;
            }
        }
        else
        {
            LastRawKey = RawKey;
            StableCount = 1U;
        }

        if (StableCount >= 2U)
        {
            if (Keypad_KeyFlag == 0U)
            {
                Keypad_KeyValue = RawKey;
                Keypad_KeyFlag = 1U;
            }
            KeyLatched = 1U;
            StableCount = 0U;
            LastRawKey = 0U;
        }
    }
    else
    {
        LastRawKey = 0U;
        StableCount = 0U;
    }
}
