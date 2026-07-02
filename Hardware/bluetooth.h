#ifndef __BLUETOOTH_H
#define __BLUETOOTH_H

#include "stm32f10x.h"

#ifndef BLUETOOTH_OK
#define BLUETOOTH_OK                     0U
#endif

#ifndef BLUETOOTH_ERR
#define BLUETOOTH_ERR                    1U
#endif

/* Lightweight frame protocol:
 *   SOF1 | SOF2 | LEN | SEQ | CMD | DATA... | CHK
 *   LEN  = number of bytes from SEQ to the last DATA byte
 *   CHK  = low 8-bit sum of LEN + SEQ + CMD + DATA...
 */
#define BLUETOOTH_FRAME_SOF1             0x5AU
#define BLUETOOTH_FRAME_SOF2             0xA5U

#define BLUETOOTH_MAX_DATA_LEN           140U
#define BLUETOOTH_RX_BYTE_BUFFER_SIZE     128U
#define BLUETOOTH_FRAME_QUEUE_SIZE       4U
#define BLUETOOTH_RX_TIMEOUT_MS          3000U

/* Optional STATE pin handling.
 * Default wiring suggestion:
 *   STATE -> PB4
 *   EN/KEY -> PA1
 *
 * If your board does not connect these pins, set the corresponding
 * USE macro to 0U.
 */
#define BLUETOOTH_USE_STATE_PIN          1U
#define BLUETOOTH_STATE_PORT             GPIOB
#define BLUETOOTH_STATE_PIN              GPIO_Pin_4

#define BLUETOOTH_USE_EN_PIN             0U
#define BLUETOOTH_EN_PORT                GPIOA
#define BLUETOOTH_EN_PIN                 GPIO_Pin_1

typedef enum
{
    BLUETOOTH_LINK_UNKNOWN = 0U,
    BLUETOOTH_LINK_DISCONNECTED = 1U,
    BLUETOOTH_LINK_CONNECTED = 2U
} Bluetooth_LinkState_t;

typedef struct
{
    uint8_t Seq;
    uint8_t Cmd;
    uint8_t DataLen;
    uint8_t Data[BLUETOOTH_MAX_DATA_LEN];
} Bluetooth_Frame_t;

void Bluetooth_Init(void);
void Bluetooth_Task(void);

void Bluetooth_SendFrame(uint8_t Cmd, const uint8_t *Data, uint8_t DataLen);
void Bluetooth_SendFrameWithSeq(uint8_t Seq, uint8_t Cmd, const uint8_t *Data, uint8_t DataLen);
void Bluetooth_SendRaw(const uint8_t *Data, uint8_t DataLen);

uint8_t Bluetooth_GetFrame(Bluetooth_Frame_t *Frame);
void Bluetooth_ClearFrame(Bluetooth_Frame_t *Frame);
Bluetooth_LinkState_t Bluetooth_GetLinkState(void);
uint8_t Bluetooth_IsConnected(void);

void Bluetooth_SetEnKey(uint8_t Enable);

extern volatile uint8_t Bluetooth_RxActivityFlag;

#endif
