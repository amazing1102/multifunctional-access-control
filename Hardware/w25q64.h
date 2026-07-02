#ifndef __W25Q64_H
#define __W25Q64_H

#include "stm32f10x.h"

#define W25Q64_SECTOR_SIZE       4096U
#define W25Q64_PAGE_SIZE         256U

#ifndef MI_OK
#define MI_OK                    0U
#endif

#ifndef MI_ERR
#define MI_ERR                   1U
#endif

void W25Q64_Init(void);
uint8_t W25Q64_ReadSR1(void);
uint8_t W25Q64_WaitBusy(void);
void W25Q64_WriteEnable(void);
void W25Q64_WriteDisable(void);
uint8_t W25Q64_ReadID(uint8_t *ManufacturerID, uint8_t *MemoryType, uint8_t *CapacityID);
void W25Q64_ReadBytes(uint32_t Address, uint8_t *Buffer, uint32_t Length);
uint8_t W25Q64_EraseSector(uint32_t Address);
uint8_t W25Q64_EraseChip(void);
uint8_t W25Q64_WriteBytes(uint32_t Address, const uint8_t *Buffer, uint32_t Length);

#endif
