#include "my_spi.h"

/*
 * SPI1 wiring from the project design:
 *   SCK  -> PA5
 *   MISO -> PA6
 *   MOSI -> PA7
 *
 * This file only handles the shared SPI1 bus configuration and a
 * generic byte exchange helper. Chip-select handling belongs to
 * higher-level device drivers such as RC522 and W25Q64.
 */

#define MY_SPI_PORT_RCC            RCC_APB2Periph_GPIOA
#define MY_SPI_PERIPH_RCC          RCC_APB2Periph_SPI1
#define MY_SPI_PORT                GPIOA
#define MY_SPI_SCK_PIN             GPIO_Pin_5
#define MY_SPI_MISO_PIN            GPIO_Pin_6
#define MY_SPI_MOSI_PIN            GPIO_Pin_7
#define MY_SPI_INSTANCE             SPI1
#define MY_SPI_TIMEOUT              20000U

static uint8_t MySPI_WaitFlag(uint16_t Flag, FlagStatus Expected)
{
    uint32_t Timeout = MY_SPI_TIMEOUT;

    while (SPI_I2S_GetFlagStatus(MY_SPI_INSTANCE, Flag) != Expected)
    {
        if (Timeout-- == 0U)
        {
            return 0U;
        }
    }

    return 1U;
}

void MySPI_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    SPI_InitTypeDef SPI_InitStructure;

    RCC_APB2PeriphClockCmd(MY_SPI_PORT_RCC | MY_SPI_PERIPH_RCC, ENABLE);

    GPIO_InitStructure.GPIO_Pin = MY_SPI_SCK_PIN | MY_SPI_MOSI_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(MY_SPI_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = MY_SPI_MISO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(MY_SPI_PORT, &GPIO_InitStructure);

    SPI_I2S_DeInit(MY_SPI_INSTANCE);

    SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8;
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_InitStructure.SPI_CRCPolynomial = 7U;
    SPI_Init(MY_SPI_INSTANCE, &SPI_InitStructure);

    SPI_NSSInternalSoftwareConfig(MY_SPI_INSTANCE, SPI_NSSInternalSoft_Set);
    SPI_Cmd(MY_SPI_INSTANCE, ENABLE);
}

uint8_t MySPI_SwapByte(uint8_t Byte)
{
    if (MySPI_WaitFlag(SPI_I2S_FLAG_TXE, SET) == 0U)
    {
        return 0xFFU;
    }

    SPI_I2S_SendData(MY_SPI_INSTANCE, Byte);

    if (MySPI_WaitFlag(SPI_I2S_FLAG_RXNE, SET) == 0U)
    {
        return 0xFFU;
    }

    Byte = (uint8_t)SPI_I2S_ReceiveData(MY_SPI_INSTANCE);

    if (MySPI_WaitFlag(SPI_I2S_FLAG_BSY, RESET) == 0U)
    {
        return 0xFFU;
    }

    return Byte;
}
