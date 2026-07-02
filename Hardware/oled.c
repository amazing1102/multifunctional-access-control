#include "stm32f10x.h"
#include "oled.h"
#include "oled_font.h"
#include "oled_cn_font.h"

/*
 * Project wiring from the design document:
 *   OLED 0.96 inch SSD1306, I2C1
 *   SCL -> PB6, SDA -> PB7, VCC -> 3.3 V, GND -> GND
 */
#define OLED_I2C                       I2C1
#define OLED_I2C_RCC                   RCC_APB1Periph_I2C1
#define OLED_GPIO_RCC                  RCC_APB2Periph_GPIOB
#define OLED_GPIO_PORT                 GPIOB
#define OLED_SCL_PIN                   GPIO_Pin_6
#define OLED_SDA_PIN                   GPIO_Pin_7

#define OLED_I2C_ADDRESS               0x78U
#define OLED_CONTROL_COMMAND           0x00U
#define OLED_CONTROL_DATA              0x40U
#define OLED_I2C_TIMEOUT               20000U

static void OLED_DelayCycles(volatile uint32_t Count)
{
    while (Count-- != 0U)
    {
    }
}

static void OLED_BusRecovery(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    uint8_t i;

    RCC_APB2PeriphClockCmd(OLED_GPIO_RCC, ENABLE);

    GPIO_InitStructure.GPIO_Pin = OLED_SCL_PIN | OLED_SDA_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(OLED_GPIO_PORT, &GPIO_InitStructure);

    GPIO_SetBits(OLED_GPIO_PORT, OLED_SCL_PIN | OLED_SDA_PIN);
    OLED_DelayCycles(50U);

    for (i = 0U; i < 9U; i++)
    {
        GPIO_ResetBits(OLED_GPIO_PORT, OLED_SCL_PIN);
        OLED_DelayCycles(50U);
        GPIO_SetBits(OLED_GPIO_PORT, OLED_SCL_PIN);
        OLED_DelayCycles(50U);
    }

    GPIO_ResetBits(OLED_GPIO_PORT, OLED_SDA_PIN);
    OLED_DelayCycles(50U);
    GPIO_SetBits(OLED_GPIO_PORT, OLED_SCL_PIN);
    OLED_DelayCycles(50U);
    GPIO_SetBits(OLED_GPIO_PORT, OLED_SDA_PIN);
    OLED_DelayCycles(50U);
}

static uint32_t OLED_DecodeUTF8(const uint8_t *String, uint32_t *Consumed)
{
    uint8_t Byte0;
    uint8_t Byte1;
    uint8_t Byte2;
    uint8_t Byte3;

    if ((String == 0) || (Consumed == 0))
    {
        if (Consumed != 0)
        {
            *Consumed = 0U;
        }
        return (uint32_t)'?';
    }

    Byte0 = String[0];
    if (Byte0 < 0x80U)
    {
        *Consumed = 1U;
        return (uint32_t)Byte0;
    }

    if ((Byte0 & 0xE0U) == 0xC0U)
    {
        Byte1 = String[1];
        if ((Byte1 & 0xC0U) == 0x80U)
        {
            *Consumed = 2U;
            return (uint32_t)(((uint32_t)(Byte0 & 0x1FU) << 6) |
                               (uint32_t)(Byte1 & 0x3FU));
        }
    }
    else if ((Byte0 & 0xF0U) == 0xE0U)
    {
        Byte1 = String[1];
        Byte2 = String[2];
        if (((Byte1 & 0xC0U) == 0x80U) && ((Byte2 & 0xC0U) == 0x80U))
        {
            *Consumed = 3U;
            return (uint32_t)(((uint32_t)(Byte0 & 0x0FU) << 12) |
                               ((uint32_t)(Byte1 & 0x3FU) << 6) |
                               (uint32_t)(Byte2 & 0x3FU));
        }
    }
    else if ((Byte0 & 0xF8U) == 0xF0U)
    {
        Byte1 = String[1];
        Byte2 = String[2];
        Byte3 = String[3];
        if (((Byte1 & 0xC0U) == 0x80U) &&
            ((Byte2 & 0xC0U) == 0x80U) &&
            ((Byte3 & 0xC0U) == 0x80U))
        {
            *Consumed = 4U;
            return (uint32_t)(((uint32_t)(Byte0 & 0x07U) << 18) |
                               ((uint32_t)(Byte1 & 0x3FU) << 12) |
                               ((uint32_t)(Byte2 & 0x3FU) << 6) |
                               (uint32_t)(Byte3 & 0x3FU));
        }
    }

    *Consumed = 1U;
    return (uint32_t)'?';
}

static const OLED_CN_Glyph_t *OLED_FindChineseGlyph(uint32_t CodePoint)
{
    uint16_t i;

    for (i = 0U; i < OLED_CN_GLYPH_COUNT; i++)
    {
        if ((uint32_t)OLED_CN_GlyphTable[i].CodePoint == CodePoint)
        {
            return &OLED_CN_GlyphTable[i];
        }
    }

    return 0;
}

static void OLED_ShowGlyph16(uint8_t Line, uint8_t X, const uint8_t *Data)
{
    uint8_t i;

    if ((Data == 0) || (Line < 1U) || (Line > 4U) || (X > 112U))
    {
        return;
    }

    OLED_SetCursor((uint8_t)((Line - 1U) * 2U), X);
    for (i = 0U; i < 16U; i++)
    {
        OLED_WriteData(Data[i]);
    }

    OLED_SetCursor((uint8_t)((Line - 1U) * 2U + 1U), X);
    for (i = 16U; i < 32U; i++)
    {
        OLED_WriteData(Data[i]);
    }
}

static void OLED_ShowChineseCodePoint(uint8_t Line, uint8_t X, uint32_t CodePoint)
{
    const OLED_CN_Glyph_t *Glyph;

    Glyph = OLED_FindChineseGlyph(CodePoint);
    if (Glyph == 0)
    {
        OLED_ShowChar(Line, (uint8_t)(X / 8U + 1U), '?');
        return;
    }

    OLED_ShowGlyph16(Line, X, Glyph->Data);
}

static uint8_t OLED_WaitEvent(uint32_t Event)
{
    uint32_t Timeout = OLED_I2C_TIMEOUT;

    while (I2C_CheckEvent(OLED_I2C, Event) != SUCCESS)
    {
        if (Timeout-- == 0U)
        {
            return 0U;
        }
    }

    return 1U;
}

static uint8_t OLED_WaitNotBusy(void)
{
    uint32_t Timeout = OLED_I2C_TIMEOUT;

    while (I2C_GetFlagStatus(OLED_I2C, I2C_FLAG_BUSY) == SET)
    {
        if (Timeout-- == 0U)
        {
            return 0U;
        }
    }

    return 1U;
}

static void OLED_I2C_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    I2C_InitTypeDef I2C_InitStructure;

    OLED_BusRecovery();

    RCC_APB2PeriphClockCmd(OLED_GPIO_RCC, ENABLE);
    RCC_APB1PeriphClockCmd(OLED_I2C_RCC, ENABLE);

    GPIO_InitStructure.GPIO_Pin = OLED_SCL_PIN | OLED_SDA_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(OLED_GPIO_PORT, &GPIO_InitStructure);

    I2C_DeInit(OLED_I2C);
    I2C_InitStructure.I2C_Mode = I2C_Mode_I2C;
    I2C_InitStructure.I2C_DutyCycle = I2C_DutyCycle_2;
    I2C_InitStructure.I2C_OwnAddress1 = 0x00U;
    I2C_InitStructure.I2C_Ack = I2C_Ack_Enable;
    I2C_InitStructure.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_InitStructure.I2C_ClockSpeed = 400000U;
    I2C_Init(OLED_I2C, &I2C_InitStructure);
    I2C_Cmd(OLED_I2C, ENABLE);
}

static uint8_t OLED_WriteByte(uint8_t Control, uint8_t Byte)
{
    if (OLED_WaitNotBusy() == 0U)
    {
        I2C_GenerateSTOP(OLED_I2C, ENABLE);
        return 0U;
    }

    I2C_GenerateSTART(OLED_I2C, ENABLE);
    if (OLED_WaitEvent(I2C_EVENT_MASTER_MODE_SELECT) == 0U)
    {
        I2C_GenerateSTOP(OLED_I2C, ENABLE);
        return 0U;
    }

    I2C_Send7bitAddress(OLED_I2C, OLED_I2C_ADDRESS, I2C_Direction_Transmitter);
    if (OLED_WaitEvent(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) == 0U)
    {
        I2C_GenerateSTOP(OLED_I2C, ENABLE);
        return 0U;
    }

    I2C_SendData(OLED_I2C, Control);
    if (OLED_WaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED) == 0U)
    {
        I2C_GenerateSTOP(OLED_I2C, ENABLE);
        return 0U;
    }

    I2C_SendData(OLED_I2C, Byte);
    if (OLED_WaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED) == 0U)
    {
        I2C_GenerateSTOP(OLED_I2C, ENABLE);
        return 0U;
    }

    I2C_GenerateSTOP(OLED_I2C, ENABLE);
    return 1U;
}

void OLED_WriteCommand(uint8_t Command)
{
    (void)OLED_WriteByte(OLED_CONTROL_COMMAND, Command);
}

void OLED_WriteData(uint8_t Data)
{
    (void)OLED_WriteByte(OLED_CONTROL_DATA, Data);
}

void OLED_SetCursor(uint8_t Page, uint8_t X)
{
    if ((Page > 7U) || (X > 127U))
    {
        return;
    }

    OLED_WriteCommand((uint8_t)(0xB0U | Page));
    OLED_WriteCommand((uint8_t)(0x10U | ((X & 0xF0U) >> 4U)));
    OLED_WriteCommand((uint8_t)(0x00U | (X & 0x0FU)));
}

void OLED_Clear(void)
{
    uint8_t Page;
    uint8_t X;

    for (Page = 0U; Page < 8U; Page++)
    {
        OLED_SetCursor(Page, 0U);
        for (X = 0U; X < 128U; X++)
        {
            OLED_WriteData(0x00U);
        }
    }
}

void OLED_ClearLine(uint8_t Line)
{
    uint8_t Page;
    uint8_t X;

    if ((Line < 1U) || (Line > 4U))
    {
        return;
    }

    for (Page = (uint8_t)((Line - 1U) * 2U); Page < (uint8_t)((Line - 1U) * 2U + 2U); Page++)
    {
        OLED_SetCursor(Page, 0U);
        for (X = 0U; X < 128U; X++)
        {
            OLED_WriteData(0x00U);
        }
    }
}

void OLED_DisplayOn(void)
{
    OLED_WriteCommand(0x8DU);
    OLED_WriteCommand(0x14U);
    OLED_WriteCommand(0xAFU);
}

void OLED_DisplayOff(void)
{
    OLED_WriteCommand(0x8DU);
    OLED_WriteCommand(0x10U);
    OLED_WriteCommand(0xAEU);
}

void OLED_ShowChar(uint8_t Line, uint8_t Column, char Char)
{
    uint8_t i;
    uint8_t FontIndex;

    if ((Line < 1U) || (Line > 4U) || (Column < 1U) || (Column > 16U))
    {
        return;
    }

    if ((Char < ' ') || (Char > '~'))
    {
        Char = ' ';
    }

    FontIndex = (uint8_t)(Char - ' ');
    OLED_SetCursor((uint8_t)((Line - 1U) * 2U), (uint8_t)((Column - 1U) * 8U));
    for (i = 0U; i < 8U; i++)
    {
        OLED_WriteData(OLED_F8x16[FontIndex][i]);
    }

    OLED_SetCursor((uint8_t)((Line - 1U) * 2U + 1U), (uint8_t)((Column - 1U) * 8U));
    for (i = 0U; i < 8U; i++)
    {
        OLED_WriteData(OLED_F8x16[FontIndex][i + 8U]);
    }
}

void OLED_ShowString(uint8_t Line, uint8_t Column, const char *String)
{
    uint8_t Offset = 0U;

    if (String == 0)
    {
        return;
    }

    while ((String[Offset] != '\0') && ((Column + Offset) <= 16U))
    {
        OLED_ShowChar(Line, (uint8_t)(Column + Offset), String[Offset]);
        Offset++;
    }
}

void OLED_ShowUTF8String(uint8_t Line, uint8_t X, const char *String)
{
    uint32_t Consumed;
    uint32_t CodePoint;

    if ((String == 0) || (Line < 1U) || (Line > 4U) || (X >= 128U))
    {
        return;
    }

    while ((String[0] != '\0') && (X < 128U))
    {
        Consumed = 0U;
        CodePoint = OLED_DecodeUTF8((const uint8_t *)String, &Consumed);
        if (Consumed == 0U)
        {
            break;
        }

        if (CodePoint < 0x80U)
        {
            if ((CodePoint < ' ') || (CodePoint > '~'))
            {
                CodePoint = (uint32_t)' ';
            }

            if (X > 120U)
            {
                break;
            }

            OLED_ShowChar(Line, (uint8_t)(X / 8U + 1U), (char)CodePoint);
            X = (uint8_t)(X + 8U);
        }
        else
        {
            if (X > 112U)
            {
                break;
            }

            OLED_ShowChineseCodePoint(Line, X, CodePoint);
            X = (uint8_t)(X + 16U);
        }

        String += Consumed;
    }
}

static uint32_t OLED_Pow(uint32_t X, uint8_t Y)
{
    uint32_t Result = 1U;

    while (Y-- != 0U)
    {
        Result *= X;
    }

    return Result;
}

void OLED_ShowNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length)
{
    uint8_t i;

    for (i = 0U; i < Length; i++)
    {
        OLED_ShowChar(Line,
                      (uint8_t)(Column + i),
                      (char)((Number / OLED_Pow(10U, (uint8_t)(Length - i - 1U))) % 10U + '0'));
    }
}

void OLED_ShowSignedNum(uint8_t Line, uint8_t Column, int32_t Number, uint8_t Length)
{
    uint8_t i;
    uint32_t AbsNumber;

    if (Number >= 0)
    {
        OLED_ShowChar(Line, Column, '+');
        AbsNumber = (uint32_t)Number;
    }
    else
    {
        OLED_ShowChar(Line, Column, '-');
        AbsNumber = (uint32_t)(-(Number + 1)) + 1U;
    }

    for (i = 0U; i < Length; i++)
    {
        OLED_ShowChar(Line,
                      (uint8_t)(Column + i + 1U),
                      (char)((AbsNumber / OLED_Pow(10U, (uint8_t)(Length - i - 1U))) % 10U + '0'));
    }
}

void OLED_ShowHexNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length)
{
    uint8_t i;
    uint8_t SingleNumber;

    for (i = 0U; i < Length; i++)
    {
        SingleNumber = (uint8_t)((Number / OLED_Pow(16U, (uint8_t)(Length - i - 1U))) % 16U);
        if (SingleNumber < 10U)
        {
            OLED_ShowChar(Line, (uint8_t)(Column + i), (char)(SingleNumber + '0'));
        }
        else
        {
            OLED_ShowChar(Line, (uint8_t)(Column + i), (char)(SingleNumber - 10U + 'A'));
        }
    }
}

void OLED_ShowBinNum(uint8_t Line, uint8_t Column, uint32_t Number, uint8_t Length)
{
    uint8_t i;

    for (i = 0U; i < Length; i++)
    {
        OLED_ShowChar(Line,
                      (uint8_t)(Column + i),
                      (char)((Number / OLED_Pow(2U, (uint8_t)(Length - i - 1U))) % 2U + '0'));
    }
}

void OLED_Init(void)
{
    OLED_DelayCycles(720000U);
    OLED_I2C_Init();

    OLED_WriteCommand(0xAEU);
    OLED_WriteCommand(0xD5U);
    OLED_WriteCommand(0x80U);
    OLED_WriteCommand(0xA8U);
    OLED_WriteCommand(0x3FU);
    OLED_WriteCommand(0xD3U);
    OLED_WriteCommand(0x00U);
    OLED_WriteCommand(0x40U);
    OLED_WriteCommand(0xA1U);
    OLED_WriteCommand(0xC8U);
    OLED_WriteCommand(0xDAU);
    OLED_WriteCommand(0x12U);
    OLED_WriteCommand(0x81U);
    OLED_WriteCommand(0xCFU);
    OLED_WriteCommand(0xD9U);
    OLED_WriteCommand(0xF1U);
    OLED_WriteCommand(0xDBU);
    OLED_WriteCommand(0x30U);
    OLED_WriteCommand(0xA4U);
    OLED_WriteCommand(0xA6U);
    OLED_WriteCommand(0x8DU);
    OLED_WriteCommand(0x14U);
    OLED_WriteCommand(0xAFU);

    OLED_Clear();
}
