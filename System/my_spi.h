#ifndef __MY_SPI_H
#define __MY_SPI_H

#include "stm32f10x.h"

void MySPI_Init(void);
uint8_t MySPI_SwapByte(uint8_t Byte);

#endif
