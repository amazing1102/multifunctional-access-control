#ifndef __JQ8900_H
#define __JQ8900_H

#include "stm32f10x.h"

#ifndef MI_OK
#define MI_OK                    0U
#endif

#ifndef MI_ERR
#define MI_ERR                   1U
#endif

#define JQ8900_CMD_HEADER        0xAAU

#define JQ8900_PLAY_STATE_STOP   0x00U
#define JQ8900_PLAY_STATE_PLAY   0x01U
#define JQ8900_PLAY_STATE_PAUSE  0x02U

#define JQ8900_DISK_USB          0x00U
#define JQ8900_DISK_SD           0x01U
#define JQ8900_DISK_FLASH        0x02U

#define JQ8900_EQ_NORMAL         0x00U
#define JQ8900_EQ_POP            0x01U
#define JQ8900_EQ_ROCK           0x02U
#define JQ8900_EQ_JAZZ           0x03U
#define JQ8900_EQ_CLASSIC        0x04U

#define JQ8900_CHANNEL_MP3       0x00U
#define JQ8900_CHANNEL_AUX       0x01U
#define JQ8900_CHANNEL_MIX       0x02U

void JQ8900_Init(void);
void JQ8900_SendCommand(uint8_t Command, const uint8_t *Data, uint8_t Length);
uint8_t JQ8900_IsBusy(void);
uint8_t JQ8900_ReadPlayState(uint8_t *PlayState);

void JQ8900_QueryPlayState(void);
void JQ8900_Play(void);
void JQ8900_Pause(void);
void JQ8900_Stop(void);
void JQ8900_Previous(void);
void JQ8900_Next(void);
void JQ8900_PlayTrack(uint16_t Track);
void JQ8900_PlayTrackInserting(uint8_t Disk, uint16_t Track);
void JQ8900_StopInsert(void);
void JQ8900_SetVolume(uint8_t Volume);
void JQ8900_VolumeUp(void);
void JQ8900_VolumeDown(void);
void JQ8900_SetLoopMode(uint8_t LoopMode);
void JQ8900_SetLoopCount(uint16_t Count);
void JQ8900_SetEQ(uint8_t EQ);
void JQ8900_SetChannel(uint8_t Channel);
void JQ8900_Sleep(void);

#endif
