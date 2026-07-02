#ifndef __APP_RTC_H
#define __APP_RTC_H

#include "stm32f10x.h"

typedef struct
{
    uint16_t Year;
    uint8_t Month;
    uint8_t Day;
    uint8_t DayOfWeek;
    uint8_t Hour;
    uint8_t Minute;
    uint8_t Second;
} AppRTC_DateTime_t;

void AppRTC_Init(void);
uint8_t AppRTC_IsReady(void);
void AppRTC_GetDateTime(AppRTC_DateTime_t *DateTime);
uint8_t AppRTC_SetDateTime(const AppRTC_DateTime_t *DateTime);
uint8_t AppRTC_SetAlarm(uint32_t SecondsFromNow);
void AppRTC_CancelAlarm(void);
uint8_t AppRTC_ConsumeAlarmFlag(void);
void AppRTC_FormatDate(const AppRTC_DateTime_t *DateTime, char *Buffer, uint8_t BufferSize);
void AppRTC_FormatTime(const AppRTC_DateTime_t *DateTime, char *Buffer, uint8_t BufferSize);
void AppRTC_RTC_IRQHandler(void);
void AppRTC_RTCAlarm_IRQHandler(void);

#endif
