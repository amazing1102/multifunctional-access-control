#include "bluetooth.h"
#include "timer.h"
#include "misc.h"
#include <string.h>

/*
 * Bluetooth module (HC-05 compatible) on USART2:
 *   PA2 -> TX
 *   PA3 -> RX
 *
 * Design goals:
 *   - RX bytes are collected in the USART2 ISR only.
 *   - Frame parsing runs in Bluetooth_Task() from the main loop.
 *   - No dynamic memory allocation is used.
 *   - Both byte buffer and frame queue overwrite the oldest data on overflow.
 *   - Optional STATE/EN pin handling is compile-time configurable.
 */

#define BLUETOOTH_USART                 USART2
#define BLUETOOTH_GPIO_PORT             GPIOA
#define BLUETOOTH_GPIO_RCC              (RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO)
#define BLUETOOTH_USART_RCC             RCC_APB1Periph_USART2
#define BLUETOOTH_TX_PIN                GPIO_Pin_2
#define BLUETOOTH_RX_PIN                GPIO_Pin_3
#define BLUETOOTH_BAUDRATE              9600U
#define BLUETOOTH_MAX_BODY_LEN          (2U + BLUETOOTH_MAX_DATA_LEN)
#define BLUETOOTH_TX_FRAME_MAX_LEN      (6U + BLUETOOTH_MAX_DATA_LEN)

typedef enum
{
    BLUETOOTH_PARSE_WAIT_SOF1 = 0U,
    BLUETOOTH_PARSE_WAIT_SOF2,
    BLUETOOTH_PARSE_WAIT_LEN,
    BLUETOOTH_PARSE_WAIT_BODY,
    BLUETOOTH_PARSE_WAIT_CHK
} Bluetooth_ParseState_t;

static volatile uint8_t Bluetooth_RxBuffer[BLUETOOTH_RX_BYTE_BUFFER_SIZE];
static volatile uint8_t Bluetooth_RxHead = 0U;
static volatile uint8_t Bluetooth_RxTail = 0U;

static Bluetooth_Frame_t Bluetooth_FrameQueue[BLUETOOTH_FRAME_QUEUE_SIZE];
static uint8_t Bluetooth_FrameHead = 0U;
static uint8_t Bluetooth_FrameTail = 0U;

static Bluetooth_ParseState_t Bluetooth_ParseState = BLUETOOTH_PARSE_WAIT_SOF1;
static uint8_t Bluetooth_FrameBodyLen = 0U;
static uint8_t Bluetooth_FrameBodyIndex = 0U;
static uint8_t Bluetooth_FrameChecksum = 0U;
static uint8_t Bluetooth_FrameBody[2U + BLUETOOTH_MAX_DATA_LEN];
static uint8_t Bluetooth_Seq = 0U;
static uint32_t Bluetooth_TimeoutMs = 0U;
static volatile uint32_t Bluetooth_LastRxTime = 0U;
static volatile uint8_t Bluetooth_RxOverrunFlag = 0U;
volatile uint8_t Bluetooth_RxActivityFlag = 0U;

static uint8_t Bluetooth_RxAvailable(void)
{
    if (Bluetooth_RxHead >= Bluetooth_RxTail)
    {
        return (uint8_t)(Bluetooth_RxHead - Bluetooth_RxTail);
    }

    return (uint8_t)(BLUETOOTH_RX_BYTE_BUFFER_SIZE - Bluetooth_RxTail + Bluetooth_RxHead);
}

static uint8_t Bluetooth_RxPop(uint8_t *Byte)
{
    if ((Byte == 0) || (Bluetooth_RxHead == Bluetooth_RxTail))
    {
        return 0U;
    }

    *Byte = Bluetooth_RxBuffer[Bluetooth_RxTail];
    Bluetooth_RxTail = (uint8_t)((Bluetooth_RxTail + 1U) % BLUETOOTH_RX_BYTE_BUFFER_SIZE);
    return 1U;
}

static void Bluetooth_RxPush(uint8_t Byte)
{
    uint8_t NextHead;

    NextHead = (uint8_t)((Bluetooth_RxHead + 1U) % BLUETOOTH_RX_BYTE_BUFFER_SIZE);
    if (NextHead == Bluetooth_RxTail)
    {
        /* Overwrite oldest byte to avoid overflow deadlock. */
        Bluetooth_RxTail = (uint8_t)((Bluetooth_RxTail + 1U) % BLUETOOTH_RX_BYTE_BUFFER_SIZE);
    }

    Bluetooth_RxBuffer[Bluetooth_RxHead] = Byte;
    Bluetooth_RxHead = NextHead;
}

static void Bluetooth_FrameQueuePush(const Bluetooth_Frame_t *Frame)
{
    uint8_t NextHead;

    if (Frame == 0)
    {
        return;
    }

    Bluetooth_FrameQueue[Bluetooth_FrameHead] = *Frame;
    NextHead = (uint8_t)((Bluetooth_FrameHead + 1U) % BLUETOOTH_FRAME_QUEUE_SIZE);

    if (NextHead == Bluetooth_FrameTail)
    {
        Bluetooth_FrameTail = (uint8_t)((Bluetooth_FrameTail + 1U) % BLUETOOTH_FRAME_QUEUE_SIZE);
    }

    Bluetooth_FrameHead = NextHead;
}

static uint8_t Bluetooth_FrameQueuePop(Bluetooth_Frame_t *Frame)
{
    if ((Frame == 0) || (Bluetooth_FrameHead == Bluetooth_FrameTail))
    {
        return BLUETOOTH_ERR;
    }

    *Frame = Bluetooth_FrameQueue[Bluetooth_FrameTail];
    memset(&Bluetooth_FrameQueue[Bluetooth_FrameTail], 0, sizeof(Bluetooth_Frame_t));
    Bluetooth_FrameTail = (uint8_t)((Bluetooth_FrameTail + 1U) % BLUETOOTH_FRAME_QUEUE_SIZE);
    return BLUETOOTH_OK;
}

static uint8_t Bluetooth_CalcChecksum(const uint8_t *Data, uint8_t Length)
{
    uint16_t Sum;
    uint8_t Index;

    Sum = 0U;
    for (Index = 0U; Index < Length; Index++)
    {
        Sum = (uint16_t)(Sum + Data[Index]);
    }

    return (uint8_t)Sum;
}

static void Bluetooth_ResetParser(void)
{
    Bluetooth_ParseState = BLUETOOTH_PARSE_WAIT_SOF1;
    Bluetooth_FrameBodyLen = 0U;
    Bluetooth_FrameBodyIndex = 0U;
    Bluetooth_FrameChecksum = 0U;
    memset(Bluetooth_FrameBody, 0, sizeof(Bluetooth_FrameBody));
}

static uint8_t Bluetooth_WaitTxEmpty(void)
{
    uint32_t Timeout;

    Timeout = 0x20000U;
    while (USART_GetFlagStatus(BLUETOOTH_USART, USART_FLAG_TXE) == RESET)
    {
        if (Timeout-- == 0U)
        {
            return BLUETOOTH_ERR;
        }
    }

    return BLUETOOTH_OK;
}

static uint8_t Bluetooth_SendByte(uint8_t Byte)
{
    if (Bluetooth_WaitTxEmpty() != BLUETOOTH_OK)
    {
        return BLUETOOTH_ERR;
    }

    USART_SendData(BLUETOOTH_USART, Byte);
    return BLUETOOTH_OK;
}

static uint8_t Bluetooth_SendBuffer(const uint8_t *Data, uint8_t Length)
{
    uint8_t Index;

    if ((Length > 0U) && (Data == 0))
    {
        return BLUETOOTH_ERR;
    }

    for (Index = 0U; Index < Length; Index++)
    {
        if (Bluetooth_SendByte(Data[Index]) != BLUETOOTH_OK)
        {
            return BLUETOOTH_ERR;
        }
    }

    return BLUETOOTH_OK;
}

void Bluetooth_ClearFrame(Bluetooth_Frame_t *Frame)
{
    if (Frame == 0)
    {
        return;
    }

    Frame->Seq = 0U;
    Frame->Cmd = 0U;
    Frame->DataLen = 0U;
    memset(Frame->Data, 0, sizeof(Frame->Data));
}

void Bluetooth_SetEnKey(uint8_t Enable)
{
#if BLUETOOTH_USE_EN_PIN
    if (Enable != 0U)
    {
        GPIO_SetBits(BLUETOOTH_EN_PORT, BLUETOOTH_EN_PIN);
    }
    else
    {
        GPIO_ResetBits(BLUETOOTH_EN_PORT, BLUETOOTH_EN_PIN);
    }
#else
    (void)Enable;
#endif
}

void Bluetooth_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_APB2PeriphClockCmd(BLUETOOTH_GPIO_RCC, ENABLE);
    RCC_APB1PeriphClockCmd(BLUETOOTH_USART_RCC, ENABLE);

    /* Free PB3/PB4/PA15 if the board uses optional pins there. */
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);

    GPIO_InitStructure.GPIO_Pin = BLUETOOTH_TX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(BLUETOOTH_GPIO_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = BLUETOOTH_RX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(BLUETOOTH_GPIO_PORT, &GPIO_InitStructure);

#if BLUETOOTH_USE_STATE_PIN
    GPIO_InitStructure.GPIO_Pin = BLUETOOTH_STATE_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(BLUETOOTH_STATE_PORT, &GPIO_InitStructure);
#endif

#if BLUETOOTH_USE_EN_PIN
    GPIO_InitStructure.GPIO_Pin = BLUETOOTH_EN_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(BLUETOOTH_EN_PORT, &GPIO_InitStructure);
    Bluetooth_SetEnKey(0U);
#endif

    USART_DeInit(BLUETOOTH_USART);
    USART_InitStructure.USART_BaudRate = BLUETOOTH_BAUDRATE;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(BLUETOOTH_USART, &USART_InitStructure);

    USART_ITConfig(BLUETOOTH_USART, USART_IT_RXNE, ENABLE);
    USART_Cmd(BLUETOOTH_USART, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel = USART2_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 3U;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2U;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    Bluetooth_TimeoutMs = (uint32_t)BLUETOOTH_RX_TIMEOUT_MS;
    if (Bluetooth_TimeoutMs == 0U)
    {
        Bluetooth_TimeoutMs = 1U;
    }

    memset(Bluetooth_FrameQueue, 0, sizeof(Bluetooth_FrameQueue));
    memset(Bluetooth_FrameBody, 0, sizeof(Bluetooth_FrameBody));
    Bluetooth_RxHead = 0U;
    Bluetooth_RxTail = 0U;
    Bluetooth_FrameHead = 0U;
    Bluetooth_FrameTail = 0U;
    Bluetooth_Seq = 0U;
    Bluetooth_RxOverrunFlag = 0U;
    Bluetooth_ResetParser();

    /* Drain any junk bytes that may have arrived before USART was configured */
    while (USART_GetFlagStatus(BLUETOOTH_USART, USART_FLAG_RXNE) != RESET)
    {
        (void)USART_ReceiveData(BLUETOOTH_USART);
    }
    Bluetooth_LastRxTime = GetSystemTick();
}

void Bluetooth_SendFrameWithSeq(uint8_t Seq, uint8_t Cmd, const uint8_t *Data, uint8_t DataLen)
{
    uint8_t Frame[BLUETOOTH_TX_FRAME_MAX_LEN];
    uint8_t ChecksumLength;
    uint8_t Index;
    uint8_t BodyLen;
    uint8_t TotalLen;

    if ((DataLen > BLUETOOTH_MAX_DATA_LEN) || ((DataLen > 0U) && (Data == 0)))
    {
        return;
    }

    BodyLen = (uint8_t)(2U + DataLen);
    TotalLen = (uint8_t)(6U + DataLen);
    ChecksumLength = (uint8_t)(1U + BodyLen);

    Frame[0] = BLUETOOTH_FRAME_SOF1;
    Frame[1] = BLUETOOTH_FRAME_SOF2;
    Frame[2] = BodyLen;
    Frame[3] = Seq;
    Frame[4] = Cmd;

    for (Index = 0U; Index < DataLen; Index++)
    {
        Frame[(uint8_t)(5U + Index)] = Data[Index];
    }

    Frame[(uint8_t)(5U + DataLen)] = Bluetooth_CalcChecksum(&Frame[2], ChecksumLength);

    (void)Bluetooth_SendBuffer(Frame, TotalLen);
}

void Bluetooth_SendFrame(uint8_t Cmd, const uint8_t *Data, uint8_t DataLen)
{
    Bluetooth_SendFrameWithSeq(Bluetooth_Seq, Cmd, Data, DataLen);
    Bluetooth_Seq++;
}

void Bluetooth_SendRaw(const uint8_t *Data, uint8_t DataLen)
{
    (void)Bluetooth_SendBuffer(Data, DataLen);
}

uint8_t Bluetooth_GetFrame(Bluetooth_Frame_t *Frame)
{
    uint8_t Result;

    Result = Bluetooth_FrameQueuePop(Frame);
    if (Result == BLUETOOTH_OK)
    {
        /* The queue slot is scrubbed in Bluetooth_FrameQueuePop(). */
    }

    return Result;
}

Bluetooth_LinkState_t Bluetooth_GetLinkState(void)
{
#if BLUETOOTH_USE_STATE_PIN
    if (GPIO_ReadInputDataBit(BLUETOOTH_STATE_PORT, BLUETOOTH_STATE_PIN) == Bit_SET)
    {
        return BLUETOOTH_LINK_CONNECTED;
    }

    return BLUETOOTH_LINK_DISCONNECTED;
#else
    return BLUETOOTH_LINK_UNKNOWN;
#endif
}

uint8_t Bluetooth_IsConnected(void)
{
    return (uint8_t)(Bluetooth_GetLinkState() == BLUETOOTH_LINK_CONNECTED);
}

void Bluetooth_Task(void)
{
    uint8_t Byte;
    uint8_t Progressed;
    uint8_t i;
    Bluetooth_Frame_t ParsedFrame;

    if (Bluetooth_RxOverrunFlag != 0U)
    {
        Bluetooth_RxOverrunFlag = 0U;
        /* Only reset the frame parser on overrun, keep valid buffered bytes. */
        Bluetooth_ResetParser();
    }

    Progressed = 0U;

    while (Bluetooth_RxAvailable() > 0U)
    {
        if (Bluetooth_RxPop(&Byte) == 0U)
        {
            break;
        }

        Progressed = 1U;

        switch (Bluetooth_ParseState)
        {
            case BLUETOOTH_PARSE_WAIT_SOF1:
                if (Byte == BLUETOOTH_FRAME_SOF1)
                {
                    Bluetooth_ParseState = BLUETOOTH_PARSE_WAIT_SOF2;
                }
                break;

            case BLUETOOTH_PARSE_WAIT_SOF2:
                if (Byte == BLUETOOTH_FRAME_SOF2)
                {
                    Bluetooth_ParseState = BLUETOOTH_PARSE_WAIT_LEN;
                }
                else if (Byte == BLUETOOTH_FRAME_SOF1)
                {
                    Bluetooth_ParseState = BLUETOOTH_PARSE_WAIT_SOF2;
                }
                else
                {
                    Bluetooth_ParseState = BLUETOOTH_PARSE_WAIT_SOF1;
                }
                break;

            case BLUETOOTH_PARSE_WAIT_LEN:
                if ((Byte < 2U) || (Byte > BLUETOOTH_MAX_BODY_LEN))
                {
                    Bluetooth_ResetParser();
                    if (Byte == BLUETOOTH_FRAME_SOF1)
                    {
                        Bluetooth_ParseState = BLUETOOTH_PARSE_WAIT_SOF2;
                    }
                    break;
                }

                Bluetooth_FrameBodyLen = Byte;
                Bluetooth_FrameBodyIndex = 0U;
                Bluetooth_FrameChecksum = Byte;
                Bluetooth_ParseState = BLUETOOTH_PARSE_WAIT_BODY;
                break;

            case BLUETOOTH_PARSE_WAIT_BODY:
                if (Bluetooth_FrameBodyIndex >= (uint8_t)sizeof(Bluetooth_FrameBody))
                {
                    Bluetooth_ResetParser();
                    break;
                }

                Bluetooth_FrameBody[Bluetooth_FrameBodyIndex] = Byte;
                Bluetooth_FrameChecksum = (uint8_t)(Bluetooth_FrameChecksum + Byte);
                Bluetooth_FrameBodyIndex++;

                if (Bluetooth_FrameBodyIndex >= Bluetooth_FrameBodyLen)
                {
                    Bluetooth_ParseState = BLUETOOTH_PARSE_WAIT_CHK;
                }
                break;

            case BLUETOOTH_PARSE_WAIT_CHK:
                if (Byte == Bluetooth_FrameChecksum)
                {
                    Bluetooth_ClearFrame(&ParsedFrame);

                    ParsedFrame.Seq = Bluetooth_FrameBody[0];
                    ParsedFrame.Cmd = Bluetooth_FrameBody[1];
                    ParsedFrame.DataLen = (uint8_t)(Bluetooth_FrameBodyLen - 2U);

                    if (ParsedFrame.DataLen > BLUETOOTH_MAX_DATA_LEN)
                    {
                        ParsedFrame.DataLen = BLUETOOTH_MAX_DATA_LEN;
                    }

                    if (ParsedFrame.DataLen > 0U)
                    {
                        for (i = 0U; i < ParsedFrame.DataLen; i++)
                        {
                            ParsedFrame.Data[i] = Bluetooth_FrameBody[(uint8_t)(2U + i)];
                        }
                    }

                    Bluetooth_FrameQueuePush(&ParsedFrame);
                    if (Byte == BLUETOOTH_FRAME_SOF1)
                    {
                        Bluetooth_ParseState = BLUETOOTH_PARSE_WAIT_SOF2;
                    }
                    else
                    {
                        Bluetooth_ParseState = BLUETOOTH_PARSE_WAIT_SOF1;
                    }
                }
                else
                {
                    Bluetooth_ResetParser();
                }
                Bluetooth_FrameBodyLen = 0U;
                Bluetooth_FrameBodyIndex = 0U;
                Bluetooth_FrameChecksum = 0U;
                break;

            default:
                Bluetooth_ResetParser();
                break;
        }
    }

    /* Timeout check AFTER processing buffered bytes:
     * If the parser is mid-frame and no new byte arrived for > timeout,
     * reset and wait for a fresh frame. */
    uint32_t NowTime = GetSystemTick();
    if ((Bluetooth_ParseState != BLUETOOTH_PARSE_WAIT_SOF1) &&
        ((uint32_t)(NowTime - Bluetooth_LastRxTime) > Bluetooth_TimeoutMs))
    {
        Bluetooth_ResetParser();
    }
}

void USART2_IRQHandler(void)
{
    if (USART_GetFlagStatus(BLUETOOTH_USART, USART_FLAG_ORE) != RESET)
    {
        volatile uint32_t Dummy;

        Dummy = BLUETOOTH_USART->SR;
        Dummy = BLUETOOTH_USART->DR;
        (void)Dummy;
        Bluetooth_RxOverrunFlag = 1U;
    }

    if (USART_GetITStatus(BLUETOOTH_USART, USART_IT_RXNE) != RESET)
    {
        Bluetooth_RxPush((uint8_t)USART_ReceiveData(BLUETOOTH_USART));
        Bluetooth_LastRxTime = GetSystemTick();
        Bluetooth_RxActivityFlag = 1U;
    }
}
