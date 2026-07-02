#ifndef __AS608_H
#define __AS608_H

#include "stm32f10x.h"

#define AS608_OK                         0x00U
#define AS608_TIMEOUT                    0xFEU
#define AS608_INVALID                    0xFFU

#define AS608_DEFAULT_ADDRESS            0xFFFFFFFFUL
#define AS608_DEFAULT_PASSWORD           0x00000000UL
#define AS608_BAUDRATE_DEFAULT           57600U

#define AS608_PID_COMMAND                0x01U
#define AS608_PID_DATA                   0x02U
#define AS608_PID_ACK                    0x07U
#define AS608_PID_ENDDATA                0x08U

#define AS608_CMD_GET_IMAGE              0x01U
#define AS608_CMD_GEN_CHAR               0x02U
#define AS608_CMD_MATCH                  0x03U
#define AS608_CMD_SEARCH                 0x04U
#define AS608_CMD_REG_MODEL              0x05U
#define AS608_CMD_STORE_CHAR             0x06U
#define AS608_CMD_LOAD_CHAR              0x07U
#define AS608_CMD_UP_CHAR                0x08U
#define AS608_CMD_DOWN_CHAR              0x09U
#define AS608_CMD_UP_IMAGE               0x0AU
#define AS608_CMD_DOWN_IMAGE             0x0BU
#define AS608_CMD_DELETE_CHAR            0x0CU
#define AS608_CMD_EMPTY                  0x0DU
#define AS608_CMD_WRITE_REG              0x0EU
#define AS608_CMD_READ_SYS_PARA          0x0FU
#define AS608_CMD_ENROLL                 0x10U
#define AS608_CMD_IDENTIFY               0x11U
#define AS608_CMD_SET_PWD                0x12U
#define AS608_CMD_VERIFY_PWD             0x13U
#define AS608_CMD_GET_RANDOM_CODE        0x14U
#define AS608_CMD_SET_ADDR               0x15U
#define AS608_CMD_READ_INF_PAGE          0x16U
#define AS608_CMD_PORT_CONTROL           0x17U
#define AS608_CMD_WRITE_NOTEPAD          0x18U
#define AS608_CMD_READ_NOTEPAD           0x19U
#define AS608_CMD_BURN_CODE              0x1AU
#define AS608_CMD_HIGH_SPEED_SEARCH      0x1BU
#define AS608_CMD_GEN_BIN_IMAGE          0x1CU
#define AS608_CMD_VALID_TEMPLATE_NUM     0x1DU

#define AS608_BUFFER_1                   0x01U
#define AS608_BUFFER_2                   0x02U

typedef struct
{
    uint16_t StatusReg;
    uint16_t SensorType;
    uint16_t Capacity;
    uint16_t SecurityLevel;
    uint32_t DeviceAddress;
    uint16_t DataPacketSize;
    uint16_t BaudRateSetting;
} AS608_SysPara_t;

void AS608_Init(void);

uint8_t AS608_VerifyPassword(uint32_t Password, uint32_t *Address);
uint8_t AS608_Handshake(uint32_t *Address);
uint8_t AS608_SetPassword(uint32_t Password);
uint8_t AS608_SetChipAddr(uint32_t NewAddress);

uint8_t AS608_ReadSysPara(AS608_SysPara_t *Para);

uint8_t AS608_GetImage(void);
uint8_t AS608_Image2Tz(uint8_t BufferID);
uint8_t AS608_Compare(void);
uint8_t AS608_Search(uint8_t BufferID, uint16_t StartPage, uint16_t PageNum, uint16_t *PageID, uint16_t *Score);
uint8_t AS608_RegModel(void);
uint8_t AS608_StoreModel(uint8_t BufferID, uint16_t PageID);
uint8_t AS608_DeleteModel(uint16_t PageID, uint16_t Num);
uint8_t AS608_Empty(void);

uint8_t AS608_AutoEnroll(uint16_t *PageID);
uint8_t AS608_Identify(uint16_t *PageID, uint16_t *Score);
uint8_t AS608_HighSpeedSearch(uint8_t BufferID, uint16_t StartPage, uint16_t PageNum, uint16_t *PageID, uint16_t *Score);
uint8_t AS608_ValidTemplateNum(uint16_t *Count);

uint8_t AS608_Enroll(uint16_t PageID);

#endif
