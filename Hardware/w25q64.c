#include "w25q64.h"
#include "my_spi.h"

/*
 * W25Q64 bus wiring:
 *   SPI1 SCK  -> PA5
 *   SPI1 MISO -> PA6
 *   SPI1 MOSI -> PA7
 *   CS        -> PA15
 *
 * Bus policy:
 *   - PA15 and RC522 CS (PA4) are always driven high in init.
 *   - Selecting W25Q64 forces RC522 CS high first.
 *   - This driver never configures SPI1; System/my_spi.c owns that.
 */

#define W25Q64_GPIO_RCC           RCC_APB2Periph_GPIOA
#define W25Q64_AFIO_RCC           RCC_APB2Periph_AFIO
#define W25Q64_GPIO_PORT          GPIOA
#define W25Q64_CS_PIN             GPIO_Pin_15
#define W25Q64_RC522_CS_PIN       GPIO_Pin_4

#define W25Q64_CMD_WRITE_ENABLE   0x06U
#define W25Q64_CMD_WRITE_DISABLE  0x04U
#define W25Q64_CMD_READ_STATUS1   0x05U
#define W25Q64_CMD_READ_DATA      0x03U
#define W25Q64_CMD_PAGE_PROGRAM   0x02U
#define W25Q64_CMD_SECTOR_ERASE   0x20U
#define W25Q64_CMD_CHIP_ERASE     0xC7U
#define W25Q64_CMD_JEDEC_ID       0x9FU

#define W25Q64_MAX_ADDRESS        0x007FFFFFU

static void W25Q64_Select(void)
{
    GPIO_SetBits(W25Q64_GPIO_PORT, W25Q64_RC522_CS_PIN);
    GPIO_ResetBits(W25Q64_GPIO_PORT, W25Q64_CS_PIN);
}

static void W25Q64_Deselect(void)
{
    GPIO_SetBits(W25Q64_GPIO_PORT, W25Q64_CS_PIN);
}

static void W25Q64_DeselectAll(void)
{
    GPIO_SetBits(W25Q64_GPIO_PORT, W25Q64_RC522_CS_PIN | W25Q64_CS_PIN);
}

static uint8_t W25Q64_TransmitByte(uint8_t Byte)
{
    return MySPI_SwapByte(Byte);
}

static uint8_t W25Q64_IsAddressValid(uint32_t Address)
{
    return (Address <= W25Q64_MAX_ADDRESS) ? 1U : 0U;
}

void W25Q64_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(W25Q64_GPIO_RCC | W25Q64_AFIO_RCC, ENABLE);
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);

    GPIO_InitStructure.GPIO_Pin = W25Q64_CS_PIN | W25Q64_RC522_CS_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(W25Q64_GPIO_PORT, &GPIO_InitStructure);

    W25Q64_DeselectAll();
}

uint8_t W25Q64_ReadSR1(void)
{
    uint8_t Status;

    W25Q64_Select();
    W25Q64_TransmitByte(W25Q64_CMD_READ_STATUS1);
    Status = W25Q64_TransmitByte(0xFFU);
    W25Q64_Deselect();

    return Status;
}

uint8_t W25Q64_WaitBusy(void)
{
    uint32_t Timeout = 1000000U;

    while ((W25Q64_ReadSR1() & 0x01U) != 0U)
    {
        if (Timeout-- == 0U)
        {
            return MI_ERR;
        }
    }

    return MI_OK;
}

void W25Q64_WriteEnable(void)
{
    (void)W25Q64_WaitBusy();

    W25Q64_Select();
    W25Q64_TransmitByte(W25Q64_CMD_WRITE_ENABLE);
    W25Q64_Deselect();
}

void W25Q64_WriteDisable(void)
{
    W25Q64_Select();
    W25Q64_TransmitByte(W25Q64_CMD_WRITE_DISABLE);
    W25Q64_Deselect();
}

uint8_t W25Q64_ReadID(uint8_t *ManufacturerID, uint8_t *MemoryType, uint8_t *CapacityID)
{
    if ((ManufacturerID == 0) || (MemoryType == 0) || (CapacityID == 0))
    {
        return MI_ERR;
    }

    if (W25Q64_WaitBusy() != MI_OK)
    {
        return MI_ERR;
    }

    W25Q64_Select();
    W25Q64_TransmitByte(W25Q64_CMD_JEDEC_ID);
    *ManufacturerID = W25Q64_TransmitByte(0xFFU);
    *MemoryType = W25Q64_TransmitByte(0xFFU);
    *CapacityID = W25Q64_TransmitByte(0xFFU);
    W25Q64_Deselect();

    return MI_OK;
}

void W25Q64_ReadBytes(uint32_t Address, uint8_t *Buffer, uint32_t Length)
{
    uint32_t i;

    if ((Buffer == 0) || (Length == 0U) || (W25Q64_IsAddressValid(Address) == 0U))
    {
        return;
    }

    if (Length > (W25Q64_MAX_ADDRESS - Address + 1U))
    {
        Length = W25Q64_MAX_ADDRESS - Address + 1U;
    }

    if (W25Q64_WaitBusy() != MI_OK)
    {
        return;
    }

    W25Q64_Select();
    W25Q64_TransmitByte(W25Q64_CMD_READ_DATA);
    W25Q64_TransmitByte((uint8_t)(Address >> 16));
    W25Q64_TransmitByte((uint8_t)(Address >> 8));
    W25Q64_TransmitByte((uint8_t)Address);

    for (i = 0U; i < Length; i++)
    {
        Buffer[i] = W25Q64_TransmitByte(0xFFU);
    }

    W25Q64_Deselect();
}

static uint8_t W25Q64_PageProgram(uint32_t Address, const uint8_t *Buffer, uint32_t Length)
{
    uint32_t i;

    if ((Buffer == 0) || (Length == 0U))
    {
        return MI_ERR;
    }

    if (((Address & 0xFFU) + Length) > W25Q64_PAGE_SIZE)
    {
        return MI_ERR;
    }

    if ((W25Q64_WaitBusy() != MI_OK))
    {
        return MI_ERR;
    }

    W25Q64_WriteEnable();

    W25Q64_Select();
    W25Q64_TransmitByte(W25Q64_CMD_PAGE_PROGRAM);
    W25Q64_TransmitByte((uint8_t)(Address >> 16));
    W25Q64_TransmitByte((uint8_t)(Address >> 8));
    W25Q64_TransmitByte((uint8_t)Address);
    for (i = 0U; i < Length; i++)
    {
        W25Q64_TransmitByte(Buffer[i]);
    }
    W25Q64_Deselect();

    return W25Q64_WaitBusy();
}

uint8_t W25Q64_EraseSector(uint32_t Address)
{
    uint32_t SectorAddress;

    if (W25Q64_IsAddressValid(Address) == 0U)
    {
        return MI_ERR;
    }

    SectorAddress = Address & ~(W25Q64_SECTOR_SIZE - 1U);

    if (W25Q64_WaitBusy() != MI_OK)
    {
        return MI_ERR;
    }

    W25Q64_WriteEnable();

    W25Q64_Select();
    W25Q64_TransmitByte(W25Q64_CMD_SECTOR_ERASE);
    W25Q64_TransmitByte((uint8_t)(SectorAddress >> 16));
    W25Q64_TransmitByte((uint8_t)(SectorAddress >> 8));
    W25Q64_TransmitByte((uint8_t)SectorAddress);
    W25Q64_Deselect();

    return W25Q64_WaitBusy();
}

uint8_t W25Q64_EraseChip(void)
{
    if (W25Q64_WaitBusy() != MI_OK)
    {
        return MI_ERR;
    }

    W25Q64_WriteEnable();

    W25Q64_Select();
    W25Q64_TransmitByte(W25Q64_CMD_CHIP_ERASE);
    W25Q64_Deselect();

    return W25Q64_WaitBusy();
}

uint8_t W25Q64_WriteBytes(uint32_t Address, const uint8_t *Buffer, uint32_t Length)
{
    uint32_t SectorAddress;
    uint32_t SectorRemain;
    uint32_t PageRemain;
    uint32_t WriteLength;

    if ((Buffer == 0) || (Length == 0U) || (W25Q64_IsAddressValid(Address) == 0U))
    {
        return MI_ERR;
    }

    if (Length > (W25Q64_MAX_ADDRESS - Address + 1U))
    {
        return MI_ERR;
    }

    while (Length != 0U)
    {
        SectorAddress = Address & ~(W25Q64_SECTOR_SIZE - 1U);
        SectorRemain = W25Q64_SECTOR_SIZE - (Address - SectorAddress);
        WriteLength = (Length < SectorRemain) ? Length : SectorRemain;

        if (W25Q64_EraseSector(SectorAddress) != MI_OK)
        {
            return MI_ERR;
        }

        while (WriteLength != 0U)
        {
            PageRemain = W25Q64_PAGE_SIZE - (Address & (W25Q64_PAGE_SIZE - 1U));
            if (PageRemain > WriteLength)
            {
                PageRemain = WriteLength;
            }

            if (W25Q64_PageProgram(Address, Buffer, PageRemain) != MI_OK)
            {
                return MI_ERR;
            }

            Address += PageRemain;
            Buffer += PageRemain;
            Length -= PageRemain;
            WriteLength -= PageRemain;
        }
    }

    return MI_OK;
}
