#include "rc522.h"
#include "my_spi.h"
#include "delay.h"

/*
 * RC522 bus wiring:
 *   SPI1 SCK  -> PA5
 *   SPI1 MISO -> PA6
 *   SPI1 MOSI -> PA7
 *   NSS/SDA   -> PA4
 *   RST       -> PB5
 *
 * This driver does not initialize SPI1. System/my_spi.c owns the bus
 * configuration, and this file only manages RC522-specific GPIO and protocol.
 */

#define RC522_GPIO_RCC            RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO
#define RC522_GPIO_PORTA          GPIOA
#define RC522_GPIO_PORTB          GPIOB
#define RC522_CS_PIN              GPIO_Pin_4
#define RC522_W25_CS_PIN          GPIO_Pin_15
#define RC522_RST_PIN             GPIO_Pin_5

#define RC522_CMD_IDLE            0x00U
#define RC522_CMD_CALCCRC         0x03U
#define RC522_CMD_TRANSCEIVE      0x0CU

#define RC522_REG_COMM_IE         0x02U
#define RC522_REG_COMM_IRQ        0x04U
#define RC522_REG_DIV_IRQ         0x05U
#define RC522_REG_ERROR           0x06U
#define RC522_REG_STATUS1         0x07U
#define RC522_REG_COMMAND         0x01U
#define RC522_REG_FIFO_DATA       0x09U
#define RC522_REG_FIFO_LEVEL      0x0AU
#define RC522_REG_CONTROL         0x0CU
#define RC522_REG_BIT_FRAMING     0x0DU
#define RC522_REG_MODE            0x11U
#define RC522_REG_TX_MODE         0x12U
#define RC522_REG_RX_MODE         0x13U
#define RC522_REG_TX_CONTROL      0x14U
#define RC522_REG_TX_ASK          0x15U
#define RC522_REG_T_MODE          0x2AU
#define RC522_REG_T_PRESCALER     0x2BU
#define RC522_REG_T_RELOAD_H      0x2CU
#define RC522_REG_T_RELOAD_L      0x2DU
#define RC522_REG_CRC_RESULT_M    0x21U
#define RC522_REG_CRC_RESULT_L    0x22U

#define RC522_MAX_LEN             18U

static void RC522_Select(void)
{
    GPIO_SetBits(RC522_GPIO_PORTA, RC522_W25_CS_PIN);
    GPIO_ResetBits(RC522_GPIO_PORTA, RC522_CS_PIN);
}

static void RC522_Deselect(void)
{
    GPIO_SetBits(RC522_GPIO_PORTA, RC522_CS_PIN);
}

static void RC522_DeselectAll(void)
{
    GPIO_SetBits(RC522_GPIO_PORTA, RC522_CS_PIN | RC522_W25_CS_PIN);
}

static void RC522_RST_Low(void)
{
    GPIO_ResetBits(RC522_GPIO_PORTB, RC522_RST_PIN);
}

static void RC522_RST_High(void)
{
    GPIO_SetBits(RC522_GPIO_PORTB, RC522_RST_PIN);
}

static void RC522_WriteReg(uint8_t Addr, uint8_t Value)
{
    RC522_Select();
    (void)MySPI_SwapByte((uint8_t)((Addr << 1) & 0x7EU));
    (void)MySPI_SwapByte(Value);
    RC522_Deselect();
}

static uint8_t RC522_ReadReg(uint8_t Addr)
{
    uint8_t Value;

    RC522_Select();
    (void)MySPI_SwapByte((uint8_t)(((Addr << 1) & 0x7EU) | 0x80U));
    Value = MySPI_SwapByte(0xFFU);
    RC522_Deselect();

    return Value;
}

static void RC522_SetBitMask(uint8_t Reg, uint8_t Mask)
{
    uint8_t Temp;

    Temp = RC522_ReadReg(Reg);
    RC522_WriteReg(Reg, (uint8_t)(Temp | Mask));
}

static void RC522_ClearBitMask(uint8_t Reg, uint8_t Mask)
{
    uint8_t Temp;

    Temp = RC522_ReadReg(Reg);
    RC522_WriteReg(Reg, (uint8_t)(Temp & (uint8_t)(~Mask)));
}

static void RC522_AntennaOn(void)
{
    uint8_t Temp = RC522_ReadReg(RC522_REG_TX_CONTROL);

    if ((Temp & 0x03U) != 0x03U)
    {
        RC522_SetBitMask(RC522_REG_TX_CONTROL, 0x03U);
    }
}

static uint8_t RC522_CalculateCRC(const uint8_t *Data, uint8_t Length, uint8_t *Result)
{
    uint8_t Irq;
    uint16_t Timeout = 0x1FFFU;
    uint8_t i;

    if ((Data == 0) || (Result == 0) || (Length == 0U))
    {
        return MI_ERR;
    }

    RC522_ClearBitMask(RC522_REG_DIV_IRQ, 0x04U);
    RC522_SetBitMask(RC522_REG_FIFO_LEVEL, 0x80U);
    RC522_WriteReg(RC522_REG_COMMAND, RC522_CMD_IDLE);

    for (i = 0U; i < Length; i++)
    {
        RC522_WriteReg(RC522_REG_FIFO_DATA, Data[i]);
    }

    RC522_WriteReg(RC522_REG_COMMAND, RC522_CMD_CALCCRC);

    do
    {
        Irq = RC522_ReadReg(RC522_REG_DIV_IRQ);
        if (Timeout-- == 0U)
        {
            return MI_ERR;
        }
    } while ((Irq & 0x04U) == 0U);

    Result[0] = RC522_ReadReg(RC522_REG_CRC_RESULT_L);
    Result[1] = RC522_ReadReg(RC522_REG_CRC_RESULT_M);
    RC522_WriteReg(RC522_REG_COMMAND, RC522_CMD_IDLE);

    return MI_OK;
}

static uint8_t RC522_ToCard(uint8_t Command, const uint8_t *SendData, uint8_t SendLen, uint8_t *BackData, uint16_t *BackLen)
{
    uint8_t irqEn = 0U;
    uint8_t waitFor = 0U;
    uint8_t n;
    uint16_t timeout = 0x1FFFU;
    uint8_t i;
    uint8_t lastBits;
    uint8_t error;

    if ((SendData == 0) || (BackLen == 0) || ((Command == RC522_CMD_TRANSCEIVE) && (BackData == 0)))
    {
        return MI_ERR;
    }

    switch (Command)
    {
        case RC522_CMD_TRANSCEIVE:
            irqEn = 0x77U;
            waitFor = 0x30U;
            break;
        case RC522_CMD_CALCCRC:
            irqEn = 0x04U;
            waitFor = 0x04U;
            break;
        default:
            break;
    }

    RC522_WriteReg(RC522_REG_COMM_IE, (uint8_t)(irqEn | 0x80U));
    RC522_ClearBitMask(RC522_REG_COMM_IRQ, 0x80U);
    RC522_SetBitMask(RC522_REG_FIFO_LEVEL, 0x80U);
    RC522_WriteReg(RC522_REG_COMMAND, RC522_CMD_IDLE);

    for (i = 0U; i < SendLen; i++)
    {
        RC522_WriteReg(RC522_REG_FIFO_DATA, SendData[i]);
    }

    RC522_WriteReg(RC522_REG_COMMAND, Command);
    if (Command == RC522_CMD_TRANSCEIVE)
    {
        RC522_SetBitMask(RC522_REG_BIT_FRAMING, 0x80U);
    }

    do
    {
        n = RC522_ReadReg(RC522_REG_COMM_IRQ);
        if (timeout-- == 0U)
        {
            RC522_ClearBitMask(RC522_REG_BIT_FRAMING, 0x80U);
            return MI_ERR;
        }
    } while (((n & waitFor) == 0U) && ((n & 0x01U) == 0U));

    RC522_ClearBitMask(RC522_REG_BIT_FRAMING, 0x80U);

    error = RC522_ReadReg(RC522_REG_ERROR);
    if ((error & 0x1BU) != 0U)
    {
        return MI_ERR;
    }

    if (Command == RC522_CMD_TRANSCEIVE)
    {
        n = RC522_ReadReg(RC522_REG_FIFO_LEVEL);
        lastBits = RC522_ReadReg(RC522_REG_CONTROL) & 0x07U;

        if (lastBits != 0U)
        {
            *BackLen = (uint16_t)(((uint16_t)(n - 1U) * 8U) + lastBits);
        }
        else
        {
            *BackLen = (uint16_t)(n * 8U);
        }

        if (n == 0U)
        {
            return MI_ERR;
        }
        if (n > RC522_MAX_LEN)
        {
            n = RC522_MAX_LEN;
        }

        for (i = 0U; i < n; i++)
        {
            BackData[i] = RC522_ReadReg(RC522_REG_FIFO_DATA);
        }
    }

    return MI_OK;
}

void RC522_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RC522_GPIO_RCC, ENABLE);
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);

    GPIO_InitStructure.GPIO_Pin = RC522_CS_PIN | RC522_W25_CS_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(RC522_GPIO_PORTA, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = RC522_RST_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(RC522_GPIO_PORTB, &GPIO_InitStructure);

    RC522_DeselectAll();
    RC522_RST_High();

    RC522_RST_Low();
    delay_ms(10U);
    RC522_RST_High();
    delay_ms(10U);

    RC522_WriteReg(RC522_REG_COMMAND, RC522_CMD_IDLE);
    RC522_WriteReg(RC522_REG_T_MODE, 0x8DU);
    RC522_WriteReg(RC522_REG_T_PRESCALER, 0x3EU);
    RC522_WriteReg(RC522_REG_T_RELOAD_H, 0x00U);
    RC522_WriteReg(RC522_REG_T_RELOAD_L, 30U);
    RC522_WriteReg(RC522_REG_TX_ASK, 0x40U);
    RC522_WriteReg(RC522_REG_MODE, 0x3DU);
    RC522_WriteReg(RC522_REG_TX_MODE, 0x00U);
    RC522_WriteReg(RC522_REG_RX_MODE, 0x00U);
    RC522_AntennaOn();
}

uint8_t RC522_Request(uint8_t ReqMode, uint8_t *TagType)
{
    uint8_t Status;
    uint16_t BackBits = 0U;
    uint8_t Buf[1];

    if (TagType == 0)
    {
        return MI_ERR;
    }

    RC522_WriteReg(RC522_REG_BIT_FRAMING, 0x07U);

    Buf[0] = ReqMode;
    Status = RC522_ToCard(RC522_CMD_TRANSCEIVE, Buf, 1U, TagType, &BackBits);
    RC522_WriteReg(RC522_REG_BIT_FRAMING, 0x00U);

    if ((Status != MI_OK) || (BackBits != 0x10U))
    {
        return MI_ERR;
    }

    return MI_OK;
}

uint8_t RC522_Anticoll(uint8_t *SerNum)
{
    uint8_t Status;
    uint16_t BackBits = 0U;
    uint8_t Buf[2];
    uint8_t i;
    uint8_t CheckSum = 0U;

    if (SerNum == 0)
    {
        return MI_ERR;
    }

    RC522_WriteReg(RC522_REG_BIT_FRAMING, 0x00U);

    Buf[0] = PICC_ANTICOLL;
    Buf[1] = 0x20U;
    Status = RC522_ToCard(RC522_CMD_TRANSCEIVE, Buf, 2U, SerNum, &BackBits);

    if (Status != MI_OK)
    {
        return MI_ERR;
    }

    if (BackBits != 0x28U)
    {
        return MI_ERR;
    }

    for (i = 0U; i < 4U; i++)
    {
        CheckSum ^= SerNum[i];
    }

    if (CheckSum != SerNum[4])
    {
        return MI_ERR;
    }

    return MI_OK;
}

uint8_t RC522_ReadCardSerial(uint8_t *SerNum)
{
    uint8_t TagType[2];

    if (RC522_Request(PICC_REQIDL, TagType) != MI_OK)
    {
        return MI_ERR;
    }

    return RC522_Anticoll(SerNum);
}

uint8_t RC522_Halt(void)
{
    uint8_t Buf[4];
    uint8_t Status;
    uint16_t BackBits = 0U;

    Buf[0] = 0x50U;
    Buf[1] = 0x00U;

    if (RC522_CalculateCRC(Buf, 2U, &Buf[2]) != MI_OK)
    {
        return MI_ERR;
    }

    RC522_WriteReg(RC522_REG_BIT_FRAMING, 0x00U);
    Status = RC522_ToCard(RC522_CMD_TRANSCEIVE, Buf, 4U, Buf, &BackBits);
    if (Status != MI_OK)
    {
        return MI_ERR;
    }

    return MI_OK;
}

uint8_t RC522_GetVersion(void)
{
    return RC522_ReadReg(0x37U);
}
