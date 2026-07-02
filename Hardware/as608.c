#include "as608.h"
#include "misc.h"
#include "timer.h"
#include <string.h>

/*
 * AS608 fingerprint module wiring:
 *   USART1_TX -> PA9
 *   USART1_RX -> PA10
 *
 * Protocol notes from the manual:
 *   - Default UART: 57600 bps, 8 data bits, 2 stop bits, no parity
 *   - Default chip address: 0xFFFFFFFF
 *   - Command packet: EF01 + address + 0x01 + length + command + params + checksum
 *   - Checksum is the low 16 bits of the sum from PacketID through payload
 *
 * Driver policy:
 *   - RX bytes are collected in USART1_IRQHandler only.
 *   - All protocol parsing happens in the foreground with timeouts.
 *   - No blocking dead-wait loops without timeout.
 */

#define AS608_USART                      USART1
#define AS608_GPIO_PORT                  GPIOA
#define AS608_GPIO_RCC                   (RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO)
#define AS608_USART_RCC                  RCC_APB2Periph_USART1
#define AS608_TX_PIN                     GPIO_Pin_9
#define AS608_RX_PIN                     GPIO_Pin_10

#define AS608_RX_BUFFER_SIZE             128U
#define AS608_MAX_PACKET_DATA_LEN        64U
#define AS608_FRAME_TIMEOUT_MS           500U

typedef struct
{
    uint32_t Address;
    uint8_t PacketID;
    uint16_t DataLength;
    uint8_t Data[AS608_MAX_PACKET_DATA_LEN];
} AS608_Packet_t;

static volatile uint8_t AS608_RxBuffer[AS608_RX_BUFFER_SIZE];
static volatile uint8_t AS608_RxHead = 0U;
static volatile uint8_t AS608_RxTail = 0U;
static volatile uint8_t AS608_RxOverrun = 0U;

static uint32_t AS608_DeviceAddress = AS608_DEFAULT_ADDRESS;

static uint8_t AS608_RxPop(uint8_t *Byte)
{
    if ((Byte == 0) || (AS608_RxHead == AS608_RxTail))
    {
        return 0U;
    }

    *Byte = AS608_RxBuffer[AS608_RxTail];
    AS608_RxTail = (uint8_t)((AS608_RxTail + 1U) % AS608_RX_BUFFER_SIZE);
    return 1U;
}

static void AS608_RxPush(uint8_t Byte)
{
    uint8_t NextHead;

    NextHead = (uint8_t)((AS608_RxHead + 1U) % AS608_RX_BUFFER_SIZE);
    if (NextHead == AS608_RxTail)
    {
        AS608_RxOverrun = 1U;
        return;
    }

    AS608_RxBuffer[AS608_RxHead] = Byte;
    AS608_RxHead = NextHead;
}

static void AS608_RxFlush(void)
{
    AS608_RxHead = 0U;
    AS608_RxTail = 0U;
    AS608_RxOverrun = 0U;
}

static uint32_t AS608_GetNow(void)
{
    return GetSystemTick();
}

static uint8_t AS608_TimeExpired(uint32_t Start, uint32_t TimeoutMs)
{
    return (uint8_t)((uint32_t)(AS608_GetNow() - Start) > TimeoutMs);
}

static uint8_t AS608_WaitByte(uint8_t *Byte, uint32_t Start, uint32_t TimeoutMs)
{
    while (AS608_TimeExpired(Start, TimeoutMs) == 0U)
    {
        if (AS608_RxPop(Byte) != 0U)
        {
            return AS608_OK;
        }

        if (AS608_RxOverrun != 0U)
        {
            AS608_RxFlush();
            return AS608_INVALID;
        }
    }

    return AS608_TIMEOUT;
}

static uint8_t AS608_SendByte(uint8_t Byte)
{
    uint32_t Start;

    Start = AS608_GetNow();
    while (USART_GetFlagStatus(AS608_USART, USART_FLAG_TXE) == RESET)
    {
        if (AS608_TimeExpired(Start, AS608_FRAME_TIMEOUT_MS) != 0U)
        {
            return AS608_TIMEOUT;
        }
    }

    USART_SendData(AS608_USART, Byte);
    return AS608_OK;
}

static uint16_t AS608_CalcChecksum(uint8_t PacketID, uint16_t Length, const uint8_t *Payload, uint16_t PayloadLen)
{
    uint32_t Sum;
    uint16_t Index;

    Sum = (uint32_t)PacketID + (uint32_t)((Length >> 8) & 0xFFU) + (uint32_t)(Length & 0xFFU);
    for (Index = 0U; Index < PayloadLen; Index++)
    {
        Sum += Payload[Index];
    }

    return (uint16_t)Sum;
}

static void AS608_ClearPacket(AS608_Packet_t *Packet)
{
    if (Packet == 0)
    {
        return;
    }

    Packet->Address = 0U;
    Packet->PacketID = 0U;
    Packet->DataLength = 0U;
    memset(Packet->Data, 0, sizeof(Packet->Data));
}

static uint8_t AS608_SendCommandPacket(uint8_t Command, const uint8_t *Params, uint16_t ParamLen)
{
    uint8_t Frame[AS608_MAX_PACKET_DATA_LEN + 12U];
    uint16_t PayloadLen;
    uint16_t PacketLen;
    uint16_t TotalLen;
    uint16_t Checksum;
    uint16_t Index;
    uint8_t Result;

    if ((ParamLen > AS608_MAX_PACKET_DATA_LEN) || ((ParamLen > 0U) && (Params == 0)))
    {
        return AS608_INVALID;
    }

    PayloadLen = (uint16_t)(1U + ParamLen);
    PacketLen = (uint16_t)(PayloadLen + 2U);
    TotalLen = (uint16_t)(11U + PayloadLen);

    Frame[0] = 0xEFU;
    Frame[1] = 0x01U;
    Frame[2] = (uint8_t)((AS608_DeviceAddress >> 24) & 0xFFU);
    Frame[3] = (uint8_t)((AS608_DeviceAddress >> 16) & 0xFFU);
    Frame[4] = (uint8_t)((AS608_DeviceAddress >> 8) & 0xFFU);
    Frame[5] = (uint8_t)(AS608_DeviceAddress & 0xFFU);
    Frame[6] = AS608_PID_COMMAND;
    Frame[7] = (uint8_t)((PacketLen >> 8) & 0xFFU);
    Frame[8] = (uint8_t)(PacketLen & 0xFFU);
    Frame[9] = Command;

    for (Index = 0U; Index < ParamLen; Index++)
    {
        Frame[(uint16_t)(10U + Index)] = Params[Index];
    }

    Checksum = AS608_CalcChecksum(AS608_PID_COMMAND, PacketLen, &Frame[9], PayloadLen);
    Frame[(uint16_t)(9U + PayloadLen)] = (uint8_t)((Checksum >> 8) & 0xFFU);
    Frame[(uint16_t)(10U + PayloadLen)] = (uint8_t)(Checksum & 0xFFU);

    for (Index = 0U; Index < TotalLen; Index++)
    {
        Result = AS608_SendByte(Frame[Index]);
        if (Result != AS608_OK)
        {
            return Result;
        }
    }

    return AS608_OK;
}

static uint8_t AS608_ReadPacket(AS608_Packet_t *Packet, uint32_t TimeoutMs)
{
    uint32_t Start;
    uint8_t Byte;
    uint8_t PrevByte;
    uint16_t Length;
    uint16_t PayloadLen;
    uint16_t Checksum;
    uint8_t CheckHigh;
    uint8_t CheckLow;
    uint8_t Index;
    uint8_t SumData[AS608_MAX_PACKET_DATA_LEN + 3U];

    if (Packet == 0)
    {
        return AS608_INVALID;
    }

    AS608_ClearPacket(Packet);
    Start = AS608_GetNow();
    PrevByte = 0U;

    while (AS608_TimeExpired(Start, TimeoutMs) == 0U)
    {
        if (AS608_RxOverrun != 0U)
        {
            AS608_RxFlush();
            return AS608_INVALID;
        }

        if (AS608_RxPop(&Byte) == 0U)
        {
            continue;
        }

        if ((PrevByte == 0xEFU) && (Byte == 0x01U))
        {
            break;
        }

        PrevByte = Byte;
    }

    if (AS608_TimeExpired(Start, TimeoutMs) != 0U)
    {
        return AS608_TIMEOUT;
    }

    if (AS608_WaitByte(&Byte, Start, TimeoutMs) != AS608_OK)
    {
        return AS608_TIMEOUT;
    }
    Packet->Address = ((uint32_t)Byte << 24);

    if (AS608_WaitByte(&Byte, Start, TimeoutMs) != AS608_OK)
    {
        return AS608_TIMEOUT;
    }
    Packet->Address |= ((uint32_t)Byte << 16);

    if (AS608_WaitByte(&Byte, Start, TimeoutMs) != AS608_OK)
    {
        return AS608_TIMEOUT;
    }
    Packet->Address |= ((uint32_t)Byte << 8);

    if (AS608_WaitByte(&Byte, Start, TimeoutMs) != AS608_OK)
    {
        return AS608_TIMEOUT;
    }
    Packet->Address |= (uint32_t)Byte;

    if (AS608_WaitByte(&Byte, Start, TimeoutMs) != AS608_OK)
    {
        return AS608_TIMEOUT;
    }
    Packet->PacketID = Byte;

    if (AS608_WaitByte(&Byte, Start, TimeoutMs) != AS608_OK)
    {
        return AS608_TIMEOUT;
    }
    Length = (uint16_t)((uint16_t)Byte << 8);

    if (AS608_WaitByte(&Byte, Start, TimeoutMs) != AS608_OK)
    {
        return AS608_TIMEOUT;
    }
    Length |= (uint16_t)Byte;

    if (Length < 2U)
    {
        return AS608_INVALID;
    }

    PayloadLen = (uint16_t)(Length - 2U);
    if (PayloadLen > AS608_MAX_PACKET_DATA_LEN)
    {
        return AS608_INVALID;
    }

    SumData[0] = Packet->PacketID;
    SumData[1] = (uint8_t)((Length >> 8) & 0xFFU);
    SumData[2] = (uint8_t)(Length & 0xFFU);

    for (Index = 0U; Index < PayloadLen; Index++)
    {
        if (AS608_WaitByte(&Byte, Start, TimeoutMs) != AS608_OK)
        {
            return AS608_TIMEOUT;
        }

        Packet->Data[Index] = Byte;
        SumData[(uint16_t)(3U + Index)] = Byte;
    }
    Packet->DataLength = PayloadLen;

    if (AS608_WaitByte(&CheckHigh, Start, TimeoutMs) != AS608_OK)
    {
        return AS608_TIMEOUT;
    }

    if (AS608_WaitByte(&CheckLow, Start, TimeoutMs) != AS608_OK)
    {
        return AS608_TIMEOUT;
    }

    Checksum = 0U;
    for (Index = 0U; Index < (uint16_t)(3U + PayloadLen); Index++)
    {
        Checksum = (uint16_t)(Checksum + SumData[Index]);
    }

    if ((CheckHigh != (uint8_t)((Checksum >> 8) & 0xFFU)) || (CheckLow != (uint8_t)(Checksum & 0xFFU)))
    {
        return AS608_INVALID;
    }

    AS608_DeviceAddress = Packet->Address;
    return AS608_OK;
}

static uint8_t AS608_CommandAck(uint8_t Command, const uint8_t *Params, uint16_t ParamLen, AS608_Packet_t *Packet)
{
    uint8_t Result;

    AS608_RxFlush();
    Result = AS608_SendCommandPacket(Command, Params, ParamLen);
    if (Result != AS608_OK)
    {
        return Result;
    }

    return AS608_ReadPacket(Packet, AS608_FRAME_TIMEOUT_MS);
}

static uint8_t AS608_ReadAckCode(const AS608_Packet_t *Packet, uint8_t *AckCode)
{
    if ((Packet == 0) || (AckCode == 0) || (Packet->PacketID != AS608_PID_ACK) || (Packet->DataLength < 1U))
    {
        return AS608_INVALID;
    }

    *AckCode = Packet->Data[0];
    return AS608_OK;
}

void AS608_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_APB2PeriphClockCmd(AS608_GPIO_RCC, ENABLE);
    RCC_APB2PeriphClockCmd(AS608_USART_RCC, ENABLE);

    GPIO_InitStructure.GPIO_Pin = AS608_TX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(AS608_GPIO_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = AS608_RX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(AS608_GPIO_PORT, &GPIO_InitStructure);

    USART_DeInit(AS608_USART);
    USART_InitStructure.USART_BaudRate = AS608_BAUDRATE_DEFAULT;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_2;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(AS608_USART, &USART_InitStructure);
    USART_Cmd(AS608_USART, ENABLE);
    USART_ITConfig(AS608_USART, USART_IT_RXNE, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1U;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1U;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    AS608_RxFlush();
    AS608_DeviceAddress = AS608_DEFAULT_ADDRESS;
}

uint8_t AS608_VerifyPassword(uint32_t Password, uint32_t *Address)
{
    AS608_Packet_t Packet;
    uint8_t Param[4];
    uint8_t AckCode;
    uint8_t Result;

    Param[0] = (uint8_t)((Password >> 24) & 0xFFU);
    Param[1] = (uint8_t)((Password >> 16) & 0xFFU);
    Param[2] = (uint8_t)((Password >> 8) & 0xFFU);
    Param[3] = (uint8_t)(Password & 0xFFU);

    Result = AS608_CommandAck(AS608_CMD_VERIFY_PWD, Param, 4U, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if (AS608_ReadAckCode(&Packet, &AckCode) != AS608_OK)
    {
        return AS608_INVALID;
    }

    if (AckCode != AS608_OK)
    {
        return AckCode;
    }

    if (Address != 0)
    {
        *Address = AS608_DeviceAddress;
    }

    return AS608_OK;
}

uint8_t AS608_Handshake(uint32_t *Address)
{
    return AS608_VerifyPassword(AS608_DEFAULT_PASSWORD, Address);
}

uint8_t AS608_SetPassword(uint32_t Password)
{
    AS608_Packet_t Packet;
    uint8_t Param[4];
    uint8_t AckCode;
    uint8_t Result;

    Param[0] = (uint8_t)((Password >> 24) & 0xFFU);
    Param[1] = (uint8_t)((Password >> 16) & 0xFFU);
    Param[2] = (uint8_t)((Password >> 8) & 0xFFU);
    Param[3] = (uint8_t)(Password & 0xFFU);

    Result = AS608_CommandAck(AS608_CMD_SET_PWD, Param, 4U, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if (AS608_ReadAckCode(&Packet, &AckCode) != AS608_OK)
    {
        return AS608_INVALID;
    }

    return AckCode;
}

uint8_t AS608_SetChipAddr(uint32_t NewAddress)
{
    AS608_Packet_t Packet;
    uint8_t Param[4];
    uint8_t AckCode;
    uint8_t Result;

    Param[0] = (uint8_t)((NewAddress >> 24) & 0xFFU);
    Param[1] = (uint8_t)((NewAddress >> 16) & 0xFFU);
    Param[2] = (uint8_t)((NewAddress >> 8) & 0xFFU);
    Param[3] = (uint8_t)(NewAddress & 0xFFU);

    Result = AS608_CommandAck(AS608_CMD_SET_ADDR, Param, 4U, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if (AS608_ReadAckCode(&Packet, &AckCode) != AS608_OK)
    {
        return AS608_INVALID;
    }

    if (AckCode == AS608_OK)
    {
        AS608_DeviceAddress = NewAddress;
    }

    return AckCode;
}

uint8_t AS608_ReadSysPara(AS608_SysPara_t *Para)
{
    AS608_Packet_t Packet;
    uint8_t AckCode;
    uint8_t Result;

    if (Para == 0)
    {
        return AS608_INVALID;
    }

    Result = AS608_CommandAck(AS608_CMD_READ_SYS_PARA, 0, 0, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if ((AS608_ReadAckCode(&Packet, &AckCode) != AS608_OK) || (Packet.DataLength < 17U))
    {
        return AS608_INVALID;
    }

    if (AckCode != AS608_OK)
    {
        return AckCode;
    }

    Para->StatusReg = (uint16_t)(((uint16_t)Packet.Data[1] << 8) | Packet.Data[2]);
    Para->SensorType = (uint16_t)(((uint16_t)Packet.Data[3] << 8) | Packet.Data[4]);
    Para->Capacity = (uint16_t)(((uint16_t)Packet.Data[5] << 8) | Packet.Data[6]);
    Para->SecurityLevel = (uint16_t)(((uint16_t)Packet.Data[7] << 8) | Packet.Data[8]);
    Para->DeviceAddress = ((uint32_t)Packet.Data[9] << 24) |
                          ((uint32_t)Packet.Data[10] << 16) |
                          ((uint32_t)Packet.Data[11] << 8) |
                          (uint32_t)Packet.Data[12];
    Para->DataPacketSize = (uint16_t)(((uint16_t)Packet.Data[13] << 8) | Packet.Data[14]);
    Para->BaudRateSetting = (uint16_t)(((uint16_t)Packet.Data[15] << 8) | Packet.Data[16]);
    return AS608_OK;
}

uint8_t AS608_GetImage(void)
{
    AS608_Packet_t Packet;
    uint8_t AckCode;
    uint8_t Result;

    Result = AS608_CommandAck(AS608_CMD_GET_IMAGE, 0, 0, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if (AS608_ReadAckCode(&Packet, &AckCode) != AS608_OK)
    {
        return AS608_INVALID;
    }

    return AckCode;
}

uint8_t AS608_Image2Tz(uint8_t BufferID)
{
    AS608_Packet_t Packet;
    uint8_t AckCode;
    uint8_t Param;
    uint8_t Result;

    if ((BufferID != AS608_BUFFER_1) && (BufferID != AS608_BUFFER_2))
    {
        return AS608_INVALID;
    }

    Param = BufferID;
    Result = AS608_CommandAck(AS608_CMD_GEN_CHAR, &Param, 1U, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if (AS608_ReadAckCode(&Packet, &AckCode) != AS608_OK)
    {
        return AS608_INVALID;
    }

    return AckCode;
}

uint8_t AS608_Compare(void)
{
    AS608_Packet_t Packet;
    uint8_t AckCode;
    uint8_t Result;

    Result = AS608_CommandAck(AS608_CMD_MATCH, 0, 0, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if ((Packet.PacketID != AS608_PID_ACK) || (Packet.DataLength < 3U))
    {
        return AS608_INVALID;
    }

    AckCode = Packet.Data[0];
    return AckCode;
}

uint8_t AS608_Search(uint8_t BufferID, uint16_t StartPage, uint16_t PageNum, uint16_t *PageID, uint16_t *Score)
{
    AS608_Packet_t Packet;
    uint8_t Param[5];
    uint8_t AckCode;
    uint8_t Result;

    if (((BufferID != AS608_BUFFER_1) && (BufferID != AS608_BUFFER_2)) || (PageID == 0) || (Score == 0))
    {
        return AS608_INVALID;
    }

    Param[0] = BufferID;
    Param[1] = (uint8_t)((StartPage >> 8) & 0xFFU);
    Param[2] = (uint8_t)(StartPage & 0xFFU);
    Param[3] = (uint8_t)((PageNum >> 8) & 0xFFU);
    Param[4] = (uint8_t)(PageNum & 0xFFU);

    Result = AS608_CommandAck(AS608_CMD_SEARCH, Param, 5U, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if ((Packet.PacketID != AS608_PID_ACK) || (Packet.DataLength < 5U))
    {
        return AS608_INVALID;
    }

    AckCode = Packet.Data[0];
    if (AckCode != AS608_OK)
    {
        return AckCode;
    }

    *PageID = (uint16_t)(((uint16_t)Packet.Data[1] << 8) | Packet.Data[2]);
    *Score = (uint16_t)(((uint16_t)Packet.Data[3] << 8) | Packet.Data[4]);
    return AS608_OK;
}

uint8_t AS608_RegModel(void)
{
    AS608_Packet_t Packet;
    uint8_t AckCode;
    uint8_t Result;

    Result = AS608_CommandAck(AS608_CMD_REG_MODEL, 0, 0, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if (AS608_ReadAckCode(&Packet, &AckCode) != AS608_OK)
    {
        return AS608_INVALID;
    }

    return AckCode;
}

uint8_t AS608_StoreModel(uint8_t BufferID, uint16_t PageID)
{
    AS608_Packet_t Packet;
    uint8_t Param[3];
    uint8_t AckCode;
    uint8_t Result;

    if ((BufferID != AS608_BUFFER_1) && (BufferID != AS608_BUFFER_2))
    {
        return AS608_INVALID;
    }

    Param[0] = BufferID;
    Param[1] = (uint8_t)((PageID >> 8) & 0xFFU);
    Param[2] = (uint8_t)(PageID & 0xFFU);

    Result = AS608_CommandAck(AS608_CMD_STORE_CHAR, Param, 3U, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if (AS608_ReadAckCode(&Packet, &AckCode) != AS608_OK)
    {
        return AS608_INVALID;
    }

    return AckCode;
}

uint8_t AS608_DeleteModel(uint16_t PageID, uint16_t Num)
{
    AS608_Packet_t Packet;
    uint8_t Param[4];
    uint8_t AckCode;
    uint8_t Result;

    Param[0] = (uint8_t)((PageID >> 8) & 0xFFU);
    Param[1] = (uint8_t)(PageID & 0xFFU);
    Param[2] = (uint8_t)((Num >> 8) & 0xFFU);
    Param[3] = (uint8_t)(Num & 0xFFU);

    Result = AS608_CommandAck(AS608_CMD_DELETE_CHAR, Param, 4U, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if (AS608_ReadAckCode(&Packet, &AckCode) != AS608_OK)
    {
        return AS608_INVALID;
    }

    return AckCode;
}

uint8_t AS608_Empty(void)
{
    AS608_Packet_t Packet;
    uint8_t AckCode;
    uint8_t Result;

    Result = AS608_CommandAck(AS608_CMD_EMPTY, 0, 0, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if (AS608_ReadAckCode(&Packet, &AckCode) != AS608_OK)
    {
        return AS608_INVALID;
    }

    return AckCode;
}

uint8_t AS608_AutoEnroll(uint16_t *PageID)
{
    AS608_Packet_t Packet;
    uint8_t AckCode;
    uint8_t Result;

    Result = AS608_CommandAck(AS608_CMD_ENROLL, 0, 0, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if ((Packet.PacketID != AS608_PID_ACK) || (Packet.DataLength < 3U))
    {
        return AS608_INVALID;
    }

    AckCode = Packet.Data[0];
    if (AckCode != AS608_OK)
    {
        return AckCode;
    }

    if (PageID != 0)
    {
        *PageID = (uint16_t)(((uint16_t)Packet.Data[1] << 8) | Packet.Data[2]);
    }

    return AS608_OK;
}

uint8_t AS608_Identify(uint16_t *PageID, uint16_t *Score)
{
    AS608_Packet_t Packet;
    uint8_t AckCode;
    uint8_t Result;

    if ((PageID == 0) || (Score == 0))
    {
        return AS608_INVALID;
    }

    Result = AS608_CommandAck(AS608_CMD_IDENTIFY, 0, 0, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if ((Packet.PacketID != AS608_PID_ACK) || (Packet.DataLength < 5U))
    {
        return AS608_INVALID;
    }

    AckCode = Packet.Data[0];
    if (AckCode != AS608_OK)
    {
        return AckCode;
    }

    *PageID = (uint16_t)(((uint16_t)Packet.Data[1] << 8) | Packet.Data[2]);
    *Score = (uint16_t)(((uint16_t)Packet.Data[3] << 8) | Packet.Data[4]);
    return AS608_OK;
}

uint8_t AS608_HighSpeedSearch(uint8_t BufferID, uint16_t StartPage, uint16_t PageNum, uint16_t *PageID, uint16_t *Score)
{
    AS608_Packet_t Packet;
    uint8_t Param[5];
    uint8_t AckCode;
    uint8_t Result;

    if (((BufferID != AS608_BUFFER_1) && (BufferID != AS608_BUFFER_2)) || (PageID == 0) || (Score == 0))
    {
        return AS608_INVALID;
    }

    Param[0] = BufferID;
    Param[1] = (uint8_t)((StartPage >> 8) & 0xFFU);
    Param[2] = (uint8_t)(StartPage & 0xFFU);
    Param[3] = (uint8_t)((PageNum >> 8) & 0xFFU);
    Param[4] = (uint8_t)(PageNum & 0xFFU);

    Result = AS608_CommandAck(AS608_CMD_HIGH_SPEED_SEARCH, Param, 5U, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if ((Packet.PacketID != AS608_PID_ACK) || (Packet.DataLength < 5U))
    {
        return AS608_INVALID;
    }

    AckCode = Packet.Data[0];
    if (AckCode != AS608_OK)
    {
        return AckCode;
    }

    *PageID = (uint16_t)(((uint16_t)Packet.Data[1] << 8) | Packet.Data[2]);
    *Score = (uint16_t)(((uint16_t)Packet.Data[3] << 8) | Packet.Data[4]);
    return AS608_OK;
}

uint8_t AS608_ValidTemplateNum(uint16_t *Count)
{
    AS608_Packet_t Packet;
    uint8_t AckCode;
    uint8_t Result;

    if (Count == 0)
    {
        return AS608_INVALID;
    }

    Result = AS608_CommandAck(AS608_CMD_VALID_TEMPLATE_NUM, 0, 0, &Packet);
    if (Result != AS608_OK)
    {
        return Result;
    }

    if ((Packet.PacketID != AS608_PID_ACK) || (Packet.DataLength < 3U))
    {
        return AS608_INVALID;
    }

    AckCode = Packet.Data[0];
    if (AckCode != AS608_OK)
    {
        return AckCode;
    }

    *Count = (uint16_t)(((uint16_t)Packet.Data[1] << 8) | Packet.Data[2]);
    return AS608_OK;
}

uint8_t AS608_Enroll(uint16_t PageID)
{
    uint8_t Result;
    uint16_t FoundPage;
    uint16_t Score;

    /*
     * This is a host-side enrollment flow helper.
     * The actual SOC auto-enroll command is AS608_AutoEnroll().
     */

    Result = AS608_GetImage();
    if (Result != AS608_OK)
    {
        return Result;
    }

    Result = AS608_Image2Tz(AS608_BUFFER_1);
    if (Result != AS608_OK)
    {
        return Result;
    }

    Result = AS608_GetImage();
    if (Result != AS608_OK)
    {
        return Result;
    }

    Result = AS608_Image2Tz(AS608_BUFFER_2);
    if (Result != AS608_OK)
    {
        return Result;
    }

    Result = AS608_Compare();
    if (Result != AS608_OK)
    {
        return Result;
    }

    Result = AS608_RegModel();
    if (Result != AS608_OK)
    {
        return Result;
    }

    Result = AS608_Search(AS608_BUFFER_1, 0U, 0x03FFU, &FoundPage, &Score);
    if (Result == AS608_OK)
    {
        if (FoundPage == PageID)
        {
            return AS608_INVALID;
        }
    }

    return AS608_StoreModel(AS608_BUFFER_1, PageID);
}

void USART1_IRQHandler(void)
{
    if (USART_GetFlagStatus(AS608_USART, USART_FLAG_ORE) != RESET)
    {
        volatile uint32_t Dummy;

        Dummy = AS608_USART->SR;
        Dummy = AS608_USART->DR;
        (void)Dummy;
        AS608_RxOverrun = 1U;
    }

    if (USART_GetITStatus(AS608_USART, USART_IT_RXNE) != RESET)
    {
        AS608_RxPush((uint8_t)USART_ReceiveData(AS608_USART));
    }
}
