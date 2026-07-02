#include "rtc.h"

#include <string.h>

#include "stm32f10x_bkp.h"
#include "stm32f10x_exti.h"
#include "stm32f10x_pwr.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_rtc.h"
#include "misc.h"
#include "timer.h"

#define APP_RTC_MAGIC              0xAC71A001UL
#define APP_RTC_SOURCE_NONE        0x0000U
#define APP_RTC_SOURCE_LSE         0x0001U
#define APP_RTC_SOURCE_LSI         0x0002U

#define APP_RTC_LSE_TIMEOUT_MS     3000U
#define APP_RTC_LSI_TIMEOUT_MS      100U
#define APP_RTC_SYNC_TIMEOUT_MS    1000U

#define APP_RTC_DEFAULT_YEAR       2026U
#define APP_RTC_DEFAULT_MONTH      1U
#define APP_RTC_DEFAULT_DAY        1U
#define APP_RTC_DEFAULT_HOUR       0U
#define APP_RTC_DEFAULT_MINUTE     0U
#define APP_RTC_DEFAULT_SECOND     0U

#define APP_RTC_TIMEOUT_TICKS      0x20000U

static uint8_t s_ready = 0U;
static volatile uint8_t s_alarm_triggered = 0U;

static uint8_t AppRTC_IsLeapYear(uint16_t Year)
{
    if (((Year % 4U) == 0U) && (((Year % 100U) != 0U) || ((Year % 400U) == 0U)))
    {
        return 1U;
    }

    return 0U;
}

static uint8_t AppRTC_DaysInMonth(uint16_t Year, uint8_t Month)
{
    static const uint8_t Days[12] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};

    if ((Month < 1U) || (Month > 12U))
    {
        return 0U;
    }

    if ((Month == 2U) && (AppRTC_IsLeapYear(Year) != 0U))
    {
        return 29U;
    }

    return Days[Month - 1U];
}

static uint8_t AppRTC_ParseMonth(const char *Text)
{
    if (Text == 0)
    {
        return 0U;
    }

    if ((Text[0] == 'J') && (Text[1] == 'a') && (Text[2] == 'n'))
    {
        return 1U;
    }
    if ((Text[0] == 'F') && (Text[1] == 'e') && (Text[2] == 'b'))
    {
        return 2U;
    }
    if ((Text[0] == 'M') && (Text[1] == 'a') && (Text[2] == 'r'))
    {
        return 3U;
    }
    if ((Text[0] == 'A') && (Text[1] == 'p') && (Text[2] == 'r'))
    {
        return 4U;
    }
    if ((Text[0] == 'M') && (Text[1] == 'a') && (Text[2] == 'y'))
    {
        return 5U;
    }
    if ((Text[0] == 'J') && (Text[1] == 'u') && (Text[2] == 'n'))
    {
        return 6U;
    }
    if ((Text[0] == 'J') && (Text[1] == 'u') && (Text[2] == 'l'))
    {
        return 7U;
    }
    if ((Text[0] == 'A') && (Text[1] == 'u') && (Text[2] == 'g'))
    {
        return 8U;
    }
    if ((Text[0] == 'S') && (Text[1] == 'e') && (Text[2] == 'p'))
    {
        return 9U;
    }
    if ((Text[0] == 'O') && (Text[1] == 'c') && (Text[2] == 't'))
    {
        return 10U;
    }
    if ((Text[0] == 'N') && (Text[1] == 'o') && (Text[2] == 'v'))
    {
        return 11U;
    }
    if ((Text[0] == 'D') && (Text[1] == 'e') && (Text[2] == 'c'))
    {
        return 12U;
    }

    return 0U;
}

static uint8_t AppRTC_CalcDayOfWeek(uint16_t Year, uint8_t Month, uint8_t Day)
{
    static const uint8_t MonthOffset[12] = {0U, 3U, 2U, 5U, 0U, 3U, 5U, 1U, 4U, 6U, 2U, 4U};

    if (Month < 3U)
    {
        Year--;
    }

    return (uint8_t)((Year + (Year / 4U) - (Year / 100U) + (Year / 400U) + MonthOffset[Month - 1U] + Day) % 7U);
}

static void AppRTC_GetBuildDefault(AppRTC_DateTime_t *DateTime)
{
    uint16_t Year;
    uint8_t Month;
    uint8_t Day;
    uint8_t Hour;
    uint8_t Minute;
    uint8_t Second;

    if (DateTime == 0)
    {
        return;
    }

    Month = AppRTC_ParseMonth(&__DATE__[0]);
    Day = (uint8_t)((__DATE__[4] == ' ') ? (uint8_t)(__DATE__[5] - '0') : (uint8_t)((__DATE__[4] - '0') * 10U + (__DATE__[5] - '0')));
    Year = (uint16_t)((__DATE__[7] - '0') * 1000U +
                      (__DATE__[8] - '0') * 100U +
                      (__DATE__[9] - '0') * 10U +
                      (__DATE__[10] - '0'));
    Hour = (uint8_t)((__TIME__[0] - '0') * 10U + (__TIME__[1] - '0'));
    Minute = (uint8_t)((__TIME__[3] - '0') * 10U + (__TIME__[4] - '0'));
    Second = (uint8_t)((__TIME__[6] - '0') * 10U + (__TIME__[7] - '0'));

    if ((Month < 1U) || (Month > 12U))
    {
        Year = APP_RTC_DEFAULT_YEAR;
        Month = APP_RTC_DEFAULT_MONTH;
        Day = APP_RTC_DEFAULT_DAY;
        Hour = APP_RTC_DEFAULT_HOUR;
        Minute = APP_RTC_DEFAULT_MINUTE;
        Second = APP_RTC_DEFAULT_SECOND;
    }

    if ((Year < 2000U) || (Year > 2099U) ||
        (AppRTC_DaysInMonth(Year, Month) == 0U) ||
        (Day < 1U) || (Day > AppRTC_DaysInMonth(Year, Month)))
    {
        Year = APP_RTC_DEFAULT_YEAR;
        Month = APP_RTC_DEFAULT_MONTH;
        Day = APP_RTC_DEFAULT_DAY;
        Hour = APP_RTC_DEFAULT_HOUR;
        Minute = APP_RTC_DEFAULT_MINUTE;
        Second = APP_RTC_DEFAULT_SECOND;
    }

    DateTime->Year = Year;
    DateTime->Month = Month;
    DateTime->Day = Day;
    DateTime->DayOfWeek = AppRTC_CalcDayOfWeek(Year, Month, Day);
    DateTime->Hour = Hour;
    DateTime->Minute = Minute;
    DateTime->Second = Second;
}

static uint8_t AppRTC_ValidateDateTime(const AppRTC_DateTime_t *DateTime)
{
    uint8_t MaxDay;

    if (DateTime == 0)
    {
        return 0U;
    }

    if ((DateTime->Year < 2000U) || (DateTime->Year > 2099U))
    {
        return 0U;
    }

    if ((DateTime->Month < 1U) || (DateTime->Month > 12U))
    {
        return 0U;
    }

    MaxDay = AppRTC_DaysInMonth(DateTime->Year, DateTime->Month);
    if ((DateTime->Day < 1U) || (DateTime->Day > MaxDay))
    {
        return 0U;
    }

    if (DateTime->Hour > 23U)
    {
        return 0U;
    }

    if (DateTime->Minute > 59U)
    {
        return 0U;
    }

    if (DateTime->Second > 59U)
    {
        return 0U;
    }

    return 1U;
}

static uint32_t AppRTC_DateTimeToSeconds(const AppRTC_DateTime_t *DateTime)
{
    uint32_t Days = 0U;
    uint16_t Year;
    uint8_t Month;

    if ((DateTime == 0) || (AppRTC_ValidateDateTime(DateTime) == 0U))
    {
        return 0U;
    }

    for (Year = 2000U; Year < DateTime->Year; Year++)
    {
        Days += (uint32_t)(AppRTC_IsLeapYear(Year) != 0U ? 366U : 365U);
    }

    for (Month = 1U; Month < DateTime->Month; Month++)
    {
        Days += (uint32_t)AppRTC_DaysInMonth(DateTime->Year, Month);
    }

    Days += (uint32_t)(DateTime->Day - 1U);

    return (((Days * 24U) + (uint32_t)DateTime->Hour) * 3600U) +
           ((uint32_t)DateTime->Minute * 60U) +
           (uint32_t)DateTime->Second;
}

static void AppRTC_SecondsToDateTime(uint32_t Seconds, AppRTC_DateTime_t *DateTime)
{
    uint32_t Days;
    uint32_t Remainder;
    uint32_t TotalDays;
    uint16_t Year;
    uint8_t Month;
    uint16_t MaxDay;

    if (DateTime == 0)
    {
        return;
    }

    TotalDays = Seconds / 86400U;
    Days = TotalDays;
    Remainder = Seconds % 86400U;

    Year = 2000U;
    while (1U)
    {
        MaxDay = (uint16_t)(AppRTC_IsLeapYear(Year) != 0U ? 366U : 365U);
        if (Days < (uint32_t)MaxDay)
        {
            break;
        }

        Days -= (uint32_t)MaxDay;
        Year++;
    }

    Month = 1U;
    while (1U)
    {
        MaxDay = AppRTC_DaysInMonth(Year, Month);
        if (Days < (uint32_t)MaxDay)
        {
            break;
        }

        Days -= (uint32_t)MaxDay;
        Month++;
    }

    DateTime->Year = Year;
    DateTime->Month = Month;
    DateTime->Day = (uint8_t)(Days + 1U);
    DateTime->DayOfWeek = (uint8_t)((6U + TotalDays) % 7U);
    DateTime->Hour = (uint8_t)(Remainder / 3600U);
    Remainder %= 3600U;
    DateTime->Minute = (uint8_t)(Remainder / 60U);
    DateTime->Second = (uint8_t)(Remainder % 60U);
}

static uint8_t AppRTC_WaitReady(uint8_t Flag, uint32_t Timeout)
{
    while (Timeout > 0U)
    {
        if (RCC_GetFlagStatus(Flag) != RESET)
        {
            return 1U;
        }

        Timeout--;
    }

    return 0U;
}

static uint8_t AppRTC_WaitReadyMs(uint8_t Flag, uint32_t TimeoutMs)
{
    uint32_t Start = GetSystemTick();

    while ((GetSystemTick() - Start) < TimeoutMs)
    {
        if (RCC_GetFlagStatus(Flag) != RESET)
        {
            return 1U;
        }
    }

    return 0U;
}

static uint8_t AppRTC_WaitForSynchro(uint32_t Timeout)
{
    RTC->CRL &= (uint16_t)~RTC_FLAG_RSF;

    while (Timeout > 0U)
    {
        if (RTC_GetFlagStatus(RTC_FLAG_RSF) != RESET)
        {
            return 1U;
        }

        Timeout--;
    }

    return 0U;
}

static uint8_t AppRTC_WaitForLastTask(uint32_t Timeout)
{
    while (Timeout > 0U)
    {
        if (RTC_GetFlagStatus(RTC_FLAG_RTOFF) != RESET)
        {
            return 1U;
        }

        Timeout--;
    }

    return 0U;
}

static void AppRTC_ConfigAlarmWakeup(void)
{
    EXTI_InitTypeDef EXTI_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);

    EXTI_ClearITPendingBit(EXTI_Line17);
    EXTI_InitStructure.EXTI_Line = EXTI_Line17;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);

    NVIC_InitStructure.NVIC_IRQChannel = RTCAlarm_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0U;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0U;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
}

static void AppRTC_Write2Digits(char *Buffer, uint8_t Value)
{
    Buffer[0] = (char)('0' + (Value / 10U));
    Buffer[1] = (char)('0' + (Value % 10U));
}

static void AppRTC_Write4Digits(char *Buffer, uint16_t Value)
{
    Buffer[0] = (char)('0' + (Value / 1000U) % 10U);
    Buffer[1] = (char)('0' + (Value / 100U) % 10U);
    Buffer[2] = (char)('0' + (Value / 10U) % 10U);
    Buffer[3] = (char)('0' + (Value % 10U));
}

void AppRTC_Init(void)
{
    AppRTC_DateTime_t DefaultTime;
    uint32_t Source;
    uint32_t Prescaler;

    s_ready = 0U;
    s_alarm_triggered = 0U;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
    AppRTC_ConfigAlarmWakeup();

    if (BKP_ReadBackupRegister(BKP_DR1) != (uint16_t)(APP_RTC_MAGIC & 0xFFFFU))
    {
        /* Cold boot: backup domain lost power, full re-initialization needed */
        BKP_DeInit();

        /* Try LSE first if not previously known to be bad */
        if (BKP_ReadBackupRegister(BKP_DR3) != APP_RTC_SOURCE_LSE)
        {
            /* Set LSE to high drive for reliable oscillation */
            RCC->BDCR |= (uint32_t)(0x03U << 3U);
            RCC_LSEConfig(RCC_LSE_ON);
            if (AppRTC_WaitReadyMs(RCC_FLAG_LSERDY, APP_RTC_LSE_TIMEOUT_MS) != 0U)
            {
                Source = APP_RTC_SOURCE_LSE;
                Prescaler = 32767U;
            }
            else
            {
                RCC_LSEConfig(RCC_LSE_OFF);
                /* LSE failed, mark it as unavailable */
                BKP_WriteBackupRegister(BKP_DR3, (uint16_t)APP_RTC_SOURCE_LSE);
                Source = APP_RTC_SOURCE_NONE;
            }
        }
        else
        {
            /* LSE known bad, skip directly to LSI */
            Source = APP_RTC_SOURCE_NONE;
        }

        /* Fall back to LSI if LSE not available */
        if (Source == APP_RTC_SOURCE_NONE)
        {
            RCC_LSICmd(ENABLE);
            if (AppRTC_WaitReadyMs(RCC_FLAG_LSIRDY, APP_RTC_LSI_TIMEOUT_MS) != 0U)
            {
                Source = APP_RTC_SOURCE_LSI;
                Prescaler = 39999U;
            }
            else
            {
                return;
            }
        }

        if (Source == APP_RTC_SOURCE_LSE)
        {
            RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);
        }
        else
        {
            RCC_RTCCLKConfig(RCC_RTCCLKSource_LSI);
        }

        RCC_RTCCLKCmd(ENABLE);
        if (AppRTC_WaitForSynchro(APP_RTC_TIMEOUT_TICKS) == 0U)
        {
            return;
        }

        AppRTC_GetBuildDefault(&DefaultTime);
        if (AppRTC_ValidateDateTime(&DefaultTime) == 0U)
        {
            DefaultTime.Year = APP_RTC_DEFAULT_YEAR;
            DefaultTime.Month = APP_RTC_DEFAULT_MONTH;
            DefaultTime.Day = APP_RTC_DEFAULT_DAY;
            DefaultTime.DayOfWeek = AppRTC_CalcDayOfWeek(APP_RTC_DEFAULT_YEAR, APP_RTC_DEFAULT_MONTH, APP_RTC_DEFAULT_DAY);
            DefaultTime.Hour = APP_RTC_DEFAULT_HOUR;
            DefaultTime.Minute = APP_RTC_DEFAULT_MINUTE;
            DefaultTime.Second = APP_RTC_DEFAULT_SECOND;
        }

        if (AppRTC_WaitForLastTask(APP_RTC_TIMEOUT_TICKS) == 0U)
        {
            return;
        }

        RTC_SetPrescaler(Prescaler);
        if (AppRTC_WaitForLastTask(APP_RTC_TIMEOUT_TICKS) == 0U)
        {
            return;
        }

        RTC_SetCounter(AppRTC_DateTimeToSeconds(&DefaultTime));
        if (AppRTC_WaitForLastTask(APP_RTC_TIMEOUT_TICKS) == 0U)
        {
            return;
        }

        BKP_WriteBackupRegister(BKP_DR1, (uint16_t)(APP_RTC_MAGIC & 0xFFFFU));
        BKP_WriteBackupRegister(BKP_DR2, (uint16_t)Source);
    }
    else
    {
        /* Warm boot: restore saved clock source and enable RTC */
        Source = (uint32_t)BKP_ReadBackupRegister(BKP_DR2);

        /* If BKP_DR2 has invalid value, default to LSI */
        if ((Source != APP_RTC_SOURCE_LSE) && (Source != APP_RTC_SOURCE_LSI))
        {
            Source = APP_RTC_SOURCE_LSI;
        }

        if (Source == APP_RTC_SOURCE_LSE)
        {
            RCC->BDCR |= (uint32_t)(0x03U << 3U);
            RCC_LSEConfig(RCC_LSE_ON);
            if (AppRTC_WaitReadyMs(RCC_FLAG_LSERDY, APP_RTC_LSE_TIMEOUT_MS) == 0U)
            {
                /* LSE was used before but now not ready, fall back to LSI */
                RCC_LSEConfig(RCC_LSE_OFF);
                Source = APP_RTC_SOURCE_LSI;
            }
            else
            {
                RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);
            }
        }

        if (Source == APP_RTC_SOURCE_LSI)
        {
            RCC_LSICmd(ENABLE);
            if (AppRTC_WaitReadyMs(RCC_FLAG_LSIRDY, APP_RTC_LSI_TIMEOUT_MS) != 0U)
            {
                RCC_RTCCLKConfig(RCC_RTCCLKSource_LSI);
            }
            else
            {
                return;
            }
        }

        RCC_RTCCLKCmd(ENABLE);
        if (AppRTC_WaitForSynchro(APP_RTC_TIMEOUT_TICKS) == 0U)
        {
            return;
        }
    }

    s_ready = 1U;
    AppRTC_CancelAlarm();
}

uint8_t AppRTC_IsReady(void)
{
    return s_ready;
}

void AppRTC_GetDateTime(AppRTC_DateTime_t *DateTime)
{
    if (DateTime == 0)
    {
        return;
    }

    if (s_ready == 0U)
    {
        AppRTC_GetBuildDefault(DateTime);
        return;
    }

    if (AppRTC_WaitForSynchro(APP_RTC_TIMEOUT_TICKS) == 0U)
    {
        AppRTC_GetBuildDefault(DateTime);
        return;
    }

    AppRTC_SecondsToDateTime(RTC_GetCounter(), DateTime);
}

uint8_t AppRTC_SetDateTime(const AppRTC_DateTime_t *DateTime)
{
    uint32_t Seconds;

    if ((s_ready == 0U) || (AppRTC_ValidateDateTime(DateTime) == 0U))
    {
        return 0U;
    }

    Seconds = AppRTC_DateTimeToSeconds(DateTime);
    if (Seconds == 0U)
    {
        return 0U;
    }

    if (AppRTC_WaitForLastTask(APP_RTC_TIMEOUT_TICKS) == 0U)
    {
        return 0U;
    }

    RTC_SetCounter(Seconds);
    if (AppRTC_WaitForLastTask(APP_RTC_TIMEOUT_TICKS) == 0U)
    {
        return 0U;
    }

    return 1U;
}

uint8_t AppRTC_SetAlarm(uint32_t SecondsFromNow)
{
    uint32_t Current;
    uint32_t AlarmValue;

    if ((s_ready == 0U) || (SecondsFromNow == 0U))
    {
        return 0U;
    }

    if (AppRTC_WaitForLastTask(APP_RTC_TIMEOUT_TICKS) == 0U)
    {
        return 0U;
    }

    if (AppRTC_WaitForSynchro(APP_RTC_TIMEOUT_TICKS) == 0U)
    {
        return 0U;
    }

    Current = RTC_GetCounter();
    if (SecondsFromNow > (0xFFFFFFFFUL - Current))
    {
        return 0U;
    }

    AlarmValue = Current + SecondsFromNow;
    if ((AlarmValue == 0U) || (AlarmValue <= Current))
    {
        return 0U;
    }

    RTC_ITConfig(RTC_IT_ALR, DISABLE);
    RTC_ClearFlag(RTC_FLAG_ALR);
    RTC_ClearITPendingBit(RTC_IT_ALR);
    EXTI_ClearITPendingBit(EXTI_Line17);

    RTC_SetAlarm(AlarmValue);
    if (AppRTC_WaitForLastTask(APP_RTC_TIMEOUT_TICKS) == 0U)
    {
        return 0U;
    }

    s_alarm_triggered = 0U;
    RTC_ClearFlag(RTC_FLAG_ALR);
    RTC_ClearITPendingBit(RTC_IT_ALR);
    EXTI_ClearITPendingBit(EXTI_Line17);
    RTC_ITConfig(RTC_IT_ALR, ENABLE);
    return 1U;
}

void AppRTC_CancelAlarm(void)
{
    if (s_ready == 0U)
    {
        return;
    }

    RTC_ITConfig(RTC_IT_ALR, DISABLE);
    RTC_ClearFlag(RTC_FLAG_ALR);
    RTC_ClearITPendingBit(RTC_IT_ALR);
    EXTI_ClearITPendingBit(EXTI_Line17);
    s_alarm_triggered = 0U;
}

uint8_t AppRTC_ConsumeAlarmFlag(void)
{
    uint8_t Fired = s_alarm_triggered;

    s_alarm_triggered = 0U;
    return Fired;
}

void AppRTC_FormatDate(const AppRTC_DateTime_t *DateTime, char *Buffer, uint8_t BufferSize)
{
    if ((Buffer == 0) || (BufferSize < 11U))
    {
        if (Buffer != 0)
        {
            Buffer[0] = '\0';
        }
        return;
    }

    if ((DateTime == 0) || (AppRTC_ValidateDateTime(DateTime) == 0U))
    {
        strncpy(Buffer, "0000/00/00", BufferSize);
        Buffer[10] = '\0';
        return;
    }

    AppRTC_Write4Digits(&Buffer[0], DateTime->Year);
    Buffer[4] = '/';
    AppRTC_Write2Digits(&Buffer[5], DateTime->Month);
    Buffer[7] = '/';
    AppRTC_Write2Digits(&Buffer[8], DateTime->Day);
    Buffer[10] = '\0';
}

void AppRTC_FormatTime(const AppRTC_DateTime_t *DateTime, char *Buffer, uint8_t BufferSize)
{
    if ((Buffer == 0) || (BufferSize < 9U))
    {
        if (Buffer != 0)
        {
            Buffer[0] = '\0';
        }
        return;
    }

    if ((DateTime == 0) || (AppRTC_ValidateDateTime(DateTime) == 0U))
    {
        strncpy(Buffer, "00:00:00", BufferSize);
        Buffer[8] = '\0';
        return;
    }

    AppRTC_Write2Digits(&Buffer[0], DateTime->Hour);
    Buffer[2] = ':';
    AppRTC_Write2Digits(&Buffer[3], DateTime->Minute);
    Buffer[5] = ':';
    AppRTC_Write2Digits(&Buffer[6], DateTime->Second);
    Buffer[8] = '\0';
}

void AppRTC_RTC_IRQHandler(void)
{
    if (RTC_GetITStatus(RTC_IT_SEC) != RESET)
    {
        RTC_ClearITPendingBit(RTC_IT_SEC);
    }

    if (RTC_GetITStatus(RTC_IT_OW) != RESET)
    {
        RTC_ClearITPendingBit(RTC_IT_OW);
    }
}

void AppRTC_RTCAlarm_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line17) != RESET)
    {
        EXTI_ClearITPendingBit(EXTI_Line17);
    }

    if (RTC_GetITStatus(RTC_IT_ALR) != RESET)
    {
        RTC_ClearITPendingBit(RTC_IT_ALR);
        s_alarm_triggered = 1U;
    }
}
