#ifndef __RC522_H
#define __RC522_H

#include "stm32f10x.h"

#ifndef MI_OK
#define MI_OK                    0U
#endif

#ifndef MI_ERR
#define MI_ERR                   1U
#endif

#define PICC_REQIDL              0x26U
#define PICC_REQALL              0x52U
#define PICC_ANTICOLL            0x93U

void RC522_Init(void);
uint8_t RC522_Request(uint8_t ReqMode, uint8_t *TagType);
uint8_t RC522_Anticoll(uint8_t *SerNum);
uint8_t RC522_ReadCardSerial(uint8_t *SerNum);
uint8_t RC522_Halt(void);

uint8_t RC522_GetVersion(void);
#endif
