#include "JQ8900.h"
#include "misc.h"
#include "delay.h"

/*
 * JQ8900-16P voice module wiring:
 *   USART3_TX -> PB10
 *   USART3_RX -> PB11
 *   BUSY      -> PB9  (active high while audio is playing)
 *
 * This driver only pushes UART commands. It never waits for the module
 * to finish playback. The BUSY pin is exposed as a helper for higher-level
 * state machines that may choose to interrupt or skip commands.
 */

#define JQ8900_GPIO_RCC          RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO
#define JQ8900_USART_RCC         RCC_APB1Periph_USART3
#define JQ8900_GPIO_PORT         GPIOB
#define JQ8900_TX_PIN            GPIO_Pin_10
#define JQ8900_RX_PIN            GPIO_Pin_11
#define JQ8900_BUSY_PIN          GPIO_Pin_9
#define JQ8900_UART              USART3
#define JQ8900_BAUDRATE          9600U
#define JQ8900_RX_BUFFER_SIZE     64U

static volatile uint8_t JQ8900_RxBuffer[JQ8900_RX_BUFFER_SIZE];
static volatile uint8_t JQ8900_RxHead = 0U;
static volatile uint8_t JQ8900_RxTail = 0U;

static uint8_t JQ8900_RxAvailable(void)
{
    if (JQ8900_RxHead >= JQ8900_RxTail)
    {
        return (uint8_t)(JQ8900_RxHead - JQ8900_RxTail);
    }

    return (uint8_t)(JQ8900_RX_BUFFER_SIZE - JQ8900_RxTail + JQ8900_RxHead);
}

static uint8_t JQ8900_RxPop(uint8_t *Byte)
{
    if (Byte == 0)
    {
        return 0U;
    }

    if (JQ8900_RxHead == JQ8900_RxTail)
    {
        return 0U;
    }

    *Byte = JQ8900_RxBuffer[JQ8900_RxTail];
    JQ8900_RxTail = (uint8_t)((JQ8900_RxTail + 1U) % JQ8900_RX_BUFFER_SIZE);
    return 1U;
}

static uint8_t JQ8900_RxPeek(uint8_t Offset, uint8_t *Byte)
{
    uint8_t Index;
    uint8_t Available;

    if (Byte == 0)
    {
        return 0U;
    }

    Available = JQ8900_RxAvailable();
    if (Offset >= Available)
    {
        return 0U;
    }

    Index = JQ8900_RxTail;
    while (Offset-- != 0U)
    {
        Index = (uint8_t)((Index + 1U) % JQ8900_RX_BUFFER_SIZE);
    }

    *Byte = JQ8900_RxBuffer[Index];
    return 1U;
}

static void JQ8900_RxPush(uint8_t Byte)
{
    uint8_t NextHead = (uint8_t)((JQ8900_RxHead + 1U) % JQ8900_RX_BUFFER_SIZE);

    if (NextHead == JQ8900_RxTail)
    {
        return;
    }

    JQ8900_RxBuffer[JQ8900_RxHead] = Byte;
    JQ8900_RxHead = NextHead;
}

static uint8_t JQ8900_WaitTxEmpty(void)
{
    uint32_t Timeout = 0x20000U;

    while (USART_GetFlagStatus(JQ8900_UART, USART_FLAG_TXE) == RESET)
    {
        if (Timeout-- == 0U)
        {
            return 0U;
        }
    }

    return 1U;
}

static uint8_t JQ8900_SendByte(uint8_t Byte)
{
    if (JQ8900_WaitTxEmpty() == 0U)
    {
        return 0U;
    }

    USART_SendData(JQ8900_UART, Byte);
    return 1U;
}

static uint8_t JQ8900_AppendChecksum(uint8_t *Frame, uint8_t Length)
{
    uint16_t Sum = 0U;
    uint8_t i;

    if ((Frame == 0) || (Length < 3U))
    {
        return 0U;
    }

    for (i = 0U; i < Length; i++)
    {
        Sum += Frame[i];
    }

    Frame[Length] = (uint8_t)Sum;
    return 1U;
}

void JQ8900_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_APB2PeriphClockCmd(JQ8900_GPIO_RCC, ENABLE);
    RCC_APB1PeriphClockCmd(JQ8900_USART_RCC, ENABLE);

    GPIO_InitStructure.GPIO_Pin = JQ8900_TX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(JQ8900_GPIO_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = JQ8900_RX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(JQ8900_GPIO_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = JQ8900_BUSY_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(JQ8900_GPIO_PORT, &GPIO_InitStructure);

    USART_DeInit(JQ8900_UART);
    USART_InitStructure.USART_BaudRate = JQ8900_BAUDRATE;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(JQ8900_UART, &USART_InitStructure);
    USART_Cmd(JQ8900_UART, ENABLE);
    USART_ITConfig(JQ8900_UART, USART_IT_RXNE, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel = USART3_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2U;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2U;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    JQ8900_RxHead = 0U;
    JQ8900_RxTail = 0U;
}

void JQ8900_SendCommand(uint8_t Command, const uint8_t *Data, uint8_t Length)
{
    uint8_t Frame[32];
    uint8_t Index;

    if ((Length > 0U) && (Data == 0))
    {
        return;
    }

    if (((uint16_t)Length + 4U) > (uint16_t)sizeof(Frame))
    {
        return;
    }

    Frame[0] = JQ8900_CMD_HEADER;
    Frame[1] = Command;
    Frame[2] = Length;

    for (Index = 0U; Index < Length; Index++)
    {
        Frame[3U + Index] = Data[Index];
    }

    if (JQ8900_AppendChecksum(Frame, (uint8_t)(Length + 3U)) == 0U)
    {
        return;
    }

    for (Index = 0U; Index < (uint8_t)(Length + 4U); Index++)
    {
        if (JQ8900_SendByte(Frame[Index]) == 0U)
        {
            return;
        }
    }
}

uint8_t JQ8900_IsBusy(void)
{
    return (uint8_t)(GPIO_ReadInputDataBit(JQ8900_GPIO_PORT, JQ8900_BUSY_PIN) == Bit_SET);
}

uint8_t JQ8900_ReadPlayState(uint8_t *PlayState)
{
    uint8_t Frame[5];
    uint8_t i;
    uint16_t Checksum;

    if (PlayState == 0)
    {
        return MI_ERR;
    }

    while (JQ8900_RxAvailable() >= 5U)
    {
        if (JQ8900_RxPeek(0U, &Frame[0]) == 0U)
        {
            return MI_ERR;
        }

        if (Frame[0] != JQ8900_CMD_HEADER)
        {
            (void)JQ8900_RxPop(&Frame[0]);
            continue;
        }

        for (i = 1U; i < 5U; i++)
        {
            if (JQ8900_RxPeek(i, &Frame[i]) == 0U)
            {
                return MI_ERR;
            }
        }

        Checksum = (uint16_t)(Frame[0] + Frame[1] + Frame[2] + Frame[3]);
        if ((uint8_t)Checksum != Frame[4])
        {
            (void)JQ8900_RxPop(&Frame[0]);
            continue;
        }

        if ((Frame[1] == 0x01U) && (Frame[2] == 0x01U))
        {
            *PlayState = Frame[3];
            for (i = 0U; i < 5U; i++)
            {
                (void)JQ8900_RxPop(&Frame[0]);
            }
            return MI_OK;
        }

        (void)JQ8900_RxPop(&Frame[0]);
    }

    return MI_ERR;
}

void JQ8900_QueryPlayState(void)
{
    JQ8900_SendCommand(0x01U, 0, 0U);
}

void JQ8900_Play(void)
{
    JQ8900_SendCommand(0x02U, 0, 0U);
}

void JQ8900_Pause(void)
{
    JQ8900_SendCommand(0x03U, 0, 0U);
}

void JQ8900_Stop(void)
{
    JQ8900_SendCommand(0x04U, 0, 0U);
}

void JQ8900_Previous(void)
{
    JQ8900_SendCommand(0x05U, 0, 0U);
}

void JQ8900_Next(void)
{
    JQ8900_SendCommand(0x06U, 0, 0U);
}

void JQ8900_PlayTrack(uint16_t Track)
{
    uint8_t Data[2];

    Data[0] = (uint8_t)(Track >> 8);
    Data[1] = (uint8_t)Track;
    JQ8900_SendCommand(0x07U, Data, 2U);
}

void JQ8900_PlayTrackInserting(uint8_t Disk, uint16_t Track)
{
    uint8_t Data[3];

    /* Switch to target storage device first (0x0B) */
    Data[0] = Disk;
    JQ8900_SendCommand(0x0BU, Data, 1U);
    delay_ms(100U);

    /* Play specified track from the device (0x16) */
    Data[0] = Disk;
    Data[1] = (uint8_t)(Track >> 8);
    Data[2] = (uint8_t)Track;
    JQ8900_SendCommand(0x16U, Data, 3U);
}

void JQ8900_StopInsert(void)
{
    JQ8900_SendCommand(0x10U, 0, 0U);
}

void JQ8900_SetVolume(uint8_t Volume)
{
    uint8_t Data[1];

    if (Volume > 30U)
    {
        Volume = 30U;
    }

    Data[0] = Volume;
    JQ8900_SendCommand(0x13U, Data, 1U);
}

void JQ8900_VolumeUp(void)
{
    JQ8900_SendCommand(0x14U, 0, 0U);
}

void JQ8900_VolumeDown(void)
{
    JQ8900_SendCommand(0x15U, 0, 0U);
}

void JQ8900_SetLoopMode(uint8_t LoopMode)
{
    uint8_t Data[1];

    if (LoopMode > 7U)
    {
        LoopMode = 7U;
    }

    Data[0] = LoopMode;
    JQ8900_SendCommand(0x18U, Data, 1U);
}

void JQ8900_SetLoopCount(uint16_t Count)
{
    uint8_t Data[2];

    Data[0] = (uint8_t)(Count >> 8);
    Data[1] = (uint8_t)Count;
    JQ8900_SendCommand(0x19U, Data, 2U);
}

void JQ8900_SetEQ(uint8_t EQ)
{
    uint8_t Data[1];

    if (EQ > JQ8900_EQ_CLASSIC)
    {
        EQ = JQ8900_EQ_NORMAL;
    }

    Data[0] = EQ;
    JQ8900_SendCommand(0x1AU, Data, 1U);
}

void JQ8900_SetChannel(uint8_t Channel)
{
    uint8_t Data[1];

    if (Channel > JQ8900_CHANNEL_MIX)
    {
        Channel = JQ8900_CHANNEL_MP3;
    }

    Data[0] = Channel;
    JQ8900_SendCommand(0x1DU, Data, 1U);
}

void JQ8900_Sleep(void)
{
    JQ8900_SendCommand(0x1BU, 0, 0U);
}

void USART3_IRQHandler(void)
{
    if (USART_GetITStatus(JQ8900_UART, USART_IT_RXNE) != RESET)
    {
        JQ8900_RxPush((uint8_t)USART_ReceiveData(JQ8900_UART));
    }
}
